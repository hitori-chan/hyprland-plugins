// The inline-reply field.
//
// KDE's inline-reply protocol is the one the Linux chat apps speak: a sender
// that sees the "inline-reply" capability adds an action keyed `inline-reply`
// and waits for a NotificationReplied signal. Telegram Desktop checks the
// capability and offers NO reply affordance at all when it is missing, so
// advertising it is the whole difference between replying from a banner and
// having to raise the app.
//
// The field opens on demand — Reply in the card menu (or Tab on the
// center's selection) — under the card's words, in the banner or the center
// card alike; the banner holds its clock while it is open.
//
// The hard part is that we have no keyboard focus to give a text field: the
// cards are drawn by the compositor, not by a layer surface with
// keyboard_interactivity. So the field TAKES the keyboard the way the shell's
// menubar prompt does — while one is armed every key press is ours, and the
// ones we have no use for are simply dropped rather than leaking into
// whatever holds focus underneath. Releases always pass (crash class 3).
//
// Editing is deliberately append-and-backspace, not a readline: a reply box
// is a sentence, and a caret that cannot move needs no measurement of text it
// is not at the end of (which would re-key the raster cache per keystroke).

#include "ui.hpp"

#include "../core/hop.hpp"
#include "../core/keys.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>

namespace NAwesome::Notify {

    static uint32_t    s_id = 0; // the card whose field is armed; 0 = none
    static std::string s_text;
    static std::string s_target; // replyTarget of the card when armed
    // the send changes the model (the card goes): out of the key emission
    static NAwesome::CHop s_pendingSend;
    static NAwesome::Keys::CRepeat s_repeat; // a held key types on (core/keys.hpp)

    // the card's field can show: it is a banner, or a card of the open center
    static bool onScreen(const SP<SNotif>& n) {
        return !n->waiting && (centerVisible() ? !Model::offShade(n) : n->banner);
    }

    // Self-healing: the card can be dismissed, expire or be swept while the
    // user is mid-sentence, the center can close under it, its sender can
    // leave the bus (canReply drops) and a replace can move it to another
    // conversation. A stale arm would go on eating every keystroke in the
    // session and send into nowhere, so the check IS the liveness test
    // rather than something a caller has to remember.
    bool               replyArmed() {
        if (!s_id)
            return false;
        if (const auto N = Model::byId(s_id); N && N->canReply && replyTarget(*N) == s_target && onScreen(N))
            return true;
        s_id = 0;
        s_text.clear();
        s_repeat.stop();
        return false;
    }
    uint32_t replyId() {
        return s_id;
    }
    bool replyHeld() {
        return s_id != 0;
    }
    bool replyArmedOn(uint32_t id) {
        return s_id != 0 && s_id == id;
    }
    const std::string& replyText() {
        return s_text;
    }
    const std::string& replyTargetKey() {
        return s_target;
    }

    void replyOpen(uint32_t id) {
        if (s_id == id)
            return;
        const auto N = Model::byId(id);
        if (!N || !N->canReply || !onScreen(N)) // replaced or gone since its menu was laid out
            return;
        s_repeat.stop();
        s_id     = id;
        s_target = replyTarget(*N);
        s_text.clear();
        refreshBannerHold(); // a banner holds its clock while its field is open
        notifChanged();
    }

    void replySendRefused(uint32_t id, const std::string& text, const std::string& target) {
        const auto N = Model::byId(id);
        if (!N || !N->canReply || replyTarget(*N) != target || !onScreen(N) || (s_id && s_id != id))
            return; // the card moved on (or another field is armed): nothing to put back into
        s_id     = id;
        s_target = target;
        s_text   = text;
        refreshBannerHold();
        notifChanged();
    }

    void replyClose() {
        s_repeat.stop();
        if (!s_id)
            return;
        s_id = 0;
        s_text.clear();
        refreshBannerHold();
        notifChanged();
    }

    void replyExit() { // a lock, the panel gone, teardown: no notifChanged
        s_id = 0;
        s_text.clear();
        s_pendingSend.reset();
        s_repeat.stop();
    }

    static void replyRepeat(uint32_t keycode) {
        if (!replyArmed()) {
            s_repeat.stop();
            return;
        }
        replyKey(NAwesome::Keys::keyboardFor(keycode), keycode, /*repeating=*/true);
    }

    void replyInit() {
        s_repeat.init(replyRepeat);
    }

    void replyTeardown() {
        replyExit();
        s_repeat.teardown();
    }

    void replyKeyEvent(const IKeyboard::SKeyEvent& e) {
        s_repeat.onKey(e);
    }

    // A key press while the field is armed. False means "not ours" — the
    // caller passes it on, so a chord the user bound still works.
    bool replyKey(const SP<IKeyboard>& KB, uint32_t evdev, bool repeating) {
        xkb_state* const state = KB ? KB->m_xkbState : nullptr;
        if (!s_id || !state)
            return false;
        const xkb_keycode_t keycode = evdev + 8;
        // a key that types (or erases) keeps typing while held
        const auto typed = [&]() {
            if (!repeating)
                s_repeat.start(KB, evdev);
            notifChanged();
            return true;
        };

        const auto SYM = xkb_state_key_get_one_sym(state, keycode);
        const bool CTRL = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
        const bool ALT  = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
        const bool LOGO = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0;

        if (LOGO || ALT)
            return false; // window-management chords are never the field's

        if (SYM >= XKB_KEY_Shift_L && SYM <= XKB_KEY_Hyper_R)
            return false; // a bare modifier press is not a character; swallowing it
                          // would keep the press out of the keybind layer's input
                          // state and break side-constrained binds (SUPER_L + …)

        if (CTRL) {
            switch (SYM) {
                case XKB_KEY_u: // readline's kill-line, the one chord worth keeping
                case XKB_KEY_U:
                    s_text.clear();
                    notifChanged();
                    return true;
                case XKB_KEY_w: // and kill-word, for the same reason
                case XKB_KEY_W: {
                    while (!s_text.empty() && s_text.back() == ' ')
                        s_text.pop_back();
                    while (!s_text.empty() && s_text.back() != ' ')
                        s_text.pop_back();
                    notifChanged();
                    return true;
                }
                default: return false; // every other Ctrl chord belongs to the user
            }
        }

        switch (SYM) {
            case XKB_KEY_Escape:
                replyClose();
                return true;
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter: {
                const auto ID = s_id;
                auto       TX = s_text; // replyClose wipes the buffer under us
                auto       TG = s_target;
                replyClose();
                if (!TX.empty())
                    s_pendingSend.arm([ID, TX = std::move(TX), TG = std::move(TG)]() {
                        if (!Bus::sendReply(ID, TX, TG))
                            replySendRefused(ID, TX, TG);
                    });
                return true;
            }
            case XKB_KEY_BackSpace: {
                // one UTF-8 codepoint, not one byte
                while (!s_text.empty() && ((unsigned char)s_text.back() & 0xc0) == 0x80)
                    s_text.pop_back();
                if (!s_text.empty())
                    s_text.pop_back();
                return typed();
            }
            default: break;
        }

        // anything that types a character types into the field
        // the return is the length NEEDED (snprintf-style): past the buffer
        // the bytes were cut, and appending N would read past it
        char       buf[64]{};
        const int  N = xkb_state_key_get_utf8(state, keycode, buf, sizeof buf);
        if (N > 0 && (size_t)N < sizeof buf && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f) {
            if (s_text.size() < 2000) // a reply, not a document
                s_text.append(buf, (size_t)N);
            return typed();
        }
        // function and media keys are the user's binds (the shade's own F12
        // toggle, volume, brightness): they pass while the field is armed
        if ((SYM >= XKB_KEY_F1 && SYM <= XKB_KEY_F35) || (SYM >> 16) == 0x1008)
            return false;
        // a key with no character and no meaning here (arrows, Home): the
        // field swallows it rather than letting it act on the window beneath,
        // because the user is typing, not driving the desktop
        return true;
    }

} // namespace NAwesome::Notify
