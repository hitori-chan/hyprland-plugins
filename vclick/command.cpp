// The hyprctl command: parse a request, run it, report status.
#include "vclick.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace VClick {
    static std::string lastStop; // why the last glide or paced scroll ended early

    void noteStop(std::string why) {
        lastStop = std::move(why);
    }

    void clearStop() {
        lastStop.clear();
    }
}

namespace VClick::Command {
    constexpr std::string_view USAGE =
        "usage: vclick click <window> <x> <y> [button] [double|triple] [glide|jump]\n"
        "       vclick scroll <window> <x> <y> up|down|left|right [steps] [<ms>ms] [glide|jump]\n"
        "       vclick move <window> <x> <y> [glide|jump]\n"
        "       vclick key <window> <key> [mods]\n"
        "       vclick hide | status | where | mode [mouse|tap] | profile [reload [file]] | image <window>\n"
        "  window: a window selector (address:0x..., class:...), or a bare 0x... address\n"
        "  button: left (default), right, middle, side, extra, or a code 272-279\n"
        "  steps: wheel detents, 1-20 (default 1); <ms>ms: between them, 0ms-1000ms (default 50ms; 0ms: at once)\n"
        "  key: an xkb key name, as send_shortcut takes it (Next, Prior, Return, a, F5), or code:<n>; pressed and released\n"
        "  mods: modifiers held for it, joined by + (ctrl, shift, alt, super)\n"
        "  mode mouse: glide there, and stay; the app keeps the hover (the default)\n"
        "  mode tap: go at once, and leave after the input; no cursor stays\n"
        "  glide|jump: this request's way there, whatever the mode";

    static bool mouseMode = true;

    static std::vector<std::string_view> words(std::string_view s) {
        std::vector<std::string_view> out;
        size_t                        i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(sc<unsigned char>(s[i])))
                ++i;
            const size_t START = i;
            while (i < s.size() && !std::isspace(sc<unsigned char>(s[i])))
                ++i;
            if (i > START)
                out.emplace_back(s.substr(START, i - START));
        }
        return out;
    }

    template <typename T>
    static std::optional<T> number(std::string_view s) {
        T          v{};
        const auto END        = s.data() + s.size();
        const auto [ptr, err] = std::from_chars(s.data(), END, v);
        if (err != std::errc{} || ptr != END)
            return std::nullopt;
        return v;
    }

    static std::string selectorOf(std::string_view s) {
        return s.starts_with("0x") ? "address:" + std::string{s} : std::string{s};
    }

    static std::optional<uint32_t> buttonCode(std::string_view s) {
        if (s == "left")
            return BTN_LEFT;
        if (s == "right")
            return BTN_RIGHT;
        if (s == "middle")
            return BTN_MIDDLE;
        if (s == "side")
            return BTN_SIDE;
        if (s == "extra")
            return BTN_EXTRA;
        const auto CODE = number<uint32_t>(s);
        if (CODE && *CODE >= BTN_LEFT && *CODE <= BTN_TASK) // the BTN_MOUSE block
            return CODE;
        return std::nullopt;
    }

    static std::optional<SWheel> wheelOf(std::string_view s) {
        if (s == "down")
            return SWheel{WL_POINTER_AXIS_VERTICAL_SCROLL, +1};
        if (s == "up")
            return SWheel{WL_POINTER_AXIS_VERTICAL_SCROLL, -1};
        if (s == "right")
            return SWheel{WL_POINTER_AXIS_HORIZONTAL_SCROLL, +1};
        if (s == "left")
            return SWheel{WL_POINTER_AXIS_HORIZONTAL_SCROLL, -1};
        return std::nullopt;
    }

    static bool isInput(std::string_view verb) {
        return verb == "click" || verb == "scroll" || verb == "move";
    }

    // "vclick click|scroll|move <window> <x> <y> [option...]": the options
    // after the point, in any order, each at most once
    static std::expected<SAction, std::string> parse(const std::vector<std::string_view>& w) {
        const auto BAD = [](std::string why) { return std::unexpected(std::format("error: {}\n{}", why, USAGE)); };
        if (w.size() < 5)
            return std::unexpected(std::string{USAGE});
        const auto VERB = w[1];

        SAction    c{.selector = selectorOf(w[2])};
        const auto X = number<double>(w[3]);
        const auto Y = number<double>(w[4]);
        if (!X || !Y || !std::isfinite(*X) || !std::isfinite(*Y))
            return BAD(std::format("bad coordinates '{} {}'", w[3], w[4]));
        c.pos  = {*X, *Y};
        c.move = VERB == "move";

        bool button = false, count = false, steps = false, pace = false;
        for (size_t i = 5; i < w.size(); ++i) {
            const auto O    = w[i];
            const auto TWICE = [&] { return BAD(std::format("'{}': that option is given twice", O)); };
            if (O == "glide" || O == "jump") {
                if (c.glide)
                    return TWICE();
                c.glide = O == "glide";
                continue;
            }
            if (VERB == "click") {
                if (O == "double" || O == "triple") {
                    if (std::exchange(count, true))
                        return TWICE();
                    c.count = O == "double" ? 2 : 3;
                    continue;
                }
                if (const auto B = buttonCode(O)) {
                    if (std::exchange(button, true))
                        return TWICE();
                    c.button = *B;
                    continue;
                }
            } else if (VERB == "scroll") {
                if (const auto D = wheelOf(O)) {
                    if (c.wheel)
                        return TWICE();
                    c.wheel = D;
                    continue;
                }
                if (O.ends_with("ms")) {
                    const auto MS = number<int>(O.substr(0, O.size() - 2));
                    if (!MS || *MS < 0 || *MS > MAX_MS)
                        return BAD(std::format("'{}': the pace must be 0ms-{}ms", O, MAX_MS));
                    if (std::exchange(pace, true))
                        return TWICE();
                    c.ms = *MS;
                    continue;
                }
                if (const auto N = number<int>(O)) {
                    if (*N < 1 || *N > MAX_STEPS)
                        return BAD(std::format("'{}': steps must be 1-{}", O, MAX_STEPS));
                    if (std::exchange(steps, true))
                        return TWICE();
                    c.count = *N;
                    continue;
                }
            }
            return BAD(std::format("'{}' is not an option of {}", O, VERB));
        }
        if (VERB == "scroll" && !c.wheel)
            return BAD("a scroll needs its direction: up, down, left or right");
        return c;
    }

    static bool busy() {
        return Glide::active() || Pointer::scrollStatus();
    }

    static std::string busyError() {
        if (Glide::active())
            return "error: busy: still moving; retry later";
        return std::format("error: busy: still {}; retry later", *Pointer::scrollStatus());
    }

    static std::string status() {
        if (Glide::active())
            return "moving";
        if (const auto S = Pointer::scrollStatus())
            return *S;
        return lastStop.empty() ? "idle" : "idle; " + lastStop;
    }

    // take the mouse away: the cursor goes, the app's pointer leaves
    static void away() {
        Cursor::hide();
        Pointer::release();
    }

    // the commands without a point
    static std::optional<std::string> query(const std::vector<std::string_view>& w) {
        if (w.size() == 2) {
            if (w[1] == "status")
                return status();
            if (w[1] == "where")
                return Cursor::where();
            if (w[1] == "hide") {
                if (busy())
                    return busyError();
                away();
                return "ok";
            }
        }
        if (w[1] == "mode" && w.size() <= 3) {
            if (w.size() == 3) {
                if (w[2] != "mouse" && w[2] != "tap")
                    return std::string{USAGE};
                const bool MOUSE = w[2] == "mouse";
                if (mouseMode && !MOUSE && !busy())
                    away(); // nothing stays in the tap mode
                mouseMode = MOUSE;
            }
            return mouseMode ? "mouse" : "tap";
        }
        if (w[1] == "profile" && w.size() <= 4) {
            if (w.size() == 2)
                return Path::describe(Glide::profile());
            if (w[2] != "reload")
                return std::string{USAGE};
            return Glide::loadProfile(w.size() == 4 ? std::string{w[3]} : "") + "\n" + Path::describe(Glide::profile());
        }
        if (w[1] == "image" && w.size() == 3)
            return Cursor::describe(selectorOf(w[2]));
        return std::nullopt;
    }

    static std::string input(const std::vector<std::string_view>& w) {
        auto c = parse(w);
        if (!c)
            return c.error();
        if (busy())
            return busyError();
        c->stay = mouseMode;

        // a hover held in another app ends here, as a mouse leaves it
        const auto TW = Pointer::windowOf(c->selector);
        Pointer::release(Pointer::clientOf(TW));

        if (c->glide.value_or(mouseMode)) {
            if (const auto R = Glide::start(*c))
                return *R ? "ok" : R->error();
        }
        const auto R = Pointer::dispatch(*c);
        if (!R)
            return R.error();
        if (TW) {
            Pointer::rememberPos(TW, c->pos);
            if (mouseMode)
                Cursor::place(TW, c->pos);
        }
        return "ok";
    }

    // "vclick key <window> <key> [mods]": one key, pressed and released
    static std::string key(const std::vector<std::string_view>& w) {
        if (w.size() < 4 || w.size() > 5)
            return std::string{USAGE};
        auto k = Keyboard::parse(w[3], w.size() == 5 ? w[4] : std::string_view{});
        if (!k)
            return std::format("{}\n{}", k.error(), USAGE);
        if (busy())
            return busyError();
        k->selector  = selectorOf(w[2]);
        const auto R = Keyboard::send(*k);
        return R ? "ok" : R.error();
    }

    std::string handle(const std::string& request) {
        return run(words(request));
    }

    std::string run(const std::vector<std::string_view>& W) {
        if (W.size() < 2 || W[0] != NAME)
            return std::string{USAGE};
        if (isInput(W[1]))
            return input(W);
        if (W[1] == "key")
            return key(W);
        if (const auto Q = query(W))
            return *Q;
        return std::string{USAGE};
    }
}
