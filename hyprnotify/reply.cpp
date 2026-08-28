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
// Editing keeps byte offsets on UTF-8 boundaries and a bounded selection.
// Clipboard transfer is asynchronous through common/clipboard.hpp; an
// untrusted selection can never block dispatch or grow the reply past 2 KiB.

#include "common/clipboard.hpp"
#include "ui.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>

namespace NHyprnotify {

    static constexpr size_t            MAX_REPLY_BYTES = 2000;

    static uint32_t                    s_id = 0; // the card whose field is armed; 0 = none
    static std::string                 s_text;
    static size_t                      s_cursor = 0, s_anchor = 0;
    static NHyprCommon::CClipboardRead s_clipboard;

    static size_t                      prevChar(size_t p) {
        while (p > 0 && ((unsigned char)s_text[--p] & 0xc0) == 0x80) {}
        return p;
    }

    static size_t nextChar(size_t p) {
        if (p < s_text.size())
            for (p++; p < s_text.size() && ((unsigned char)s_text[p] & 0xc0) == 0x80; p++) {}
        return p;
    }

    static size_t prevWord(size_t p) {
        while (p > 0 && s_text[p - 1] == ' ')
            p--;
        while (p > 0 && s_text[p - 1] != ' ')
            p = prevChar(p);
        return p;
    }

    static size_t nextWord(size_t p) {
        while (p < s_text.size() && s_text[p] == ' ')
            p = nextChar(p);
        while (p < s_text.size() && s_text[p] != ' ')
            p = nextChar(p);
        return p;
    }

    static std::pair<size_t, size_t> selection() {
        return std::minmax(s_cursor, s_anchor);
    }

    static bool eraseSelection() {
        const auto [A, B] = selection();
        if (A == B)
            return false;
        s_text.erase(A, B - A);
        s_cursor = s_anchor = A;
        return true;
    }

    static void moveCursor(size_t target, bool extend) {
        target = std::min(target, s_text.size());
        if (!extend && s_cursor != s_anchor) {
            const auto [A, B] = selection();
            target            = target < s_cursor ? A : B;
        }
        s_cursor = target;
        if (!extend)
            s_anchor = target;
        notifChanged();
    }

    static void moveBoundary(size_t target, bool extend) {
        s_cursor = std::min(target, s_text.size());
        if (!extend)
            s_anchor = s_cursor;
        notifChanged();
    }

    static void insert(std::string text) {
        for (char& C : text)
            if (C == '\n' || C == '\r' || C == '\t')
                C = ' ';
        std::erase_if(text, [](unsigned char C) { return C < 0x20 || C == 0x7f; });

        const auto [A, B] = selection();
        const size_t BASE = s_text.size() - (B - A);
        if (BASE >= MAX_REPLY_BYTES)
            text.clear();
        else if (text.size() > MAX_REPLY_BYTES - BASE) {
            text.resize(MAX_REPLY_BYTES - BASE);
            while (!text.empty() && ((unsigned char)text.back() & 0xc0) == 0x80)
                text.pop_back();
            if (!text.empty()) {
                const unsigned char LEAD = text.back();
                const size_t        NEED = LEAD < 0x80 ? 1 : (LEAD & 0xe0) == 0xc0 ? 2 : (LEAD & 0xf0) == 0xe0 ? 3 : (LEAD & 0xf8) == 0xf0 ? 4 : 1;
                if (NEED > 1)
                    text.pop_back();
            }
        }

        s_text.replace(A, B - A, text);
        s_cursor = s_anchor = A + text.size();
        notifChanged();
    }

    static void requestPaste() {
        const uint32_t ID = s_id;
        s_clipboard.request(MAX_REPLY_BYTES, [ID](std::string text) {
            if (s_id == ID && replyArmed())
                insert(std::move(text));
        });
    }

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
        s_cursor = s_anchor = 0;
        s_clipboard.cancel();
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
    size_t replyCursor() {
        return s_cursor;
    }
    std::pair<size_t, size_t> replySelection() {
        return selection();
    }

    void replyOpen(uint32_t id) {
        if (s_id == id)
            return;
        s_id = id;
        s_text.clear();
        s_cursor = s_anchor = 0;
        s_clipboard.cancel();
        notifChanged();
    }

    void replyClose() {
        if (!s_id)
            return;
        s_id = 0;
        s_text.clear();
        s_cursor = s_anchor = 0;
        s_clipboard.cancel();
        notifChanged();
    }

    void replyExit() { // teardown: no notifChanged, the model is going away
        s_id = 0;
        s_text.clear();
        s_cursor = s_anchor = 0;
        s_clipboard.cancel();
    }

    // A key press while the field is armed. The center has no general keyboard
    // map; once this field is visible it owns the complete pressed-key stream.
    bool replyKey(xkb_state* state, uint32_t keycode) {
        if (!s_id || !state)
            return false;

        const auto SYM   = xkb_state_key_get_one_sym(state, keycode);
        const bool CTRL = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
        const bool ALT   = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
        const bool SHIFT = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;

        if (CTRL && !ALT) {
            switch (SYM) {
                case XKB_KEY_a:
                case XKB_KEY_A:
                    s_anchor = 0;
                    s_cursor = s_text.size();
                    notifChanged();
                    return true;
                case XKB_KEY_e:
                case XKB_KEY_E: moveBoundary(s_text.size(), SHIFT); return true;
                case XKB_KEY_b:
                case XKB_KEY_B: moveCursor(prevChar(s_cursor), SHIFT); return true;
                case XKB_KEY_f:
                case XKB_KEY_F: moveCursor(nextChar(s_cursor), SHIFT); return true;
                case XKB_KEY_d:
                case XKB_KEY_D:
                    if (!eraseSelection() && s_cursor < s_text.size())
                        s_text.erase(s_cursor, nextChar(s_cursor) - s_cursor);
                    notifChanged();
                    return true;
                case XKB_KEY_h:
                case XKB_KEY_H:
                    if (!eraseSelection() && s_cursor > 0) {
                        const auto FROM = prevChar(s_cursor);
                        s_text.erase(FROM, s_cursor - FROM);
                        s_cursor = s_anchor = FROM;
                    }
                    notifChanged();
                    return true;
                case XKB_KEY_u:
                case XKB_KEY_U:
                    if (!eraseSelection()) {
                        s_text.erase(0, s_cursor);
                        s_cursor = s_anchor = 0;
                    }
                    notifChanged();
                    return true;
                case XKB_KEY_w:
                case XKB_KEY_W: {
                    if (!eraseSelection()) {
                        const auto FROM = prevWord(s_cursor);
                        s_text.erase(FROM, s_cursor - FROM);
                        s_cursor = s_anchor = FROM;
                    }
                    notifChanged();
                    return true;
                }
                case XKB_KEY_v:
                case XKB_KEY_V: requestPaste(); return true;
                default: return true; // an armed reply owns the complete key stream
            }
        }

        if (ALT && !CTRL) {
            switch (SYM) {
                case XKB_KEY_b:
                case XKB_KEY_B: moveCursor(prevWord(s_cursor), SHIFT); return true;
                case XKB_KEY_f:
                case XKB_KEY_F: moveCursor(nextWord(s_cursor), SHIFT); return true;
                case XKB_KEY_d:
                case XKB_KEY_D:
                    if (!eraseSelection())
                        s_text.erase(s_cursor, nextWord(s_cursor) - s_cursor);
                    notifChanged();
                    return true;
                default: return true;
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
            case XKB_KEY_Left: moveCursor(prevChar(s_cursor), SHIFT); return true;
            case XKB_KEY_Right: moveCursor(nextChar(s_cursor), SHIFT); return true;
            case XKB_KEY_Home: moveBoundary(0, SHIFT); return true;
            case XKB_KEY_End: moveBoundary(s_text.size(), SHIFT); return true;
            case XKB_KEY_Delete:
                if (!eraseSelection() && s_cursor < s_text.size())
                    s_text.erase(s_cursor, nextChar(s_cursor) - s_cursor);
                notifChanged();
                return true;
            case XKB_KEY_BackSpace: {
                if (!eraseSelection() && s_cursor > 0) {
                    const auto FROM = prevChar(s_cursor);
                    s_text.erase(FROM, s_cursor - FROM);
                    s_cursor = s_anchor = FROM;
                }
                notifChanged();
                return true;
            }
            default: break;
        }

        // anything that types a character types into the field
        char       buf[8]{};
        const int  N = xkb_state_key_get_utf8(state, keycode, buf, sizeof buf);
        if (N > 0 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f) {
            insert(std::string{buf, (size_t)N});
            return true;
        }
        // A key with no character and no meaning here (F-keys, arrows, or a
        // modifier chord) is still consumed while the field is armed. This
        // prevents a desktop binding from firing through the reply surface.
        return true;
    }

} // namespace NHyprnotify
