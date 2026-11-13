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
    constexpr std::string_view           USAGE = "usage: vclick click <window> <x> <y> [button] [double|triple] [glide|jump] [size:<px>]\n"
                                                 "       vclick scroll <window> <x> <y> up|down|left|right [steps] [<ms>ms] [<n>px] [glide|jump] [size:<px>]\n"
                                                 "       vclick move <window> <x> <y> [glide|jump] [size:<px>]\n"
                                                 "       vclick press <window> <x> <y> [button] [glide|jump] [size:<px>]\n"
                                                 "       vclick drag <window> <x> <y> <x2> <y2> [button] [glide|jump] [size:<px>]\n"
                                                 "       vclick pinch <window> <x> <y> <scale> [<deg>deg] [glide|jump] [size:<px>]\n"
                                                 "       vclick key <window> <key> [mods]\n"
                                                 "       vclick release | hide | status | where | mode [mouse|tap] | profile [reload [file]] | image <window>\n"
                                                 "  window: a window selector (address:0x..., class:...), or a bare 0x... address\n"
                                                 "  button: left (default), right, middle, side, extra, or a code 272-279\n"
                                                 "  steps: wheel detents, 1-20 (default 1); <ms>ms: between them, 0ms-1000ms (default 50ms; 0ms: at once)\n"
                                                 "  <n>px: instead, a finger's scroll on a touchpad, 1px-10000px, at the user's pace\n"
                                                 "  key: an xkb key name, as send_shortcut takes it (Next, Prior, Return, a, F5), or code:<n>; pressed and released\n"
                                                 "  mods: modifiers held for it, joined by + (ctrl, shift, alt, super)\n"
                                                 "  mode mouse: glide there, and stay; the app keeps the hover (the default)\n"
                                                 "  mode tap: go at once, and leave after the input; no cursor stays\n"
                                                 "  press: hold the button down; move drags it, release lets go\n"
                                                 "  drag: press at x y, glide to x2 y2 at the user's pace, let go there\n"
                                                 "  pinch: two fingers on a touchpad spread to scale (0.1-10; 2: zoom in twice), turning deg (-360-360, clockwise)\n"
                                                 "  glide|jump: this request's way there, whatever the mode\n"
                                                 "  size:<px>: the target's size, for the glide's time (default 32)";

    static bool                          mouseMode = true;

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

    static std::optional<EVerb> verbOf(std::string_view verb) {
        for (const auto V : {EVerb::CLICK, EVerb::SCROLL, EVerb::MOVE, EVerb::PRESS, EVerb::DRAG, EVerb::PINCH})
            if (verb == verbName(V))
                return V;
        return std::nullopt;
    }

    // "vclick click|scroll|move|press|drag <window> <x> <y> [<x2> <y2>] [option...]": the options
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
        c.pos       = {*X, *Y};
        c.verb      = *verbOf(VERB);
        size_t opts = 5;
        if (c.verb == EVerb::DRAG) { // drag <window> <x> <y> <x2> <y2>: where it lets go
            const auto X2 = w.size() > 6 ? number<double>(w[5]) : std::nullopt;
            const auto Y2 = w.size() > 6 ? number<double>(w[6]) : std::nullopt;
            if (!X2 || !Y2 || !std::isfinite(*X2) || !std::isfinite(*Y2))
                return BAD(w.size() > 6 ? std::format("bad coordinates '{} {}'", w[5], w[6]) : "a drag needs where it lets go: <x2> <y2>");
            c.to = {*X2, *Y2};
            opts = 7;
        } else if (c.verb == EVerb::PINCH) { // pinch <window> <x> <y> <scale>
            const auto S = w.size() > 5 ? number<double>(w[5]) : std::nullopt;
            if (!S || !(*S >= MIN_SCALE && *S <= MAX_SCALE))
                return BAD(std::format("a pinch needs its scale: {:g}-{:g} (2: twice as large)", MIN_SCALE, MAX_SCALE));
            c.scale = *S;
            opts    = 6;
        }

        bool button = false, count = false, steps = false, pace = false, size = false;
        for (size_t i = opts; i < w.size(); ++i) {
            const auto O     = w[i];
            const auto TWICE = [&] { return BAD(std::format("'{}': that option is given twice", O)); };
            if (O == "glide" || O == "jump") {
                if (c.glide)
                    return TWICE();
                c.glide = O == "glide";
                continue;
            }
            if (O.starts_with("size:")) {
                const auto PX = number<double>(O.substr(5));
                if (!PX || !(*PX >= Path::MIN_TARGET_W && *PX <= Path::MAX_TARGET_W))
                    return BAD(std::format("'{}': the target's size must be {:g}-{:g} px", O, Path::MIN_TARGET_W, Path::MAX_TARGET_W));
                if (std::exchange(size, true))
                    return TWICE();
                c.size = *PX;
                continue;
            }
            if (VERB == "click" || VERB == "press" || VERB == "drag") {
                if (VERB == "click" && (O == "double" || O == "triple")) {
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
            } else if (VERB == "pinch") {
                if (O.ends_with("deg")) {
                    const auto D = number<double>(O.substr(0, O.size() - 3));
                    if (!D || !(*D >= -MAX_DEG && *D <= MAX_DEG))
                        return BAD(std::format("'{}': a turn must be -{:g}deg-{:g}deg", O, MAX_DEG, MAX_DEG));
                    if (c.deg != 0)
                        return TWICE();
                    c.deg = *D;
                    continue;
                }
            } else if (VERB == "scroll") {
                if (const auto D = wheelOf(O)) {
                    if (c.wheel)
                        return TWICE();
                    c.wheel = D;
                    continue;
                }
                if (O.ends_with("px")) {
                    const auto PX = number<double>(O.substr(0, O.size() - 2));
                    if (!PX || !(*PX >= 1 && *PX <= MAX_PX))
                        return BAD(std::format("'{}': a scroll by px must be 1px-{:g}px", O, MAX_PX));
                    if (c.px > 0)
                        return TWICE();
                    c.px = *PX;
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
        if (c.verb == EVerb::PINCH && c.scale == 1 && c.deg == 0)
            return BAD("a pinch needs a scale other than 1, or a turn");
        if (c.px > 0 && (steps || pace))
            return BAD("a scroll by px takes no steps or pace: a finger's goes at the user's");
        return c;
    }

    // what runs now, or nothing
    static std::optional<std::string> running() {
        if (Glide::active())
            return Glide::dragging() ? "dragging" : "moving";
        if (const auto S = Stroke::status())
            return S;
        return Pointer::scrollStatus();
    }

    static bool busy() {
        return running().has_value();
    }

    static std::string busyError() {
        return std::format("error: busy: still {}; retry later", *running());
    }

    // "idle", what runs, then a held button and why the last input ended early
    static std::string status() {
        if (const auto R = running())
            return *R;
        Pointer::holdLost();
        std::string out = "idle";
        if (const auto B = Pointer::heldButton())
            out += "; holding " + Pointer::buttonName(*B);
        return lastStop.empty() ? out : out + "; " + lastStop;
    }

    // take the mouse away, a hand lifting off it: a held button goes up,
    // the cursor goes, the app's pointer leaves
    static void away() {
        Cursor::hide();
        if (Pointer::heldButton())
            std::ignore = Pointer::release();
        Pointer::unhover();
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
            if (w[1] == "release") {
                if (busy())
                    return busyError();
                if (const auto R = Pointer::release(); !R)
                    return R.error();
                if (!mouseMode) { // the tap mode: the pointer leaves after the input
                    Pointer::unhover();
                    Cursor::rest(false);
                }
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

        // a held button: the hand moves (in that window) or lets go first
        const auto TW = Pointer::windowOf(c->selector);
        Pointer::holdLost();
        if (const auto B = Pointer::heldButton()) {
            if (c->verb != EVerb::MOVE)
                return std::format("error: busy: holding {}; release it first", Pointer::buttonName(*B));
            if (TW != Pointer::heldWindow())
                return std::format("error: busy: holding {} in another window; release it first", Pointer::buttonName(*B));
        }
        if ((c->verb == EVerb::PRESS || c->verb == EVerb::DRAG) && !Pointer::dragGuard)
            return "error: no drag-and-drop guard (the initiateDrag hook is out): a held button could hand a drag to the user's mouse";
        if (c->verb == EVerb::DRAG && TW) {
            const auto SIZE = Pointer::windowSize(TW);
            if (c->to.x < 0 || c->to.y < 0 || c->to.x >= SIZE.x || c->to.y >= SIZE.y)
                return std::format("error: {:g},{:g} is outside the window ({:g}x{:g})", c->to.x, c->to.y, SIZE.x, SIZE.y);
        }

        // a hover kept in another app ends here, as a mouse leaves it
        Pointer::unhover(Pointer::clientOf(TW));

        if (c->glide.value_or(mouseMode)) {
            if (const auto R = Glide::start(*c))
                return *R ? "ok" : R->error();
        }
        const auto R = Pointer::dispatch(*c);
        if (!R)
            return R.error();
        if (TW && !Glide::active()) { // a drag's way, set off by its press, ends it itself
            Pointer::rememberPos(TW, c->pos);
            if (mouseMode)
                Cursor::place(TW, c->pos);
            else if (Pointer::heldButton())
                Cursor::moveTo(c->pos); // the tap mode shows a held button's cursor
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
        if (verbOf(W[1]))
            return input(W);
        if (W[1] == "key")
            return key(W);
        if (const auto Q = query(W))
            return *Q;
        return std::string{USAGE};
    }
}
