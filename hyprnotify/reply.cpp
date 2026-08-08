// hyprnotify/reply.cpp — the inline-reply field.
//
// KDE's inline-reply protocol is the one the Linux chat apps speak: a sender
// that sees the "inline-reply" capability adds an action keyed `inline-reply`
// and waits for a NotificationReplied signal. Telegram Desktop checks the
// capability and offers NO reply affordance at all when it is missing, so
// advertising it is the whole difference between replying from the shade and
// having to raise the app.
//
// The hard part is that we have no keyboard focus to give a text field: the
// shade is drawn by the compositor, not by a layer surface with
// keyboard_interactivity. So the field TAKES the keyboard the way hyprbar's
// menubar prompt does — while one is armed every key press is ours, and the
// ones we have no use for are simply dropped rather than leaking into
// whatever holds focus underneath. Releases always pass (crash class 3).
//
// Editing is deliberately append-and-backspace, not a readline: a reply box
// is a sentence, and a caret that cannot move needs no measurement of text it
// is not at the end of (which would re-key the raster cache per keystroke).

#include "ui.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>

namespace NHyprnotify {

    static uint32_t    s_id = 0; // the card whose field is armed; 0 = none
    static std::string s_text;

    // Self-healing: the card can be dismissed, expire or be swept while the
    // user is mid-sentence, and the shade can close under it. A stale arm
    // would go on eating every keystroke in the session, so the check IS the
    // liveness test rather than something a caller has to remember.
    bool               replyArmed() {
        if (!s_id)
            return false;
        if (centerVisible()) {
            const bool MODEL_ALIVE = std::ranges::any_of(notifs, [](const auto& N) { return N->id == s_id && N->canReply; });
            const bool FIELD_VISIBLE = std::ranges::any_of(cards, [](const auto& C) {
                return (C.kind == SCard::ROW || C.kind == SCard::CHILD) && C.id == s_id && C.replyField.w > 0;
            });
            if (MODEL_ALIVE && FIELD_VISIBLE)
                return true;
        }
        s_id = 0;
        s_text.clear();
        return false;
    }
    bool replyArmedOn(uint32_t id) {
        if (s_id == 0 || s_id != id)
            return false;
        return std::ranges::any_of(notifs, [id](const auto& N) { return N->id == id && N->canReply; });
    }
    const std::string& replyText() {
        return s_text;
    }

    void replyOpen(uint32_t id) {
        if (s_id == id)
            return;
        s_id = id;
        s_text.clear();
        notifChanged();
    }

    void replyClose() {
        if (!s_id)
            return;
        s_id = 0;
        s_text.clear();
        notifChanged();
    }

    void replyExit() { // teardown: no notifChanged, the model is going away
        s_id = 0;
        s_text.clear();
    }

    // A key press while the field is armed. The center has no general keyboard
    // map; once this field is visible it owns the complete pressed-key stream.
    bool replyKey(xkb_state* state, uint32_t keycode) {
        if (!s_id || !state)
            return false;

        const auto SYM = xkb_state_key_get_one_sym(state, keycode);
        const bool CTRL = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;

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
                default: return true; // an armed reply owns the complete key stream
            }
        }

        switch (SYM) {
            case XKB_KEY_Escape:
                replyClose();
                return true;
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter: {
                if (s_text.empty())
                    return true; // the disabled Send state keeps the draft open
                const auto ID = s_id;
                auto       TX = s_text; // replyClose wipes the buffer under us
                replyClose();
                Bus::sendReply(ID, TX);
                return true;
            }
            case XKB_KEY_BackSpace: {
                // one UTF-8 codepoint, not one byte
                while (!s_text.empty() && ((unsigned char)s_text.back() & 0xc0) == 0x80)
                    s_text.pop_back();
                if (!s_text.empty())
                    s_text.pop_back();
                notifChanged();
                return true;
            }
            default: break;
        }

        // anything that types a character types into the field
        char       buf[8]{};
        const int  N = xkb_state_key_get_utf8(state, keycode, buf, sizeof buf);
        if (N > 0 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f) {
            if (s_text.size() < 2000) // a reply, not a document
                s_text.append(buf, (size_t)N);
            notifChanged();
            return true;
        }
        // A key with no character and no meaning here (F-keys, arrows, or a
        // modifier chord) is still consumed while the field is armed. This
        // prevents a desktop binding from firing through the reply surface.
        return true;
    }

} // namespace NHyprnotify
