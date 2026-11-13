// Awesome's textclock: the state (one string) and its widget. A click does
// nothing: the bell is the center's one door.

#include "shell/shell.hpp"

#include <locale.h>

#include <ctime>
#include <utility>

namespace Awesome::Shell {

    static std::string clockText;
    // the user's LC_TIME: the compositor never calls setlocale, so a plain
    // strftime would speak the C locale's English (awesome's os.date speaks
    // the session's)
    static locale_t    timeLocale      = (locale_t)0;
    static bool        timeLocaleTried = false;

    namespace Clock {
        bool refresh() {
            char       buf[128];
            const auto NOW = std::time(nullptr);
            std::tm    tm{};
            // localtime_r never re-reads the zone: a timezone change (travel,
            // timedatectl) would keep the old zone until a restart. Once a
            // minute, re-read it.
            tzset();
            if (!localtime_r(&NOW, &tm))
                return false;
            if (!std::exchange(timeLocaleTried, true))
                timeLocale = newlocale(LC_TIME_MASK, "", (locale_t)0);
            const auto FORMAT = [&](const char* fmt) { return timeLocale ? strftime_l(buf, sizeof(buf), fmt, &tm, timeLocale) : std::strftime(buf, sizeof(buf), fmt, &tm); };
            // strftime answers 0 for an empty or overlong result: the
            // default format stands in
            if (FORMAT(cfg().getS("plugin:awesome:shell:clock_format").c_str()) == 0 &&
                FORMAT(std::string{SPECS[CfgKey{"plugin:awesome:shell:clock_format"}.index].sval}.c_str()) == 0)
                return false;
            if (clockText == buf)
                return false;
            clockText = buf;
            return true;
        }

        void exit() {
            clockText.clear();
            if (timeLocale)
                freelocale(timeLocale);
            timeLocale      = (locale_t)0;
            timeLocaleTried = false;
        }
    } // namespace Clock

    namespace {
        class CClockWidget : public IWidget {
          public:
            double fit(const SPaint& P, const SFrame& F) override {
                const auto TEX = textTex(clockText, F.fg, P.pt);
                return textInkW(TEX, P.scale);
            }
            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                P.texIn(textTex(clockText, F.fg, P.pt), box);
            }
        };
    } // namespace

    IWidget& clockWidget() {
        static CClockWidget W;
        return W;
    }

} // namespace Awesome::Shell
