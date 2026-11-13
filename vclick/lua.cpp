// The Lua functions: hl.plugin.vclick.<verb>(...) runs the hyprctl verb of
// that name with the same words in the same order (a number where hyprctl
// takes one) and the same checks. `ok` comes back as true, an error as nil
// and its line, any other answer (status, where, mode, profile, image) as
// its string.
#include "vclick.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <lua.hpp>

#include <array>
#include <format>
#include <utility>

namespace VClick::Lua {
    constexpr std::array<std::string_view, 13> VERBS    = {"click", "scroll", "move", "press", "release", "drag", "key", "hide", "status", "where", "mode", "profile", "image"};
    constexpr int                              MAX_ARGS = 8; // the longest requests: a scroll's and a drag's

    static int answer(lua_State* L, std::string_view out) {
        if (out == "ok") {
            lua_pushboolean(L, 1);
            return 1;
        }
        if (out.starts_with("error:") || out.starts_with("usage:")) {
            out = out.substr(0, out.find('\n')); // the usage after an error is hyprctl's
            lua_pushnil(L);
            lua_pushlstring(L, out.data(), out.size());
            return 2;
        }
        lua_pushlstring(L, out.data(), out.size());
        return 1;
    }

    static int call(lua_State* L, std::string_view verb) {
        const int N = lua_gettop(L);
        if (N > MAX_ARGS)
            return answer(L, std::format("error: {} takes at most {} arguments", verb, MAX_ARGS));
        std::array<std::string, MAX_ARGS> args;
        std::vector<std::string_view>     w{NAME, verb};
        for (int i = 1; i <= N; ++i) {
            auto& a = args[i - 1];
            if (lua_type(L, i) == LUA_TSTRING) {
                size_t      len = 0;
                const char* S   = lua_tolstring(L, i, &len);
                a.assign(S, len);
            } else if (lua_type(L, i) == LUA_TNUMBER)
                a = lua_isinteger(L, i) ? std::to_string(lua_tointeger(L, i)) : std::format("{}", lua_tonumber(L, i));
            else
                return answer(L, std::format("error: argument {} is a {}: a string or a number, as hyprctl takes it", i, luaL_typename(L, i)));
            w.emplace_back(a);
        }
        return answer(L, Command::run(w));
    }

    template <size_t I>
    static int verb(lua_State* L) {
        return call(L, VERBS[I]);
    }

    template <size_t... I>
    static void add(void* handle, std::index_sequence<I...>) {
        (HyprlandAPI::addLuaFunction(handle, NAME, std::string{VERBS[I]}, &verb<I>), ...);
    }

    void init(void* handle) {
        add(handle, std::make_index_sequence<VERBS.size()>{}); // false without a Lua config: hyprctl still works
    }
}
