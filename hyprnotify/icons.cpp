// hyprnotify/icons.cpp — notification images: content avatars and identity
// icons via hyprgraphics, raw image-data pixmaps

#include "common/fileindex.hpp"
#include "common/icons.hpp"

#include "hyprbar/desktop_exec.hpp"

#include "hyprnotify.hpp"
#include "ui.hpp"

#include <hyprland/src/render/AsyncResourceGatherer.hpp>

#include <cstring>
#include <filesystem>
#include <random>
#include <sstream>

namespace NHyprnotify {

    // ---- fallback_icon_dir: iconless cards wear a face (the left identity) ----

    static std::vector<std::string> fallbackFiles;
    static bool                     fallbackScanned = false;

    void                            resetFallbackCache() {
        fallbackFiles.clear();
        fallbackScanned = false;
    }

    // One roll per card (bus keeps the pick across in-place replaces). The
    // listing is scanned once per config life, from the warm pass — never
    // the render or a bus dispatch.
    static std::string pickFallback() {
        const auto DIR = cfg.fallbackIconDir->value();
        if (DIR.empty())
            return "";
        if (!fallbackScanned) {
            fallbackScanned = true;
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(DIR, std::filesystem::directory_options::skip_permission_denied, ec); !ec && it != std::filesystem::end(it);
                 it.increment(ec)) {
                if (!it->is_regular_file(ec))
                    continue;
                auto ext = it->path().extension().string();
                std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp" || ext == ".avif" || ext == ".jxl" || ext == ".svg")
                    fallbackFiles.push_back(it->path().string());
            }
        }
        if (fallbackFiles.empty())
            return "";
        static std::mt19937 rng{std::random_device{}()};
        return fallbackFiles[std::uniform_int_distribution<size_t>{0, fallbackFiles.size() - 1}(rng)];
    }

    // ---- async decode + the desktop-entry index (F/F2) ----
    //
    // A file icon's decode used to happen on the compositor thread, inside
    // the warm pass: one 4K app icon stalled every frame. The decode now
    // rides the compositor's own async resource gatherer (one shared
    // worker); the warm pass only scales an already-decoded bounded result
    // and uploads it. A card whose icon is still decoding shows its fallback
    // face (or the generic mark) until the 16 ms poll sees the resource
    // ready and re-warms.
    //
    // The desktop-entry index is the same idea for the Icon= lookup: a
    // bounded enumeration of every $XDG_DATA_DIRS/applications *.desktop
    // (the helper process keeps it off our threads), parsed into a
    // name -> Icon= map. Arrivals read the map; an entry that lands after
    // the card arrived upgrades it in place.

    static constexpr size_t MAX_PENDING_IMAGE_RESOURCES = 24; // the gatherer is shared with the compositor
    static constexpr size_t MAX_IMAGE_FILE_BYTES        = 32 * 1024 * 1024;
    static constexpr double MAX_DECODED_IMAGE_PIXELS    = 16.0 * 1024 * 1024;
    static constexpr size_t MAX_DESKTOP_FILES           = 4096;
    static constexpr size_t MAX_DESKTOP_VISITED         = 16384;

    struct SDecodeJob {
        std::string                                                    source;
        int                                                            svgPx   = 0;
        Hyprutils::Memory::CAtomicSharedPointer<Hyprgraphics::CImageResource> resource;
        bool                                                           rejected = false; // too big or not a file: settled null
        bool                                                           readyAtPoll = false;
    };

    static std::vector<SDecodeJob>   decodeJobs;
    static SP<CEventLoopTimer>       decodePoll;
    static bool                      slotFreed           = false;
    static bool                      waitedForDecodeSlot = false;

    static NHyprCommon::CAsyncFileIndex                 desktopIndex;
    static std::unordered_map<std::string, std::string> desktopIcons;
    static uint64_t                                     desktopGeneration = 0;
    static bool                                         desktopScanning   = false;

    static std::unordered_map<std::string, SP<ITexture>> generatedAvatars;

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

    // find-or-enqueue the gatherer's job for (source, svg viewport). nullptr:
    // the slot is taken — the caller keeps the card unsettled and a freed
    // slot re-warms (a decode cannot be cancelled after enqueue, so the cap
    // also bounds replaced and deleted cards)
    static SDecodeJob* decodeJobFor(const std::string& source, int svgPx) {
        const int SVG_PX = NHyprCommon::isSvgIconPath(source) ? std::clamp(svgPx, 1, 256) : 0;
        if (const auto IT = std::ranges::find_if(decodeJobs, [&](const auto& job) { return job.source == source && job.svgPx == SVG_PX; }); IT != decodeJobs.end())
            return &*IT;
        if (decodeJobs.size() >= MAX_PENDING_IMAGE_RESOURCES) {
            waitedForDecodeSlot = true;
            return nullptr;
        }
        if (!admissibleImageFile(source)) {
            decodeJobs.push_back(SDecodeJob{.source = source, .rejected = true});
            return &decodeJobs.back();
        }
        // hyprgraphics needs an explicit viewport for SVG files; without it
        // the path constructor settles with "invalid size". Rasters ignore it.
        auto resource = SVG_PX > 0 ? makeAtomicShared<Hyprgraphics::CImageResource>(source, Vector2D{(double)SVG_PX, (double)SVG_PX})
                                   :
                                   makeAtomicShared<Hyprgraphics::CImageResource>(source);
        g_pAsyncResourceGatherer->enqueue(resource);
        decodeJobs.push_back(SDecodeJob{.source = source, .svgPx = SVG_PX, .resource = std::move(resource)});
        armDecodePoll();
        return &decodeJobs.back();
    }

    // The texture is uploaded now; the decoded image dies with the job. A
    // freed slot unblocks the card that waited at the cap.
    static void dropDecodeJob(const SDecodeJob* job) {
        const auto IT = std::ranges::find_if(decodeJobs, [&](const auto& j) { return &j == job; });
        if (IT != decodeJobs.end()) {
            decodeJobs.erase(IT);
            slotFreed = true;
        }
    }

    static void pollDecodeJobs() {
        bool becameReady = false;
        for (auto& job : decodeJobs) {
            const bool READY = resourceReady(job);
            becameReady |= READY && !job.readyAtPoll;
            job.readyAtPoll |= READY;
        }
        const bool SLOT = slotFreed;
        slotFreed       = false;
        armDecodePoll();
        if (becameReady || (waitedForDecodeSlot && SLOT))
            notifChanged();
    }

    static std::string lowerKey(std::string value) {
        std::ranges::transform(value, value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return value;
    }

    // application dirs in XDG order: per-user first, then the data dirs
    // (Flatpak/system exports)
    static std::vector<std::string> appDirs() {
        auto dirs = NHyprCommon::xdgDataDirs();
        for (auto& D : dirs)
            D += "/applications";
        return dirs;
    }

    static void indexDesktopEntry(const NHyprCommon::CAsyncFileIndex::SEntry& entry) {
        std::istringstream F(entry.contents);
        std::string        icon, wmClass, line;
        bool               inEntry = false;
        while (std::getline(F, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.starts_with("[")) {
                if (inEntry)
                    break;
                inEntry = line == "[Desktop Entry]";
                continue;
            }
            if (!inEntry)
                continue;
            if (icon.empty() && line.starts_with("Icon=")) {
                if (const auto VALUE = NHyprbar::DesktopExec::unescapeString(std::string_view{line}.substr(5)))
                    icon = *VALUE;
            } else if (wmClass.empty() && line.starts_with("StartupWMClass=")) {
                if (const auto VALUE = NHyprbar::DesktopExec::unescapeString(std::string_view{line}.substr(15)))
                    wmClass = *VALUE;
            }
            if (!icon.empty() && !wmClass.empty())
                break;
        }
        if (icon.empty())
            return;
        // the desktop-file basename AND StartupWMClass: a sender's hint is
        // often the window class, rarely the file name
        const auto remember = [&](const std::string& key) {
            if (!key.empty())
                desktopIcons.try_emplace(lowerKey(key), icon);
        };
        remember(entry.path.stem().string());
        remember(wmClass);
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
        for (const auto& directory : appDirs())
            request.roots.emplace_back(directory);
        desktopIndex.request(std::move(request));
        if (decodePoll)
            decodePoll->updateTimeout(std::chrono::milliseconds(2));
    }

    // the index's batch landed: fold it in, and upgrade every live card that
    // is still riding the icon-name stand-in for its desktop entry
    static void pollDesktopIndex() {
        if (!desktopScanning)
            return;
        std::vector<NHyprCommon::CAsyncFileIndex::SEntry> entries;
        const bool COMPLETE = desktopIndex.poll(desktopGeneration, entries, 8);
        const size_t BEFORE = desktopIcons.size();
        for (const auto& entry : entries)
            indexDesktopEntry(entry);
        desktopScanning = !COMPLETE;
        if (desktopIcons.size() != BEFORE) {
            bool identityChanged = false;
            const int ICONPX      = std::max(8, (int)cfg.maxIcon->value());
            for (const auto& N : notifs) {
                if (!N->identityFromDesktop)
                    continue;
                const auto ICON = resolveDesktopEntryIcon(N->desktopEntry, ICONPX);
                if (ICON.empty() || ICON == N->identity)
                    continue;
                N->identity      = ICON; // the entry's own Icon= is authoritative
                N->identTex.reset();
                N->identFor.clear();
                N->identIconPx   = 0;
                N->identSettled  = false;
                N->identityFromDesktop = false;
                identityChanged = true;
            }
            if (identityChanged)
                notifChanged();
        }
        armDecodePoll();
    }

    // the desktop entry's own Icon= (the index's value), resolved to a file.
    // Empty while the index has not reached the entry.
    std::string resolveDesktopEntryIcon(const std::string& entry, int sizePx) {
        const auto IT = desktopIcons.find(lowerKey(entry));
        return IT == desktopIcons.end() ? "" : Parse::resolveImage(IT->second, sizePx);
    }

    // one deterministic initials face per (identity, name, font, size): the
    // facepile never shows a blank square for a faceless sender
    static SP<ITexture> generatedAvatar(const std::string& identity, const std::string& name, int iconPx) {
        const auto BG   = color(cfg.colBg);
        const bool DARK = BG.r + BG.g + BG.b < 1.5f;
        const auto TINT = Pixel::avatarColor(identity, DARK);
        const int  PX   = std::clamp(iconPx, 16, 128);
        std::string KEY;
        const auto appendKey = [&](std::string_view value) {
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
        auto*      CR     = cairo_create(SURF);
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

    // Scale into exactly W x H, covering the box: the overflowing axis is
    // center-cropped (the hero treatment for previews). When the source
    // aspect matches, cover == fit and nothing is lost.
    static SP<ITexture> coverTex(cairo_surface_t* src, double sw, double sh, int W, int H) {
        auto*        SMALL = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        auto*        CR    = cairo_create(SMALL);
        const double S     = std::max(W / sw, H / sh);
        cairo_translate(CR, (W - sw * S) / 2.0, (H - sh * S) / 2.0);
        cairo_scale(CR, S, S);
        cairo_set_source_surface(CR, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(CR), CAIRO_FILTER_GOOD);
        cairo_paint(CR);
        cairo_destroy(CR);
        cairo_surface_flush(SMALL);

        auto tex = g_pHyprRenderer->createTexture(SMALL);
        cairo_surface_destroy(SMALL);
        return tex;
    }

    // A source wide enough for the hero layout: HERO_ASPECT and at least
    // half the hero box, so a tiny wide icon never blows up to card width.
    static bool heroWorthy(double sw, double sh, int heroWPx) {
        return heroWPx > 0 && sh > 0 && sw / sh >= HERO_ASPECT && sw * 2 >= heroWPx;
    }

    // The freedesktop symbolic convention, shared with hyprbar's loadIcon:
    // a pure-shape mark the toolkit repaints with the widget's foreground.
    // Adwaita bakes them near-black, so on a dark card the brightness OSD
    // icon was invisible until fileTex repaints it.
    static bool isSymbolicIconPath(const std::string& path) {
        return NHyprCommon::isSvgIconPath(path) && (path.find("-symbolic") != std::string::npos || path.find("/symbolic/") != std::string::npos);
    }

    // Repaint a rasterized symbolic surface to col, premultiplied (the same
    // pixel math as hyprbar's loadSvg recolor; alpha untouched).
    static void tintSurface(cairo_surface_t* SURF, const CHyprColor& col) {
        const uint8_t R = (uint8_t)(col.r * 255), G = (uint8_t)(col.g * 255), B = (uint8_t)(col.b * 255);
        auto*         D      = cairo_image_surface_get_data(SURF);
        const int     W      = cairo_image_surface_get_width(SURF), H = cairo_image_surface_get_height(SURF);
        const int     STRIDE = cairo_image_surface_get_stride(SURF);
        for (int y = 0; y < H; y++) {
            auto* row = D + (size_t)y * STRIDE;
            for (int x = 0; x < W; x++) {
                const uint8_t A = row[x * 4 + 3];
                row[x * 4]      = (uint8_t)(B * A / 255);
                row[x * 4 + 1]  = (uint8_t)(G * A / 255);
                row[x * 4 + 2]  = (uint8_t)(R * A / 255);
            }
        }
        cairo_surface_mark_dirty(SURF);
    }

    struct SFileTexResult {
        SP<ITexture> texture;
        bool         settled = false; // false: an async decode is in flight
        bool         hero    = false;
    };

    // CImage's size hint only bounds SVG rasters; raster formats decode full
    // size transiently and get scaled here. The synchronous path: always
    // settled, used for symbolic icons (the tint would bake into the
    // gatherer's shared surface) and SVG heroes (a 256 px async viewport
    // could never cover them).
    static SP<ITexture> syncFileTex(const std::string& path, int iconPx, int heroWPx, int heroHCapPx, bool& hero, bool tintSymbolic = false) {
        const int            HINT = std::max(iconPx, heroWPx);
        Hyprgraphics::CImage image(path, Vector2D{(double)HINT, (double)HINT});
        if (!image.success())
            return nullptr;

        const auto SURF = image.cairoSurface();
        if (!SURF || SURF->status() != CAIRO_STATUS_SUCCESS)
            return nullptr;

        const auto SZ = SURF->size();
        if (SZ.x <= 0 || SZ.y <= 0)
            return nullptr;

        if (tintSymbolic && isSymbolicIconPath(path))
            tintSurface(SURF->cairo(), color(cfg.colFg));

        hero = heroWorthy(SZ.x, SZ.y, heroWPx);
        if (hero)
            return coverTex(SURF->cairo(), SZ.x, SZ.y, heroWPx, std::min((int)std::lround(heroWPx * SZ.y / SZ.x), heroHCapPx));
        return scaledTex(SURF->cairo(), SZ.x, SZ.y, iconPx);
    }

    // The async front door: a file icon decodes on the gatherer's worker and
    // the warm pass only scales the ready result. settled=false means "ask
    // again after the next warm" — the 16 ms poll re-warms when the
    // resource lands.
    static SFileTexResult fileTex(const std::string& path, int iconPx, int heroWPx, int heroHCapPx, bool tintSymbolic = false) {
        if (tintSymbolic || (heroWPx > 0 && NHyprCommon::isSvgIconPath(path))) {
            bool hero = false;
            return {.texture = syncFileTex(path, iconPx, heroWPx, heroHCapPx, hero, tintSymbolic), .settled = true, .hero = hero};
        }
        if (!g_pAsyncResourceGatherer) {
            bool hero = false;
            return {.texture = syncFileTex(path, iconPx, heroWPx, heroHCapPx, hero), .settled = true, .hero = hero};
        }
        const auto JOB = decodeJobFor(path, iconPx);
        if (!JOB || !resourceReady(*JOB))
            return {}; // the slot is taken, or the decode is in flight
        // Capture before the drop: erasing the job destroys the struct (and
        // releases our reference); the resource copy below outlives the call
        const bool REJECTED = JOB->rejected;
        const auto RESOURCE = JOB->resource;
        dropDecodeJob(JOB);
        SFileTexResult result{.settled = true};
        if (REJECTED)
            return result;
        const auto SURF = RESOURCE->m_asset.cairoSurface;
        if (!SURF || SURF->status() != CAIRO_STATUS_SUCCESS)
            return result;
        const auto SZ = SURF->size();
        if (SZ.x <= 0 || SZ.y <= 0 || (double)SZ.x * SZ.y > MAX_DECODED_IMAGE_PIXELS)
            return result;
        result.hero = heroWorthy(SZ.x, SZ.y, heroWPx);
        if (result.hero)
            result.texture = coverTex(SURF->cairo(), SZ.x, SZ.y, heroWPx, std::min((int)std::lround((double)heroWPx * SZ.y / SZ.x), heroHCapPx));
        else
            result.texture = scaledTex(SURF->cairo(), SZ.x, SZ.y, iconPx);
        return result;
    }

    // CPU-side cap for image-data buffers at unpack time (bus.cpp) — same
    // premultiplied-BGRA layout in and out, so warm's hash/upload path is
    // untouched. Row-copied out: cairo's stride is its own business.
    void shrinkPixels(SNotif& n, int maxPx) {
        if (n.pixels.empty() || (n.pw <= maxPx && n.ph <= maxPx))
            return;

        auto* SRC = cairo_image_surface_create_for_data(n.pixels.data(), CAIRO_FORMAT_ARGB32, n.pw, n.ph, n.pw * 4);
        if (cairo_surface_status(SRC) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(SRC);
            return;
        }

        const double SCALE = std::min((double)maxPx / n.pw, (double)maxPx / n.ph);
        const int    W     = std::max(1, (int)std::lround(n.pw * SCALE));
        const int    H     = std::max(1, (int)std::lround(n.ph * SCALE));

        auto*        DST = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        auto*        CR  = cairo_create(DST);
        cairo_scale(CR, (double)W / n.pw, (double)H / n.ph);
        cairo_set_source_surface(CR, SRC, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(CR), CAIRO_FILTER_GOOD);
        cairo_paint(CR);
        cairo_destroy(CR);
        cairo_surface_flush(DST);
        cairo_surface_destroy(SRC);

        if (cairo_surface_status(DST) == CAIRO_STATUS_SUCCESS) {
            const int   STRIDE = cairo_image_surface_get_stride(DST);
            const auto* DATA   = cairo_image_surface_get_data(DST);
            std::vector<uint8_t> out((size_t)W * H * 4);
            for (int y = 0; y < H; y++)
                std::memcpy(out.data() + (size_t)y * W * 4, DATA + (size_t)y * STRIDE, (size_t)W * 4);
            n.pixels = std::move(out);
            n.pw     = W;
            n.ph     = H;
        }
        cairo_surface_destroy(DST);
    }

    static uint64_t fnv1a(const void* data, size_t len, uint64_t h) {
        const auto* P = (const uint8_t*)data;
        for (size_t i = 0; i < len; i++)
            h = (h ^ P[i]) * 0x100000001b3ULL;
        return h;
    }

    // The icon anatomy: n.iconTex carries the CONTENT (image-data pixmap or
    // image-path file, hero-capable), n.identTex the IDENTITY (app_icon /
    // desktop-entry, icon-box only). The render decides which leads and
    // whether the identity rides as the corner badge.
    // the deterministic generic mark an iconless card wears (identity empty
    // AND the fallback face dir empty): the theme's application-default-icon
    // when it exists, else an apps-grid glyph in the theme ink — no card is
    // ever faceless
    static SP<ITexture> genericMark(int px) {
        if (const auto PATH = Parse::resolveImage("application-default-icon", px); !PATH.empty()) {
            bool hero = false;
            return syncFileTex(PATH, px, 0, 0, hero); // a small theme SVG: decode here, not on the worker
        }

        auto*        SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
        auto*        CT   = cairo_create(SURF);
        const auto   INK  = color(cfg.colFg), PLATE = color(cfg.colFrame);
        auto         rounded = [&](double x, double y, double w, double h, double r) {
            cairo_new_sub_path(CT);
            cairo_arc(CT, x + w - r, y + r, r, -M_PI_2, 0);
            cairo_arc(CT, x + w - r, y + h - r, r, 0, M_PI_2);
            cairo_arc(CT, x + r, y + h - r, r, M_PI_2, M_PI);
            cairo_arc(CT, x + r, y + r, r, M_PI, 1.5 * M_PI);
            cairo_close_path(CT);
        };
        rounded(0, 0, px, px, px * 0.22);
        cairo_set_source_rgba(CT, PLATE.r, PLATE.g, PLATE.b, PLATE.a * 0.55);
        cairo_fill(CT);
        const double G = px * 0.16, GAP = px * 0.12;
        const double X0 = (px - (2 * G + GAP)) / 2;
        for (int q = 0; q < 2; q++)
            for (int r = 0; r < 2; r++) {
                rounded(X0 + q * (G + GAP), X0 + r * (G + GAP), G, G, G * 0.35);
                cairo_set_source_rgba(CT, INK.r, INK.g, INK.b, INK.a * 0.7);
                cairo_fill(CT);
            }
        cairo_destroy(CT);
        if (cairo_surface_status(SURF) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(SURF);
            return nullptr;
        }
        auto TEX = scaledTex(SURF, px, px, px);
        cairo_surface_destroy(SURF);
        return TEX;
    }

    void ensureIconTex(SNotif& n, int iconPx, int heroWPx, int heroHCapPx) {
        // IDENTITY (the left lead): the app_icon/desktop-entry, drawn at every
        // size it appears (lead avatar, group header). An iconless card rolls a
        // face from fallback_icon_dir so a sender is never faceless; the roll
        // is kept in fallbackPick across in-place replaces. heroWPx 0: an
        // identity is icon-box only — a wide waifu never goes hero.
        if (!n.identity.empty()) {
            n.fallbackPick.clear();
            // a symbolic identity bakes colFg into its pixels (fileTex): the
            // key carries the fg it was tinted with, so a theme change
            // rebuilds it — hyprbar's dropStaleTint, per-card edition
            const auto SYM = isSymbolicIconPath(n.identity);
            const auto KEY = SYM ? n.identity + "\x1f" + std::to_string((uint64_t)cfg.colFg->value()) : n.identity;
            if (n.identFor != KEY || n.identIconPx != iconPx) {
                n.identTex.reset();
                n.identFor     = KEY;
                n.identIconPx  = iconPx;
                n.identSettled = false;
            }
            if (!n.identSettled) {
                const auto TEX = fileTex(n.identity, iconPx, 0, 0, SYM);
                if (TEX.settled) {
                    n.identTex     = TEX.texture;
                    n.identSettled = true; // a failed load stays failed: no disk retry per warm
                    if (!n.identTex) {
                        // a resolved path can disappear or fail to decode: the
                        // fallback face (or the generic mark) takes over next
                        // warm, not a retry of the known-bad source
                        n.identity.clear();
                        n.identFor.clear();
                        n.identIconPx  = 0;
                        n.identSettled = false;
                    }
                }
            }
        } else {
            if (n.fallbackPick.empty())
                n.fallbackPick = pickFallback();
            if (n.fallbackPick.empty()) {
                // the generic mark: one neutral face for every iconless
                // card; the key carries the fg it was drawn in, so a theme
                // change rebuilds it (the symbolic-identity contract)
                const auto GENERIC = "__hyprnotify_generic__\x1f" + std::to_string((uint64_t)cfg.colFg->value());
                if (n.identFor != GENERIC || n.identIconPx != iconPx) {
                    n.identTex.reset();
                    n.identFor     = GENERIC;
                    n.identIconPx  = iconPx;
                    n.identSettled = false;
                }
                if (n.identFor == GENERIC && !n.identSettled) {
                    n.identTex     = genericMark(iconPx);
                    n.identSettled = n.identTex != nullptr;
                }
            } else if (n.identFor != n.fallbackPick || n.identIconPx != iconPx) {
                n.identTex.reset();
                n.identFor     = n.fallbackPick;
                n.identIconPx  = iconPx;
                n.identSettled = false;
            }
            if (n.identFor == n.fallbackPick && !n.identSettled) {
                const auto TEX = fileTex(n.fallbackPick, iconPx, 0, 0, false);
                if (TEX.settled) {
                    n.identTex     = TEX.texture;
                    n.identSettled = true; // a dead face stays failed: no disk retry per warm
                    if (!n.identTex)
                        n.fallbackPick.clear(); // the re-roll, or the generic mark, takes over
                }
            }
        }

        if (n.hasPixels) {
            if (n.pixels.empty())
                return; // uploaded by an earlier warm; the texture carries it now

            uint64_t h = fnv1a(n.pixels.data(), n.pixels.size(), 0xcbf29ce484222325ULL);
            h          = fnv1a(&n.pw, sizeof(n.pw), h);
            h          = fnv1a(&n.ph, sizeof(n.ph), h);
            if (!n.iconTex || n.pixelsFor != h) {
                n.heroTex = heroWorthy(n.pw, n.ph, heroWPx);
                if (n.heroTex || n.pw > iconPx || n.ph > iconPx) {
                    // stride pw*4 is how unpackImageData lays the buffer out
                    auto* SRC = cairo_image_surface_create_for_data(n.pixels.data(), CAIRO_FORMAT_ARGB32, n.pw, n.ph, n.pw * 4);
                    if (cairo_surface_status(SRC) != CAIRO_STATUS_SUCCESS)
                        n.iconTex = nullptr;
                    else if (n.heroTex)
                        n.iconTex = coverTex(SRC, n.pw, n.ph, heroWPx, std::min((int)std::lround((double)heroWPx * n.ph / n.pw), heroHCapPx));
                    else
                        n.iconTex = scaledTex(SRC, n.pw, n.ph, iconPx);
                    cairo_surface_destroy(SRC);
                } else
                    n.iconTex = g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, n.pixels.data(), n.pw * 4, Vector2D{(double)n.pw, (double)n.ph});
                n.pixelsFor = h;
                n.imageFor.clear();
            }
            n.pixels.clear();
            n.pixels.shrink_to_fit();
            return;
        }

        if (n.image.empty()) {
            // no content image: the card shows its identity (or the rolled
            // fallback face) alone — content has no fallback of its own
            n.iconTex.reset();
            n.imageFor.clear();
            n.pixelsFor = 0;
            n.heroTex   = false;
            n.imageSettled = true;
            return;
        }
        if (n.imageFor != n.image || n.imageIconPx != iconPx) {
            n.iconTex.reset();
            n.imageFor     = n.image;
            n.imageIconPx  = iconPx;
            n.pixelsFor    = 0;
            n.imageSettled = false;
        }
        if (!n.imageSettled) {
            const auto TEX = fileTex(n.image, iconPx, heroWPx, heroHCapPx, false);
            if (TEX.settled) {
                n.iconTex      = TEX.texture;
                n.imageSettled = true; // a known-bad source: no disk retry per warm
                n.heroTex      = TEX.hero;
                if (!n.iconTex) {
                    n.image.clear();
                    n.imageFor.clear();
                    n.imageIconPx  = 0;
                    n.imageSettled = false;
                }
            }
        }
    }

    void ensureActionIcon(SNotif& n, SAction& a, int iconPx) {
        if (!n.actionIcons) {
            a.iconTex.reset();
            a.iconFor.clear(); // so re-enabling the hint rebuilds
            a.iconSettled = false;
            a.iconPx = 0;
            return;
        }
        std::string path = a.id;
        if (path.starts_with("file://"))
            path.erase(0, 7);
        if (!path.starts_with('/'))
            path = NHyprCommon::resolveIconName(a.id, iconPx);

        // a symbolic action bakes colFg into its pixels: same fg-carrying key
        // as the identity; the bare id keeps remembering failed resolves
        const auto SYM = isSymbolicIconPath(path);
        const auto KEY = SYM ? a.id + "\x1f" + std::to_string((uint64_t)cfg.colFg->value()) : a.id;
        if (a.iconFor != KEY || a.iconPx != iconPx) {
            a.iconFor     = KEY;
            a.iconPx      = iconPx;
            a.iconTex.reset();
            a.iconSettled = false;
        }
        if (a.iconSettled)
            return;
        if (path.empty()) {
            a.iconSettled = true; // a failed resolve stays failed: no rescan per warm
            return;
        }
        const auto TEX = fileTex(path, iconPx, 0, 0, SYM); // icon box only, never hero
        if (TEX.settled) {
            a.iconTex     = TEX.texture;
            a.iconSettled = true;
        }
    }

    void ensureBodyImage(SBodyImage& im, int maxPx) {
        if (im.src.empty()) {
            im.tex.reset();
            im.builtFor.clear();
            im.settled = false;
            im.builtPx = 0;
            return;
        }
        if (im.builtFor != im.src || im.builtPx != maxPx) {
            im.builtFor = im.src;
            im.builtPx  = maxPx;
            im.tex.reset();
            im.settled = false;
        }
        if (im.settled)
            return;
        const auto TEX = fileTex(im.src, maxPx, 0, 0, false); // fit a box, never hero
        if (TEX.settled) {
            im.tex     = TEX.texture;
            im.settled = true; // a failed load stays failed: the alt line keeps it
        }
    }

    // a facepile avatar: the sender's icon, or a generated initials face
    // when the sender is faceless — keyed like an identity, failures
    // remembered
    void ensureAvatarTex(SParticipant& p, int px) {
        if (p.icon.empty()) {
            const std::string KEY = "__hyprnotify_avatar:\x1f" + p.key + "\x1f" + p.name + "\x1f" + cfg.font->value() + "\x1f" + std::to_string(px);
            if (p.avatarFor == KEY)
                return;
            p.avatarTex     = generatedAvatar(p.key, p.name, px);
            p.avatarFor     = KEY;
            p.avatarPx      = px;
            p.avatarSettled = p.avatarTex != nullptr;
            return;
        }
        const auto KEY = p.icon + "\x1f" + std::to_string(px);
        if (p.avatarFor != KEY || p.avatarPx != px) {
            p.avatarTex.reset();
            p.avatarFor     = KEY;
            p.avatarPx      = px;
            p.avatarSettled = false;
        }
        if (p.avatarSettled)
            return;
        const auto TEX = fileTex(p.icon, px, 0, 0, false);
        if (TEX.settled) {
            p.avatarTex     = TEX.texture;
            p.avatarSettled = true;
            if (!p.avatarTex) {
                // a dead icon: the sender falls back to the generated face
                p.icon.clear();
                p.avatarFor.clear();
                p.avatarPx = 0;
                p.avatarSettled = false;
                ensureAvatarTex(p, px);
            }
        }
    }

    void iconsInit() {
        decodeJobs.clear();
        decodeJobs.reserve(MAX_PENDING_IMAGE_RESOURCES);
        if (!g_pEventLoopManager || !g_pCompositor)
            return;
        decodePoll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) {
            pollDecodeJobs();
            pollDesktopIndex();
        }, nullptr);
        g_pEventLoopManager->addTimer(decodePoll);
        if (desktopIndex.init(g_pCompositor->m_wlEventLoop))
            startDesktopIndex();
    }

    void iconsExit() {
        if (decodePoll && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(decodePoll);
        decodePoll.reset();
        decodeJobs.clear();
        desktopIndex.exit();
        desktopIcons.clear();
        generatedAvatars.clear();
        desktopScanning     = false;
        waitedForDecodeSlot = false;
        slotFreed           = false;
    }

} // namespace NHyprnotify
