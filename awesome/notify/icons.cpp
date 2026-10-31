// awesome/notify/icons.cpp — notification images: content avatars and identity
// icons via hyprgraphics, raw image-data pixmaps

#include "ui.hpp"

#include "../core/config.hpp"
#include "../core/text.hpp"
#include "../core/desktopindex.hpp"
#include "../core/fileindex.hpp"

#include <hyprland/src/render/AsyncResourceGatherer.hpp>

#include <cstring>
#include <iterator>
#include <list>
#include <random>
#include <sstream>
#include <utility>

namespace NAwesome::Notify {

    // ---- fallback_icon_dir: iconless cards wear a face (the left identity) ----
    //
    // One roll per card (bus keeps the pick across in-place replaces). The
    // listing comes from the core's file-index helper, a process: walked on
    // this thread, a pictures folder cost tens of ms of the warm, seconds
    // cold, and a hung mount held the loop. Until it lands an iconless card
    // wears the generic mark, and the warm the listing triggers re-rolls it.
    // Scanned once per config life (a reload rescans).

    static NAwesome::CAsyncFileIndex     fallbackIndex;
    static SP<CEventLoopTimer>           fallbackPoll;
    static std::vector<std::string>      fallbackFiles;
    static uint64_t                      fallbackGen = 0;
    static std::chrono::steady_clock::time_point fallbackDeadline;
    enum class EFallbackScan : uint8_t {
        NONE,
        SCANNING,
        DONE,
    };
    static EFallbackScan fallbackScan = EFallbackScan::NONE;

    static std::unordered_map<std::string, SP<ITexture>> genericMarks;

    void                 resetFallbackCache() {
        genericMarks.clear();
        fallbackIndex.cancel(++fallbackGen);
        fallbackFiles.clear();
        fallbackScan = EFallbackScan::NONE;
        if (fallbackPoll)
            fallbackPoll->updateTimeout(std::nullopt);
    }

    static void pollFallback() {
        if (fallbackScan != EFallbackScan::SCANNING)
            return;
        std::vector<NAwesome::CAsyncFileIndex::SEntry> entries;
        const bool COMPLETE = fallbackIndex.poll(fallbackGen, entries, 256);
        for (const auto& E : entries)
            if (!E.directory)
                fallbackFiles.push_back(E.path.string());
        if (!COMPLETE && std::chrono::steady_clock::now() < fallbackDeadline) {
            fallbackPoll->updateTimeout(std::chrono::milliseconds(16));
            return;
        }
        if (!COMPLETE)
            fallbackIndex.cancel(++fallbackGen); // a hung mount: keep what came
        fallbackScan = EFallbackScan::DONE;
        if (!fallbackFiles.empty())
            notifChanged();
    }

    static std::string pickFallback() {
        const auto DIR = std::string{NAwesome::cfg().getS("plugin:awesome:notify:fallback_icon_dir")};
        if (DIR.empty())
            return "";
        if (fallbackScan == EFallbackScan::NONE) {
            fallbackScan = EFallbackScan::DONE; // without the loop: no faces
            if (!fallbackPoll)
                return "";
            NAwesome::CAsyncFileIndex::SRequest request;
            request.generation = ++fallbackGen;
            request.roots      = {DIR};
            request.extensions = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".avif", ".jxl", ".svg"};
            request.maxEntries = 4096;
            request.maxVisited = 65536;
            request.recursive  = true;
            fallbackIndex.request(std::move(request));
            fallbackScan     = EFallbackScan::SCANNING;
            fallbackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            fallbackPoll->updateTimeout(std::chrono::milliseconds(2));
            return "";
        }
        if (fallbackScan != EFallbackScan::DONE || fallbackFiles.empty())
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
    // ...and so is its ONE worker: the compositor's own renderText (the
    // config error bar, the debug overlay) enqueues there and blocks its
    // thread until the job is done — behind every decode of ours queued
    // ahead of it (a screenshot's 100-300 ms, longer under load). Two in
    // flight keep the worker busy; the rest wait here, not there.
    static constexpr size_t MAX_DECODES_IN_FLIGHT = 2;
    static constexpr double MAX_DECODED_IMAGE_PIXELS    = 16.0 * 1024 * 1024;

    // What a source becomes for one card's ask: its icon-box face, or a
    // hero cover plus the square thumbnail the shade row leads with (cut
    // from the whole image); a symbolic icon repainted in the ink
    struct SScaleSpec {
        int        iconPx = 0, heroW = 0, heroH = 0;
        int        badgePx = 0; // > 0: compose the identity's corner badge too
        bool       hero = false, tint = false;
        CHyprColor ink;
        bool       operator==(const SScaleSpec& o) const {
            return iconPx == o.iconPx && heroW == o.heroW && heroH == o.heroH && badgePx == o.badgePx && hero == o.hero && tint == o.tint && (!tint || ink == o.ink);
        }
    };

    // one ask's scale of a large decode on the raster worker: out lands with
    // its done, and the job is kept until it has (decodeWarmEnd)
    struct SScaleReq {
        SScaleSpec                    spec;
        uint64_t                      ticket = 0;
        std::shared_ptr<SScaledImage> out;
    };

    struct SDecodeJob {
        std::string                                                    source;
        int                                                            svgPx   = 0;
        // the admitted file, held open: the gatherer's worker decodes
        // /proc/self/fd/N — the inode the core checked, never the path
        // again (a swap after the check cannot reach it) — and reads it off
        // this thread. Closed with the job, once the decode is done.
        NAwesome::CImageFile                                           file;
        Hyprutils::Memory::CAtomicSharedPointer<Hyprgraphics::CImageResource> resource;
        bool                                                           rejected = false; // too big or not a file: settled null
        bool                                                           readyAtPoll = false;
        bool                                                           wanted      = false; // asked for by the current warm
        bool                                                           consumed    = false; // a card built from it: it goes when the warm ends
        std::vector<SScaleReq>                                         scales;
    };

    static std::vector<SDecodeJob>   decodeJobs;
    static SP<CEventLoopTimer>       decodePoll;
    static bool                      slotFreed           = false;
    static bool                      waitedForDecodeSlot = false;
    static bool                      scaleDeferred       = false; // a ready decode waits for the scale budget

    // A decode lands full size (the gatherer ignores the hint for rasters)
    // and a large one scales on the raster worker. When the worker refuses
    // (a full queue), it scales here; past 4 MP that costs tens of ms even
    // boxed first: one per window, the rest wait for the next — a burst of
    // screenshots costs a few short stalls, not one long
    constexpr double                      SCALE_LARGE_PIXELS = 4.0 * 1024 * 1024;
    constexpr auto                        SCALE_WINDOW       = std::chrono::milliseconds(100);
    static std::chrono::steady_clock::time_point lastLargeScale;
    static bool                           largeScaleOpen() {
        return std::chrono::steady_clock::now() - lastLargeScale >= SCALE_WINDOW;
    }

    // one face per (identity, name, font, size, bg): the facepile never
    // shows a blank square for a faceless sender. The cache is an LRU, not a
    // size cap: a busy group outgrows it in DISTINCT senders, and evicting
    // an arbitrary entry (unordered_map's begin()) dropped the faces the
    // senders cycled back into a visible window — cairo + pango + a GL
    // upload per re-request, the stall the async decode redesign removed.
    // The working set is the senders the facepiles keep re-asking for, which
    // LRU keeps hot.
    static std::list<std::string> avatarLru; // most recently used at the back
    static std::unordered_map<std::string, std::pair<SP<ITexture>, std::list<std::string>::iterator>> generatedAvatars;
    static constexpr size_t       MAX_GENERATED_AVATARS = 128;

    static bool resourceReady(const SDecodeJob& job) {
        return job.rejected || (job.resource && job.resource->m_ready.load(std::memory_order_acquire));
    }

    static void armDecodePoll() {
        if (!decodePoll)
            return;
        const bool PENDING = std::ranges::any_of(decodeJobs, [](const auto& job) { return !resourceReady(job); });
        decodePoll->updateTimeout(PENDING ? std::optional{std::chrono::milliseconds(16)} :
                                      scaleDeferred ? std::optional<std::chrono::milliseconds>{SCALE_WINDOW} :
                                                      std::nullopt);
    }

    // find-or-enqueue the gatherer's job for (source, svg viewport). nullptr:
    // the slot is taken — the caller keeps the card unsettled and a freed
    // slot re-warms (a decode cannot be cancelled after enqueue, so the cap
    // also bounds replaced and deleted cards). An SVG's viewport is the
    // size it rasters at: an icon's, or a hero's box width.
    static SDecodeJob* decodeJobFor(const std::string& source, int svgPx) {
        const int SVG_PX = NAwesome::isSvgIconPath(source) ? std::clamp(svgPx, 1, 1024) : 0;
        if (const auto IT = std::ranges::find_if(decodeJobs, [&](const auto& job) { return job.source == source && job.svgPx == SVG_PX; }); IT != decodeJobs.end()) {
            IT->wanted = true;
            return &*IT;
        }
        if (decodeJobs.size() >= MAX_PENDING_IMAGE_RESOURCES ||
            (size_t)std::ranges::count_if(decodeJobs, [](const auto& job) { return !resourceReady(job); }) >= MAX_DECODES_IN_FLIGHT) {
            waitedForDecodeSlot = true; // a landing decode re-warms (the poll), a freed slot too
            return nullptr;
        }
        auto file = NAwesome::openAdmissibleImage(source);
        if (!file) {
            decodeJobs.push_back(SDecodeJob{.source = source, .rejected = true, .wanted = true});
            return &decodeJobs.back();
        }
        // hyprgraphics needs an explicit viewport for SVG files; without it
        // the path constructor settles with "invalid size". Rasters ignore it.
        auto resource = SVG_PX > 0 ? makeAtomicShared<Hyprgraphics::CImageResource>(file.path(), Vector2D{(double)SVG_PX, (double)SVG_PX}) :
                                     makeAtomicShared<Hyprgraphics::CImageResource>(file.path());
        g_pAsyncResourceGatherer->enqueue(resource);
        decodeJobs.push_back(SDecodeJob{.source = source, .svgPx = SVG_PX, .file = std::move(file), .resource = std::move(resource), .wanted = true});
        armDecodePoll();
        return &decodeJobs.back();
    }

    // A finished decode no card asked for during a full warm is an orphan:
    // its card was replaced (notify-send -r, a player's next track), dismissed
    // or re-keyed while the decode ran, and nothing will ever ask again.
    // Without this sweep each one held a slot for good, and at the cap every
    // new file icon stayed on the fallback until reload.
    void decodeWarmBegin() {
        for (auto& job : decodeJobs)
            job.wanted = false;
    }
    void decodeWarmEnd() {
        const auto SETTLED = [](const SDecodeJob& job) { return resourceReady(job) && std::ranges::all_of(job.scales, [](const auto& r) { return r.out != nullptr; }); };
        if (std::erase_if(decodeJobs, [&](const auto& job) { return (job.consumed || !job.wanted) && SETTLED(job); }) > 0)
            slotFreed = true;
    }

    static void pollDecodeJobs() {
        bool becameReady = false;
        for (auto& job : decodeJobs) {
            const bool READY = resourceReady(job);
            becameReady |= READY && !job.readyAtPoll;
            job.readyAtPoll |= READY;
        }
        const bool SLOT     = slotFreed;
        const bool DEFERRED = scaleDeferred && largeScaleOpen(); // not before its window: no warm per 16 ms tick
        scaleDeferred &= !DEFERRED;
        slotFreed           = false;
        armDecodePoll();
        if (becameReady || DEFERRED || (waitedForDecodeSlot && SLOT))
            notifChanged();
    }

    static std::string lowerKey(std::string value) {
        std::ranges::transform(value, value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return value;
    }

    // the shared .desktop index (core) grew: upgrade every live card still
    // riding the icon-name stand-in for its desktop entry
    static void onDesktopEntries() {
        bool      identityChanged = false;
        const int ICONPX          = std::max(8, (int)NAwesome::cfg().getI("plugin:awesome:notify:max_icon"));
        for (const auto& N : notifs) {
            if (!N->identityFromDesktop)
                continue;
            const auto ICON = resolveDesktopEntryIcon(N->desktopEntry, ICONPX);
            if (ICON.empty() || ICON == N->identity)
                continue;
            N->identity            = ICON; // the entry's own Icon= is authoritative
            N->identTex.reset();
            N->badgeTex.reset();
            N->identFor.clear();
            N->identIconPx         = 0;
            N->identSettled        = false;
            N->identityFromDesktop = false;
            identityChanged        = true;
        }
        if (identityChanged)
            notifChanged();
    }

    // the desktop entry's own Icon= (the index's value), resolved to a file.
    // Empty while the index has not reached the entry.
    std::string resolveDesktopEntryIcon(const std::string& entry, int sizePx) {
        const auto* ICON = NAwesome::CDesktopIndex::inst().findIcon(lowerKey(entry));
        return ICON ? Parse::resolveImage(*ICON, sizePx) : "";
    }

    // one deterministic initials face per (identity, name, font, size): the
    // facepile never shows a blank square for a faceless sender
    static SP<ITexture> generatedAvatar(const std::string& identity, const std::string& name, int iconPx) {
        const auto BG   = NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:theme:col_glass"));
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
        appendKey(std::string{NAwesome::cfg().getS("plugin:awesome:notify:font")});
        KEY += std::to_string(PX) + ":" + std::to_string(DARK);
        if (const auto IT = generatedAvatars.find(KEY); IT != generatedAvatars.end()) {
            avatarLru.splice(avatarLru.end(), avatarLru, IT->second.second);
            return IT->second.first;
        }
        if (!NAwesome::Canvas::inst().gate().mayBuild() || !g_pHyprRenderer)
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
        NAwesome::applyTextOptions(LAYOUT);
        const auto LABEL  = Pixel::initials(name.empty() ? identity : name);
        pango_layout_set_text(LAYOUT, LABEL.c_str(), -1);
        auto* FONT = pango_font_description_from_string(std::string{NAwesome::cfg().getS("plugin:awesome:notify:font")}.c_str());
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
        if (generatedAvatars.size() >= MAX_GENERATED_AVATARS) {
            const auto COLD = avatarLru.front();
            avatarLru.pop_front();
            generatedAvatars.erase(COLD);
        }
        avatarLru.push_back(KEY);
        // the map must hold the element's OWN iterator (not end(), which the
        // hit-path splice would read as a refresh) — the prev() after the
        // push_back is exactly the entry just appended
        const auto [AIT, _] = generatedAvatars.emplace(KEY, std::pair{TEX, std::prev(avatarLru.end())});
        return AIT->second.first;
    }

    // ---- scaling: inline below a size, on the raster worker above it ----
    //
    // A card's image is scaled ONCE on the CPU to what it paints (a 4K
    // pixmap kept full size would hold megabytes of VRAM to paint <=100
    // logical px, and its upload would stall the compositor thread). Past
    // ASYNC_IMAGE_PIXELS source pixels the scale leaves the compositor
    // thread for the raster worker: a box-and-GOOD scale of a 512² source
    // is about a millisecond, of a screenshot tens of them. Smaller ones
    // scale inline, in the warm that asked. The upload stays in a warm
    // either way.
    uint64_t imageJobTicket() {
        static uint64_t serial = 0;
        return ++serial;
    }

    // A picture takes the hero layout: 256 px in both dimensions, at least
    // half the hero box wide, and a picture's shape — a fifth longer one way
    // than the other, as a capture or a photo is (wide or portrait). A
    // square is a face or an icon at any size: Firefox posts a page's icon
    // at its own size (a 512 px app icon, a contact's avatar), and as a
    // hero it was cut into a card-wide strip of its middle.
    static bool heroWorthy(double sw, double sh, int heroWPx) {
        return heroWPx > 0 && sw >= 256 && sh >= 256 && sw * 2 >= heroWPx && std::max(sw, sh) >= 1.2 * std::min(sw, sh);
    }

    static SScaleSpec specFor(double sw, double sh, int iconPx, int heroWPx, int heroHCapPx, bool tint, int badgePx = 0) {
        SScaleSpec s{.iconPx = iconPx, .badgePx = badgePx, .hero = heroWorthy(sw, sh, heroWPx), .tint = tint};
        if (s.hero) {
            s.heroW = heroWPx;
            s.heroH = std::min((int)std::lround((double)heroWPx * sh / sw), heroHCapPx);
        }
        if (tint)
            s.ink = NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_fg"));
        return s;
    }

    // Repaint a rasterized symbolic icon to col, premultiplied (the same
    // pixel math as the shell's loadSvg recolor; alpha untouched).
    static void tintPixels(SPixels& p, const CHyprColor& col) {
        const uint8_t R = (uint8_t)(col.r * 255), G = (uint8_t)(col.g * 255), B = (uint8_t)(col.b * 255);
        for (size_t i = 0; i + 3 < p.px.size(); i += 4) {
            const uint8_t A = p.px[i + 3];
            p.px[i]         = (uint8_t)(B * A / 255);
            p.px[i + 1]     = (uint8_t)(G * A / 255);
            p.px[i + 2]     = (uint8_t)(R * A / 255);
        }
    }

    // The identity's corner badge as one raster at its device size: a white
    // disc, and inside a ring of whole pixels the app's mark — a colour icon
    // cropped to what it draws and covering the disc (a round icon meets
    // the ring edge to edge, nothing dark between them), a symbolic one in
    // the graphite ink, centred on the white. Every edge lies over white:
    // the GPU's three layers (white, a graphite ground, the icon clipped)
    // each antialiased their own circle, and the ground showed through as
    // a dark fringe and round an icon's transparent margin.
    static SPixels composeBadge(const Raster::SView& src, int d, bool symbolic) {
        const int RING = badgeRingPx(d), IN = d - 2 * RING;
        SPixels   mark;
        if (symbolic) {
            mark = Raster::fit(src, std::max(1, (int)std::lround(IN * 0.64)));
            tintPixels(mark, tBadgeGround());
        } else {
            Raster::SView v = src;
            if (src.format == CAIRO_FORMAT_ARGB32) { // what the icon draws: its bounds at alpha past 24
                int x0 = src.w, y0 = src.h, x1 = -1, y1 = -1;
                for (int y = 0; y < src.h; y++)
                    for (int x = 0; x < src.w; x++)
                        if (src.data[(size_t)y * src.stride + (size_t)x * 4 + 3] > 24)
                            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                if (x1 >= x0 && y1 >= y0) {
                    v.data += (size_t)y0 * src.stride + (size_t)x0 * 4;
                    v.w = x1 - x0 + 1;
                    v.h = y1 - y0 + 1;
                }
            }
            mark = Raster::cover(v, IN, IN);
        }
        SPixels     out{.px = std::vector<uint8_t>((size_t)d * d * 4), .w = d, .h = d};
        auto*       SURF = cairo_image_surface_create_for_data(out.px.data(), CAIRO_FORMAT_ARGB32, d, d, d * 4);
        auto*       CR   = cairo_create(SURF);
        const auto& RIM  = tBadgeRim();
        cairo_arc(CR, d / 2.0, d / 2.0, d / 2.0, 0, 2 * M_PI);
        cairo_set_source_rgba(CR, RIM.r, RIM.g, RIM.b, RIM.a);
        cairo_fill(CR);
        if (mark.valid()) {
            auto* MARK = cairo_image_surface_create_for_data(mark.px.data(), CAIRO_FORMAT_ARGB32, mark.w, mark.h, mark.w * 4);
            if (!symbolic) {
                cairo_arc(CR, d / 2.0, d / 2.0, IN / 2.0, 0, 2 * M_PI);
                cairo_clip(CR);
            }
            cairo_set_source_surface(CR, MARK, (d - mark.w) / 2, (d - mark.h) / 2);
            cairo_paint(CR);
            cairo_surface_destroy(MARK);
        }
        cairo_destroy(CR);
        cairo_surface_flush(SURF);
        cairo_surface_destroy(SURF);
        return out;
    }

    // the CPU half, any thread: reads src, writes only its result
    static SScaledImage scaleFor(const Raster::SView& src, const SScaleSpec& s) {
        SScaledImage out{.hero = s.hero};
        if (s.hero) {
            out.main  = Raster::cover(src, s.heroW, s.heroH);
            out.thumb = Raster::cover(src, s.iconPx, s.iconPx);
        } else
            out.main = Raster::fit(src, s.iconPx);
        if (s.badgePx > 0)
            out.badge = composeBadge(src, s.badgePx, s.tint);
        if (s.tint) {
            tintPixels(out.main, s.ink);
            tintPixels(out.thumb, s.ink);
        }
        return out;
    }

    static Raster::SView viewOf(cairo_surface_t* s) {
        cairo_surface_flush(s);
        return {cairo_image_surface_get_data(s), cairo_image_surface_get_width(s), cairo_image_surface_get_height(s), cairo_image_surface_get_stride(s),
                cairo_image_surface_get_format(s)};
    }

    static SP<ITexture> upload(const SPixels& p) {
        if (!p.valid() || !g_pHyprRenderer)
            return nullptr;
        return g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, const_cast<uint8_t*>(p.px.data()), p.w * 4, Vector2D{(double)p.w, (double)p.h});
    }

    // The freedesktop symbolic convention, shared with the shell's loadIcon:
    // a pure-shape mark the toolkit repaints with the widget's foreground.
    // Adwaita bakes them near-black, so on a dark card the brightness OSD
    // icon was invisible until fileTex repaints it.
    static bool isSymbolicIconPath(const std::string& path) {
        return NAwesome::isSvgIconPath(path) && (path.find("-symbolic") != std::string::npos || path.find("/symbolic/") != std::string::npos);
    }

    struct SFileTexResult {
        SP<ITexture> texture;
        SP<ITexture> thumb;           // a hero's icon-box face
        SP<ITexture> badge;           // an identity's corner badge
        bool         settled = false; // false: an async decode or scale is in flight
        bool         hero    = false;
    };

    static SFileTexResult uploaded(const SScaledImage& s) {
        return {.texture = upload(s.main), .thumb = s.hero ? upload(s.thumb) : nullptr, .badge = s.badge.valid() ? upload(s.badge) : nullptr, .settled = true, .hero = s.hero};
    }

    // CImage's size hint only bounds SVG rasters; raster formats decode full
    // size transiently and get scaled here. The synchronous path: always
    // settled, used without the compositor's gatherer and for the generic
    // mark (a fixed theme name, not a sender's file).
    static SFileTexResult syncFileTex(const std::string& path, int iconPx, int heroWPx, int heroHCapPx, bool tintSymbolic = false, int badgePx = 0) {
        const int  HINT  = std::max(iconPx, heroWPx);
        const auto BYTES = NAwesome::readImageFile(path); // one non-blocking open; these bytes are what decodes
        if (!BYTES)
            return {.settled = true};
        // the format named, not sniffed: AUTO loads libmagic's database per
        // image, on this thread
        const auto FORMAT = NAwesome::isSvgIconPath(path)                                                                          ? Hyprgraphics::IMAGE_FORMAT_SVG :
            BYTES->size() >= 4 && (*BYTES)[0] == 0x89 && (*BYTES)[1] == 'P' && (*BYTES)[2] == 'N' && (*BYTES)[3] == 'G' ? Hyprgraphics::IMAGE_FORMAT_PNG :
            BYTES->size() >= 2 && (*BYTES)[0] == 0xff && (*BYTES)[1] == 0xd8                                              ? Hyprgraphics::IMAGE_FORMAT_JPEG :
                                                                                                                             Hyprgraphics::IMAGE_FORMAT_AUTO;
        Hyprgraphics::CImage image(std::span<const uint8_t>{*BYTES}, FORMAT, Vector2D{(double)HINT, (double)HINT});
        if (!image.success())
            return {.settled = true};
        const auto SURF = image.cairoSurface();
        if (!SURF || SURF->status() != CAIRO_STATUS_SUCCESS)
            return {.settled = true};
        const auto SZ = SURF->size();
        if (SZ.x <= 0 || SZ.y <= 0)
            return {.settled = true};
        return uploaded(scaleFor(viewOf(SURF->cairo()), specFor(SZ.x, SZ.y, iconPx, heroWPx, heroHCapPx, tintSymbolic && isSymbolicIconPath(path), badgePx)));
    }

    // The async front door: a file icon decodes on the gatherer's worker,
    // and a large one scales on the raster worker; the warm uploads what
    // landed. settled=false means "ask again after the next warm" — the
    // decode poll and the scale's done re-warm. SVGs too, a sender's
    // included (up to 2 MiB of XML to parse): a hero candidate rasters at
    // the hero box's width, as the synchronous path's hint did, and a
    // symbolic icon is tinted on its scaled copy (the decode is shared by
    // every card asking for the file).
    static SFileTexResult fileTex(const std::string& path, int iconPx, int heroWPx, int heroHCapPx, bool tintSymbolic = false, int badgePx = 0) {
        if (!g_pAsyncResourceGatherer)
            return syncFileTex(path, iconPx, heroWPx, heroHCapPx, tintSymbolic, badgePx);
        const auto JOB = decodeJobFor(path, heroWPx > 0 ? std::max(iconPx, heroWPx) : iconPx);
        if (!JOB || !resourceReady(*JOB))
            return {}; // the slot is taken, or the decode is in flight
        // The job stays until this warm ends (decodeWarmEnd): every other
        // card asking for the same file in the same warm (an app's cards,
        // the same fallback face) takes the same decode instead of queueing
        // its own, one after another
        if (JOB->rejected) {
            JOB->consumed = true;
            return {.settled = true};
        }
        const auto SURF = JOB->resource->m_asset.cairoSurface;
        const auto SZ   = SURF && SURF->status() == CAIRO_STATUS_SUCCESS ? SURF->size() : Vector2D{};
        if (SZ.x <= 0 || SZ.y <= 0 || SZ.x * SZ.y > MAX_DECODED_IMAGE_PIXELS) {
            JOB->consumed = true;
            return {.settled = true};
        }
        const auto SPEC = specFor(SZ.x, SZ.y, iconPx, heroWPx, heroHCapPx, tintSymbolic && isSymbolicIconPath(path), badgePx);
        if ((int64_t)SZ.x * (int64_t)SZ.y <= ASYNC_IMAGE_PIXELS) {
            JOB->consumed = true;
            return uploaded(scaleFor(viewOf(SURF->cairo()), SPEC));
        }

        // large: this ask's scale on the raster worker, once
        auto REQ = std::ranges::find_if(JOB->scales, [&](const auto& r) { return r.spec == SPEC; });
        if (REQ != JOB->scales.end()) {
            if (!REQ->out)
                return {}; // in flight: its done re-warms
            JOB->consumed = true;
            return uploaded(*REQ->out);
        }
        const uint64_t TICKET = imageJobTicket();
        const auto     SRC    = viewOf(SURF->cairo());
        auto           OUT    = std::make_shared<SScaledImage>();
        // the work holds the decode alive through the gatherer's atomic
        // pointer and reads its pixels in place; nothing else crosses
        const bool QUEUED = NAwesome::CRasterWorker::inst().submit([RES = JOB->resource, SRC, SPEC, OUT]() { *OUT = scaleFor(SRC, SPEC); },
                                                                   [RES = JOB->resource, OUT, TICKET, SOURCE = JOB->source, SVGPX = JOB->svgPx]() {
                                                                       for (auto& job : decodeJobs)
                                                                           if (job.source == SOURCE && job.svgPx == SVGPX)
                                                                               for (auto& r : job.scales)
                                                                                   if (r.ticket == TICKET)
                                                                                       r.out = OUT;
                                                                       notifChanged();
                                                                   });
        if (QUEUED) {
            JOB->scales.push_back({.spec = SPEC, .ticket = TICKET});
            return {};
        }
        // the worker refused (full, or stopped): inline, at most one very
        // large scale per window
        if (SZ.x * SZ.y > SCALE_LARGE_PIXELS) {
            if (!largeScaleOpen()) {
                scaleDeferred = true; // the job stays wanted; the poll re-warms past the window
                armDecodePoll();
                return {};
            }
            lastLargeScale = std::chrono::steady_clock::now();
        }
        JOB->consumed = true;
        return uploaded(scaleFor(SRC, SPEC));
    }

    // CPU-side cap for image-data buffers (the model's retained-pixel
    // budget) — same premultiplied-BGRA layout in and out
    void shrinkPixels(SNotif& n, int maxPx) {
        if (n.pixels.empty() || (n.pw <= maxPx && n.ph <= maxPx))
            return;
        auto out = Raster::fit({n.pixels.data(), n.pw, n.ph, n.pw * 4, CAIRO_FORMAT_ARGB32}, maxPx);
        if (!out.valid())
            return;
        n.pixels     = std::move(out.px);
        n.pw         = out.w;
        n.ph         = out.h;
        n.pixelsHash = Raster::hash(SPixels{.px = n.pixels, .w = n.pw, .h = n.ph});
    }

    // The icon anatomy: n.iconTex carries the CONTENT (image-data pixmap or
    // image-path file, hero-capable), n.identTex the IDENTITY (app_icon /
    // desktop-entry, icon-box only). The render decides which leads and
    // whether the identity rides as the corner badge.
    // the deterministic generic mark an iconless card wears (identity empty
    // AND the fallback face dir empty): the theme's application-default-icon
    // when it exists, else an apps-grid glyph in the theme ink — no card is
    // ever faceless. One per (size, ink, plate), shared: every iconless card
    // built its own, a theme lookup, an SVG raster and an upload each. A
    // reload drops them (the theme may have changed).
    static SP<ITexture> genericMarkNow(int px);
    static SP<ITexture> genericMark(int px) {
        const auto KEY = std::to_string(px) + "|" + std::to_string(NAwesome::cfg().getColor("plugin:awesome:notify:col_fg")) + "|" +
            std::to_string(NAwesome::cfg().getColor("plugin:awesome:theme:col_rim"));
        if (const auto IT = genericMarks.find(KEY); IT != genericMarks.end())
            return IT->second;
        auto TEX = genericMarkNow(px);
        if (!TEX)
            return nullptr;
        if (genericMarks.size() >= 8)
            genericMarks.clear();
        genericMarks.emplace(KEY, TEX);
        return TEX;
    }

    static SP<ITexture> genericMarkNow(int px) {
        if (const auto PATH = Parse::resolveImage("application-default-icon", px); !PATH.empty())
            return syncFileTex(PATH, px, 0, 0).texture; // a small theme SVG: decode here, not on the worker

        auto*        SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
        auto*        CT   = cairo_create(SURF);
        const auto   INK  = NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_fg")), PLATE = NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:theme:col_rim"));
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
        cairo_surface_flush(SURF);
        auto TEX = g_pHyprRenderer->createTexture(SURF);
        cairo_surface_destroy(SURF);
        return TEX;
    }

    // an image-data card's face is uploaded: what it was built from is no
    // longer kept (the hash keeps an identical replace from rebuilding it)
    static void setPixelsFace(SNotif& n, SFileTexResult&& face) {
        n.iconTex   = std::move(face.texture);
        n.thumbTex  = std::move(face.thumb);
        n.heroTex   = face.hero;
        n.pixelsFor = n.pixelsHash;
        n.imageFor.clear();
        n.pixels.clear();
        n.pixels.shrink_to_fit();
    }

    // one re-warm a walk window after a name the budget deferred (cards,
    // actions): the warm resolves them again
    static SP<CEventLoopTimer> namesPoll;
    void                       iconNamesDeferred() {
        if (namesPoll && !namesPoll->armed())
            namesPoll->updateTimeout(NAwesome::ICON_WALK_WINDOW + std::chrono::milliseconds(10));
    }

    void ensureIconTex(SNotif& n, int iconPx, int heroWPx, int heroHCapPx) {
        retryDeferredNames(n);
        // IDENTITY (the left lead): the app_icon/desktop-entry, drawn at every
        // size it appears (lead avatar, group header). An iconless card rolls a
        // face from fallback_icon_dir so a sender is never faceless; the roll
        // is kept in fallbackPick across in-place replaces. heroWPx 0: an
        // identity is icon-box only — a wide waifu never goes hero.
        if (!n.identity.empty()) {
            n.fallbackPick.clear();
            // a symbolic identity bakes colFg into its pixels (fileTex): the
            // key carries the fg it was tinted with, so a theme change
            // rebuilds it — the shell's dropStaleTint, per-card edition
            const auto SYM = isSymbolicIconPath(n.identity);
            const auto KEY = SYM ? n.identity + "\x1f" + std::to_string(NAwesome::cfg().getColor("plugin:awesome:notify:col_fg")) : n.identity;
            if (n.identFor != KEY || n.identIconPx != iconPx) {
                n.identTex.reset();
                n.badgeTex.reset();
                n.identFor     = KEY;
                n.identIconPx  = iconPx;
                n.identSettled = false;
            }
            if (!n.identSettled) {
                const auto TEX = fileTex(n.identity, iconPx, 0, 0, SYM, badgePx(iconPx));
                if (TEX.settled) {
                    n.identTex     = TEX.texture;
                    n.badgeTex     = TEX.badge;
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
                const auto GENERIC = "__awesome_generic__\x1f" + std::to_string(NAwesome::cfg().getColor("plugin:awesome:notify:col_fg"));
                if (n.identFor != GENERIC || n.identIconPx != iconPx) {
                    n.identTex.reset();
                    n.badgeTex.reset();
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
                n.badgeTex.reset();
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
            if (n.pixelsJob)
                return; // its unpack or scale is on the worker: the done re-warms
            if (n.scaled) {
                const auto S = std::move(n.scaled);
                setPixelsFace(n, uploaded(*S));
                return;
            }
            if (n.pixels.empty())
                return; // uploaded by an earlier warm; the texture carries it now
            if (n.iconTex && n.pixelsFor == n.pixelsHash) {
                // a replace that carried the same image-data keeps its face
                n.pixels.clear();
                n.pixels.shrink_to_fit();
                return;
            }
            SPixels    src{.px = std::move(n.pixels), .w = n.pw, .h = n.ph};
            const auto SPEC = specFor(src.w, src.h, iconPx, heroWPx, heroHCapPx, false);
            if ((int64_t)src.w * src.h > ASYNC_IMAGE_PIXELS) {
                // the buffer moves into the job; the done finds its card by
                // the ticket (a replace or a dismissal has dropped it)
                auto           SRC    = std::make_shared<SPixels>(std::move(src));
                auto           OUT    = std::make_shared<SScaledImage>();
                const uint64_t TICKET = imageJobTicket();
                const bool     QUEUED = NAwesome::CRasterWorker::inst().submit(
                    [SRC, OUT, SPEC]() mutable {
                        *OUT = scaleFor(Raster::view(*SRC), SPEC);
                        SRC.reset();
                    },
                    [OUT, TICKET]() {
                        for (const auto& N : notifs)
                            if (N->pixelsJob == TICKET) {
                                N->pixelsJob = 0;
                                N->scaled    = std::make_unique<SScaledImage>(std::move(*OUT));
                                notifChanged();
                                return;
                            }
                    });
                if (QUEUED) {
                    n.pixelsJob = TICKET;
                    return;
                }
                src = std::move(*SRC); // refused: scale it here
            }
            if (!SPEC.hero && src.w <= iconPx && src.h <= iconPx)
                setPixelsFace(n, {.texture = upload(src), .settled = true});
            else
                setPixelsFace(n, uploaded(scaleFor(Raster::view(src), SPEC)));
            return;
        }

        if (n.image.empty()) {
            // no content image: the card shows its identity (or the rolled
            // fallback face) alone — content has no fallback of its own
            n.iconTex.reset();
            n.thumbTex.reset();
            n.imageFor.clear();
            n.pixelsFor = 0;
            n.heroTex   = false;
            n.imageSettled = true;
            return;
        }
        if (n.imageFor != n.image || n.imageIconPx != iconPx) {
            n.iconTex.reset();
            n.thumbTex.reset();
            n.imageFor     = n.image;
            n.imageIconPx  = iconPx;
            n.pixelsFor    = 0;
            n.imageSettled = false;
        }
        if (!n.imageSettled) {
            // a vector has no size of its own: rastered at the hero width it
            // always measured hero-worthy, and an icon (a theme's SVG named
            // as the image) became a card-wide strip cut from its middle
            const auto TEX = fileTex(n.image, iconPx, NAwesome::isSvgIconPath(n.image) ? 0 : heroWPx, heroHCapPx, false);
            if (TEX.settled) {
                n.iconTex      = TEX.texture;
                n.thumbTex     = TEX.thumb;
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

    // the initials face. Its tint rides the card's bg (the light/dark split
    // inside generatedAvatar): the key carries the bg it was rolled under,
    // so a theme switch re-rolls it.
    void ensureAvatarTex(SChatFace& f, int px) {
        const std::string KEY = "__awesome_avatar:\x1f" + f.key + "\x1f" + f.name + "\x1f" + std::string{NAwesome::cfg().getS("plugin:awesome:notify:font")} + "\x1f" + std::to_string(px) + "\x1f" +
                                std::to_string(NAwesome::cfg().getColor("plugin:awesome:theme:col_glass"));
        if (f.avatarFor == KEY)
            return;
        f.avatarTex = generatedAvatar(f.key, f.name, px);
        f.avatarFor = KEY;
    }

    void iconsInit() {
        decodeJobs.clear();
        decodeJobs.reserve(MAX_PENDING_IMAGE_RESOURCES);
        if (!g_pEventLoopManager || !g_pCompositor)
            return;
        decodePoll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { pollDecodeJobs(); }, nullptr);
        g_pEventLoopManager->addTimer(decodePoll);
        namesPoll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { notifChanged(); }, nullptr);
        g_pEventLoopManager->addTimer(namesPoll);
        if (fallbackIndex.init(g_pCompositor->m_wlEventLoop)) {
            fallbackPoll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { pollFallback(); }, nullptr);
            g_pEventLoopManager->addTimer(fallbackPoll);
        }
        NAwesome::CDesktopIndex::inst().subscribe(onDesktopEntries);
    }

    void iconsExit() {
        if (decodePoll && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(decodePoll);
        decodePoll.reset();
        if (namesPoll && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(namesPoll);
        namesPoll.reset();
        if (fallbackPoll && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(fallbackPoll);
        fallbackPoll.reset();
        fallbackIndex.exit(); // its helper's group is signalled and reaped
        fallbackFiles.clear();
        fallbackScan = EFallbackScan::NONE;
        genericMarks.clear();
        // A decode still on the gatherer's worker opens /proc/self/fd/N after
        // this: closing N would let the number be reused, and the worker
        // open whatever got it (a pipe would block the compositor's shared
        // gatherer). Those fds stay open — bounded by the slot cap, and only
        // an unload mid-decode leaves any.
        for (auto& job : decodeJobs)
            if (!resourceReady(job))
                job.file.release();
        decodeJobs.clear();
        avatarLru.clear();
        generatedAvatars.clear();
        waitedForDecodeSlot = false;
        slotFreed           = false;
        scaleDeferred       = false;
    }

} // namespace NAwesome::Notify
