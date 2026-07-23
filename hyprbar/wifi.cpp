// hyprbar/wifi.cpp — the Android segmented wifi wedge: a bottom dot + two
// stroked arcs with real gaps; partial strength dims segments to 25% instead
// of removing them (the silhouette never changes width); off adds the slash.
// An indicator only — network UI stays a tray app's job.
//
// Presence and link state come from /sys/class/net (a `wireless` dir marks
// the interface, operstate its link). Strength: /proc/net/wireless is
// header-only on some drivers (this laptop's iwlwifi build included), so the
// working source is nl80211 — read through a spawned `iw dev <if> link`
// with its stdout on an event-loop pipe (the hyprosd readback pattern): one
// short fork per minute tick, render/input never wait on it. The /proc
// quality column still serves as the instant estimate where it exists.

#include "common/theme.hpp"

#include "hyprbar.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace NHyprbar {

    static bool        wifiPresent = false;
    static bool        wifiOff     = true; // no interface up
    static int         wifiLevel   = 0;    // 1..3 while connected
    static std::string wifiIface;

    // ---- the async signal probe (iw, nl80211's CLI) ----

    static pid_t             probePid = -1;
    static int               probeFd  = -1;
    static wl_event_source*  probeSrc = nullptr;
    static std::string       probeOut;
    static std::vector<pid_t> probeOrphans; // still owed a waitpid after EOF raced the exit

    static void               probeDone() {
        if (probeSrc)
            wl_event_source_remove(probeSrc);
        if (probeFd >= 0)
            close(probeFd);
        probeSrc = nullptr;
        probeFd  = -1;
        if (probePid > 0 && waitpid(probePid, nullptr, WNOHANG) == 0)
            probeOrphans.push_back(probePid);
        probePid = -1;
        std::erase_if(probeOrphans, [](pid_t p) { return waitpid(p, nullptr, WNOHANG) != 0; });
    }

    static void applyLevel(int level) {
        if (level == wifiLevel || level == 0)
            return;
        wifiLevel = level;
        barChanged();
    }

    static int onProbeOut(int fd, uint32_t, void*) {
        char buf[512];
        for (;;) {
            const auto N = read(fd, buf, sizeof(buf));
            if (N > 0) {
                probeOut.append(buf, N);
                continue;
            }
            if (N < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0; // more later
            break;        // EOF: the child is done talking
        }
        // "signal: -48 dBm" — the lockscreen's proven thresholds
        if (const auto P = probeOut.find("signal:"); P != std::string::npos) {
            const int DBM = std::atoi(probeOut.c_str() + P + 7);
            if (DBM < 0)
                applyLevel(DBM >= -60 ? 3 : DBM >= -70 ? 2 : 1);
        }
        probeOut.clear();
        probeDone();
        return 0;
    }

    static void probeSignal() {
        if (probePid > 0 || wifiIface.empty() || !g_pCompositor)
            return; // one probe in flight is plenty
        int pfd[2];
        if (pipe2(pfd, O_CLOEXEC | O_NONBLOCK) != 0)
            return;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
        const char* ARGV[] = {"iw", "dev", wifiIface.c_str(), "link", nullptr};
        pid_t       pid    = -1;
        const int   RC     = posix_spawnp(&pid, ARGV[0], &fa, nullptr, const_cast<char* const*>(ARGV), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(pfd[1]);
        if (RC != 0) {
            close(pfd[0]);
            return; // no iw: the /proc estimate is all we get
        }
        probePid = pid;
        probeFd  = pfd[0];
        probeOut.clear();
        probeSrc = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, probeFd, WL_EVENT_READABLE, onProbeOut, nullptr);
        if (!probeSrc)
            probeDone();
    }

    namespace Wifi {
        bool refresh() {
            bool            present = false, up = false;
            std::string     upIface;
            std::error_code ec;
            for (const auto& E : std::filesystem::directory_iterator("/sys/class/net", ec)) {
                if (!std::filesystem::is_directory(E.path() / "wireless", ec))
                    continue;
                present = true;
                std::ifstream o(E.path() / "operstate");
                std::string   st;
                if (o && std::getline(o, st) && st == "up") {
                    up      = true;
                    upIface = E.path().filename();
                    break;
                }
            }
            wifiIface = upIface;

            int level = wifiLevel; // the probe's async answer persists between ticks
            if (up) {
                // the instant estimate, where the driver fills it in:
                // "wlan0: 0000   54.  -56.  ..." — link quality is column 3
                std::ifstream f("/proc/net/wireless");
                std::string   line;
                while (f && std::getline(f, line)) {
                    const auto C = line.find(':');
                    if (C == std::string::npos)
                        continue;
                    std::string name = line.substr(0, C);
                    name.erase(0, name.find_first_not_of(' '));
                    if (name != upIface)
                        continue;
                    double q = 0;
                    if (std::sscanf(line.c_str() + C + 1, " %*s %lf", &q) == 1 && q > 0) {
                        const double Q = q / 70.0;
                        level          = Q >= 0.6 ? 3 : Q >= 0.3 ? 2 : 1;
                    }
                    break;
                }
                if (level == 0)
                    level = 1; // connected, strength not in yet: show the dot
                probeSignal(); // the authoritative read lands async
            } else
                level = 0;

            const bool OFF = !up;
            if (present == wifiPresent && OFF == wifiOff && level == wifiLevel)
                return false;
            wifiPresent = present;
            wifiOff     = OFF;
            wifiLevel   = level;
            return true;
        }

        void exit() {
            probeDone();
            probeOrphans.clear();
            wifiPresent = false;
            wifiOff     = true;
            wifiLevel   = 0;
            wifiIface.clear();
        }
    } // namespace Wifi

    // ---- the glyph ----

    struct SWifiTex {
        SP<ITexture> tex;
        uint64_t     key = 0;
    };
    static std::unordered_map<int, SWifiTex> wifiCache; // per physical height

    static SP<ITexture>                      wifiGlyph(int hPx, int level, bool off, const CHyprColor& ink) {
        // 16 x 13 logical viewport, scaled to hPx height
        const double S = hPx / 13.0;
        const int    W = (int)std::ceil(16 * S), H = hPx;
        auto*        SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        auto*        CR   = cairo_create(SURF);
        cairo_scale(CR, S, S);

        const double CX = 8, CY = 12; // arcs radiate from just under the dot
        const double A0 = -M_PI * 3 / 4, A1 = -M_PI / 4; // the up quadrant
        const auto   seg = [&](double a) { cairo_set_source_rgba(CR, ink.r, ink.g, ink.b, ink.a * a); };
        const double DIM = 0.25;

        // the dot (r 2): lit while anything is connected
        seg(off ? DIM : 1.0);
        cairo_arc(CR, CX, CY - 1, 2.0, 0, 2 * M_PI);
        cairo_fill(CR);

        cairo_set_line_width(CR, 3.0);
        cairo_set_line_cap(CR, CAIRO_LINE_CAP_ROUND);
        seg(!off && level >= 2 ? 1.0 : DIM);
        cairo_arc(CR, CX, CY, 5.6, A0, A1);
        cairo_stroke(CR);
        seg(!off && level >= 3 ? 1.0 : DIM);
        cairo_arc(CR, CX, CY, 10.0, A0, A1);
        cairo_stroke(CR);

        if (off) { // the 2.4px slash, corner to corner over the wedge
            seg(1.0);
            cairo_set_line_width(CR, 2.4);
            cairo_move_to(CR, 2.5, 0.5);
            cairo_line_to(CR, 13.5, 12.5);
            cairo_stroke(CR);
        }

        cairo_surface_flush(SURF);
        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_destroy(CR);
        cairo_surface_destroy(SURF);
        return tex;
    }

    namespace {
        class CWifiWidget : public IWidget {
          public:
            double fit(const SPaint&, const SFrame&) override {
                return wifiPresent ? 18 : 0; // 16px glyph + a breath; hidden without hardware
            }

            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                const int  PX    = std::max(8, (int)std::lround(13 * P.scale));
                auto&      CACHE = wifiCache[PX];
                const auto KEY   = F.fg.getAsHex() ^ ((uint64_t)wifiLevel << 56) ^ ((uint64_t)(wifiOff ? 1 : 0) << 60);
                if (CACHE.key != KEY || !CACHE.tex) {
                    if (warmGate.mayBuild()) {
                        CACHE.tex = wifiGlyph(PX, wifiLevel, wifiOff, F.fg);
                        CACHE.key = KEY;
                    } else
                        warmGate.texStale = true; // state moved without a warm: repaint right
                }
                if (CACHE.tex && CACHE.tex->m_texID != 0) {
                    const auto B = P.toPhys(box);
                    CBox       b{B.x + (B.w - CACHE.tex->m_size.x) / 2.0, B.y + (B.h - CACHE.tex->m_size.y) / 2.0, CACHE.tex->m_size.x, CACHE.tex->m_size.y};
                    P.tex(CACHE.tex, b.round());
                }
                // an indicator: no hit box, no popover
            }
        };
    } // namespace

    IWidget& wifiWidget() {
        static CWifiWidget W;
        return W;
    }

} // namespace NHyprbar
