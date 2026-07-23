// hyprbar/kbdlayout.cpp — the keyboard-layout chip, leftmost in the status
// island (the lockscreen's switcher was removed; this is where the state
// lives now). An indicator in v1 — layouts switch by keybind as always.

#include "common/theme.hpp"

#include "hyprbar.hpp"

namespace NHyprbar {

    static std::string kbdShort; // "en", "vi", ...

    // "English (US)" -> "en", "Vietnamese" -> "vi": the first two letters,
    // lowercased — xkb layout names lead with the language
    static std::string shorten(const std::string& name) {
        std::string out;
        for (const char C : name) {
            if (std::isalpha((unsigned char)C))
                out += (char)std::tolower((unsigned char)C);
            if (out.size() == 2)
                break;
        }
        return out;
    }

    namespace Kbd {
        void onLayout(const std::string& layoutName) {
            const auto S = shorten(layoutName);
            if (S == kbdShort)
                return;
            kbdShort = S;
            barChanged();
        }

        void init() {
            if (const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr)
                kbdShort = shorten(KB->getActiveLayout());
        }

        void exit() {
            kbdShort.clear();
        }
    } // namespace Kbd

    namespace {
        class CKbdWidget : public IWidget {
          public:
            double fit(const SPaint& P, const SFrame& F) override {
                if (kbdShort.empty())
                    return 0;
                const auto TEX = textTex(kbdShort, F.fg, P.pt, 0, "", 600);
                return TEX ? TEX->m_size.x / P.scale : 0;
            }
            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                P.texIn(textTex(kbdShort, F.fg, P.pt, 0, "", 600), box);
            }
        };
    } // namespace

    IWidget& kbdWidget() {
        static CKbdWidget W;
        return W;
    }

} // namespace NHyprbar
