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
        "usage: vclick <window> <x> <y> [button [clicks 1-3] | scroll-up|scroll-down|scroll-left|scroll-right [steps 1-20 [ms 0-1000]] | move] [glide|jump]\n"
        "       vclick status | mode [glide|jump] | profile [reload [file]] | cursor <window>\n"
        "  button: left (default), right, middle, side, extra, or a code 272-279\n"
        "  ms: between scroll steps (default 50); 0 sends them all at once\n"
        "  move: go there and stay (a hover)\n"
        "  glide: go there along a curve, at the user's pace (the default mode); jump: go at once";

    static bool glideMode = true; // the way there when a request names none

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
        if (s == "scroll-down")
            return SWheel{WL_POINTER_AXIS_VERTICAL_SCROLL, +1};
        if (s == "scroll-up")
            return SWheel{WL_POINTER_AXIS_VERTICAL_SCROLL, -1};
        if (s == "scroll-right")
            return SWheel{WL_POINTER_AXIS_HORIZONTAL_SCROLL, +1};
        if (s == "scroll-left")
            return SWheel{WL_POINTER_AXIS_HORIZONTAL_SCROLL, -1};
        return std::nullopt;
    }

    // "vclick <window> <x> <y> [button|scroll-*|move] [count] [ms] [glide|jump]"
    static std::expected<SAction, std::string> parse(std::vector<std::string_view> w) {
        if (w.size() < 4 || w.size() > 8 || w[0] != NAME)
            return std::unexpected(std::string{USAGE});

        SAction c{.selector = std::string{w[1]}};
        if (w.size() > 4 && (w.back() == "glide" || w.back() == "jump")) {
            c.glide = w.back() == "glide";
            w.pop_back();
        }
        if (w.size() > 7)
            return std::unexpected(std::string{USAGE});
        const auto X = number<double>(w[2]);
        const auto Y = number<double>(w[3]);
        if (!X || !Y || !std::isfinite(*X) || !std::isfinite(*Y))
            return std::unexpected(std::format("error: bad coordinates '{} {}'\n{}", w[2], w[3], USAGE));
        c.pos = {*X, *Y};

        if (w.size() > 4 && w[4] == "move") {
            if (w.size() > 5)
                return std::unexpected(std::string{USAGE});
            c.move = true;
            return c;
        }
        if (w.size() > 4) {
            c.wheel = wheelOf(w[4]);
            if (!c.wheel) {
                const auto B = buttonCode(w[4]);
                if (!B)
                    return std::unexpected(std::format("error: unknown button '{}'\n{}", w[4], USAGE));
                c.button = *B;
            }
        }
        if (w.size() > 5) {
            const int  MAX = c.wheel ? MAX_STEPS : MAX_CLICKS;
            const auto N   = number<int>(w[5]);
            if (!N || *N < 1 || *N > MAX)
                return std::unexpected(std::format("error: {} must be 1-{}\n{}", c.wheel ? "steps" : "count", MAX, USAGE));
            c.count = *N;
        }
        if (w.size() > 6) {
            if (!c.wheel)
                return std::unexpected(std::string{USAGE});
            const auto MS = number<int>(w[6]);
            if (!MS || *MS < 0 || *MS > MAX_MS)
                return std::unexpected(std::format("error: ms must be 0-{}\n{}", MAX_MS, USAGE));
            c.ms = *MS;
        }
        return c;
    }

    static std::string status() {
        if (Glide::active())
            return "moving";
        if (const auto S = Pointer::scrollStatus())
            return *S;
        return lastStop.empty() ? "idle" : "idle; " + lastStop;
    }

    // the commands without a window and point: status, mode, profile, cursor
    static std::optional<std::string> query(const std::vector<std::string_view>& w) {
        if (w.size() < 2 || w.size() > 4 || w[0] != NAME)
            return std::nullopt;
        if (w[1] == "status" && w.size() == 2)
            return status();
        if (w[1] == "mode" && w.size() <= 3) {
            if (w.size() == 3) {
                if (w[2] != "glide" && w[2] != "jump")
                    return std::string{USAGE};
                glideMode = w[2] == "glide";
            }
            return glideMode ? "glide" : "jump";
        }
        if (w[1] == "profile") {
            if (w.size() == 2)
                return Path::describe(Glide::profile());
            if (w[2] != "reload")
                return std::string{USAGE};
            return Glide::loadProfile(w.size() == 4 ? std::string{w[3]} : "") + "\n" + Path::describe(Glide::profile());
        }
        if (w[1] == "cursor" && w.size() == 3)
            return Cursor::describe(w[2]);
        return std::nullopt;
    }

    std::string handle(const std::string& request) {
        const auto W = words(request);
        if (const auto Q = query(W))
            return *Q;

        const auto C = parse(W);
        if (!C)
            return C.error();
        if (Glide::active())
            return "error: busy: still moving; retry later";
        if (const auto S = Pointer::scrollStatus())
            return std::format("error: busy: still {}; retry later", *S);

        // a hover held in another app ends here
        const auto TW = Pointer::windowOf(C->selector);
        Pointer::release(Pointer::clientOf(TW));

        if (C->glide.value_or(glideMode)) {
            if (const auto R = Glide::start(*C))
                return *R ? "ok" : R->error();
        }
        const auto R = Pointer::dispatch(*C);
        if (!R)
            return R.error();
        if (TW)
            Pointer::rememberPos(TW, C->pos);
        return "ok";
    }
}
