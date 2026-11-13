// The cursor image at the virtual pointer. An app asks for its cursor image
// (wl_pointer.set_cursor) or names a shape (cursor-shape-v1) when its
// pointer enters, and again when it wants another; the compositor refuses
// both unless the app holds the real pointer focus, so the seat never shows
// a glide's image. vclick keeps each app's latest request (main.cpp hooks
// the seat manager's set_cursor and hears the shapes) and draws that image
// itself at the virtual pointer: the app's own image, or the named shape
// from the cursor theme (theme.cpp), or nothing where the app hid its
// cursor; the built-in arrow only where the theme has no such shape. It
// draws in the window's own render pass (so a window above it covers it, as
// it covers the app), never in a capture. The user's own pointer coming
// into that app takes the app's one pointer: the cursor goes, and the next
// input starts where their pointer left the window, as one mouse would. In
// the mouse mode it stays where the last input left it, as a mouse at rest;
// in the tap mode it shows while a glide runs and LINGER_MS after, then
// fades.
#include "vclick.hpp"
#include "theme.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

#include <drm_fourcc.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <unordered_map>

namespace VClick::Cursor {
    constexpr int LINGER_MS = 1200;
    constexpr int FADE_MS   = 300;
    constexpr int FRAME_MS  = 16; // the fade's damage pace
    constexpr int ARROW_W = 12, ARROW_H = 19;
    constexpr int MAX_SHAPES = 64; // cursor-shape-v1 names 34

    bool appImages = false;

    struct SImage {
        WP<CWLSurfaceResource> surf;
        Vector2D               hotspot;
        std::string            shape; // a named shape (cursor-shape-v1); empty: the surface (set_cursor), none or empty one hiding it
    };
    static std::unordered_map<wl_client*, SImage> images;
    static SP<Render::ITexture>                   arrow;

    // a named shape from the cursor theme, in logical px
    struct SThemed {
        std::vector<SP<Render::ITexture>> frames;
        std::vector<uint32_t>             delays;    // ms each frame shows
        uint32_t                          cycle = 0; // an animation's whole round; 0: one frame
        Vector2D                          size, hotspot;
    };
    static struct {
        std::string                                             key;     // the theme, size and file size the shapes are for
        std::unordered_map<std::string, std::optional<SThemed>> shapes;  // nullopt: the theme has none
        std::string                                             missing; // a shape render found unloaded
        SP<CEventLoopTimer>                                     loader;  // loads it from the event loop
    } theme;

    // what is drawn now
    static struct {
        PHLWINDOWREF        window;
        Vector2D            pos; // window coords
        bool                shown  = false;
        bool                pinned = false; // an input runs: no fade
        bool                stay   = false; // the mouse mode: at rest, no fade
        Time::steady_tp     fadeAt;         // whole until then, then fading
        CBox                drawn;          // the box last damaged, global logical px
        SP<CEventLoopTimer> timer;          // the fade
        PHLWINDOWREF        taken;          // the user's pointer is in this window's app: it has the app's pointer
        bool                frameDrawn = false; // drawn in this frame already (a window can render twice: main and popups)
        Time::steady_tp     since;              // shown since: an animated shape's clock
        SP<CEventLoopTimer> anim;               // an animated shape's next frame
    } g;

    // The theme the session names (XCURSOR_THEME, XCURSOR_SIZE, as the
    // compositor reads them), its file size for the sharpest output: the
    // size times the highest scale, rounded up.
    struct SWant {
        std::string name;
        int         size = 24, nominal = 24;
        std::string key;
    };
    static SWant want() {
        SWant       w;
        const char* T = std::getenv("XCURSOR_THEME");
        w.name        = T && *T ? T : "default";
        if (const char* S = std::getenv("XCURSOR_SIZE"); S && *S) {
            int v             = 0;
            const auto [_, E] = std::from_chars(S, S + std::char_traits<char>::length(S), v);
            if (E == std::errc{} && v > 0 && v <= Theme::MAX_SIDE)
                w.size = v;
        }
        float scale = 1;
        for (const auto& m : State::monitorState()->monitors())
            scale = std::max(scale, m->m_scale);
        w.nominal     = sc<int>(w.size * std::ceil(scale));
        const char* P = std::getenv("XCURSOR_PATH");
        w.key         = std::format("{}:{}:{}:{}", w.name, w.size, w.nominal, P ? P : "");
        return w;
    }

    // the shape's frames, read and made textures on a miss: the event loop
    // only (file reads, texture uploads), never inside a render
    static const SThemed* loadShape(const std::string& shape) {
        const auto W = want();
        if (W.key != theme.key) {
            theme.shapes.clear();
            theme.key = W.key;
        }
        if (const auto IT = theme.shapes.find(shape); IT != theme.shapes.end())
            return IT->second ? &*IT->second : nullptr;
        if (theme.shapes.size() >= MAX_SHAPES)
            theme.shapes.clear();
        std::optional<SThemed> out;
        const auto             FILE = Theme::find(W.name, shape, Theme::searchPath());
        const auto             IMG  = FILE.empty() ? std::unexpected("") : Theme::readFile(FILE, W.nominal);
        if (IMG && g_pHyprRenderer) {
            const double K  = sc<double>(W.size) / IMG->nominal; // the file's px to logical px
            const auto&  F0 = IMG->frames.front();
            SThemed      t{.size = {F0.w * K, F0.h * K}, .hotspot = {F0.hotX * K, F0.hotY * K}};
            for (const auto& f : IMG->frames) {
                auto tex = g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, rc<uint8_t*>(const_cast<uint32_t*>(f.argb.data())), f.w * 4, Vector2D{f.w, f.h});
                if (!tex)
                    break;
                t.frames.emplace_back(std::move(tex));
                t.delays.emplace_back(std::clamp<uint32_t>(f.delayMs, 10, 10000));
            }
            if (t.frames.size() > 1)
                for (const auto D : t.delays)
                    t.cycle += D;
            if (!t.frames.empty())
                out = std::move(t);
        }
        const auto& SLOT = theme.shapes[shape] = std::move(out);
        return SLOT ? &*SLOT : nullptr;
    }

    struct SDraw {
        SP<Render::ITexture>   tex; // null: nothing (the app hid its cursor)
        Vector2D               size, hotspot;
        SP<CWLSurfaceResource> surf;             // the app's cursor surface, when that is the image
        const SThemed*         themed = nullptr; // the theme's shape, when that is the image
        std::string            missing;          // a shape not loaded yet: the arrow meanwhile
    };

    // the frame an animated shape shows now
    static size_t frameOf(const SThemed& t) {
        if (!t.cycle)
            return 0;
        auto ms = sc<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - g.since).count() % t.cycle);
        for (size_t i = 0; i < t.delays.size(); ++i) {
            if (ms < t.delays[i])
                return i;
            ms -= t.delays[i];
        }
        return 0;
    }

    // what the window's app shows as its cursor: its own image, nothing
    // (hidden), or its named shape ("default" until it names one) from the
    // theme, the arrow where the theme has none; load: may read the theme
    static SDraw imageFor(const PHLWINDOW& w, bool load) {
        std::string shape = "default";
        if (const auto CLIENT = Pointer::clientOf(w)) {
            if (const auto IT = images.find(CLIENT); IT != images.end()) {
                const auto SURF = IT->second.surf.lock();
                if (SURF && SURF->m_current.texture && SURF->m_current.size.x > 0 && SURF->m_current.size.y > 0)
                    return {SURF->m_current.texture, SURF->m_current.size, IT->second.hotspot, SURF};
                // no surface, or one without a buffer (GTK's blank cursor):
                // hidden, as the compositor shows it
                if (IT->second.shape.empty())
                    return {};
                shape = IT->second.shape;
            }
        }
        const SThemed* t = nullptr;
        if (load)
            t = loadShape(shape);
        else if (const auto IT = theme.shapes.find(shape); IT != theme.shapes.end()) // as loaded last: a render reads no environment
            t = IT->second ? &*IT->second : nullptr;
        else
            return {arrow, {ARROW_W, ARROW_H}, {0, 0}, nullptr, nullptr, shape};
        if (t)
            return {t->frames[frameOf(*t)], t->size, t->hotspot, nullptr, t};
        return {arrow, {ARROW_W, ARROW_H}, {0, 0}};
    }

    // where the window is drawn now: its animated position, and its
    // workspace's slide (as the renderer places it), so a cursor at rest
    // moves with the window instead of waiting at its goal
    static Vector2D origin(const PHLWINDOW& w) {
        auto       at = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto WS = w->m_workspace;
        if (WS && WS->m_renderOffset->isBeingAnimated() && !(w->m_state & Desktop::View::WINDOW_STATE_PINNED))
            at += WS->m_renderOffset->value();
        return at;
    }

    static std::optional<CBox> box() {
        const auto W = g.window.lock();
        if (!g.shown || !W)
            return std::nullopt;
        const auto IMG = imageFor(W, false);
        if (!IMG.tex)
            return std::nullopt;
        return CBox{origin(W) + g.pos - IMG.hotspot, IMG.size};
    }

    static void damage() {
        if (!g_pHyprRenderer)
            return;
        if (!g.drawn.empty())
            g_pHyprRenderer->damageBox(g.drawn.copy().expand(2));
        const auto BOX = box();
        g.drawn        = BOX ? *BOX : CBox{};
        if (BOX)
            g_pHyprRenderer->damageBox(BOX->copy().expand(2));
    }

    static float alpha() {
        if (!g.shown)
            return 0;
        if (g.pinned || g.stay)
            return 1;
        const double T = std::chrono::duration<double, std::milli>(Time::steadyNow() - g.fadeAt).count();
        return T <= 0 ? 1.F : T >= FADE_MS ? 0.F : sc<float>(1 - T / FADE_MS);
    }

    static void stopTimer() {
        if (g.timer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(g.timer);
        g.timer.reset();
    }

    void remember(wl_client* client, const SP<CWLSurfaceResource>& surf, const Vector2D& hotspot, std::string shape) {
        if (images.size() > 64)
            std::erase_if(images, [](const auto& e) { return e.second.surf.expired(); });
        images[client] = {surf, hotspot, std::move(shape)};
        if (!g.shown)
            return;
        if (const auto W = g.window.lock(); W && Pointer::clientOf(W) == client)
            imageFor(W, true); // a named shape loads here, on the event loop
        damage();
    }

    void prepare(const PHLWINDOW& w) {
        if (w)
            imageFor(w, true);
        if (!theme.loader && g_pEventLoopManager) {
            theme.loader = makeShared<CEventLoopTimer>(
                std::nullopt,
                [](SP<CEventLoopTimer>, void*) {
                    if (const auto SHAPE = std::exchange(theme.missing, ""); !SHAPE.empty() && loadShape(SHAPE) && g.shown)
                        damage();
                },
                nullptr);
            g.anim = makeShared<CEventLoopTimer>(
                std::nullopt,
                [](SP<CEventLoopTimer>, void*) {
                    if (g.shown)
                        damage();
                },
                nullptr);
            g_pEventLoopManager->addTimer(theme.loader);
            g_pEventLoopManager->addTimer(g.anim);
        }
        if (arrow || !g_pHyprRenderer)
            return;
        static constexpr std::array<std::string_view, ARROW_H> ART = {
            "B...........", "BB..........", "BWB.........", "BWWB........", "BWWWB.......", "BWWWWB......", "BWWWWWB.....",
            "BWWWWWWB....", "BWWWWWWWB...", "BWWWWWWWWB..", "BWWWWWWWWWB.", "BWWWWWWBBBBB", "BWWWBWWB....", "BWWBBWWB....",
            "BWB..BWWB...", "BB...BWWB...", "B.....BWWB..", "......BWWB..", ".......BB...",
        };
        std::array<unsigned char, ARROW_W * ARROW_H * 4> px{};
        for (int y = 0; y < ARROW_H; ++y) {
            for (int x = 0; x < ARROW_W; ++x) {
                if (ART[y][x] == '.')
                    continue;
                const unsigned char V = ART[y][x] == 'W' ? 255 : 0;
                unsigned char*      p = &px[(y * ARROW_W + x) * 4];
                p[0] = p[1] = p[2] = V;
                p[3]               = 255; // opaque: premultiplied either way
            }
        }
        arrow = g_pHyprRenderer->createTexture(ARROW_W, ARROW_H, px.data());
    }

    void show(const PHLWINDOW& w, const Vector2D& pos) {
        stopTimer();
        if (!g.shown || g.window.lock() != w)
            g.since = Time::steadyNow();
        g.window = w;
        g.pos    = pos;
        g.shown  = true;
        g.pinned = true;
        g.stay   = false;
        damage();
    }

    void place(const PHLWINDOW& w, const Vector2D& pos) {
        if (Pointer::userIn(Pointer::clientOf(w))) {
            // the app's pointer is the user's: nothing rests there
            g.window = w;
            g.taken  = w;
            hide();
            return;
        }
        prepare(w);
        show(w, pos);
        rest(true);
    }

    void moveTo(const Vector2D& pos) {
        g.pos = pos;
        if (g.shown)
            damage();
    }

    void hide() {
        stopTimer();
        g.shown  = false;
        g.pinned = false;
        g.stay   = false;
        damage();
    }

    void userMoved() {
        if (g_pCompositor->m_isShuttingDown)
            return;
        if (const auto T = g.taken.lock(); T && !Pointer::userIn(Pointer::clientOf(T))) {
            // the user's pointer left the app: its pointer was last where
            // they left the window, so the next input there starts there
            if (Desktop::View::validMapped(T)) {
                const auto AT = g_pInputManager->getMouseCoordsInternal() - Pointer::windowPos(T);
                Pointer::rememberPos(T, Path::clampTo(AT, Pointer::windowSize(T)));
            }
            g.taken.reset();
        }
        const auto W = g.window.lock();
        if (W && g.taken.lock() != W && !Glide::active() && Pointer::userIn(Pointer::clientOf(W))) {
            // the user's pointer came into the app: one pointer, theirs (a
            // glide hands itself over on its next frame)
            g.taken = W;
            hide();
            return;
        }
        if (g.shown)
            damage();
    }

    static void fadeTick() {
        if (alpha() <= 0) {
            hide();
            return;
        }
        damage();
        g.timer->updateTimeout(std::chrono::milliseconds(FRAME_MS));
    }

    void rest(bool stay) {
        if (!g.shown)
            return;
        g.pinned = false;
        g.stay   = stay;
        if (stay) {
            stopTimer();
            damage();
            return;
        }
        g.fadeAt = Time::steadyNow() + std::chrono::milliseconds(LINGER_MS);
        stopTimer();
        g.timer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(LINGER_MS), [](SP<CEventLoopTimer>, void*) { fadeTick(); }, nullptr);
        g_pEventLoopManager->addTimer(g.timer);
    }

    void render(const Event::SRenderStageEvent& ev) {
        // the cheap tests first: this runs for every window of every frame
        if (ev.stage == RENDER_BEGIN) {
            g.frameDrawn = false;
            return;
        }
        if (!g.shown || g.frameDrawn || ev.stage != RENDER_POST_WINDOW || !ev.monitor || !ev.context)
            return;
        const auto& CTX = ev.context->get();
        if (CTX.m_renderingCapture || CTX.m_renderingSnapshot)
            return;
        const auto W = g.window.lock();
        // right after the window's own surfaces: the windows above it come
        // later in the pass and cover the cursor where they cover the app
        if (!W || CTX.m_data.currentWindow.lock() != W || !Desktop::View::validMapped(W))
            return;
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
            return;
        if (Pointer::userIn(Pointer::clientOf(W)))
            return; // the app's pointer is the user's: one cursor in it, theirs
        const float A   = alpha();
        const auto  IMG = imageFor(W, false);
        if (!IMG.missing.empty() && theme.loader) {
            theme.missing = IMG.missing; // the arrow this frame, the shape from the event loop
            theme.loader->updateTimeout(std::chrono::milliseconds(1));
        }
        if (A <= 0 || !IMG.tex)
            return;
        const auto MON = ev.monitor;
        const auto AT  = origin(W);
        CBox       at{AT + g.pos - IMG.hotspot - MON->m_position, IMG.size};
        CBox       clip{AT - MON->m_position, W->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)}; // over the window only
        at.scale(MON->m_scale);
        clip.scale(MON->m_scale);

        CTexPassElement::SRenderData data;
        data.tex     = IMG.tex;
        data.box     = at.round();
        data.a       = A;
        data.clipBox = clip.round();
        g_pHyprRenderer->addPassElement(ev.context->get(), makeUnique<CTexPassElement>(std::move(data)));
        g.frameDrawn = true;
        if (IMG.surf && !CTX.m_blockSurfaceFeedback)
            IMG.surf->frame(Time::steadyNow()); // an animated cursor keeps going
        if (IMG.themed && IMG.themed->cycle && g.anim) {
            // an animated shape: a frame again when this one's time is up
            const auto MS = std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - g.since).count() % IMG.themed->cycle;
            uint32_t   at = 0;
            for (const auto D : IMG.themed->delays)
                if ((at += D) > MS)
                    break;
            g.anim->updateTimeout(std::chrono::milliseconds(at - MS));
        }
    }

    std::string where() {
        const auto W = g.window.lock();
        if (!g.shown || !W || !Desktop::View::validMapped(W))
            return "nowhere";
        auto out = std::format("address:0x{:x} {:g} {:g}", rc<uintptr_t>(W.get()), g.pos.x, g.pos.y);
        if (!g.pinned && !g.stay)
            out += " (fading)";
        return out;
    }

    std::string describe(std::string_view selector) {
        const auto W      = Pointer::windowOf(selector);
        const auto CLIENT = Pointer::clientOf(W);
        if (!CLIENT)
            return "error: window not found";
        const auto  IMG   = imageFor(W, true);
        const auto  IT    = images.find(CLIENT);
        const auto  SHAPE = IT != images.end() && !IT->second.shape.empty() ? IT->second.shape : std::string{"default"};
        std::string what;
        if (IMG.surf)
            what = std::format("the app's image, {:g}x{:g}, hotspot {:g},{:g}", IMG.size.x, IMG.size.y, IMG.hotspot.x, IMG.hotspot.y);
        else if (!IMG.tex)
            what = "nothing: the app hid its cursor";
        else if (IMG.themed)
            what = std::format("the theme's '{}', {:g}x{:g}, hotspot {:g},{:g}", SHAPE, IMG.size.x, IMG.size.y, IMG.hotspot.x, IMG.hotspot.y);
        else
            what = std::format("the built-in arrow: no '{}' in the cursor theme", SHAPE);
        if (IT == images.end())
            what += "; the app has named none yet";
        return appImages ? what : what + " (no set_cursor hook: no app images)";
    }

    void reset() {
        hide();
        for (auto* t : {&theme.loader, &g.anim}) {
            if (*t && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(*t);
            t->reset();
        }
        g.taken.reset();
        g.frameDrawn = false;
        arrow.reset();
        images.clear();
        theme.shapes.clear();
        theme.key.clear();
        theme.missing.clear();
    }
}
