// hyprbar/clock.cpp — awesome's textclock: the state (one string) and its widget

#include "hyprbar.hpp"

namespace NHyprbar {

    static std::string clockText;

    namespace Clock {
        bool refresh() {
            char       buf[64];
            const auto NOW = std::time(nullptr);
            // awesome's default format, trimmed — padding is the widget's
            // explicit margin, not spaces baked into the text
            std::strftime(buf, sizeof(buf), "%a %b %d, %H:%M", std::localtime(&NOW));
            if (clockText == buf)
                return false;
            clockText = buf;
            return true;
        }

        void exit() {
            clockText.clear();
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

} // namespace NHyprbar
