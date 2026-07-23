// hyprbar/bell.cpp — the notification bell + badge, and its bus link to
// hyprnotify (org.hitori.hyprnotify on the Notifications object — the
// sanctioned cross-plugin channel is the bus, never symbols).
//
// The badge counts live + kept and hides at zero; a click calls Toggle
// (the center). DND has NO bar presence — that state lives in the center's
// ⊖ only. The glyph is Material's filled notifications bell (Apache-2.0),
// transcribed and drawn natively.

#include "common/theme.hpp"

#include "hyprbar.hpp"

namespace NHyprbar {

    // ---- the state hyprnotify pushes ----

    static uint32_t                       bellLive = 0, bellKept = 0;
    static std::unique_ptr<sdbus::IProxy> proxy; // on the tray's session-bus link

    static void                           applyState(uint32_t live, uint32_t kept) {
        if (live == bellLive && kept == bellKept)
            return;
        bellLive = live;
        bellKept = kept;
        barChanged();
    }

    namespace Bell {
        static constexpr const char* CIFACE = "org.hitori.hyprnotify";

        void                         daemonUp() {
            // (re)read the counts — the daemon (re)appeared after us; the
            // signal match survives name-owner churn (the broker resolves
            // well-known-name senders), only the snapshot needs a refresh
            if (!proxy)
                return;
            try {
                proxy->callMethodAsync("State").onInterface(CIFACE).uponReplyInvoke([](std::optional<sdbus::Error> e, uint32_t live, uint32_t kept, bool, bool) {
                    if (!e)
                        applyState(live, kept);
                });
                Tray::pollSoon();
            } catch (...) {} // no daemon yet: the badge stays hidden
        }

        void init() {
            if (!Tray::bus.conn())
                return; // no session bus: no bell state, glyph still draws
            try {
                proxy = sdbus::createProxy(*Tray::bus.conn(), sdbus::ServiceName{"org.freedesktop.Notifications"}, sdbus::ObjectPath{"/org/freedesktop/Notifications"});
                proxy->uponSignal("State").onInterface(CIFACE).call([](uint32_t live, uint32_t kept, bool, bool) { applyState(live, kept); });
                daemonUp();
            } catch (...) {
                proxy.reset();
            }
        }

        void exit() {
            proxy.reset(); // before the tray's connection dies (tray.cpp calls this from dropOwned)
            bellLive = bellKept = 0;
        }
    } // namespace Bell

    // ---- the glyph: Material "notifications" filled, 24dp ----

    // just enough SVG for the path below: absolute/relative M L H V C S Z
    // with implicit command repetition (battery.cpp's parser is absolute-only)
    static void playPathRel(cairo_t* CR, const char* d) {
        const char* p   = d;
        char        cmd = 0;
        double      x = 0, y = 0, sx = 0, sy = 0, cx = 0, cy = 0; // current, subpath start, last cubic control
        bool        hadCtrl = false;
        const auto  num     = [&]() {
            while (*p == ',' || *p == ' ')
                p++;
            char*  e;
            double v = std::strtod(p, &e);
            p        = e;
            return v;
        };
        const auto more = [&]() { // another coordinate follows: the command repeats
            const char* q = p;
            while (*q == ',' || *q == ' ')
                q++;
            return *q == '-' || *q == '.' || std::isdigit((unsigned char)*q);
        };
        while (*p) {
            if (*p == ',' || *p == ' ') {
                p++;
                continue;
            }
            if (std::isalpha((unsigned char)*p))
                cmd = *p++;
            const bool REL = std::islower((unsigned char)cmd);
            switch (std::toupper((unsigned char)cmd)) {
                case 'M': {
                    const double NX = num() + (REL ? x : 0), NY = num() + (REL ? y : 0);
                    cairo_move_to(CR, NX, NY);
                    x = sx = NX;
                    y = sy = NY;
                    cmd     = REL ? 'l' : 'L'; // further pairs are linetos
                    hadCtrl = false;
                    break;
                }
                case 'L': {
                    x = num() + (REL ? x : 0);
                    y = num() + (REL ? y : 0);
                    cairo_line_to(CR, x, y);
                    hadCtrl = false;
                    break;
                }
                case 'H':
                    x = num() + (REL ? x : 0);
                    cairo_line_to(CR, x, y);
                    hadCtrl = false;
                    break;
                case 'V':
                    y = num() + (REL ? y : 0);
                    cairo_line_to(CR, x, y);
                    hadCtrl = false;
                    break;
                case 'C': {
                    const double C1X = num() + (REL ? x : 0), C1Y = num() + (REL ? y : 0);
                    const double C2X = num() + (REL ? x : 0), C2Y = num() + (REL ? y : 0);
                    const double NX = num() + (REL ? x : 0), NY = num() + (REL ? y : 0);
                    cairo_curve_to(CR, C1X, C1Y, C2X, C2Y, NX, NY);
                    cx      = C2X;
                    cy      = C2Y;
                    x       = NX;
                    y       = NY;
                    hadCtrl = true;
                    break;
                }
                case 'S': { // smooth cubic: first control reflects the last one
                    const double C1X = hadCtrl ? 2 * x - cx : x, C1Y = hadCtrl ? 2 * y - cy : y;
                    const double C2X = num() + (REL ? x : 0), C2Y = num() + (REL ? y : 0);
                    const double NX = num() + (REL ? x : 0), NY = num() + (REL ? y : 0);
                    cairo_curve_to(CR, C1X, C1Y, C2X, C2Y, NX, NY);
                    cx      = C2X;
                    cy      = C2Y;
                    x       = NX;
                    y       = NY;
                    hadCtrl = true;
                    break;
                }
                case 'Z':
                    cairo_close_path(CR);
                    x       = sx;
                    y       = sy;
                    hadCtrl = false;
                    break;
                default: return; // unknown command: stop rather than misdraw
            }
            if (!std::isalpha((unsigned char)*p) && !more() && *p)
                p++; // safety against a stuck parse on stray bytes
        }
    }

    // Material Symbols "notifications" filled (24dp viewport, Apache-2.0)
    static constexpr const char* BELL_PATH = "M12 22c1.1 0 2-.9 2-2h-4c0 1.1.9 2 2 2zm6-6v-5c0-3.07-1.63-5.63-4.5-6.32V4c0-.83-.67-1.5-1.5-1.5s-1.5.67-1.5 "
                                             "1.5v.68C7.63 5.36 6 7.92 6 11v5l-2 2v1h16v-1l-2-2z";

    struct SBellTex {
        SP<ITexture> tex;
        uint64_t     key = 0;
    };
    static std::unordered_map<int, SBellTex> bellCache; // per physical height

    static SP<ITexture>                      bellGlyph(double hPx, const CHyprColor& ink) {
        const int PX = std::max(8, (int)std::lround(hPx));
        auto*     SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, PX, PX);
        auto*     CR   = cairo_create(SURF);
        cairo_scale(CR, PX / 24.0, PX / 24.0);
        playPathRel(CR, BELL_PATH);
        cairo_set_source_rgba(CR, ink.r, ink.g, ink.b, ink.a);
        cairo_fill(CR);
        cairo_surface_flush(SURF);
        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_destroy(CR);
        cairo_surface_destroy(SURF);
        return tex;
    }

    // ---- the widget ----

    namespace {
        class CBellWidget : public IWidget {
          public:
            double fit(const SPaint& P, const SFrame&) override {
                return 28; // the spec's hit target; the glyph is 16px inside
            }

            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                const double GLYPH = 16;
                const int    PX    = (int)std::lround(GLYPH * P.scale);
                auto&        CACHE = bellCache[PX];
                const auto   KEY   = F.fg.getAsHex();
                if (CACHE.key != KEY || !CACHE.tex) {
                    if (warmGate.mayBuild()) {
                        CACHE.tex = bellGlyph(PX, F.fg);
                        CACHE.key = KEY;
                    }
                }

                const bool HOV = barHover.widget == this;
                if (HOV) {
                    if (stripMode()) // full-height wash, like every strip cell
                        P.rect(CBox{box.x - 2, box.y, box.w + 4, box.h}, tFill2());
                    else
                        P.rect(CBox{box.x - 2, box.y + (box.h - 24) / 2, box.w + 4, 24}, tFill2(), (int)std::lround(8 * P.scale));
                }

                if (CACHE.tex && CACHE.tex->m_texID != 0) {
                    const auto B = P.toPhys(box);
                    CBox       b{B.x + (B.w - CACHE.tex->m_size.x) / 2.0, B.y + (B.h - CACHE.tex->m_size.y) / 2.0, CACHE.tex->m_size.x, CACHE.tex->m_size.y};
                    P.tex(CACHE.tex, b.round());
                }

                // the badge: live + kept, hidden at zero; min-w 15, 9/700,
                // offset -3/-4 off the glyph's top-right
                const uint32_t COUNT = bellLive + bellKept;
                if (P.fp)
                    *P.fp = *P.fp * 1099511628211ULL + COUNT; // the count can move without a bar event
                if (COUNT > 0) {
                    const auto   TXT  = COUNT > 99 ? std::string{"99+"} : std::to_string(COUNT);
                    const auto   BPT  = std::max(6, (int)std::round(9.0 * P.scale));
                    const auto   TT   = textTex(TXT, tOnAccent(), BPT, 0, "", 700);
                    const double TW   = TT ? TT->m_size.x / P.scale : 6;
                    const double BW   = std::max(15.0, TW + 6);
                    const CBox   BB{box.x + box.w / 2 + GLYPH / 2 - 3, box.y + (box.h - 24) / 2 - 4 + 3, BW, 15};
                    P.rect(BB, color(cfg.colActive), (int)std::lround(7.5 * P.scale));
                    P.texIn(TT, BB);
                }

                SHit h;
                h.box    = CBox{box.x - 2, box.y, box.w + 4, box.h};
                h.widget = this;
                P.hits->push_back(h);
            }

            void onHit(const SHit&, uint32_t bit, bool) override {
                if (bit != 1u || !proxy)
                    return;
                try {
                    proxy->callMethodAsync("Toggle").onInterface(Bell::CIFACE).uponReplyInvoke([](std::optional<sdbus::Error>) {});
                    Tray::pollSoon();
                } catch (...) {} // no daemon: the click is a no-op
            }
        };
    } // namespace

    IWidget& bellWidget() {
        static CBellWidget W;
        return W;
    }

} // namespace NHyprbar
