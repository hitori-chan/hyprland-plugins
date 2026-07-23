// hyprbar/clock.cpp — the bold HH:MM (Android 16's bold status clock; the
// date left the bar with the redesign)

#include "hyprbar.hpp"

namespace NHyprbar {

    static std::string clockText;

    namespace Clock {
        bool refresh() {
            char       buf[16];
            const auto NOW = std::time(nullptr);
            const auto* TM = std::localtime(&NOW);
            if (!TM)
                return false;
            std::strftime(buf, sizeof(buf), "%H:%M", TM);
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
                const auto TEX = textTex(clockText, F.fg, P.pt, 0, "", 700);
                return TEX ? TEX->m_size.x / P.scale : 0;
            }
            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                P.texIn(textTex(clockText, F.fg, P.pt, 0, "", 700), box);
            }
        };
    } // namespace

    IWidget& clockWidget() {
        static CClockWidget W;
        return W;
    }

} // namespace NHyprbar
