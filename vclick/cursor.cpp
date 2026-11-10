// The cursor image at the virtual pointer. An app asks for its cursor image
// (wl_pointer.set_cursor) when its pointer enters, and again when it wants
// another; the compositor refuses it unless the app holds the real pointer
// focus, so the seat never shows a glide's image. vclick keeps each app's
// latest request (main.cpp hooks the seat manager's set_cursor) and draws
// that image itself at the virtual pointer: over the window only, never in
// a capture, while an input runs and LINGER_MS after, then fading. An app
// that asked for a named shape (cursor-shape-v1), hid its cursor or set none
// gets a built-in arrow.
#include "vclick.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <array>
#include <chrono>
#include <format>
#include <unordered_map>

namespace VClick::Cursor {
    constexpr int LINGER_MS = 1200;
    constexpr int FADE_MS   = 300;
    constexpr int FRAME_MS  = 16; // the fade's damage pace
    constexpr int ARROW_W = 12, ARROW_H = 19;

    bool appImages = false;

    struct SImage {
        WP<CWLSurfaceResource> surf;
        Vector2D               hotspot;
        std::string            shape; // a named shape, or "hidden"
    };
    static std::unordered_map<wl_client*, SImage> images;
    static SP<Render::ITexture>                   arrow;

    // what is drawn now
    static struct {
        PHLWINDOWREF        window;
        Vector2D            pos; // window coords
        bool                shown  = false;
        bool                pinned = false; // an input runs: no fade
        Time::steady_tp     fadeAt;         // whole until then, then fading
        CBox                drawn;          // the box last damaged, global logical px
        SP<CEventLoopTimer> timer;          // the fade
    } g;

    struct SDraw {
        SP<Render::ITexture>   tex;
        Vector2D               size, hotspot;
        SP<CWLSurfaceResource> surf; // the app's cursor surface, when that is the image
    };

    static SDraw imageFor(const PHLWINDOW& w) {
        if (const auto CLIENT = Pointer::clientOf(w)) {
            if (const auto IT = images.find(CLIENT); IT != images.end()) {
                const auto SURF = IT->second.surf.lock();
                if (SURF && SURF->m_current.texture && SURF->m_current.size.x > 0 && SURF->m_current.size.y > 0)
                    return {SURF->m_current.texture, SURF->m_current.size, IT->second.hotspot, SURF};
            }
        }
        return {arrow, {ARROW_W, ARROW_H}, {0, 0}, nullptr};
    }

    static std::optional<CBox> box() {
        const auto W = g.window.lock();
        if (!g.shown || !W)
            return std::nullopt;
        const auto IMG = imageFor(W);
        return CBox{Pointer::windowPos(W) + g.pos - IMG.hotspot, IMG.size};
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
        if (g.pinned)
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
        // no surface and no shape: the app hid its cursor
        images[client] = {surf, hotspot, surf || !shape.empty() ? std::move(shape) : std::string{"hidden"}};
        if (g.shown)
            damage();
    }

    void prepare() {
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
        g.window = w;
        g.pos    = pos;
        g.shown  = true;
        g.pinned = true;
        damage();
    }

    void moveTo(const Vector2D& pos) {
        g.pos = pos;
        if (g.shown)
            damage();
    }

    void hide() {
        stopTimer();
        g.shown = false;
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

    void linger() {
        if (!g.shown)
            return;
        g.pinned = false;
        g.fadeAt = Time::steadyNow() + std::chrono::milliseconds(LINGER_MS);
        stopTimer();
        g.timer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(LINGER_MS), [](SP<CEventLoopTimer>, void*) { fadeTick(); }, nullptr);
        g_pEventLoopManager->addTimer(g.timer);
    }

    void render(const Event::SRenderStageEvent& ev) {
        // the cheap test first: this runs on every frame of every monitor
        if (!g.shown || ev.stage != RENDER_POST_WINDOWS || !ev.monitor || !ev.context || ev.context->get().m_renderingCapture)
            return;
        const auto W = g.window.lock();
        if (!W || !Desktop::View::validMapped(W) || !g_pHyprRenderer->shouldRenderWindow(W, ev.monitor))
            return;
        const float A   = alpha();
        const auto  IMG = imageFor(W);
        if (A <= 0 || !IMG.tex)
            return;
        const auto MON = ev.monitor;
        CBox       at{Pointer::windowPos(W) + g.pos - IMG.hotspot - MON->m_position, IMG.size};
        CBox       clip{Pointer::windowPos(W) - MON->m_position, Pointer::windowSize(W)}; // over the window only
        at.scale(MON->m_scale);
        clip.scale(MON->m_scale);

        CTexPassElement::SRenderData data;
        data.tex     = IMG.tex;
        data.box     = at.round();
        data.a       = A;
        data.clipBox = clip.round();
        g_pHyprRenderer->addPassElement(ev.context->get(), makeUnique<CTexPassElement>(std::move(data)));
        if (IMG.surf && !ev.context->get().m_blockSurfaceFeedback)
            IMG.surf->frame(Time::steadyNow()); // an animated cursor keeps going
    }

    std::string describe(std::string_view selector) {
        const auto CLIENT = Pointer::clientOf(Pointer::windowOf(selector));
        if (!CLIENT)
            return "error: window not found";
        std::string what = "the built-in arrow: the app has set no cursor image yet";
        if (const auto IT = images.find(CLIENT); IT != images.end()) {
            const auto SURF = IT->second.surf.lock();
            if (SURF && SURF->m_current.texture)
                what = std::format("the app's image, {:g}x{:g}, hotspot {:g},{:g}", SURF->m_current.size.x, SURF->m_current.size.y, IT->second.hotspot.x,
                                   IT->second.hotspot.y);
            else if (!IT->second.shape.empty())
                what = std::format("the built-in arrow: the app asked for '{}'", IT->second.shape);
            else
                what = "the built-in arrow: the app's image has no buffer";
        }
        return appImages ? what : what + " (no set_cursor hook: the arrow only)";
    }

    void reset() {
        hide();
        arrow.reset();
        images.clear();
    }
}
