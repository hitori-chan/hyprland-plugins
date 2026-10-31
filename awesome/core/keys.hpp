// What every compositor-drawn prompt that takes the keyboard shares: the
// launcher, the shade's keys and the reply field. None of them is a client
// with keyboard focus, so each must find the keyboard that typed a key, step
// aside for a native exclusive-keyboard layer, and repeat a held key itself.
#pragma once

#include "queries.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <chrono>

namespace NAwesome::Keys {

    // The keyboard that typed this key. The emission carries no device, and
    // the seat's keyboard switches only when a key reaches a client, never
    // while a prompt swallows them: with two keyboards (a laptop's and an
    // external one) the seat's would translate the other device's key with
    // its own layout and held modifiers. A device records the press before
    // it emits it, so the one holding the key is the typist; a synthesized
    // repeat finds the key still held there.
    inline SP<IKeyboard> keyboardFor(uint32_t keycode) {
        if (g_pInputManager)
            for (const auto& K : g_pInputManager->m_keyboards)
                if (K && K->m_enabled && K->m_xkbState && K->getPressed(keycode))
                    return K;
        return g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
    }

    // some enabled keyboard still holds the key: a repeat's liveness (a
    // device unplugged mid-hold never sends the release)
    inline bool held(uint32_t keycode) {
        if (g_pInputManager)
            for (const auto& K : g_pInputManager->m_keyboards)
                if (K && K->m_enabled && K->getPressed(keycode))
                    return true;
        return false;
    }

    // An exclusive-keyboard layer (a launcher or a power menu a bind opened
    // over the prompt) owns typing natively: a prompt steps aside instead of
    // eating what is typed into it.
    inline bool exclusiveLayer() {
        return g_pInputManager && !g_pInputManager->m_exclusiveKeyboardLSes.empty();
    }

    // The compositor repeats only its own binds; clients repeat on their
    // side, and a prompt is no client. A held key repeats here, at its
    // keyboard's own delay and rate, from the event loop (never inside an
    // emission), until its release, another press, a lock, an input capture
    // or the key no longer being held anywhere. The owner replays the press
    // in fn and filters the keys that act once before start(). Releases are
    // never touched (invariant 3).
    class CRepeat {
      public:
        using FN = void (*)(uint32_t keycode);

        void init(FN fn) {
            m_fn = fn;
            if (!g_pEventLoopManager)
                return;
            m_timer = makeShared<CEventLoopTimer>(std::nullopt, [this](SP<CEventLoopTimer>, void*) { tick(); }, nullptr);
            g_pEventLoopManager->addTimer(m_timer);
        }

        void teardown() {
            m_key = 0;
            if (m_timer && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(m_timer);
            m_timer.reset();
        }

        void start(const SP<IKeyboard>& KB, uint32_t keycode) {
            if (!KB || !KB->m_xkbState || KB->m_repeatRate <= 0 || !m_timer)
                return;
            if (!xkb_keymap_key_repeats(xkb_state_get_keymap(KB->m_xkbState), keycode + 8))
                return;
            m_key = keycode;
            m_gap = std::max(1, 1000 / KB->m_repeatRate);
            m_timer->updateTimeout(std::chrono::milliseconds(std::max(KB->m_repeatDelay, 1)));
        }

        void stop() {
            if (!m_key)
                return; // nothing armed: every key press anywhere lands here
            m_key = 0;
            if (m_timer)
                m_timer->updateTimeout(std::nullopt);
        }

        // a press of any key ends the repeat (a keyboard's own rule), the
        // release of the repeating one too
        void onKey(const IKeyboard::SKeyEvent& e) {
            if (e.state == WL_KEYBOARD_KEY_STATE_PRESSED || e.keycode == m_key)
                stop();
        }

      private:
        void tick() {
            if (!m_key || sessionLocked() || nativeInputCaptureActive() || exclusiveLayer() || !held(m_key)) {
                stop();
                return;
            }
            try {
                m_fn(m_key);
            } catch (...) { // a timer callback: nothing may unwind into the loop
                stop();
                return;
            }
            if (m_key && m_timer)
                m_timer->updateTimeout(std::chrono::milliseconds(m_gap));
        }

        FN                  m_fn  = nullptr;
        SP<CEventLoopTimer> m_timer;
        uint32_t            m_key = 0; // evdev code; 0 = none
        int                 m_gap = 0; // ms between repeats
    };

} // namespace NAwesome::Keys
