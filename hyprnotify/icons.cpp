// hyprnotify/icons.cpp — notification images: content avatars and identity
// icons via hyprgraphics, raw image-data pixmaps

#include "common/fileindex.hpp"
#include "common/icons.hpp"

#include "hyprnotify.hpp"

#include <filesystem>
#include <random>

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
            ASP<Hyprgraphics::CImageResource>      resource;
            bool                                   rejected          = false;
            bool                                   readyAtWarmStart = false;
            bool                                   usedThisWarm     = false;
        };

        struct SFileTexResult {
            SP<ITexture> texture;
            bool         settled = false;
            bool         hero    = false;
        };

        std::vector<SDecodeJob>   decodeJobs;
        SP<CEventLoopTimer>       decodePoll;
        bool                      waitedForDecodeSlot = false;

        constexpr size_t          MAX_FALLBACK_FILES   = 2048;
        constexpr size_t          MAX_FALLBACK_VISITED = 65536;
        NHyprCommon::CAsyncFileIndex fallbackIndex;
        std::vector<std::string>    fallbackFiles;
        uint64_t                    fallbackGeneration = 0;
        bool                        fallbackScanning   = false;
        std::string                 fallbackDirectory;

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
            const bool PENDING = fallbackScanning || std::ranges::any_of(decodeJobs, [](const auto& job) { return !resourceReady(job); });
            decodePoll->updateTimeout(PENDING ? std::optional{std::chrono::milliseconds(16)} : std::nullopt);
        }

        static void pollDecodeJobs() {
            bool becameReady = false;
            for (auto& job : decodeJobs) {
                const bool READY = resourceReady(job);
                becameReady |= READY && !job.readyAtWarmStart;
                job.readyAtWarmStart |= READY;
            }
            if (fallbackScanning) {
                std::vector<NHyprCommon::CAsyncFileIndex::SEntry> found;
                const bool COMPLETE = fallbackIndex.poll(fallbackGeneration, found, 32);
                for (const auto& entry : found)
                    fallbackFiles.push_back(entry.path.string());
                fallbackScanning = !COMPLETE;
                becameReady |= !found.empty();
            }
            armDecodePoll();
            if (becameReady)
                notifChanged();
        }

        static SDecodeJob* decodeJobFor(const std::string& source) {
            if (const auto it = std::ranges::find(decodeJobs, source, &SDecodeJob::source); it != decodeJobs.end())
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

            auto resource = makeAtomicShared<Hyprgraphics::CImageResource>(source);
            g_pAsyncResourceGatherer->enqueue(resource);
            decodeJobs.emplace_back(source, std::move(resource));
            armDecodePoll();
            return &decodeJobs.back();
        }
    }

    void iconsInit() {
        decodeJobs.clear();
        decodeJobs.reserve(MAX_PENDING_IMAGE_RESOURCES);
        if (!g_pEventLoopManager)
            return;
        decodePoll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { pollDecodeJobs(); }, nullptr);
        g_pEventLoopManager->addTimer(decodePoll);
    }

    void iconsExit() {
        if (decodePoll && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(decodePoll);
        decodePoll.reset();
        decodeJobs.clear();
        fallbackIndex.exit();
        fallbackFiles.clear();
        fallbackDirectory.clear();
        fallbackScanning = false;
        waitedForDecodeSlot = false;
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

    // ---- fallback_icon_dir: iconless cards wear a face (the left identity) ----

    void                            resetFallbackCache() {
        fallbackFiles.clear();
        fallbackDirectory.clear();
        fallbackScanning = false;
        fallbackIndex.cancel(++fallbackGeneration);
        for (const auto& N : notifs) {
            N->fallbackPick.clear();
            N->identFor.clear();
            N->identIconPx = 0;
            N->identSettled = false;
            N->identTex.reset();
        }
    }

    // One roll per card (bus keeps the pick across in-place replaces). The
    // scan starts from warm but its bounded file walk stays off the compositor
    // thread; the timer above only adopts ready path batches.
    static std::string pickFallback() {
        const auto DIR = cfg.fallbackIconDir->value();
        if (DIR.empty())
            return "";
        if (fallbackDirectory != DIR) {
            fallbackFiles.clear();
            fallbackDirectory = DIR;
            fallbackScanning = true;
            fallbackIndex.request({.generation = ++fallbackGeneration,
                                   .roots = {DIR},
                                   .extensions = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".avif", ".jxl", ".svg"},
                                   .maxEntries = MAX_FALLBACK_FILES,
                                   .maxVisited = MAX_FALLBACK_VISITED,
                                   .recursive = true});
            armDecodePoll();
        }
        if (fallbackFiles.empty())
            return "";
        static std::mt19937 rng{std::random_device{}()};
        return fallbackFiles[std::uniform_int_distribution<size_t>{0, fallbackFiles.size() - 1}(rng)];
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

    // CImageResource decodes on the compositor-owned gatherer. The warm pass
    // only scales the ready bounded result and uploads its texture.
    static SFileTexResult fileTex(const std::string& path, int iconPx, int heroWPx, int heroHCapPx) {
        const auto JOB = decodeJobFor(path);
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

        result.hero = heroWorthy(SZ.x, SZ.y, heroWPx);
        if (result.hero)
            result.texture = coverTex(SURF->cairo(), SZ.x, SZ.y, heroWPx, std::min((int)std::lround(heroWPx * SZ.y / SZ.x), heroHCapPx));
        else
            result.texture = scaledTex(SURF->cairo(), SZ.x, SZ.y, iconPx);
        return result;
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
    void ensureIconTex(SNotif& n, int iconPx, int heroWPx, int heroHCapPx) {
        // IDENTITY (the left lead): the app_icon/desktop-entry, drawn at every
        // size it appears (lead avatar, group header). An iconless card rolls a
        // face from fallback_icon_dir so a sender is never faceless; the roll
        // is kept in fallbackPick across in-place replaces. heroWPx 0: an
        // identity is icon-box only — a wide waifu never goes hero.
        if (!n.identity.empty()) {
            n.fallbackPick.clear();
            if (n.identFor != n.identity || n.identIconPx != iconPx) {
                n.identTex.reset();
                n.identFor = n.identity;
                n.identIconPx = iconPx;
                n.identSettled = false;
            }
            if (!n.identSettled) {
                const auto TEX = fileTex(n.identity, iconPx, 0, 0);
                if (TEX.settled) {
                    n.identTex = TEX.texture;
                    n.identSettled = true;
                }
            }
        } else {
            if (n.fallbackPick.empty())
                n.fallbackPick = pickFallback();
            if (n.fallbackPick.empty()) {
                n.identTex.reset();
                n.identFor.clear();
                n.identIconPx = 0;
                n.identSettled = true;
            } else if (n.identFor != n.fallbackPick) {
                n.identTex.reset();
                n.identFor = n.fallbackPick;
                n.identIconPx = iconPx;
                n.identSettled = false;
            } else if (n.identIconPx != iconPx) {
                n.identTex.reset();
                n.identIconPx = iconPx;
                n.identSettled = false;
            }
            if (!n.identSettled) {
                const auto TEX = fileTex(n.fallbackPick, iconPx, 0, 0);
                if (TEX.settled) {
                    n.identTex = TEX.texture;
                    n.identSettled = true;
                }
            }
        }

        if (n.hasPixels) {
            if (n.pixels.empty())
                return; // malformed input was rejected before a source was retained

            uint64_t h = fnv1a(n.pixels.data(), n.pixels.size(), 0xcbf29ce484222325ULL);
            h          = fnv1a(&n.pw, sizeof(n.pw), h);
            h          = fnv1a(&n.ph, sizeof(n.ph), h);
            if (!n.iconTex || n.pixelsFor != h || n.pixelsIconPx != iconPx || n.pixelsHeroWPx != heroWPx || n.pixelsHeroHCapPx != heroHCapPx) {
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
                n.pixelsIconPx = iconPx;
                n.pixelsHeroWPx = heroWPx;
                n.pixelsHeroHCapPx = heroHCapPx;
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
            // no content image: the card shows its identity (or the rolled
            // fallback face) alone — content has no fallback of its own
            n.iconTex.reset();
            n.imageFor.clear();
            n.pixelsFor = 0;
            n.pixelsIconPx = n.pixelsHeroWPx = n.pixelsHeroHCapPx = 0;
            n.imageSettled = true;
            n.heroTex   = false;
            return;
        }
        if (n.imageFor != n.image || n.imageIconPx != iconPx || n.imageHeroWPx != heroWPx || n.imageHeroHCapPx != heroHCapPx) {
            n.iconTex.reset();
            n.imageFor  = n.image;
            n.pixelsFor = 0;
            n.imageIconPx = iconPx;
            n.imageHeroWPx = heroWPx;
            n.imageHeroHCapPx = heroHCapPx;
            n.imageSettled = false;
            n.heroTex = false;
        }
        if (!n.imageSettled) {
            const auto TEX = fileTex(n.image, iconPx, heroWPx, heroHCapPx);
            if (TEX.settled) {
                n.iconTex = TEX.texture;
                n.heroTex = TEX.hero;
                n.imageSettled = true;
            }
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
        const auto TEX = fileTex(path, iconPx, 0, 0); // icon box only, never hero
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
        const auto TEX = fileTex(im.src, maxPx, 0, 0); // fit a box, never hero
        if (TEX.settled) {
            im.tex = TEX.texture;
            im.settled = true;
        }
    }

} // namespace NHyprnotify
