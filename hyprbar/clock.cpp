// hyprbar/clock.cpp — the bold clock. The text is plugin:hyprbar:clock_format
// (strftime; default the bare %H:%M) — the user's awesome bar ran the stock
// textclock "%a %b %d, %H:%M", and the format ticks per MINUTE: seconds
// would go stale between refreshes.

#include "hyprbar.hpp"

namespace NHyprbar {

    static std::string clockText;

    namespace Clock {
        bool refresh() {
            char       buf[64];
            const auto NOW = std::time(nullptr);
            const auto* TM = std::localtime(&NOW);
            if (!TM)
                return false;
            const std::string FMT = cfg.clockFormat ? cfg.clockFormat->value() : std::string{};
            if (std::strftime(buf, sizeof(buf), FMT.empty() ? "%H:%M" : FMT.c_str(), TM) == 0)
                std::snprintf(buf, sizeof(buf), "--:--"); // format overflowed the cell — show SOMETHING
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
