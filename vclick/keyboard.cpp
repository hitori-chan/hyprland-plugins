// The app's own keyboard. One key goes to the target client's wl_keyboard
// objects (enter, modifiers, press, release, modifiers, leave), never
// through the seat: the user's keyboard focus, held keys and modifiers stay
// the user's, and nothing the user types goes anywhere else meanwhile. The
// press and the release go together: no key is ever left held in the app.
// When the user's keyboard is in the same app, the key goes only where it
// already is (docs/design.md, the keyboard).
#include "vclick.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/keybinds/Resolver.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>

#include <algorithm>
#include <cctype>
#include <format>

namespace VClick::Keyboard {
    // evdev KEY_* codes below the button blocks: every key of a keyboard
    constexpr uint32_t MAX_KEY = 0xff;

    std::expected<SKey, std::string> parse(std::string_view key, std::string_view mods) {
        SKey k;
        if (!g_pSeatManager->m_keyboard)
            return std::unexpected("error: no keyboard: the seat has none to take the keymap from");
        // the compositor's own resolver, as send_shortcut's: an xkb key
        // name in the active layout, or code:<n> (an xkb keycode)
        const auto CODE = Keybinds::resolver()->resolveKeycode(std::string{key});
        if (!CODE || *CODE < 9 || *CODE - 8 > MAX_KEY)
            return std::unexpected(std::format("error: '{}' is not a key", key));
        k.code = *CODE - 8;
        for (size_t i = 0; i < mods.size();) {
            const size_t END = std::min(mods.find_first_of("+_", i), mods.size());
            std::string  name{mods.substr(i, END - i)};
            std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::toupper(c); });
            const auto MOD = Keybinds::modifierFromString(name);
            if (!MOD || *MOD == Input::HL_MODIFIER_CAPS)
                return std::unexpected(std::format("error: '{}' is not a modifier", mods.substr(i, END - i)));
            k.mods |= *MOD;
            i = END + 1;
        }
        return k;
    }

    // Every wl_keyboard the client bound, across all its wl_seat bindings.
    // A wl_keyboard object has no public lookup; its seat's list is public.
    static std::vector<SP<CWLKeyboardResource>> keyboardsOf(wl_client* client) {
        std::vector<SP<CWLKeyboardResource>> out;
        wl_client_for_each_resource(
            client,
            [](wl_resource* r, void* data) -> wl_iterator_result {
                if (std::string_view{wl_resource_get_class(r)} != "wl_seat" || !wl_resource_get_user_data(r))
                    return WL_ITERATOR_CONTINUE;
                if (const auto SEAT = CWLSeatResource::fromResource(r); SEAT && SEAT->good()) {
                    for (const auto& k : SEAT->m_keyboards)
                        if (const auto K = k.lock(); K && K->good())
                            sc<std::vector<SP<CWLKeyboardResource>>*>(data)->emplace_back(K);
                }
                return WL_ITERATOR_CONTINUE;
            },
            &out);
        return out;
    }

    Result send(const SKey& k) {
        if (const auto WHY = blocked())
            return fail(*WHY);
        const auto W = Pointer::windowOf(k.selector);
        if (!W)
            return fail("error: window not found");
        if (!Desktop::View::validMapped(W) || W->isHidden())
            return fail("error: the window is not mapped");
        if (W->backend().isX11())
            return fail("error: XWayland windows are not supported");
        const auto SURF = Pointer::mainSurface(W);
        if (!SURF || (g_pSessionLockManager && g_pSessionLockManager->isSurfaceSessionLock(SURF)))
            return fail("error: the window has no surface");
        const auto KB = g_pSeatManager->m_keyboard.lock();
        if (!KB)
            return fail("error: no keyboard: the seat has none to take the keymap from");
        const auto KEYBOARDS = keyboardsOf(SURF->client());
        if (KEYBOARDS.empty())
            return fail("error: the app has no keyboard (it bound no wl_keyboard)");

        // the layout and the lock keys stay what the app would see from the
        // user's keyboard; the modifiers held are the request's alone
        const auto& STATE = KB->m_modifiersState;
        const auto  MODS  = g_pInputManager->hyprlandModsToXkb(KB, k.mods);
        auto        t     = Pointer::nowMs();

        const auto  USER = g_pSeatManager->m_state.keyboardFocus.lock();
        if (USER && USER->client() == SURF->client()) {
            // The user's keyboard is in this app: one wl_keyboard, theirs. A
            // leave and an enter around the key would blur their window (a
            // text field, a menu), so the key goes only into the very window
            // they are in, and only while they hold nothing: a held key or
            // modifier would join the request's.
            if (USER != SURF)
                return fail("error: busy: the user's keyboard is in another window of this app; retry later");
            if (!g_pInputManager->getKeysFromAllKBs().empty() || STATE.depressed || STATE.latched)
                return fail("error: busy: the user holds a key in this app; retry later");
            for (const auto& K : KEYBOARDS) {
                K->sendMods(MODS, 0, STATE.locked, STATE.group);
                K->sendKey(t, k.code, WL_KEYBOARD_KEY_STATE_PRESSED);
                K->sendKey(t + 1, k.code, WL_KEYBOARD_KEY_STATE_RELEASED);
                K->sendMods(STATE.depressed, STATE.latched, STATE.locked, STATE.group);
            }
            return {};
        }

        // Another app has the user's keyboard (or the lockscreen has it): the
        // app hears this key alone, between an enter and a leave. The enter
        // names no held keys: what the user holds is theirs.
        wl_array keys;
        wl_array_init(&keys);
        for (const auto& K : KEYBOARDS) {
            K->sendEnter(SURF, &keys);
            K->sendMods(MODS, 0, STATE.locked, STATE.group);
            K->sendKey(t, k.code, WL_KEYBOARD_KEY_STATE_PRESSED);
            K->sendKey(t + 1, k.code, WL_KEYBOARD_KEY_STATE_RELEASED);
            K->sendMods(0, 0, STATE.locked, STATE.group);
            K->sendLeave();
        }
        wl_array_release(&keys);
        return {};
    }
}
