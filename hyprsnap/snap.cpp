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

#include "common/lifecycle.hpp"

#include "hyprsnap.hpp"

#include <hyprland/src/config/ConfigValue.hpp>

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
        NHyprCommon::CHop         pendingMagnet;
        bool                      magnetQueued = false;
        std::optional<CBox>       resizeStart; // resize-drag begin box: tells dragged edges from anchored

        // monitorState()->query().vec().run() allocates and RTTI-casts per
        // call; this runs per pointer motion, so mirror closestTo directly.
        PHLMONITOR monitorAt(const Vector2D& pos) {
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

        void damageZone() {
            if (!zoneBox)
                return;
            g_pHyprRenderer->damageBox(CBox{*zoneBox}.expand(2)); // fill + outline
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

    // The render listener exists only while a zone is armed: render.stage
    // fires per window per frame, and zones are live a few seconds per drag.
    static Hyprutils::Signal::CHyprSignalListener lRender;

    void                                          reset() {
        if (zoneBox)
            damageZone();
        zoneBox.reset();
        zoneMon.reset();
        lRender.reset();
        pendingMagnet.reset();
        magnetQueued = false;
        resizeStart.reset();
    }

    // one deferred run per dispatch, applied after the DragController has
    // positioned the window for this motion; the lambda reads live state
    static void queueMagnet() {
        if (magnetQueued || g_config.snapDist->value() <= 0)
            return;
        magnetQueued = true;
        pendingMagnet.arm([]() {
            magnetQueued = false;
            // the lock can engage between the motion emission and this run
            // (idle timeout mid-drag): never move windows under the lockscreen
            if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
                return;
            const double D = (double)g_config.snapDist->value();
            if (D <= 0)
                return;
            const auto POS = g_pInputManager->getMouseCoordsInternal();
            const auto MON = monitorAt(POS);
            if (!MON)
                return;

            // X11 geometry was border-inclusive: snap the BORDER flush, never
            // swallow it offscreen — inflate, snap, deflate.
            static auto  PBORDER = CConfigValue<Config::INTEGER>("general:border_size");
            const double B       = std::max((double)*PBORDER, 0.0);
            const auto   WS      = MON->m_activeWorkspace;

            const auto   othersOf = [&](SP<Layout::ITarget> self, auto&& per) {
                for (const auto& O : Desktop::windowState()->windows()) {
                    if (!O->m_isMapped || O->isHidden() || !O->m_isFloating || !O->m_target || O->m_target == self)
                        continue;
                    if (O->m_workspace != WS && !(O->m_pinned && O->m_monitor.lock() == MON))
                        continue;
                    if (Fullscreen::controller()->isFullscreen(O))
                        continue;
                    const auto OB = O->m_target->position();
                    per(CBox{OB.x - B, OB.y - B, OB.w + 2 * B, OB.h + 2 * B});
                }
            };

            if (const auto T = draggedFloatingTarget()) {
                // move: the whole box pulls — screen, workarea, then every
                // other visible client (later pulls override earlier ones)
                const CBox CUR = T->position();
                CBox       g   = CBox{CUR.x - B, CUR.y - B, CUR.w + 2 * B, CUR.h + 2 * B};
                g              = snapInside(g, MON->logicalBox(), D);
                g              = snapInside(g, MON->logicalBoxMinusReserved(), D);
                othersOf(T, [&](const CBox& o) { g = snapOutside(g, o, D); });

                g.x += B;
                g.y += B;
                if (g.x != CUR.x || g.y != CUR.y) {
                    T->setPositionGlobal(CBox{g.x, g.y, CUR.w, CUR.h});
                    T->warpPositionSize();
                }
                return;
            }

            const auto T = resizingFloatingTarget();
            if (!T || !resizeStart)
                return;

            // resize: only the dragged edges pull, anchored edges hold
            const CBox CUR = T->position();
            const bool EL = CUR.x != resizeStart->x, ER = CUR.x + CUR.w != resizeStart->x + resizeStart->w;
            const bool ET = CUR.y != resizeStart->y, EB = CUR.y + CUR.h != resizeStart->y + resizeStart->h;
            if (!(EL || ER || ET || EB))
                return;

            CBox       g          = CBox{CUR.x - B, CUR.y - B, CUR.w + 2 * B, CUR.h + 2 * B};
            const auto pullInside = [&](const CBox& sg) {
                if (EL && std::abs(g.x - sg.x) < D) {
                    g.w += g.x - sg.x;
                    g.x = sg.x;
                }
                if (ER && std::abs((sg.x + sg.w) - (g.x + g.w)) < D)
                    g.w = sg.x + sg.w - g.x;
                if (ET && std::abs(g.y - sg.y) < D) {
                    g.h += g.y - sg.y;
                    g.y = sg.y;
                }
                if (EB && std::abs((sg.y + sg.h) - (g.y + g.h)) < D)
                    g.h = sg.y + sg.h - g.y;
            };
            pullInside(MON->logicalBox());
            pullInside(MON->logicalBoxMinusReserved());
            othersOf(T, [&](const CBox& o) {
                // abut the neighbor's opposite edge, like snap_outside
                if (EL && std::abs(g.x - (o.x + o.w)) < D) {
                    g.w += g.x - (o.x + o.w);
                    g.x = o.x + o.w;
                }
                if (ER && std::abs(o.x - (g.x + g.w)) < D)
                    g.w = o.x - g.x;
                if (ET && std::abs(g.y - (o.y + o.h)) < D) {
                    g.h += g.y - (o.y + o.h);
                    g.y = o.y + o.h;
                }
                if (EB && std::abs(o.y - (g.y + g.h)) < D)
                    g.h = o.y - g.y;
            });

            const CBox RES{g.x + B, g.y + B, g.w - 2 * B, g.h - 2 * B};
            if (RES.x != CUR.x || RES.y != CUR.y || RES.w != CUR.w || RES.h != CUR.h) {
                T->setPositionGlobal(RES);
                T->warpPositionSize();
            }
        });
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

            // resize drags magnetize too: the dragged edges alone
            if (const auto RT = resizingFloatingTarget()) {
                if (!resizeStart)
                    resizeStart = RT->position(); // emission precedes this motion's resize
                queueMagnet();
            } else
                resizeStart.reset();
            return;
        }
        resizeStart.reset();

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
            const bool HAD = zoneBox.has_value();
            damageZone(); // the outgoing outline's monitor
            zoneBox = SLOT;
            zoneMon = MON;
            damageZone(); // and the incoming one's
            if (!HAD && zoneBox)
                lRender = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); });
            else if (HAD && !zoneBox)
                lRender.reset();
        }

        queueMagnet();
    }

    void onInputEndingDrag() {
        // guard AND reset: armed zones must not survive into the locked
        // session (they'd paint under session_lock_xray until a motion)
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()) {
            reset();
            return;
        }

        const auto T = draggedFloatingTarget();
        if (!T) {
            // the ended drag may have been a resize: without this a
            // motionless click-click re-grab seeds the next resize with the
            // previous drag's begin box (wrong dragged-edge classification)
            resizeStart.reset();
            return;
        }
        if (zoneBox && zoneMon.lock()) {
            // the slot is the border box; the surface sits inside it, so the
            // border stays on screen (maximize is the one full-bleed state)
            static auto  PBORDER = CConfigValue<Config::INTEGER>("general:border_size");
            const double B       = std::max((double)*PBORDER, 0.0);
            // dragEnd right after us commits the geometry we set here
            T->setPositionGlobal(CBox{zoneBox->x + B, zoneBox->y + B, zoneBox->w - 2 * B, zoneBox->h - 2 * B});
            T->warpPositionSize();
        }
        reset();
    }

    void onRenderStage(eRenderStage stage) {
        if (stage != RENDER_POST_WINDOWS || !zoneBox)
            return;
        // the input listeners reset on lock, but a lock engaging mid-drag is
        // not an input event — don't paint the zone over the lockscreen; the
        // first post-unlock motion re-arms it
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
            return;
        const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!MON || MON != zoneMon.lock())
            return;

        // CHyprColor's integer ctor computes OkLab — convert once per value,
        // not once per armed frame
        static uint64_t   colRaw = ~0ull;
        static CHyprColor col, fill;
        if (const auto RAW = (uint64_t)g_config.colFrame->value(); RAW != colRaw) {
            colRaw = RAW;
            col    = CHyprColor{RAW};
            fill   = CHyprColor(col.r, col.g, col.b, 0.10); // a 1px ring alone gets lost over busy content
        }

        const auto toPhys = [&](const CBox& b) { return CBox{b}.translate(-MON->m_position).scale(MON->m_scale).round(); };

        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = toPhys(*zoneBox), .color = fill}));
        for (const auto& R : zoneStrips(*zoneBox))
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = toPhys(R), .color = col}));
    }
}
