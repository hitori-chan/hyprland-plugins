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
#include "common/queries.hpp"

#include "hyprsnap.hpp"
#include "geometry.hpp"

#include <hyprland/src/config/ConfigValue.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <tuple>
#include <vector>

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

        std::optional<CBox> zoneBox; // constrained aerosnap border box, global logical
        PHLMONITORREF       zoneMon;
        eEdgeV              zoneV = V_NONE;
        eEdgeH              zoneH = H_NONE;
        NHyprCommon::CHop   pendingMagnet;
        bool                magnetQueued = false;
        std::optional<CBox> resizeStart; // resize-drag begin box: tells dragged edges from anchored

        using NHyprCommon::monitorAt;         // nearest output for magnetism
        using NHyprCommon::monitorContaining; // edge zones need actual containment

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

        std::optional<CBox> constrainedZone(const SP<Layout::ITarget>& target, const PHLMONITOR& monitor, eEdgeV v, eEdgeH h) {
            if (!target || !monitor)
                return std::nullopt;
            const auto WA   = monitor->logicalBoxMinusReserved();
            const auto SLOT = slotFor(v, h, WA);
            if (!SLOT)
                return std::nullopt;

            static auto PBORDER = CConfigValue<Config::INTEGER>("general:border_size");
            const auto  HA = h == H_LEFT ? Geometry::EHorizontalAnchor::LEFT :
                h == H_RIGHT ? Geometry::EHorizontalAnchor::RIGHT : Geometry::EHorizontalAnchor::CENTER;
            const auto VA = v == V_TOP ? Geometry::EVerticalAnchor::TOP : v == V_BOTTOM ? Geometry::EVerticalAnchor::BOTTOM : Geometry::EVerticalAnchor::CENTER;
            return Geometry::constrainedSlot(*SLOT, target->minSize(), target->maxSize(), std::max((double)*PBORDER, 0.0), HA, VA);
        }

        struct SNearest {
            bool   found = false;
            double distance = 0;
            double value = 0;
            CBox   key;
        };

        bool earlierGeometry(const CBox& a, const CBox& b) {
            return std::tie(a.x, a.y, a.w, a.h) < std::tie(b.x, b.y, b.w, b.h);
        }

        void considerNearest(SNearest& best, double value, double current, const CBox& key, double d) {
            const double DIST = std::abs(value - current);
            if (DIST >= d || (best.found && (DIST > best.distance || (DIST == best.distance && !earlierGeometry(key, best.key)))))
                return;
            best = {.found = true, .distance = DIST, .value = value, .key = key};
        }

        CBox snapInsideNearest(CBox g, const CBox& sg, double d) {
            SNearest X, Y;
            if (g.x > sg.x)
                considerNearest(X, sg.x, g.x, sg, d);
            if (std::abs((sg.x + sg.w) - (g.x + g.w)) < d)
                considerNearest(X, sg.x + sg.w - g.w, g.x, sg, d);
            if (g.y > sg.y)
                considerNearest(Y, sg.y, g.y, sg, d);
            if (std::abs((sg.y + sg.h) - (g.y + g.h)) < d)
                considerNearest(Y, sg.y + sg.h - g.h, g.y, sg, d);
            if (X.found)
                g.x = X.value;
            if (Y.found)
                g.y = Y.value;
            return g;
        }

        CBox snapOutsideNearest(CBox g, const std::vector<CBox>& others, double d) {
            SNearest X, Y;
            for (const auto& O : others) {
                if (g.x > O.x + O.w && g.x < O.x + O.w + d)
                    considerNearest(X, O.x + O.w, g.x, O, d);
                else if (g.x + g.w < O.x && g.x + g.w > O.x - d)
                    considerNearest(X, O.x - g.w, g.x, O, d);
                if (g.y > O.y + O.h && g.y < O.y + O.h + d)
                    considerNearest(Y, O.y + O.h, g.y, O, d);
                else if (g.y + g.h < O.y && g.y + g.h > O.y - d)
                    considerNearest(Y, O.y - g.h, g.y, O, d);
            }
            if (X.found)
                g.x = X.value;
            if (Y.found)
                g.y = Y.value;
            return g;
        }

        CBox snapResizeOutside(CBox g, const std::vector<CBox>& others, double d, bool left, bool right, bool top, bool bottom) {
            SNearest L, R, T, B;
            for (const auto& O : others) {
                // A resize edge only snaps when it is approaching the
                // neighbour from the outside. Without the direction check a
                // window already overlapping O could be pulled through it,
                // and right/bottom candidates compared the candidate's left
                // or top against the current opposite edge.
                if (left && g.x > O.x + O.w && g.x < O.x + O.w + d)
                    considerNearest(L, O.x + O.w, g.x, O, d);
                if (right && g.x + g.w < O.x && g.x + g.w > O.x - d)
                    considerNearest(R, O.x - g.w, g.x, O, d);
                if (top && g.y > O.y + O.h && g.y < O.y + O.h + d)
                    considerNearest(T, O.y + O.h, g.y, O, d);
                if (bottom && g.y + g.h < O.y && g.y + g.h > O.y - d)
                    considerNearest(B, O.y - g.h, g.y, O, d);
            }
            if (L.found) {
                g.w += g.x - L.value;
                g.x = L.value;
            }
            if (R.found)
                g.w = R.value - g.x;
            if (T.found) {
                g.h += g.y - T.value;
                g.y = T.value;
            }
            if (B.found)
                g.h = B.value - g.y;
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
        zoneV = V_NONE;
        zoneH = H_NONE;
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
            if (NHyprCommon::sessionLocked())
                return;
            const double D = (double)g_config.snapDist->value();
            if (D <= 0)
                return;
            const auto CURSOR = g_pInputManager->getMouseCoordsInternal();
            const auto MON = monitorAt(CURSOR);
            if (!MON)
                return;

            // X11 geometry was border-inclusive: snap the BORDER flush, never
            // swallow it offscreen — inflate, snap, deflate.
            static auto  PBORDER = CConfigValue<Config::INTEGER>("general:border_size");
            const double B       = std::max((double)*PBORDER, 0.0);
            const auto   WS      = MON->m_activeWorkspace;

            const auto   othersOf = [&](SP<Layout::ITarget> self) {
                std::vector<CBox> OUT;
                for (const auto& O : Desktop::windowState()->windows()) {
                    if (!O->m_isMapped || O->isHidden() || !O->m_isFloating || !O->m_target || O->m_target == self)
                        continue;
                    if (O->m_workspace != WS && !(O->m_pinned && O->m_monitor.lock() == MON))
                        continue;
                    if (Fullscreen::controller()->isFullscreen(O))
                        continue;
                    const auto OB = O->m_target->position();
                    OUT.push_back(CBox{OB.x - B, OB.y - B, OB.w + 2 * B, OB.h + 2 * B});
                }
                return OUT;
            };

            if (const auto T = draggedFloatingTarget()) {
                // move: the whole box pulls — screen, workarea, then every
                // other visible client (later pulls override earlier ones)
                const CBox CUR = T->position();
                CBox       g   = CBox{CUR.x - B, CUR.y - B, CUR.w + 2 * B, CUR.h + 2 * B};
                g              = snapInsideNearest(g, MON->logicalBox(), D);
                g              = snapInsideNearest(g, MON->logicalBoxMinusReserved(), D);
                g              = snapOutsideNearest(g, othersOf(T), D);

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
            g = snapResizeOutside(g, othersOf(T), D, EL, ER, ET, EB);

            const CBox RES{g.x + B, g.y + B, g.w - 2 * B, g.h - 2 * B};
            const auto  MINRAW = T->minSize().value_or(Vector2D{MIN_WINDOW_SIZE, MIN_WINDOW_SIZE});
            const auto  MAXRAW = T->maxSize().value_or(Math::VECTOR2D_MAX);
            const double MINX  = std::max(1.0, std::isfinite(MINRAW.x) ? MINRAW.x : 1.0);
            const double MINY  = std::max(1.0, std::isfinite(MINRAW.y) ? MINRAW.y : 1.0);
            const double MAXX  = std::max(MINX, std::isfinite(MAXRAW.x) ? MAXRAW.x : std::numeric_limits<double>::max());
            const double MAXY  = std::max(MINY, std::isfinite(MAXRAW.y) ? MAXRAW.y : std::numeric_limits<double>::max());
            const Vector2D SIZE{std::clamp(RES.w, MINX, MAXX), std::clamp(RES.h, MINY, MAXY)};
            Vector2D       NEWPOS{RES.x, RES.y};
            if (EL && !ER)
                NEWPOS.x += RES.w - SIZE.x;
            if (ET && !EB)
                NEWPOS.y += RES.h - SIZE.y;
            const CBox FINAL{NEWPOS, SIZE};
            if (FINAL.x != CUR.x || FINAL.y != CUR.y || FINAL.w != CUR.w || FINAL.h != CUR.h) {
                T->setPositionGlobal(FINAL);
                T->warpPositionSize();
            }
        });
    }

    void onMouseMove() {
        // emissions precede the compositor's lock handling: locked input
        // belongs to the lockscreen
        if (NHyprCommon::sessionLocked()) {
            reset();
            return;
        }
        if (NHyprCommon::nativeInputCaptureActive()) {
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
        const auto MON = monitorContaining(POS);
        if (!MON) {
            if (zoneBox)
                reset();
            queueMagnet();
            return;
        }

        // -- aerosnap arming + preview --
        const auto [V, HZ] = screenEdges(MON->logicalBox(), POS, std::max((double)g_config.edge->value(), 1.0));
        const auto SLOT    = constrainedZone(T, MON, V, HZ);
        const bool CHANGED = V != zoneV || HZ != zoneH || SLOT.has_value() != zoneBox.has_value() ||
            (SLOT && zoneBox && (SLOT->x != zoneBox->x || SLOT->y != zoneBox->y || SLOT->w != zoneBox->w || SLOT->h != zoneBox->h));
        if (CHANGED) {
            const bool HAD = zoneBox.has_value();
            damageZone(); // the outgoing outline's monitor
            zoneBox = SLOT;
            zoneMon = MON;
            zoneV   = V;
            zoneH   = HZ;
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
        if (NHyprCommon::sessionLocked()) {
            reset();
            return;
        }
        if (NHyprCommon::nativeInputCaptureActive()) {
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
        if (zoneBox) {
            // the slot is the border box; the surface sits inside it, so the
            // border stays on screen (maximize is the one full-bleed state)
            static auto  PBORDER = CConfigValue<Config::INTEGER>("general:border_size");
            const double B       = std::max((double)*PBORDER, 0.0);
            // Re-read workarea and client constraints at drop: either can
            // change after the last pointer motion.
            if (const auto MON = zoneMon.lock(); MON) {
                if (const auto FINAL = constrainedZone(T, MON, zoneV, zoneH); FINAL) {
                    // dragEnd right after us commits the geometry we set here
                    T->setPositionGlobal(CBox{FINAL->x + B, FINAL->y + B, FINAL->w - 2 * B, FINAL->h - 2 * B});
                    T->warpPositionSize();
                }
            }
        }
        reset();
    }

    void onRenderStage(eRenderStage stage) {
        if (stage != RENDER_POST_WINDOWS || !zoneBox)
            return;
        // the input listeners reset on lock, but a lock engaging mid-drag is
        // not an input event — don't paint the zone over the lockscreen; the
        // first post-unlock motion re-arms it
        if (NHyprCommon::sessionLocked())
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

        g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = toPhys(*zoneBox), .color = fill}));
        for (const auto& R : zoneStrips(*zoneBox))
            g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = toPhys(R), .color = col}));
    }
}
