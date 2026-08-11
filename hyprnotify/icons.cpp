// hyprnotify/icons.cpp — notification images: content avatars and identity
// icons via hyprgraphics, raw image-data pixmaps

#include "common/fileindex.hpp"
#include "common/icons.hpp"
#include "hyprbar/desktop_exec.hpp"

#include "ui.hpp"

#include <filesystem>
#include <sstream>

namespace NHyprnotify {

    namespace {
        // The gatherer owns one worker for the compositor. Keep this plugin's
        // contribution finite, including replaced/deleted cards whose decode
        // cannot be cancelled after enqueue.
        constexpr size_t MAX_PENDING_IMAGE_RESOURCES = 24;
        constexpr size_t MAX_IMAGE_FILE_BYTES        = 32 * 1024 * 1024;
        constexpr double MAX_DECODED_IMAGE_PIXELS    = 16.0 * 1024 * 1024;

        struct SDecodeJob {
            std::string                            source;
            int                                    svgPx            = 0;
            ASP<Hyprgraphics::CImageResource>      resource;
            bool                                   rejected          = false;
            bool                                   readyAtWarmStart = false;
            bool                                   usedThisWarm     = false;
        };

        struct SFileTexResult {
            SP<ITexture> texture;
            bool         settled = false;
        };

        std::vector<SDecodeJob>   decodeJobs;
        SP<CEventLoopTimer>       decodePoll;
        bool                      waitedForDecodeSlot = false;

        constexpr size_t MAX_DESKTOP_FILES   = 4096;
        constexpr size_t MAX_DESKTOP_VISITED = 16384;
        NHyprCommon::CAsyncFileIndex desktopIndex;
        SP<CEventLoopTimer>          desktopPoll;
        std::unordered_map<std::string, std::string> desktopIcons;
        uint64_t                     desktopGeneration = 0;
        bool                         desktopScanning   = false;
        std::unordered_map<std::string, SP<ITexture>> generatedAvatars;

        static std::string desktopKey(std::string value) {
            if (value.ends_with(".desktop"))
                value.erase(value.size() - 8);
            std::ranges::transform(value, value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return value;
        }

        static std::vector<std::string> desktopDirs() {
            std::vector<std::string> dirs;
            for (const auto& data : NHyprCommon::xdgDataDirs())
                dirs.push_back(data + "/applications");
            return dirs;
        }

        static void indexDesktopEntry(const NHyprCommon::CAsyncFileIndex::SEntry& entry) {
            std::istringstream file(entry.contents);
            std::string        icon, line;
            bool               inDesktopEntry = false;
            while (std::getline(file, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty() && line.front() == '[') {
                    if (inDesktopEntry)
                        break;
                    inDesktopEntry = line == "[Desktop Entry]";
                    continue;
                }
                if (!inDesktopEntry || !line.starts_with("Icon="))
                    continue;
                if (const auto value = NHyprbar::DesktopExec::unescapeString(std::string_view{line}.substr(5)); value && !value->empty())
                    icon = *value;
                break;
            }
            if (!icon.empty())
                desktopIcons.try_emplace(desktopKey(entry.path.stem().string()), std::move(icon));
        }

        static void startDesktopIndex() {
            desktopIcons.clear();
            desktopScanning = true;
            NHyprCommon::CAsyncFileIndex::SRequest request;
            request.generation   = ++desktopGeneration;
            request.extensions   = {".desktop"};
            request.maxEntries   = MAX_DESKTOP_FILES;
            request.maxVisited   = MAX_DESKTOP_VISITED;
            request.maxFileBytes = NHyprbar::DesktopExec::MAX_DESKTOP_FILE_BYTES;
            request.recursive    = true;
            request.roots        = std::ranges::to<std::vector<std::filesystem::path>>(desktopDirs());
            desktopIndex.request(std::move(request));
            if (desktopPoll)
                desktopPoll->updateTimeout(std::chrono::milliseconds(2));
        }

        static void pollDesktopIndex() {
            if (!desktopScanning)
                return;
            std::vector<NHyprCommon::CAsyncFileIndex::SEntry> entries;
            const bool COMPLETE = desktopIndex.poll(desktopGeneration, entries, 8);
            const bool FINISHED = COMPLETE;
            for (const auto& entry : entries)
                indexDesktopEntry(entry);

            bool identityChanged = false;
            const int ICONPX = std::max(8, (int)cfg.maxIcon->value());
            for (const auto& N : notifs) {
                if (!N->identityFromDesktop)
                    continue;
                const auto ICON = resolveDesktopEntryIcon(N->desktopEntry, ICONPX);
                if (ICON == N->identity)
                    continue;
                N->identity = ICON;
                N->identTex.reset();
                N->identFor.clear();
                N->identIconPx = 0;
                N->identSettled = false;
                identityChanged = true;
            }

            desktopScanning = !COMPLETE;
            if (desktopPoll)
                desktopPoll->updateTimeout(desktopScanning ? std::optional{std::chrono::milliseconds(16)} : std::nullopt);
            if ((identityChanged || FINISHED) && !notifs.empty())
                notifChanged();
        }

        static bool resourceReady(const SDecodeJob& job) {
            return job.rejected || (job.resource && job.resource->m_ready.load(std::memory_order_acquire));
        }

        static bool admissibleImageFile(const std::string& source) {
            std::error_code ec;
            const auto      status = std::filesystem::status(source, ec);
            if (ec || !std::filesystem::is_regular_file(status))
                return false;
            const auto bytes = std::filesystem::file_size(source, ec);
            return !ec && bytes <= MAX_IMAGE_FILE_BYTES;
        }

        static void armDecodePoll() {
            if (!decodePoll)
                return;
            const bool PENDING = desktopScanning || std::ranges::any_of(decodeJobs, [](const auto& job) { return !resourceReady(job); });
            decodePoll->updateTimeout(PENDING ? std::optional{std::chrono::milliseconds(16)} : std::nullopt);
        }

        static void pollDecodeJobs() {
            bool becameReady = false;
            for (auto& job : decodeJobs) {
                const bool READY = resourceReady(job);
                becameReady |= READY && !job.readyAtWarmStart;
                job.readyAtWarmStart |= READY;
            }
            armDecodePoll();
            if (becameReady)
                notifChanged();
        }

        static SDecodeJob* decodeJobFor(const std::string& source, int svgPx) {
            const int SVG_PX = NHyprCommon::isSvgIconPath(source) ? std::clamp(svgPx, 1, 256) : 0;
            if (const auto it = std::ranges::find_if(decodeJobs, [&](const auto& job) { return job.source == source && job.svgPx == SVG_PX; }); it != decodeJobs.end())
                return &*it;
            if (decodeJobs.size() >= MAX_PENDING_IMAGE_RESOURCES) {
                waitedForDecodeSlot = true;
                return nullptr;
            }
            if (!g_pAsyncResourceGatherer)
                return nullptr;
            if (!admissibleImageFile(source)) {
                decodeJobs.push_back(SDecodeJob{.source = source, .rejected = true});
                return &decodeJobs.back();
            }

            // hyprgraphics needs an explicit viewport for SVG files. Without
            // it the path constructor settles with "invalid size", which
            // used to turn valid app badges such as qBittorrent into the
            // generic application mark. Raster files ignore this viewport.
            auto resource = SVG_PX > 0 ? makeAtomicShared<Hyprgraphics::CImageResource>(source, Vector2D{(double)SVG_PX, (double)SVG_PX}) : makeAtomicShared<Hyprgraphics::CImageResource>(source);
            g_pAsyncResourceGatherer->enqueue(resource);
            decodeJobs.push_back(SDecodeJob{.source = source, .svgPx = SVG_PX, .resource = std::move(resource)});
            armDecodePoll();
            return &decodeJobs.back();
        }
    }

    void iconsInit() {
        decodeJobs.clear();
        decodeJobs.reserve(MAX_PENDING_IMAGE_RESOURCES);
        if (!g_pEventLoopManager || !g_pCompositor || !desktopIndex.init(g_pCompositor->m_wlEventLoop))
            return;
        decodePoll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { pollDecodeJobs(); }, nullptr);
        g_pEventLoopManager->addTimer(decodePoll);
        desktopPoll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { pollDesktopIndex(); }, nullptr);
        g_pEventLoopManager->addTimer(desktopPoll);
        startDesktopIndex();
    }

    void iconsExit() {
        if (decodePoll && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(decodePoll);
        decodePoll.reset();
        if (desktopPoll && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(desktopPoll);
        desktopPoll.reset();
        decodeJobs.clear();
        desktopIndex.exit();
        desktopIcons.clear();
        generatedAvatars.clear();
        desktopScanning = false;
        waitedForDecodeSlot = false;
        controlIconCacheClear();
    }

    void iconsWarmBegin() {
        waitedForDecodeSlot = false;
        for (auto& job : decodeJobs) {
            job.readyAtWarmStart = resourceReady(job);
            job.usedThisWarm     = false;
        }
    }

    bool iconsWarmEnd() {
        bool becameReadyDuringWarm = false;
        bool released              = false;
        std::erase_if(decodeJobs, [&](const auto& job) {
            const bool READY = resourceReady(job);
            becameReadyDuringWarm |= READY && !job.readyAtWarmStart;
            const bool DROP = READY && (job.readyAtWarmStart || job.usedThisWarm);
            released |= DROP;
            return DROP;
        });
        armDecodePoll();
        return becameReadyDuringWarm || (released && waitedForDecodeSlot);
    }

    void resetDesktopIconCache() {
        desktopIndex.cancel(++desktopGeneration);
        desktopIcons.clear();
        desktopScanning = false;
        const int ICONPX = std::max(8, (int)cfg.maxIcon->value());
        for (const auto& N : notifs) {
            N->identity            = Parse::resolveImage(N->appIcon, ICONPX);
            N->identityFromDesktop = N->identity.empty() && !N->desktopEntry.empty();
            N->identFor.clear();
            N->identIconPx = 0;
            N->identSettled = false;
            N->identTex.reset();
        }
        if (desktopPoll)
            startDesktopIndex();
    }

    void resetGeneratedAvatarCache() {
        generatedAvatars.clear();
        const int ICONPX = std::max(8, (int)cfg.maxIcon->value());
        for (const auto& N : notifs) {
            for (auto& participant : N->participants) {
                participant.icon = Parse::resolveImage(participant.iconSource, ICONPX);
                participant.avatarTex.reset();
                participant.avatarFor.clear();
                participant.avatarPx      = 0;
                participant.avatarSettled = false;
            }
            N->conversationIcon = Parse::resolveImage(N->conversationIconSource, ICONPX);
            N->conversationTex.reset();
            N->conversationFor.clear();
            N->conversationIconPx  = 0;
            N->conversationSettled = false;
        }
    }

    std::string resolveDesktopEntryIcon(const std::string& entry, int sizePx) {
        const auto IT = desktopIcons.find(desktopKey(entry));
        return IT == desktopIcons.end() ? "" : Parse::resolveImage(IT->second, sizePx);
    }

    // Anything bigger than the card's icon box is downscaled ONCE on the CPU
    // at load — a 4K pixmap kept full-size would hold megabytes of VRAM to
    // paint <=100 logical px, and its GL upload would stall the main thread.
    static SP<ITexture> scaledTex(cairo_surface_t* src, double sw, double sh, int maxPx) {
        if (sw <= maxPx && sh <= maxPx)
            return g_pHyprRenderer->createTexture(src);

        const double SCALE = std::min(maxPx / sw, maxPx / sh);
        const int    W     = std::max(1, (int)std::lround(sw * SCALE));
        const int    H     = std::max(1, (int)std::lround(sh * SCALE));

        auto*        SMALL = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        auto*        CR    = cairo_create(SMALL);
        cairo_scale(CR, W / sw, H / sh);
        cairo_set_source_surface(CR, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(CR), CAIRO_FILTER_GOOD);
        cairo_paint(CR);
        cairo_destroy(CR);
        cairo_surface_flush(SMALL);

        auto tex = g_pHyprRenderer->createTexture(SMALL);
        cairo_surface_destroy(SMALL);
        return tex;
    }

        // In-tree OSD senders name the state that just changed. These are
        // system controls, not an app's visual identity, so pin them to
        // stable native marks instead of whichever icon theme happens to be
        // installed. Keep this strictly inside the private OSD id band.
    static std::optional<eControlIcon> osdIcon(const SNotif& n) {
        if (!inOsdBand(n.id))
            return std::nullopt;
        const auto named = [](std::string_view value, std::string_view bare) {
            return value == bare || (value.starts_with(bare) && value.substr(bare.size()) == "-symbolic");
        };
        if (named(n.appIcon, "battery"))
            return eControlIcon::BATTERY;
        if (named(n.appIcon, "display-brightness"))
            return eControlIcon::BRIGHTNESS;
        if (named(n.appIcon, "audio-volume-muted"))
            return eControlIcon::VOLUME_MUTED;
        if (named(n.appIcon, "audio-volume-low") || named(n.appIcon, "audio-volume-medium") || named(n.appIcon, "audio-volume-high"))
            return eControlIcon::VOLUME;
        if (named(n.appIcon, "microphone-sensitivity-high"))
            return eControlIcon::MICROPHONE;
        if (named(n.appIcon, "microphone-sensitivity-muted"))
            return eControlIcon::MICROPHONE_MUTED;
        if (named(n.appIcon, "input-touchpad"))
            return eControlIcon::TOUCHPAD;
        if (named(n.appIcon, "touchpad-disabled"))
            return eControlIcon::TOUCHPAD_DISABLED;
        return std::nullopt;
    }

    // CImageResource decodes on the compositor-owned gatherer. The warm pass
    // only scales the ready bounded result and uploads its texture.
    static SFileTexResult fileTex(const std::string& path, int iconPx) {
        const auto JOB = decodeJobFor(path, iconPx);
        if (!JOB || !resourceReady(*JOB))
            return {};
        JOB->usedThisWarm = true;

        SFileTexResult result{.settled = true};
        if (JOB->rejected)
            return result;
        const auto     SURF = JOB->resource->m_asset.cairoSurface;
        if (!SURF || SURF->status() != CAIRO_STATUS_SUCCESS)
            return result;

        const auto SZ = SURF->size();
        if (SZ.x <= 0 || SZ.y <= 0 || SZ.x * SZ.y > MAX_DECODED_IMAGE_PIXELS)
            return result;

        result.texture = scaledTex(SURF->cairo(), SZ.x, SZ.y, iconPx);
        return result;
    }

    static SP<ITexture> generatedAvatar(const std::string& identity, const std::string& name, int iconPx) {
        const auto  BG        = color(cfg.colSurface);
        const bool  DARK      = BG.r + BG.g + BG.b < 1.5f;
        const auto  TINT      = Pixel::avatarColor(identity, DARK);
        const int   PX        = std::clamp(iconPx, 16, 128);
        std::string KEY       = "generated:";
        const auto  appendKey = [&](std::string_view value) {
            KEY += std::to_string(value.size());
            KEY.push_back(':');
            KEY += value;
        };
        appendKey(identity);
        appendKey(name);
        appendKey(cfg.font->value());
        KEY += std::to_string(PX) + ":" + std::to_string(DARK);
        if (const auto IT = generatedAvatars.find(KEY); IT != generatedAvatars.end())
            return IT->second;
        if (!warmGate.mayBuild() || !g_pHyprRenderer)
            return {};

        auto* SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, PX, PX);
        if (!SURF || cairo_surface_status(SURF) != CAIRO_STATUS_SUCCESS) {
            if (SURF)
                cairo_surface_destroy(SURF);
            return {};
        }
        auto* CR = cairo_create(SURF);
        cairo_set_source_rgb(CR, TINT.r, TINT.g, TINT.b);
        cairo_paint(CR);
        auto*      LAYOUT = pango_cairo_create_layout(CR);
        const auto LABEL  = Pixel::initials(name.empty() ? identity : name);
        pango_layout_set_text(LAYOUT, LABEL.c_str(), -1);
        auto* FONT = pango_font_description_from_string(cfg.font->value().c_str());
        pango_font_description_set_size(FONT, std::max(8, (int)std::lround(PX * 0.38)) * PANGO_SCALE);
        pango_font_description_set_weight(FONT, PANGO_WEIGHT_BOLD);
        pango_layout_set_font_description(LAYOUT, FONT);
        pango_layout_set_alignment(LAYOUT, PANGO_ALIGN_CENTER);
        pango_layout_set_width(LAYOUT, PX * PANGO_SCALE);
        int TW = 0, TH = 0;
        pango_layout_get_pixel_size(LAYOUT, &TW, &TH);
        const auto FG = Pixel::lightAvatarForeground(TINT) ? CHyprColor{0.98f, 0.99f, 1.f, 1.f} : CHyprColor{0.06f, 0.08f, 0.10f, 1.f};
        cairo_set_source_rgba(CR, FG.r, FG.g, FG.b, FG.a);
        cairo_move_to(CR, 0, (PX - TH) / 2.0);
        pango_cairo_show_layout(CR, LAYOUT);
        pango_font_description_free(FONT);
        g_object_unref(LAYOUT);
        cairo_destroy(CR);
        cairo_surface_flush(SURF);
        auto TEX = g_pHyprRenderer->createTexture(SURF);
        cairo_surface_destroy(SURF);
        if (!TEX)
            return {};
        if (generatedAvatars.size() >= 128)
            generatedAvatars.erase(generatedAvatars.begin());
        generatedAvatars.emplace(KEY, TEX);
        return TEX;
    }

    static void ensureParticipantAvatar(SParticipant& participant, int iconPx) {
        const std::string GENERATED = "__hyprnotify_avatar:" + participant.key;
        const std::string SOURCE    = participant.icon.empty() ? GENERATED : participant.icon;
        if (participant.avatarFor != SOURCE || participant.avatarPx != iconPx) {
            participant.avatarTex.reset();
            participant.avatarFor     = SOURCE;
            participant.avatarPx      = iconPx;
            participant.avatarSettled = false;
        }
        if (participant.avatarSettled)
            return;
        if (participant.icon.empty()) {
            participant.avatarTex     = generatedAvatar(participant.key, participant.name, iconPx);
            participant.avatarSettled = participant.avatarTex != nullptr;
            return;
        }
        const auto TEX = fileTex(participant.icon, iconPx);
        if (!TEX.settled)
            return;
        participant.avatarTex     = TEX.texture;
        participant.avatarSettled = true;
        if (!participant.avatarTex) {
            participant.icon.clear();
            participant.avatarFor.clear();
            participant.avatarPx      = 0;
            participant.avatarSettled = false;
            ensureParticipantAvatar(participant, iconPx);
        }
    }

    static uint64_t fnv1a(const void* data, size_t len, uint64_t h) {
        const auto* P = (const uint8_t*)data;
        for (size_t i = 0; i < len; i++)
            h = (h ^ P[i]) * 0x100000001b3ULL;
        return h;
    }

    // The icon anatomy: n.iconTex carries bounded CONTENT (image-data pixmap
    // or image-path file), n.identTex the IDENTITY (app_icon/desktop-entry).
    // The renderer chooses the lead and optional conversation badge.
    void ensureIconTex(SNotif& n, int iconPx) {
        constexpr std::string_view GENERIC = "__hyprnotify_aosp_generic_app__";
        const auto                  genericIdentity = [&]() {
            // A missing or unusable app identity gets one deterministic,
            // neutral application mark. It is never content art or a scan of
            // a user-provided directory.
            if (n.identFor != GENERIC || n.identIconPx != iconPx) {
                n.identTex.reset();
                n.identFor     = GENERIC;
                n.identIconPx  = iconPx;
                n.identSettled = false;
            }
            if (!n.identSettled) {
                n.identTex     = controlIcon(eControlIcon::APPS, iconPx, color(cfg.colFg), IDENTITY_GLYPH_RATIO);
                n.identSettled = n.identTex != nullptr;
            }
        };

        // OSD cards are the one app_icon class that has an explicit semantic
        // contract. Its icon changes on a fixed-id replacement, so its cache
        // key names the mark rather than retaining the previous texture.
        const auto OSD = osdIcon(n);
        if (OSD) {
            const std::string KEY = "__hyprnotify_osd_" + std::to_string((uint8_t)*OSD);
            if (n.identFor != KEY || n.identIconPx != iconPx) {
                n.identTex.reset();
                n.identFor     = KEY;
                n.identIconPx  = iconPx;
                n.identSettled = false;
            }
            if (!n.identSettled) {
                n.identTex     = controlIcon(*OSD, iconPx, color(cfg.colFg), IDENTITY_GLYPH_RATIO);
                n.identSettled = n.identTex != nullptr;
            }
        } else if (!n.identity.empty()) {
            // IDENTITY (the left lead): app_icon or the Icon= value resolved
            // from desktop-entry, drawn at every size it appears.
            if (n.identFor != n.identity || n.identIconPx != iconPx) {
                n.identTex.reset();
                n.identFor = n.identity;
                n.identIconPx = iconPx;
                n.identSettled = false;
            }
            if (!n.identSettled) {
                const auto TEX = fileTex(n.identity, iconPx);
                if (TEX.settled) {
                    n.identTex = TEX.texture;
                    n.identSettled = true;
                    if (!n.identTex) {
                        // A resolved path can disappear or fail to decode.
                        // Settle it once, then use the generic mark instead of
                        // retrying a known-bad source on every warm.
                        n.identity.clear();
                        n.identFor.clear();
                        n.identIconPx = 0;
                        n.identSettled = false;
                    }
                }
            }
        }
        if (!OSD && n.identity.empty())
            genericIdentity();

        if (n.hasPixels) {
            if (n.pixels.empty())
                return; // malformed input was rejected before a source was retained

            uint64_t h = fnv1a(n.pixels.data(), n.pixels.size(), 0xcbf29ce484222325ULL);
            h          = fnv1a(&n.pw, sizeof(n.pw), h);
            h          = fnv1a(&n.ph, sizeof(n.ph), h);
            if (!n.iconTex || n.pixelsFor != h || n.pixelsIconPx != iconPx) {
                if (n.pw > iconPx || n.ph > iconPx) {
                    // stride pw*4 is how unpackImageData lays the buffer out
                    auto* SRC = cairo_image_surface_create_for_data(n.pixels.data(), CAIRO_FORMAT_ARGB32, n.pw, n.ph, n.pw * 4);
                    if (cairo_surface_status(SRC) != CAIRO_STATUS_SUCCESS)
                        n.iconTex = nullptr;
                    else
                        n.iconTex = scaledTex(SRC, n.pw, n.ph, iconPx);
                    cairo_surface_destroy(SRC);
                } else
                    n.iconTex = g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, n.pixels.data(), n.pw * 4, Vector2D{(double)n.pw, (double)n.ph});
                n.pixelsFor = h;
                n.pixelsIconPx = iconPx;
                n.imageFor.clear();
                n.imageSettled = false;
            }
            // The GPU texture owns the uploaded result now. Retaining the
            // D-Bus-sized source buffer across warms defeats the admission
            // bound and keeps the last raw image alive until replacement.
            n.pixels.clear();
            n.pixels.shrink_to_fit();
            return;
        }

        if (n.image.empty()) {
            // no content image: the deterministic app identity leads alone
            n.iconTex.reset();
            n.imageFor.clear();
            n.pixelsFor = 0;
            n.pixelsIconPx = 0;
            n.imageSettled = true;
            return;
        }
        if (n.imageFor != n.image || n.imageIconPx != iconPx) {
            n.iconTex.reset();
            n.imageFor  = n.image;
            n.pixelsFor = 0;
            n.imageIconPx  = iconPx;
            n.imageSettled = false;
        }
        if (!n.imageSettled) {
            const auto TEX = fileTex(n.image, iconPx);
            if (TEX.settled) {
                n.iconTex      = TEX.texture;
                n.imageSettled = true;
                if (!n.iconTex) {
                    n.image.clear();
                    n.imageFor.clear();
                    n.imageIconPx  = 0;
                    n.imageSettled = false;
                }
            }
        }
    }

    void ensureConversationIcons(SNotif& n, int iconPx) {
        if (!n.conversation) {
            n.conversationTex.reset();
            n.conversationFor.clear();
            n.conversationIconPx  = 0;
            n.conversationSettled = false;
            return;
        }
        for (auto& participant : n.participants)
            ensureParticipantAvatar(participant, iconPx);

        const bool FACE_PILE = n.conversationKind == "group" && n.conversationIcon.empty() && n.participants.size() >= 2;
        if (FACE_PILE) {
            n.conversationTex.reset();
            n.conversationFor.clear();
            n.conversationIconPx  = 0;
            n.conversationSettled = false;
            return;
        }

        const auto  CONVERSATION_KEY = Pixel::conversationKey(n.appKey, n.conversationId);
        const auto& DISPLAY_NAME     = n.conversationTitle.empty() ? n.summary : n.conversationTitle;
        std::string GENERATED        = "__hyprnotify_conversation:";
        GENERATED += std::to_string(CONVERSATION_KEY.size()) + ":" + CONVERSATION_KEY;
        GENERATED += std::to_string(DISPLAY_NAME.size()) + ":" + DISPLAY_NAME;
        const bool        NEED_GENERATED = !n.conversationId.empty() && n.conversationIcon.empty() && n.participants.empty() && n.image.empty() && !n.hasPixels && !n.iconTex;
        const std::string SOURCE         = n.conversationIcon.empty() ? GENERATED : n.conversationIcon;
        if (n.conversationFor != SOURCE || n.conversationIconPx != iconPx) {
            n.conversationTex.reset();
            n.conversationFor     = SOURCE;
            n.conversationIconPx  = iconPx;
            n.conversationSettled = false;
        }
        if (n.conversationSettled)
            return;
        if (NEED_GENERATED) {
            n.conversationTex     = generatedAvatar(CONVERSATION_KEY, DISPLAY_NAME, iconPx);
            n.conversationSettled = n.conversationTex != nullptr;
            return;
        }
        if (n.conversationIcon.empty()) {
            n.conversationTex.reset();
            n.conversationSettled = true;
            return;
        }
        const auto TEX = fileTex(n.conversationIcon, iconPx);
        if (!TEX.settled)
            return;
        n.conversationTex     = TEX.texture;
        n.conversationSettled = true;
        if (!n.conversationTex) {
            n.conversationIcon.clear();
            n.conversationFor.clear();
            n.conversationIconPx  = 0;
            n.conversationSettled = false;
            ensureConversationIcons(n, iconPx);
        }
    }

    void ensureActionIcon(SNotif& n, SAction& a, int iconPx) {
        if (!n.actionIcons) {
            a.iconTex.reset();
            a.iconFor.clear(); // so re-enabling the hint rebuilds
            a.iconSettled = false;
            return;
        }
        if (a.iconFor != a.id || a.iconPx != iconPx) {
            a.iconFor = a.id;
            a.iconPx = iconPx;
            a.iconTex.reset();
            a.iconSettled = false;
        }
        if (a.iconSettled)
            return;

        std::string path = a.id;
        if (path.starts_with("file://"))
            path.erase(0, 7);
        if (!path.starts_with('/'))
            path = NHyprCommon::resolveIconName(a.id, iconPx);
        if (path.empty()) {
            a.iconSettled = true;
            return;
        }
        const auto TEX = fileTex(path, iconPx);
        if (TEX.settled) {
            a.iconTex = TEX.texture;
            a.iconSettled = true;
        }
    }

    void ensureBodyImage(SBodyImage& im, int maxPx) {
        if (im.src.empty()) {
            im.tex.reset();
            im.builtFor.clear();
            im.settled = false;
            return;
        }
        if (im.builtFor != im.src || im.builtPx != maxPx) {
            im.builtFor = im.src;
            im.builtPx = maxPx;
            im.tex.reset();
            im.settled = false;
        }
        if (im.settled)
            return;
        const auto TEX = fileTex(im.src, maxPx);
        if (TEX.settled) {
            im.tex = TEX.texture;
            im.settled = true;
        }
    }

} // namespace NHyprnotify
