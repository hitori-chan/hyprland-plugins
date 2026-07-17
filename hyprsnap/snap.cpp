// awful.mouse.snap, both of its behaviors, source-exact:
//
// - client_enabled magnetism: every motion of a floating move-drag pulls
//   the window's edges flush to the screen box, then the workarea, then
//   the other visible windows' opposite edges, whenever they come within
//   snap_distance (awesome's default_distance = 8). The DragController
//   recomputes position from its begin-anchor + delta on every motion, so
//   a correction applied AFTER it (deferred out of the move emission)
//   never accumulates or fights — it just wins the frame. (The round-4
//   "external move mid-drag = jitter" lesson was about timers racing the
//   controller; per-motion post-adjustment is the fight-free way around.)
//
// - edge_enabled aerosnap: the CURSOR within `edge` px of a screen edge
//   arms that edge's half slot; within `edge` of TWO edges at once, that
//   corner's quarter — awesome has no wide corner band, a corner is
//   literally both edges at the same time (detect_screen_edges). The armed
//   slot is previewed as an outline (awesome shows a placeholder wibox
//   ring; ours is a frame drawn in the render pass — square, the theme has
//   rounding 0 everywhere) and the drop commits it. Slots are halves and
//   quarters of the WORKAREA (the old rc placement honored it too).
//
// A drag doesn't only end on button release: the keybind layer tears down
// an active drag on the NEXT input event it sees, whatever it is — any
// mouse button and any KEY event, press or release (ensureMouseBindState
// runs at the top of both onKeyEvent and onMouseEvent; releasing Super
// mid-drag ends the drag too, and awesome trains exactly that habit). So
// the commit hangs off both event streams; both are emitted to plugins
// before the keybind layer runs, while the drag state is still intact. A
// premature commit self-corrects: the next motion recomputes the position
// from the drag anchor anyway.

#include "hyprsnap.hpp"

#include <array>

namespace NHyprsnap::Snap {

    namespace {
        enum eEdgeV {
            V_NONE,
            V_TOP,
            V_BOTTOM
        };
        enum eEdgeH {
            H_NONE,
            H_LEFT,
            H_RIGHT
        };

        std::optional<CBox>       zoneBox; // the armed aerosnap slot, global logical
        PHLMONITORREF             zoneMon;
        UP<SEventLoopDoLaterLock> pendingMagnet;
        bool                      magnetQueued = false;

        // monitorState()->query().vec().run() allocates and RTTI-casts per
        // call; this runs per pointer motion, so mirror closestTo directly.
        PHLMONITOR                monitorAt(const Vector2D& pos) {
            PHLMONITOR best;
            float      bestDist = 0.F;
            for (const auto& M : State::monitorState()->monitors()) {
                const auto BOX = M->logicalBox();
                if (BOX.containsPoint(pos))
                    return M;
                const float DIST = vecToRectDistanceSquared(pos, BOX.pos(), BOX.pos() + BOX.size());
                if (!best || DIST < bestDist) {
                    best     = M;
                    bestDist = DIST;
                }
            }
            return best;
        }

        std::array<CBox, 4> zoneStrips(const CBox& Z) {
            constexpr double BW = 1;
            return {CBox{Z.x, Z.y, Z.w, BW}, CBox{Z.x, Z.y + Z.h - BW, Z.w, BW}, CBox{Z.x, Z.y + BW, BW, Z.h - 2 * BW}, CBox{Z.x + Z.w - BW, Z.y + BW, BW, Z.h - 2 * BW}};
        }

        // damage only the outline strips, not the whole monitor
        void damageZone() {
            if (!zoneBox)
                return;
            for (const auto& R : zoneStrips(*zoneBox))
                g_pHyprRenderer->damageBox(CBox{R}.expand(2));
        }

        // detect_screen_edges: the cursor against the WHOLE screen box
        std::pair<eEdgeV, eEdgeH> screenEdges(const CBox& MB, const Vector2D& pos, double d) {
            eEdgeV v = V_NONE;
            eEdgeH h = H_NONE;
            if (pos.x >= MB.x && pos.x - MB.x <= d)
                h = H_LEFT;
            else if (std::abs(MB.x + MB.w - pos.x) <= d)
                h = H_RIGHT;
            if (pos.y >= MB.y && pos.y - MB.y <= d)
                v = V_TOP;
            else if (std::abs(MB.y + MB.h - pos.y) <= d)
                v = V_BOTTOM;
            return {v, h};
        }

        // the slot an armed edge/corner stands for: halves and quarters of
        // the workarea
        std::optional<CBox> slotFor(eEdgeV v, eEdgeH h, const CBox& WA) {
            const double hw = WA.w / 2.0, hh = WA.h / 2.0;
            if (v != V_NONE && h != H_NONE)
                return CBox{h == H_LEFT ? WA.x : WA.x + hw, v == V_TOP ? WA.y : WA.y + hh, hw, hh};
            if (h != H_NONE)
                return CBox{h == H_LEFT ? WA.x : WA.x + hw, WA.y, hw, WA.h};
            if (v != V_NONE)
                return CBox{WA.x, v == V_TOP ? WA.y : WA.y + hh, WA.w, hh};
            return std::nullopt;
        }

        // snap_inside: pull g's edges flush with the INSIDE of sg
        CBox snapInside(CBox g, const CBox& sg, double d) {
            if (g.x > sg.x && g.x - sg.x < d)
                g.x = sg.x;
            else if (std::abs((sg.x + sg.w) - (g.x + g.w)) < d)
                g.x = sg.x + sg.w - g.w;
            if (g.y > sg.y && g.y - sg.y < d)
                g.y = sg.y;
            else if (std::abs((sg.y + sg.h) - (g.y + g.h)) < d)
                g.y = sg.y + sg.h - g.h;
            return g;
        }

        // snap_outside: pull g flush AGAINST sg's opposite edges
        CBox snapOutside(CBox g, const CBox& sg, double d) {
            if (g.x > sg.x + sg.w && g.x < sg.x + sg.w + d)
                g.x = sg.x + sg.w; // my left edge to their right
            else if (g.x + g.w < sg.x && g.x + g.w > sg.x - d)
                g.x = sg.x - g.w; // my right edge to their left
            if (g.y > sg.y + sg.h && g.y < sg.y + sg.h + d)
                g.y = sg.y + sg.h;
            else if (g.y + g.h < sg.y && g.y + g.h > sg.y - d)
                g.y = sg.y - g.h;
            return g;
        }
    }

    void reset() {
        if (zoneBox)
            damageZone();
        zoneBox.reset();
        zoneMon.reset();
        pendingMagnet.reset();
        magnetQueued = false;
    }

    void onMouseMove() {
        // emissions precede the compositor's lock handling: locked input
        // belongs to the lockscreen
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()) {
            reset();
            return;
        }

        const auto T = draggedFloatingTarget();
        if (!T) {
            if (zoneBox)
                reset(); // the drag is gone, the preview must not linger
            return;
        }

        const auto POS = g_pInputManager->getMouseCoordsInternal();
        const auto MON = monitorAt(POS);
        if (!MON)
            return;

        // -- aerosnap arming + preview --
        const auto [V, HZ] = screenEdges(MON->logicalBox(), POS, std::max((double)g_config.edge->value(), 1.0));
        const auto SLOT    = slotFor(V, HZ, MON->logicalBoxMinusReserved());
        const bool CHANGED =
            SLOT.has_value() != zoneBox.has_value() || (SLOT && zoneBox && (SLOT->x != zoneBox->x || SLOT->y != zoneBox->y || SLOT->w != zoneBox->w || SLOT->h != zoneBox->h));
        if (CHANGED) {
            damageZone(); // the outgoing outline's monitor
            zoneBox = SLOT;
            zoneMon = MON;
            damageZone(); // and the incoming one's
        }

        // -- client/screen magnetism, applied after the DragController has
        // positioned the window for THIS motion. The lambda reads live state,
        // so one queued run covers every motion of the dispatch --
        if (magnetQueued || g_config.snapDist->value() <= 0)
            return;
        magnetQueued  = true;
        pendingMagnet = g_pEventLoopManager->doLaterLock([]() {
            magnetQueued = false;
            const auto T = draggedFloatingTarget();
            if (!T)
                return;
            const double D = (double)g_config.snapDist->value();
            if (D <= 0)
                return;
            const auto POS = g_pInputManager->getMouseCoordsInternal();
            const auto MON = monitorAt(POS);
            if (!MON)
                return;

            // awesome's module.snap order: screen box, workarea, then every
            // other visible client (later pulls override earlier ones)
            const CBox CUR = T->position();
            CBox       g   = snapInside(CUR, MON->logicalBox(), D);
            g              = snapInside(g, MON->logicalBoxMinusReserved(), D);

            const auto WS = MON->m_activeWorkspace;
            for (const auto& O : Desktop::windowState()->windows()) {
                if (!O->m_isMapped || O->isHidden() || !O->m_isFloating || !O->m_target || O->m_target == T)
                    continue;
                if (O->m_workspace != WS && !(O->m_pinned && O->m_monitor.lock() == MON))
                    continue;
                if (Fullscreen::controller()->isFullscreen(O))
                    continue;
                g = snapOutside(g, O->m_target->position(), D);
            }

            if (g.x != CUR.x || g.y != CUR.y) {
                T->setPositionGlobal(CBox{g.x, g.y, CUR.w, CUR.h});
                T->warpPositionSize();
            }
        });
    }

    void onInputEndingDrag() {
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
            return;

        const auto T = draggedFloatingTarget();
        if (!T)
            return;
        if (zoneBox && zoneMon.lock()) {
            // dragEnd right after us commits the geometry we set here
            T->setPositionGlobal(*zoneBox);
            T->warpPositionSize();
        }
        reset();
    }

    void onRenderStage(eRenderStage stage) {
        if (stage != RENDER_POST_WINDOWS || !zoneBox)
            return;
        const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!MON || MON != zoneMon.lock())
            return;

        // CHyprColor's integer ctor computes OkLab — convert once per value,
        // not once per armed frame
        static uint64_t   colRaw = ~0ull;
        static CHyprColor col;
        if (const auto RAW = (uint64_t)g_config.colFrame->value(); RAW != colRaw) {
            colRaw = RAW;
            col    = CHyprColor{RAW};
        }

        const auto toPhys = [&](const CBox& b) { return CBox{b}.translate(-MON->m_position).scale(MON->m_scale).round(); };

        for (const auto& R : zoneStrips(*zoneBox))
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = toPhys(R), .color = col}));
    }
}
