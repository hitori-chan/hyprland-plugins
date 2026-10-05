// awesome/shell/clock.cpp — awesome's textclock: the state (one string) and its widget

#include "shell/shell.hpp"

#include <locale.h>

#include <ctime>
#include <utility>

namespace NAwesome::Shell {

    static std::string clockText;
    // the user's LC_TIME: the compositor never calls setlocale, so a plain
    // strftime spoke the C locale's English (awesome's os.date speaks the
    // session's)
    static locale_t    timeLocale      = (locale_t)0;
    static bool        timeLocaleTried = false;

    namespace Clock {
        bool refresh() {
            char       buf[128];
            const auto NOW = std::time(nullptr);
            std::tm    tm{};
            if (!localtime_r(&NOW, &tm))
                return false;
            if (!std::exchange(timeLocaleTried, true))
                timeLocale = newlocale(LC_TIME_MASK, "", (locale_t)0);
            // awesome's default format, trimmed — padding is the widget's
            // explicit margin, not spaces baked into the text
            const size_t N = timeLocale ? strftime_l(buf, sizeof(buf), "%a %b %d, %H:%M", &tm, timeLocale) : std::strftime(buf, sizeof(buf), "%a %b %d, %H:%M", &tm);
            if (N == 0)
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
                // 6px each side, the bar's text pad
                const auto TEX = textTex(clockText, F.fg, P.pt);
                return TEX ? TEX->m_size.x / P.scale + 12 : 0;
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

} // namespace NAwesome::Shell
