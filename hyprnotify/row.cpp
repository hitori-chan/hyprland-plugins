// hyprnotify/row.cpp — the v13 shade card.
//
// One card anatomy serves every shade card (spec §2, demo scenes 1–7): a
// 37px circular lead icon at the 15px inset, a two-tone kicker, and — per
// kind — a title + body (PLAIN), a message preview that opens into one kid
// per message (1:1) or per sender (group) (CONV), or a title + body preview
// that opens into one kid per child (BCHILD bundles). Cards are stroke-free
// tiles; the expand chip (stadium, 25x17) is the only affordance on the
// collapsed surface, actions live in the expanded state, and conversation
// actions live inside the expanded kid.
//
// measure == paint: the measure* wrappers run the SAME layout code with a
// warm context (every SPaint draw no-ops while warm) and read back the
// measured height. Hit boxes are pushed only on the drawing pass.
//
// Where the spec and the approved demo disagree on a concrete value, the
// demo wins (ledger A-138): the collapsed conversation preview keeps the
// AOSP "Name: text" colon in both 1:1 and group (the ui.hpp comment's
// bullet normalization is the overridden side), the bundle kid avatar is
// the 37px app icon, and the group kicker carries the "N new messages" run.

#include "ui.hpp"

namespace NHyprnotify {

    // conversation messages carry epoch ms; the age buckets want steady
    static std::string msgAge(int64_t ms) {
        if (ms <= 0)
            return "now";
        return ageString(Time::steadyNow() - std::chrono::milliseconds(ms));
    }

    // the measured height of the last paint* run in this file (main thread
    // only, one card at a time): the measure* wrappers read it back
    static double s_measuredH = 0;

    // ---- the two-tone kicker: the on-82 identity run + the on-60 tail ----

    struct SKick {
        const SCachedText* l = nullptr;
        const SCachedText* r = nullptr;
        double             h = KICK_MIN_H;
    };

    // the header line: `titleLead` renders the left run as the pixel-parity
    // semibold title (on, T.title, 600) instead of the summary kicker tone
    static SKick kick2(const SPaint& P, const SType& T, const std::string& left, const std::string& right, double maxW, bool titleLead = false) {
        const int LX = std::max(1, (int)std::lround(maxW * P.scale));
        const int LH = linePx(T.header);
        auto&      SB = scratch();
        appendEsc(SB, right);
        const auto KR = right.empty() ? nullptr : cachedText(SB, v13On60(), T.header, LX, LH, 0, false, 400);
        const double RW = texW(KR, P.scale);
        SB.clear(); // appendEsc accumulates; the left run is its own texture
        appendEsc(SB, left);
        const int  LW = std::max(1, (int)std::lround(std::max(0.0, maxW - RW) * P.scale));
        const auto KL = left.empty() ? nullptr
                                     : (titleLead ? cachedText(SB, v13OnT(), T.title, LW, linePx(T.title), 0, false, 600)
                                                  : cachedText(SB, v13On82(), T.header, LW, LH, 0, false, 500));
        return {KL, KR, std::max(KICK_MIN_H, std::max(texH(KL, P.scale), texH(KR, P.scale)))};
    }

    static void paintKick(const SPaint& P, const SKick& K, double x, double y) {
        if (K.l && K.l->tex)
            P.tex(K.l->tex, x, y);
        if (K.r && K.r->tex) // the tail rides the lead's baseline
            P.tex(K.r->tex, x + (K.l ? texW(K.l, P.scale) : 0), y + (texH(K.l, P.scale) - texH(K.r, P.scale)));
    }

    // ---- the lead icon: a 37px circle, chip disc + initial when none ----

    // mirror of paintIconColumn's lead resolution (paint.cpp keeps its own
    // file-static): tells us whether the shared recipe drew, so the disc
    // fallback fires only when it did not
    static SP<ITexture> leadResolved(const SNotif& n) {
        if (!n.conversation)
            return texReady(n.identTex) ? n.identTex : n.iconTex;
        if (texReady(n.conversationTex))
            return n.conversationTex;
        if (n.conversationKind == "group" && n.participants.size() >= 2) {
            if (texReady(n.identTex))
                return n.identTex;
            if (texReady(n.iconTex))
                return n.iconTex;
            return nullptr;
        }
        if (!n.participants.empty() && texReady(n.participants.front().avatarTex))
            return n.participants.front().avatarTex;
        if (texReady(n.iconTex))
            return n.iconTex;
        if (texReady(n.identTex))
            return n.identTex;
        return nullptr;
    }

    static void paintLead(const SPaint& P, const SNotif& N, const CBox& cell) {
        const int R = (int)std::lround(cell.w / 2 * P.scale);
        const auto LEAD = leadResolved(N);
        if (N.conversation)
            paintIconColumn(P, N, cell, true, rPow()); // draws the lead + the app badge when one resolved
        else if (texReady(LEAD))
            P.texCover(LEAD, cell, R, 2.f);
        if (texReady(LEAD))
            return;
        // no artwork: the chip disc wears the app initial (demo .k-av)
        P.rect(cell, v13Chip(), R, 2.f);
        std::string name = N.appName;
        if (name.empty())
            name = titleForDisplay(N);
        std::string init;
        if (!name.empty())
            init = Pixel::firstCodepoint(name, 0);
        if (init.empty())
            init = "?";
        const auto GLYPH = cachedText(init, v13On(), (int)std::lround(15 * P.scale), (int)(cell.w * P.scale) + 8, linePx((int)std::lround(15 * P.scale)), 0, false, 600);
        if (GLYPH && GLYPH->tex)
            P.tex(GLYPH->tex, cell.x + (cell.w - GLYPH->tex->m_size.x / P.scale) / 2, cell.y + (cell.h - GLYPH->tex->m_size.y / P.scale) / 2);
    }

    // ---- action rows: borderless tinted text buttons (ROM: plain .act) ----

    struct SAct {
        const std::string*   id;
        const std::string*   label;
        const SP<ITexture>*  icon; // null = text only
    };

    // the synthetic inline-reply entry leads when the sender offered it; the
    // rest are the card's actions in Notify order ("default" never buttons)
    static void actionsOf(const SNotif& N, std::vector<SAct>& out) {
        out.clear();
        static const std::string  REPLY_ID  = "inline-reply";
        static const std::string  REPLY_LBL = "Reply";
        static const SP<ITexture> NONE      = nullptr;
        if (N.canReply)
            out.push_back({&REPLY_ID, N.replyActionText.empty() ? &REPLY_LBL : &N.replyActionText, &NONE});
        for (const auto& A : N.actions)
            out.push_back({&A.id, &A.label, &A.iconTex});
    }

    // measures the button rows under maxW; boxes are RELATIVE to the row's
    // origin, parallel to `acts`. Returns the total height (0 = none).
    static double layoutActions(const SPaint& P, const SType& T, std::vector<SAct>& acts, std::vector<CBox>& boxes, double maxW) {
        boxes.clear();
        if (acts.empty())
            return 0;
        const int MAXW = (int)std::max(1.0, maxW * P.scale);
        double    bx   = 0, rowY = 0;
        for (const auto& A : acts) {
            auto& SB = scratch();
            appendEsc(SB, *A.label);
            const auto   LBL = cachedText(SB, v13Action(), T.action, MAXW, linePx(T.action), 0, false, 500);
            const double LW  = texW(LBL, P.scale);
            const double IW  = A.icon ? BTN_ICON + BTN_ICON_GAP : 0;
            const double BW  = std::max(BTN_MIN_W, IW + LW + 2 * BTN_PADX);
            if (bx > 0 && bx + BW > maxW + 0.5) {
                bx = 0;
                rowY += BTN_H + BTN_GAP;
            }
            boxes.push_back(CBox{bx, rowY, BW, BTN_H});
            bx += BW + BTN_GAP;
        }
        return rowY + BTN_H;
    }

    // draws the laid-out rows at (x0, y0) and records the absolute boxes (the
    // draw pass is the one that pushes hits, from the same record); `childKey`
    // disambiguates the kid rows of one card ("" for the card itself)
    static void paintActions(const SPaint& P, const SType& T, SCard::eKind cardKind, uint32_t id, const std::string& group, const std::string& childKey,
                             std::vector<SAct>& acts, std::vector<CBox>& boxes, double x0, double y0, std::vector<CBox>& absOut) {
        absOut.clear();
        for (size_t i = 0; i < boxes.size(); i++) {
            const CBox B{x0 + boxes[i].x, y0 + boxes[i].y, boxes[i].w, boxes[i].h};
            absOut.push_back(B);
            const bool HOV = hovered.kind == cardKind && hovered.id == id && hovered.group == group && hovered.childKey == childKey && hovered.btn == (int)i;
            if (HOV)
                P.rect(B, v13RaisedH(), (int)std::lround(BTN_R * P.scale), rPow());
            double cx = B.x + BTN_PADX;
            if (acts[i].icon && texReady(*acts[i].icon)) {
                P.texFit(*acts[i].icon, CBox{cx, B.y + (B.h - BTN_ICON) / 2, BTN_ICON, BTN_ICON}, 0);
                cx += BTN_ICON + BTN_ICON_GAP;
            }
            auto& SB = scratch();
            appendEsc(SB, *acts[i].label);
            const auto LBL = cachedText(SB, v13Action(), T.action, (int)(B.w * P.scale), linePx(T.action), 0, false, 500);
            if (LBL && LBL->tex)
                P.tex(LBL->tex, cx, B.y + (B.h - LBL->tex->m_size.y / P.scale) / 2);
        }
    }

    // ---- the expand chip: a 25x17 stadium, or the count pill ----

    static double chipCountText(const SType& T, double scale, int count) {
        auto& SB = scratch();
        SB += std::to_string(count);
        const auto N = cachedText(SB, v13PillFg(), T.header, 64, linePx(T.header), 0, false, 600);
        return texW(N, scale);
    }

    static double chipW(const SType& T, double scale, int count) {
        if (count < 0)
            return CHIP_W;
        return CHIP_COUNT_PL + chipCountText(T, scale, count) + 2 + CHEV_D_COUNT + CHIP_COUNT_PR;
    }

    static void paintChip(const SPaint& P, const SType& T, double x, double y, bool open, int count, const CHyprColor& bg, const CHyprColor& fg) {
        const double H = count < 0 ? CHIP_H : CHIP_COUNT_H;
        const double W = chipW(T, P.scale, count);
        P.rect(CBox{x, y, W, H}, bg, (int)std::lround(H / 2 * P.scale), 2.f);
        double cx = x;
        if (count >= 0) {
            auto& SB = scratch();
            SB += std::to_string(count);
            const auto N = cachedText(SB, fg, T.header, 64, linePx(T.header), 0, false, 600);
            if (N && N->tex)
                P.tex(N->tex, cx + CHIP_COUNT_PL, y + (H - N->tex->m_size.y / P.scale) / 2);
            cx += CHIP_COUNT_PL + chipCountText(T, P.scale, count);
        }
        const bool NUM = count >= 0;
        const int  CPX = (int)std::lround((NUM ? CHEV_D_COUNT : CHEV_D) * P.scale);
        const auto CHEV = controlIcon(open ? eControlIcon::EXPAND_LESS : eControlIcon::EXPAND_MORE, CPX, fg);
        if (CHEV)
            P.texFit(CHEV, CBox{cx + 2, y + (H - (NUM ? CHEV_D_COUNT : CHEV_D)) / 2, NUM ? CHEV_D_COUNT : CHEV_D, NUM ? CHEV_D_COUNT : CHEV_D}, 0);
    }

    // ---- the kid rows: one per message (1:1), sender (group), child (bundle) ----

    struct SKid {
        std::string key; // "<cardId>|<suffix>" — the expansion state's key
        std::string who; // sender name, or the child's title
        std::string pkey; // participant key (avatar lookup)
        std::vector<std::string> lines; // text lines, newest first
        std::string time; // bucketed age
        uint32_t    target = 0; // invoke target: the card id (bundle: the child's)
        bool        media  = false; // the media glyph leads the newest line
        bool        exp    = false; // level 2: time + name + actions (user state)
        bool        reply  = false; // wears the armed reply field instead of actions
    };

    // the kid cap: explicit user expansion is the only expansion v13 has
    static size_t kidCap(eCardKind kind) {
        return Pixel::maxVisibleChildren(kind == eCardKind::BCHILD, Pixel::eExpansion::USER_EXPANDED);
    }

    static void convKids(const SNotif& N, std::vector<SKid>& out) {
        out.clear();
        // ROM contract (2026-08-18 captures): one child row per message for BOTH
        // 1:1 and group conversations, newest first. Each message gets its own
        // avatar, timestamp, chevron, and actions. The collapsed preview (handled
        // separately at line 329-338) is the only place that shows per-sender
        // aggregation; the expanded state always shows one row per notification.
        // presentedMessageStart skips historic messages; we iterate BACKWARD from
        // the end to produce newest-first child rows (messages vector is sorted
        // chronologically oldest-first).
        const size_t START = Pixel::presentedMessageStart(N.messages);
        for (size_t mi = N.messages.size(); mi-- > START;) {
            if (out.size() >= kidCap(eCardKind::CONV))
                break;
            const auto& M = N.messages[mi];
            out.push_back({.key = std::to_string(N.id) + "|m:" + M.id,
                           .who = M.senderName,
                           .pkey = M.senderId,
                           .lines = {M.text},
                           .time = msgAge(M.timestampMs),
                           .target = N.id});
        }
        // the media glyph belongs to the newest message line (demo sc5)
        if (!out.empty() && !N.image.empty())
            out.front().media = true;
        // the armed reply rides the newest kid; the reply itself stays
        // per-notification (ledger A-138 simplification)
        if (!out.empty() && replyArmedOn(N.id)) {
            out.front().exp   = true;
            out.front().reply = true;
        }
        for (auto& K : out)
            if (!K.reply)
                K.exp = centerKidExpanded(K.key);
    }

    static void bundleKids(const SDisp& D, std::vector<SKid>& out) {
        out.clear();
        const auto LEAD = D.items.front()->id;
        for (const auto& N : D.items) { // newest first
            if (out.size() >= kidCap(eCardKind::BCHILD))
                break;
            SKid K;
            K.key    = std::to_string(LEAD) + "|c:" + std::to_string(N->id);
            K.who    = titleForDisplay(*N);
            const auto BL = lastLine(N->body);
            if (!BL.empty())
                K.lines.push_back(BL);
            K.time   = ageString(N->arrived);
            K.target = N->id;
            out.push_back(std::move(K));
        }
    }

    // ---- the collapsed-card preview lines ----

    std::vector<SPreviewLine> previewLines(const SNotif& n, eCardKind kind, size_t cap) {
        std::vector<SPreviewLine> out;
        if (kind == eCardKind::BCHILD) {
            if (cap > 0)
                out.push_back({titleForDisplay(n), lastLine(n.body), false});
            return out;
        }
        // conversations: the newest messages, newest first — the AOSP
        // collapsed-conversation contract (one line 1:1, two for a group),
        // the "Name: text" colon kept (demo wins over the ui.hpp comment)
        size_t i = 0;
        for (auto it = n.messages.rbegin(); it != n.messages.rend() && i < cap; ++it, i++)
            out.push_back({it->senderName, it->text, false});
        if (!out.empty() && !n.image.empty())
            out.front().media = true;
        return out;
    }

    // one collapsed-conversation preview line at 18px: [mini 15] gap 6
    // [Name:] [media 15] [text, ellipsized]
    static void paintPrevLine(const SPaint& P, const SType& T, const SNotif& N, const SPreviewLine& L, double x, double y, double w) {
        const double LINE = 18;
        double       cx   = x;
        const CBox   MINI{x, y + (LINE - MINI_D) / 2, MINI_D, MINI_D};
        paintMiniAvatar(P, N, L.a, MINI);
        cx += MINI_D + PREV_ELGAP;
        auto& SB = scratch();
        appendEsc(SB, L.a);
        SB += ":";
        const auto NAME = cachedText(SB, v13On(), T.body, (int)std::max(1.0, (x + w - cx) * P.scale), linePx(T.body), 0, false, 500);
        if (NAME && NAME->tex)
            P.tex(NAME->tex, cx, y);
        cx += texW(NAME, P.scale);
        if (L.media) {
            const auto GLYPH = controlIcon(eControlIcon::MEDIA, (int)std::lround(MINI_D * P.scale), v13On60());
            if (GLYPH)
                P.texFit(GLYPH, CBox{cx + 4, y + 1, MINI_D, MINI_D}, 0);
            cx += MINI_D + 4;
        }
        SB.clear(); // the message run is its own texture, not "Name:msg"
        appendEsc(SB, L.b);
        const auto TXT = cachedText(SB, v13On82(), T.body, (int)std::max(1.0, (x + w - cx) * P.scale), linePx(T.body), 0, false, 400);
        if (TXT && TXT->tex)
            P.tex(TXT->tex, cx, y);
    }

    // ---- the armed reply field (the newest kid of a conversation card) ----

    double replyFieldH() {
        return REPLY_H;
    }

    static void paintReplyAt(const SPaint& P, const SType& T, const SP<SNotif>& N, double x, double y, double w, CBox& field, CBox& send) {
        const double H = REPLY_H;
        field = CBox{x, y, w - SEND_D, H};
        send  = CBox{x + w - SEND_D, y, SEND_D, SEND_D};
        // the draft or the placeholder, 15/18 borderless (AOSP remote_input)
        auto& SB = scratch();
        const auto DRAFT = replyArmedOn(N->id) ? replyText() : std::string{};
        const bool HAS   = !DRAFT.empty();
        appendEsc(SB, HAS ? DRAFT : N->replyPlaceholder);
        const auto TXT = cachedText(SB, HAS ? v13On() : v13On40(), T.title, (int)std::max(1.0, field.w * P.scale), linePx(T.title), 0, false, 400);
        if (TXT && TXT->tex)
            P.tex(TXT->tex, field.x, y + (H - TXT->tex->m_size.y / P.scale) / 2);
        const bool SENDHOV = hovered.kind == SCard::ROW && hovered.id == N->id && hovered.part == 4;
        if (SENDHOV && !replyText().empty())
            P.rect(send, v13RaisedH(), (int)std::lround(SEND_D / 2 * P.scale), 2.f);
        const auto GLYPH = controlIcon(eControlIcon::SEND, (int)std::lround(22 * P.scale), v13Action());
        if (GLYPH)
            P.texFit(GLYPH, CBox{send.x + (SEND_D - 22) / 2, send.y + (SEND_D - 22) / 2, 22, 22}, 0);
    }

    void paintReplyField(const SPaint& P, const SType& T, const SP<SNotif>& N, double x, double y, double w, SCard& card) {
        CBox field, send;
        paintReplyAt(P, T, N, x, y, w, field, send);
        card.replyField = field;
        card.replySend  = send;
    }

    // ---- the snooze undo row: the card's own slot while it is snoozed ----

    double snoozeRowH() {
        return 74;
    }

    void paintSnoozeRow(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box) {
        const int R  = (int)std::lround(box.h / 2 * P.scale);
        P.rect(box, v13Card(), R, rPow());
        const CBox ICON{box.x + 15, box.y + (box.h - CARD_ICON_D) / 2, CARD_ICON_D, CARD_ICON_D};
        paintLead(P, *N, ICON);
        auto& SB = scratch();
        SB += "Snoozed ";
        SB += shortDuration(N->snoozeSecs);
        const auto LBL = cachedText(SB, v13On82(), T.body, (int)std::max(1.0, (box.w - 170) * P.scale), linePx(T.body), 0, false, 400);
        if (LBL && LBL->tex)
            P.tex(LBL->tex, box.x + CARD_TEXT_X, box.y + (box.h - LBL->tex->m_size.y / P.scale) / 2);
        // the Undo button, right-aligned
        SB.clear();
        appendEsc(SB, "Undo");
        const auto U   = cachedText(SB, v13Action(), T.action, 128, linePx(T.action), 0, false, 500);
        const double UW = std::max(BTN_MIN_W, texW(U, P.scale) + 2 * BTN_PADX);
        const CBox   UB{box.x + box.w - 15 - UW, box.y + (box.h - BTN_H) / 2, UW, BTN_H};
        if (hovered.kind == SCard::SNOOZE && hovered.id == N->id && hovered.part == 8)
            P.rect(UB, v13RaisedH(), (int)std::lround(BTN_R * P.scale), rPow());
        if (U && U->tex)
            P.tex(U->tex, UB.x + BTN_PADX, UB.y + (BTN_H - U->tex->m_size.y / P.scale) / 2);

        s_measuredH = box.h;
        SCard c;
        c.kind   = SCard::SNOOZE;
        c.box    = box;
        c.id     = N->id;
        c.manage.push_back({UB, 8});
        cards.push_back(std::move(c));
    }

    // ---- the hold menu (long-press) ----
    //
    // The staged state lives in center.cpp; these readers keep the height and
    // the paint in agreement without a second layout pass.

    static int manageMode() {
        switch (centerManageMode()) {
            case Policy::eAlertingMode::PRIORITY:
                return 0;
            case Policy::eAlertingMode::SILENT:
                return 2;
            default:
                return 1;
        }
    }

    static int snoozeIndex() {
        const int64_t* const OPTS = SNOOZE_OPTS;
        const auto           S = centerManageSnoozeSecs();
        for (int i = 0; i < 4; i++)
            if (OPTS[i] == S)
                return i;
        return 2; // 1h
    }

    double holdMenuH() {
        // header: who (15/16) + conv (13/17, mt 5, conversations only) +
        // app (13/17, mt 10); the rows; the snooze section (never bundles);
        // the Dismiss/Done pair
        double H = 16 + 10 + 17;
        if (centerManageConversation())
            H += 5 + 17;
        H = std::max(MENU_HEAD_D, H);
        H += MENU_ROWS_MT + MENU_ROW_H + MENU_ROW_GAP + (manageMode() == 1 ? MENU_ROW_H_SEL : MENU_ROW_H) + MENU_ROW_GAP + MENU_ROW_H;
        if (!centerManageBundle()) {
            H += MENU_SNOOZE_MT + MENU_ROW_H;
            if (centerManageSnoozeOpen())
                H += MENU_ROW_GAP + 4 * (MENU_ROW_H + MENU_ROW_GAP) - MENU_ROW_GAP;
        }
        H += MENU_FOOT_MT + MENU_BTN_H;
        return MENU_PAD * 2 + H;
    }

    void paintHoldMenu(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, const std::string& groupKey) {
        const bool BUNDLE = !groupKey.empty();
        const int  MODE   = manageMode();
        const bool SNOZEO = centerManageSnoozeOpen();
        const int  SNOZEI = snoozeIndex();
        const int  RPX    = rRow(P.scale); // the hold tile IS a card tile

        P.rect(box, v13Card(), RPX, rPow());
        P.ring(box, v13Rim(), RPX, rPow(), 1.0);

        SCard card;
        card.kind  = SCard::MANAGE;
        card.box   = box;
        card.id    = N->id;
        card.group = groupKey;

        const double RX = box.x + MENU_PAD;
        const double RW = box.w - 2 * MENU_PAD;
        double       x  = RX;
        double       y  = box.y + MENU_PAD;

        // ---- the head: icon + the name lines (the demo's h-gear has no
        //      C++ equivalent: there are no channel settings on Wayland) ----
        const CBox ICON{x, y, MENU_HEAD_D, MENU_HEAD_D};
        paintLead(P, *N, ICON);
        x += MENU_HEAD_D + MENU_HEAD_GAP;

        std::string who = titleForDisplay(*N);
        if (N->conversation && !N->participants.empty())
            who = N->participants.front().name;
        auto& SB = scratch();
        appendEsc(SB, who);
        const auto WHO = cachedText(SB, v13On(), T.title, (int)std::max(1.0, (box.x + box.w - MENU_PAD - x) * P.scale), linePx(T.title), 0, false, 500);
        if (WHO && WHO->tex)
            P.tex(WHO->tex, x, y); // top-aligned: the lines column starts with it

        double ly = y + 16;
        if (N->conversation && !N->conversationTitle.empty()) {
            ly += 5;
            SB.clear();
            appendEsc(SB, N->conversationTitle);
            const auto CV = cachedText(SB, v13On82(), T.body, (int)std::max(1.0, (box.x + box.w - MENU_PAD - x) * P.scale), linePx(T.body), 0, false, 400);
            if (CV && CV->tex)
                P.tex(CV->tex, x, ly);
            ly += 17;
        }
        ly += 10;
        SB.clear();
        appendEsc(SB, N->appName);
        const auto AP = cachedText(SB, v13On60(), T.body, (int)std::max(1.0, (box.x + box.w - MENU_PAD - x) * P.scale), linePx(T.body), 0, false, 400);
        if (AP && AP->tex)
            P.tex(AP->tex, x, ly);
        ly += 17;
        y = box.y + MENU_PAD + std::max(MENU_HEAD_D, (double)(16 + (N->conversation && !N->conversationTitle.empty() ? 22 : 0) + 27));

        // ---- the importance rows (staged until Done) ----
        // the rows span the full menu width: the head's icon-column advance
        // above is head-local and must not leak into them
        x = RX;
        y += MENU_ROWS_MT;
        struct SRowDef {
            eControlIcon icon;
            const char*  label;
        };
        static const SRowDef ROWS[3] = {{eControlIcon::PRIORITY, "Priority"},
                                        {eControlIcon::NOTIFICATION_ALERT, "Default"},
                                        {eControlIcon::NOTIFICATION_SILENT, "Silent"}};
        for (int i = 0; i < 3; i++) {
            const bool SEL  = i == MODE;
            const bool HOVR = hovered.kind == SCard::MANAGE && hovered.id == N->id && hovered.group == groupKey && hovered.part == (uint8_t)i;
            const double RH = SEL ? MENU_ROW_H_SEL : MENU_ROW_H;
            const CBox   RB{x, y, RW, RH};
            P.rect(RB, SEL ? v13SelRow() : (HOVR ? v13Raised() : CHyprColor{0x00000000}), (int)std::lround((SEL ? 26 : PIXEL_INTERNAL_RADIUS) * P.scale), rPow());
            P.ring(RB, SEL ? v13SelRowB() : v13RowLine(), (int)std::lround((SEL ? 26 : PIXEL_INTERNAL_RADIUS) * P.scale), rPow(), 1.0);
            const auto GI = controlIcon(ROWS[i].icon, (int)std::lround(MENU_ICON_D * P.scale), SEL ? v13On() : v13On82());
            if (GI)
                P.texFit(GI, CBox{RB.x + MENU_ROW_PADX, RB.y + (RH - MENU_ICON_D) / 2, MENU_ICON_D, MENU_ICON_D}, 0);
            const double TX = RB.x + MENU_ROW_PADX + MENU_ICON_D + MENU_ICON_GAP;
            auto&      LBS = scratch();
            appendEsc(LBS, ROWS[i].label);
            const auto TL = cachedText(LBS, v13On(), T.title, (int)(RB.w * P.scale), linePx(T.title), 0, false, 500);
            if (SEL && i == 1) { // only Default wears a subtitle (the demo's sub)
                LBS.clear();
                appendEsc(LBS, "May ring or vibrate based on device settings");
                const auto SL = cachedText(LBS, v13On82(), T.body, (int)std::max(1.0, (RB.w - (TX - RB.x) - MENU_ROW_PADX) * P.scale), linePx(T.body), 0, false, 400);
                if (TL && TL->tex)
                    P.tex(TL->tex, TX, RB.y + MENU_ROW_PADY);
                if (SL && SL->tex)
                    P.tex(SL->tex, TX, RB.y + MENU_ROW_PADY + 16 + 2);
            } else if (TL && TL->tex) {
                P.tex(TL->tex, TX, RB.y + (RH - TL->tex->m_size.y / P.scale) / 2);
            }
            card.manage.push_back({RB, (uint8_t)i});
            y += RH + MENU_ROW_GAP;
        }
        y -= MENU_ROW_GAP;

        // ---- the snooze section: absent for bundles (the demo folds it in
        //      only for singleton targets) ----
        if (!BUNDLE) {
            y += MENU_SNOOZE_MT;
            {
                const bool HOVR = hovered.kind == SCard::MANAGE && hovered.id == N->id && hovered.group == groupKey && hovered.part == 3;
                const CBox RB{x, y, RW, MENU_ROW_H};
                if (HOVR)
                    P.rect(RB, v13Raised(), (int)std::lround(PIXEL_INTERNAL_RADIUS * P.scale), rPow());
                const auto GI = controlIcon(eControlIcon::SNOOZE, (int)std::lround(MENU_ICON_D * P.scale), v13On82());
                if (GI)
                    P.texFit(GI, CBox{RB.x + MENU_ROW_PADX, RB.y + (MENU_ROW_H - MENU_ICON_D) / 2, MENU_ICON_D, MENU_ICON_D}, 0);
                const double TX = RB.x + MENU_ROW_PADX + MENU_ICON_D + MENU_ICON_GAP;
                auto&      LBS = scratch();
                appendEsc(LBS, "Snooze");
                const auto TL = cachedText(LBS, v13On(), T.title, 128, linePx(T.title), 0, false, 500);
                if (TL && TL->tex)
                    P.tex(TL->tex, TX, RB.y + MENU_ROW_PADY);
                LBS.clear();
                appendEsc(LBS, "for " + shortDuration(centerManageSnoozeSecs()));
                const auto SL = cachedText(LBS, SNOZEO ? v13On82() : v13On60(), T.body, 256, linePx(T.body), 0, false, 400);
                if (SL && SL->tex)
                    P.tex(SL->tex, TX, RB.y + MENU_ROW_PADY + 16 + 2);
                const auto CHEV = controlIcon(SNOZEO ? eControlIcon::EXPAND_LESS : eControlIcon::EXPAND_MORE, (int)std::lround(12 * P.scale), v13On60());
                if (CHEV)
                    P.texFit(CHEV, CBox{RB.x + RB.w - MENU_ROW_PADX - 12, RB.y + (MENU_ROW_H - 12) / 2, 12, 12}, 0);
                card.manage.push_back({RB, 3});
                y += MENU_ROW_H; // the footer stacks below the snooze row
            }
            if (SNOZEO) {
                const int64_t* const OPTS = SNOOZE_OPTS;
                y += MENU_ROW_GAP;
                for (int i = 0; i < 4; i++) {
                    const bool SEL  = i == SNOZEI;
                    const bool HOVR = hovered.kind == SCard::MANAGE && hovered.id == N->id && hovered.group == groupKey && hovered.part == (uint8_t)(4 + i);
                    const CBox RB{x, y, RW, MENU_ROW_H};
                    P.rect(RB, SEL ? v13SelRow() : (HOVR ? v13Raised() : CHyprColor{0x00000000}), (int)std::lround((SEL ? 26 : PIXEL_INTERNAL_RADIUS) * P.scale), rPow());
                    P.ring(RB, SEL ? v13SelRowB() : v13RowLine(), (int)std::lround((SEL ? 26 : PIXEL_INTERNAL_RADIUS) * P.scale), rPow(), 1.0);
                    auto& LBS = scratch();
                    if (OPTS[i] >= 3600) {
                        LBS += std::to_string(OPTS[i] / 3600);
                        LBS += (OPTS[i] / 3600 == 1) ? " hour" : " hours";
                    } else {
                        LBS += std::to_string(OPTS[i] / 60);
                        LBS += " minutes";
                    }
                    const auto TL = cachedText(LBS, SEL ? v13On() : v13On60(), T.body, 256, linePx(T.body), 0, false, SEL ? 500 : 400);
                    if (TL && TL->tex)
                        P.tex(TL->tex, RB.x + MENU_OPT_INSET, RB.y + (MENU_ROW_H - TL->tex->m_size.y / P.scale) / 2);
                    if (SEL) {
                        const auto CK = controlIcon(eControlIcon::CHECK, (int)std::lround(MENU_ICON_D * P.scale), v13On());
                        if (CK)
                            P.texFit(CK, CBox{RB.x + RB.w - MENU_ROW_PADX - MENU_ICON_D, RB.y + (MENU_ROW_H - MENU_ICON_D) / 2, MENU_ICON_D, MENU_ICON_D}, 0);
                    }
                    card.manage.push_back({RB, (uint8_t)(4 + i)});
                    y += MENU_ROW_H + MENU_ROW_GAP;
                }
                y -= MENU_ROW_GAP;
            }
        }

        // ---- the footer: Dismiss (outline) + Done (filled) ----
        y += MENU_FOOT_MT;
        {
            const CBox DB{x, y, MENU_DISMISS_W, MENU_BTN_H};
            const bool HOVR = hovered.kind == SCard::MANAGE && hovered.id == N->id && hovered.group == groupKey && hovered.part == 9;
            if (HOVR)
                P.rect(DB, v13Raised(), (int)std::lround(MENU_BTN_R * P.scale), rPow());
            P.ring(DB, v13SelRowB(), (int)std::lround(MENU_BTN_R * P.scale), rPow(), 1.0);
            auto& SB2 = scratch();
            appendEsc(SB2, "Dismiss");
            const auto L = cachedText(SB2, v13On(), T.action, (int)(MENU_DISMISS_W * P.scale), linePx(T.action), 0, false, 500);
            if (L && L->tex)
                P.tex(L->tex, DB.x + (DB.w - L->tex->m_size.x / P.scale) / 2, DB.y + (DB.h - L->tex->m_size.y / P.scale) / 2);
            card.manage.push_back({DB, 9});

            const CBox KB{box.x + box.w - MENU_PAD - MENU_DONE_W, y, MENU_DONE_W, MENU_BTN_H};
            const bool KHOV = hovered.kind == SCard::MANAGE && hovered.id == N->id && hovered.group == groupKey && hovered.part == 8;
            P.rect(KB, KHOV ? v13Action().modifyA(std::min(1.0, (double)v13Action().a * 1.12)) : v13Action(), (int)std::lround(MENU_BTN_R * P.scale), rPow());
            SB2.clear();
            appendEsc(SB2, "Done");
            const auto L2 = cachedText(SB2, v13OnAccent(), T.action, (int)(MENU_DONE_W * P.scale), linePx(T.action), 0, false, 500);
            if (L2 && L2->tex)
                P.tex(L2->tex, KB.x + (KB.w - L2->tex->m_size.x / P.scale) / 2, KB.y + (KB.h - L2->tex->m_size.y / P.scale) / 2);
            card.manage.push_back({KB, 8});
        }

        s_measuredH = MENU_PAD * 2 + (y + MENU_BTN_H - (box.y + MENU_PAD));
        cards.push_back(std::move(card));
    }

    // ---- the card engine: layout AND paint in one pass ----

    // kids are precomputed by the caller (convKids / bundleKids); `group` is
    // the bundle key for bundles, "" for singletons; `hidden` is the number
    // of children past the kid cap (the "N more" line); the reply out-params
    // carry the field boxes so the card's hit box can wear them
    static void cardEngine(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, int hidden, eCardKind kind, const std::vector<SKid>& kids,
                           const std::string& group, CBox* replyField, CBox* replySend) {
        const bool  BUNDLE = kind == eCardKind::BCHILD;
        const double W     = box.w;
        const double TX    = CARD_TEXT_X;
        const double TW    = std::max(1.0, W - TX - CARD_TEXT_INSET);
        const int   TWPX   = (int)std::max(1.0, TW * P.scale);
        const bool  URGENT = N->urgency >= 2;
        const bool  IS_CONV = kind == eCardKind::CONV;

        // ---- the collapsed preview (conversations: the newest messages; the
        // bundle: its newest child's title + body) — decided first, the
        // header line's shape depends on it ----
        std::vector<SPreviewLine> prev;
        if (!open) {
            if (IS_CONV) {
                size_t cap = N->conversationKind == "group" ? 2 : 1;
                // A group whose two newest messages are one sender's reads
                // as a single "mipu: <text>" line (AOSP); the second line
                // appears only when a second sender spoke.
                if (cap == 2 && N->messages.size() >= 2 &&
                    N->messages.rbegin()->senderName == N->messages.rbegin()[1].senderName)
                    cap = 1;
                prev = previewLines(*N, kind, cap);
            } else if (BUNDLE)
                prev = previewLines(*N, kind, 1);
        }
        // pixel-parity header (AOSP captures): semibold title + grey " · age"
        // on ONE line beside the avatar — the group name for groups, app ·
        // conversation for 1:1s, the title for plain cards. The unread count
        // rides the count pill top-right, never the kicker.
        const bool TITLEHEAD = !open && IS_CONV && prev.size() == 1;

        // ---- the header line beside the avatar ----
        const auto TITLETXT = titleForDisplay(*N);
        std::string left, right;
        if (IS_CONV) {
            if (N->conversationKind == "group")
                left = N->conversationTitle.empty() ? "Group" : N->conversationTitle;
            else {
                left = N->appName;
                if (!N->participants.empty())
                    left += " · " + N->participants.front().name;
                // the expanded 1:1 keeps the count run (the AOSP level-2 card)
                if (open && N->unreadCount > 1)
                    left += " · " + std::to_string(N->unreadCount) + " new messages";
            }
            right = " · " + ageString(N->arrived);
        } else { // PLAIN, and the bundle lead (the newest child)
            left  = (BUNDLE || TITLETXT.empty()) ? N->appName : TITLETXT;
            right = " · " + ageString(N->arrived);
        }
        const bool   COUNT = BUNDLE || (IS_CONV && N->unreadCount > 1);
        const bool   HEADBIG = IS_CONV || !BUNDLE; // the semibold title-size lead
        const double KICKW = W - TX - (COUNT ? KICK_RIGHT_COUNT : KICK_RIGHT);
        const auto   K     = kick2(P, T, left, right, KICKW, HEADBIG);

        // ---- the body under the header line: the larger-type recipe ----
        const auto   FULLBODY = bodyForDisplay(*N);
        const CHyprColor LINKCOL = v13Action();
        const SCachedText* BODY = nullptr;
        auto& HB = scratch(); // the body composition buffer (kid loops re-take scratch())
        if (kind == eCardKind::PLAIN && !FULLBODY.empty()) {
            appendEsc(HB, open ? FULLBODY : lastLine(FULLBODY));
            // collapsed caps at two lines, ellipsized (the ROM card)
            BODY = cachedText(HB, v13On82(), T.title, TWPX, open ? -1 : 2 * linePx(T.title), 18.0 / 15.0, true, 400, &LINKCOL);
        } else if (TITLEHEAD) {
            // a group's sender leads the line ("mipu: <text>"); 1:1 is the
            // bare message. Up to two lines, ellipsized.
            if (N->conversationKind == "group" && !prev.front().a.empty()) {
                HB += "<span foreground=\"#" + hexOfCached(v13On()) + "\">";
                appendEsc(HB, prev.front().a);
                HB += ":</span> ";
                appendEsc(HB, prev.front().b);
            } else
                appendEsc(HB, prev.front().b);
            BODY = cachedText(HB, v13On82(), T.title, TWPX, 2 * linePx(T.title), 18.0 / 15.0, true, 400);
        }
        const double BODYH = texH(BODY, P.scale);

        double PROG = 0;
        if (N->progress >= 0)
            PROG = PROGRESS_GAP + PROGRESS_H;

        std::vector<SAct> acts;
        std::vector<CBox> actBoxes;
        double            actsH = 0;
        if (kind == eCardKind::PLAIN && open) {
            actionsOf(*N, acts);
            actsH = layoutActions(P, T, acts, actBoxes, TW);
        }

        // the collapsed preview block: the single-preview conversation is
        // painted as the body under the header line, not as preview rows
        const double PREVH = !open && !prev.empty() && !TITLEHEAD ? PREV_MT + (double)prev.size() * 18 + (prev.size() - 1) * PREV_GAP : 0;

        // ---- the kid block (expanded only), measured before the card is ----
        double              kidsH = 0;
        std::vector<double> kidH;
        if (open && (IS_CONV || BUNDLE) && !kids.empty()) {
            for (size_t i = 0; i < kids.size(); i++) {
                const auto& KID = kids[i];
                auto&       SB  = scratch();
                double      content = KICK2_LINE;
                if (KID.exp) {
                    // level 2 REPLACES the kicker: time (11/15 on60) + the
                    // bold name (15/18)
                    if (!KID.time.empty()) {
                        appendEsc(SB, KID.time);
                        cachedText(SB, v13On60(), T.header, (int)std::max(1.0, (W - TX - CARD_TEXT_INSET) * P.scale), linePx(T.header), 0, false, 400);
                    }
                    SB.clear(); // each run is its own texture (appendEsc accumulates)
                    appendEsc(SB, KID.who);
                    cachedText(SB, v13On(), T.title, (int)std::max(1.0, (W - TX - CARD_TEXT_INSET) * P.scale), linePx(T.title), 0, false, 500);
                    content = KICK2_LINE + KWHO2_MT + 18;
                } else {
                    appendEsc(SB, KID.who);
                    const double WHOW = texW(cachedText(SB, v13On82(), T.header, (int)std::max(1.0, (W - TX - KICK_RIGHT) * P.scale), linePx(T.header), 0, false, 500), P.scale);
                    if (!KID.time.empty()) {
                        SB.clear();
                        appendEsc(SB, " \u2022 " + KID.time);
                        cachedText(SB, v13On60(), T.header, (int)std::max(1.0, (W - TX - KICK_RIGHT - WHOW) * P.scale), linePx(T.header), 0, false, 400);
                    }
                }
                if (!KID.lines.empty()) {
                    double mL = 0;
                    for (size_t li = 0; li < KID.lines.size(); li++) {
                        const double MWR = W - TX - CARD_TEXT_INSET - (KID.media && li == 0 ? MINI_D + 4 : 0);
                        SB.clear();
                        appendEsc(SB, KID.lines[li]);
                        cachedText(SB, v13On82(), T.body, (int)std::max(1.0, MWR * P.scale), linePx(T.body), 0, false, 400);
                        mL += 18 + (li > 0 ? MSG_GAP : 0);
                    }
                    content += MSG_MT + mL;
                }
                if (KID.exp && IS_CONV && !KID.reply) {
                    std::vector<CBox> kb;
                    actionsOf(*N, acts);
                    const double ah = layoutActions(P, T, acts, kb, TW);
                    if (ah > 0)
                        content += KACTS_MT + ah;
                }
                if (KID.reply)
                    content += REPLY_MT + REPLY_H + 8;
                const double KH = KID_PAD * 2 + std::max(CARD_ICON_D, content);
                kidsH += KH + (i > 0 ? HAIR_H : 0);
                kidH.push_back(KH);
            }
        }

        // "N more" past the kid cap: one small line (display only)
        const double MOREH = (open && hidden > 0) ? HAIR_H + KID_PAD + KICK2_LINE + KID_PAD : 0;

        // ---- the card's height: pad + max(avatar, text block) + kids ----
        double headerH = K.h + (BODY ? BODY_MT + BODYH : 0) + PREVH +
                         (kind == eCardKind::PLAIN ? PROG + (actsH > 0 ? ACTS_MT + actsH : 0) : 0);
        const double PADB = (kind == eCardKind::PLAIN && open && actsH > 0) ? CARD_PADB_ACTS : CARD_PADB;
        const double ROWH = std::max(CARD_ICON_D, headerH);
        const double H    = std::max(CARD_MIN_H, CARD_PADT + ROWH + kidsH + MOREH + PADB);
        s_measuredH       = H;

        // ---- paint (SPaint no-ops everything while warm) ----
        P.rect(box, v13Card(), rRow(P.scale), rPow()); // stroke-free tile

        // the lead icon: pinned to the top when open, centered on the text
        // block when collapsed
        const double ICONY = open ? box.y + CARD_PADT : box.y + CARD_PADT + (ROWH - CARD_ICON_D) / 2;
        paintLead(P, *N, CBox{box.x + CARD_ICON_X, ICONY, CARD_ICON_D, CARD_ICON_D});

        // pixel-parity: a group avatar wears the unread count bubble
        if (IS_CONV && !open && N->conversationKind == "group" && N->unreadCount > 1) {
            const double D  = CARD_ICON_D * BADGE_D;
            const CBox   BB{box.x + CARD_ICON_X + CARD_ICON_D * (1 + BADGE_PROT) - D, ICONY + CARD_ICON_D * (1 + BADGE_PROT) - D, D, D};
            P.rect(BB, v13Chip(), (int)std::lround(D / 2 * P.scale), 2.f);
            auto& CNTS = scratch();
            CNTS += std::to_string(N->unreadCount);
            const auto CNT = cachedText(CNTS, v13On(), T.header, (int)(D * P.scale) + 8, linePx(T.header), 0, false, 600);
            if (CNT && CNT->tex)
                P.tex(CNT->tex, BB.x + (BB.w - CNT->tex->m_size.x / P.scale) / 2, BB.y + (BB.h - CNT->tex->m_size.y / P.scale) / 2);
        }

        const double TXA = box.x + TX;
        // collapsed: the text block centers on the avatar row (the ROM card)
        double       ty  = box.y + CARD_PADT + (open ? 0.0 : (ROWH - headerH) / 2);
        paintKick(P, K, TXA, ty);
        ty += K.h;

        std::vector<SCard::SLinkHit> cardLinks;
        if (BODY && BODY->tex) {
            ty += BODY_MT;
            P.tex(BODY->tex, TXA, ty);
            for (const auto& L : BODY->links) // rel logical -> global logical
                cardLinks.push_back({CBox{TXA + L.rel.x, ty + L.rel.y, L.rel.w, L.rel.h}, L.href});
            ty += BODYH;
        }
        if (N->progress >= 0) {
            ty += PROGRESS_GAP;
            paintProgress(P, TXA, ty, TW, N->progress, URGENT);
            ty += PROGRESS_H;
        }
        std::vector<CBox> plainActsAbs;
        if (kind == eCardKind::PLAIN && open && actsH > 0) {
            ty += ACTS_MT;
            paintActions(P, T, SCard::ROW, N->id, group, std::string{}, acts, actBoxes, TXA, ty, plainActsAbs);
            ty += actsH;
        }
        if (!open && !prev.empty() && !TITLEHEAD) {
            ty += PREV_MT;
            for (const auto& L : prev) {
                if (BUNDLE) {
                    // no mini, no colon: the child's title, then its body
                    auto& SB = scratch();
                    appendEsc(SB, L.a);
                    const auto TT = cachedText(SB, v13On(), T.body, TWPX, linePx(T.body), 0, false, 500);
                    if (TT && TT->tex)
                        P.tex(TT->tex, TXA, ty);
                    ty += 18 + PREV_GAP;
                    SB.clear(); // the body run is its own texture
                    appendEsc(SB, L.b);
                    const auto BB = cachedText(SB, v13On82(), T.body, TWPX, linePx(T.body), 0, false, 400);
                    if (BB && BB->tex)
                        P.tex(BB->tex, TXA, ty);
                    ty += 18 + PREV_GAP;
                } else {
                    paintPrevLine(P, T, *N, L, TXA, ty, TW);
                    ty += 18 + PREV_GAP;
                }
            }
            ty -= PREV_GAP;
        }

        // ---- the kid rows ----
        double                  ky = box.y + CARD_PADT + ROWH; // kids start below the avatar row
        std::vector<CBox>       kidBoxes;
        std::vector<std::vector<CBox>> kidActsAbs;
        for (size_t i = 0; i < kids.size(); i++) {
            if (i > 0) {
                P.lineH(CBox{box.x, ky, W, HAIR_H}, v13RowLine());
                ky += HAIR_H;
            }
            const auto& KID  = kids[i];
            const double KH  = kidH[i];
            kidBoxes.push_back(CBox{box.x, ky, W, KH});
            kidActsAbs.push_back({});

            // the avatar: the sender's (or the child app's) 37px circle, the
            // chip disc + initial when nothing resolved
            const CBox AVC{box.x + CARD_ICON_X, ky + KID_PAD, CARD_ICON_D, CARD_ICON_D};
            bool        AVOK = false;
            const int  AVR   = (int)std::lround(CARD_ICON_D / 2 * P.scale);
            if (BUNDLE) {
                const auto CN = Model::byId(KID.target);
                if (CN) {
                    const auto TEX = texReady(CN->identTex) ? CN->identTex : CN->iconTex;
                    if (texReady(TEX)) {
                        P.texCover(TEX, AVC, AVR, 2.f);
                        AVOK = true;
                    }
                }
            } else {
                const auto IT = std::ranges::find_if(N->participants, [&](const auto& p) { return p.key == KID.pkey || p.name == KID.pkey; });
                if (IT != N->participants.end() && texReady(IT->avatarTex)) {
                    P.texCover(IT->avatarTex, AVC, AVR, 2.f);
                    AVOK = true;
                }
            }
            if (!AVOK) {
                P.rect(AVC, v13Chip(), AVR, 2.f);
                std::string init;
                if (!KID.who.empty())
                    init = Pixel::firstCodepoint(KID.who, 0);
                if (init.empty())
                    init = "?";
                const auto GLYPH = cachedText(init, v13On(), (int)std::lround(15 * P.scale), (int)(CARD_ICON_D * P.scale) + 8, linePx((int)std::lround(15 * P.scale)), 0, false, 600);
                if (GLYPH && GLYPH->tex)
                    P.tex(GLYPH->tex, AVC.x + (AVC.w - GLYPH->tex->m_size.x / P.scale) / 2, AVC.y + (AVC.h - GLYPH->tex->m_size.y / P.scale) / 2);
            }

            // the kid's expand chevron: 25x17 visual, 44x44 hit, top-right
            // (conversations only — bundle kids invoke, they do not expand)
            if (IS_CONV) {
                const CBox CV{box.x + W - CHIP_X - CHIP_W, ky + KID_CHEV_Y, CHIP_W, CHIP_H};
                const bool HOV = hovered.kind == SCard::CHILD && hovered.id == N->id && hovered.childKey == KID.key && hovered.part == 1;
                if (HOV)
                    P.rect(CV, v13RaisedH(), (int)std::lround(CHIP_H / 2 * P.scale), 2.f);
                const auto CHEV = controlIcon(KID.exp ? eControlIcon::EXPAND_LESS : eControlIcon::EXPAND_MORE, (int)std::lround(CHEV_D * P.scale), HOV ? v13On() : v13On60());
                if (CHEV)
                    P.texFit(CHEV, CBox{CV.x + (CHIP_W - CHEV_D) / 2, CV.y + (CHIP_H - CHEV_D) / 2, CHEV_D, CHEV_D}, 0);
            }

            const double kx = box.x + TX;
            double       y2 = ky + KID_PAD;
            auto&        SB = scratch();
            if (!KID.exp) {
                // level 1: who (on82 500) • time (on60), one 15px line
                appendEsc(SB, KID.who);
                const auto   WHO = cachedText(SB, v13On82(), T.header, (int)std::max(1.0, (W - TX - KICK_RIGHT) * P.scale), linePx(T.header), 0, false, 500);
                const double WHOW = texW(WHO, P.scale);
                if (WHO && WHO->tex)
                    P.tex(WHO->tex, kx, y2 + (KICK2_LINE - WHO->tex->m_size.y / P.scale) / 2);
                if (!KID.time.empty()) {
                    SB.clear();
                    appendEsc(SB, " \u2022 " + KID.time);
                    const auto TM = cachedText(SB, v13On60(), T.header, (int)std::max(1.0, (W - TX - KICK_RIGHT - WHOW) * P.scale), linePx(T.header), 0, false, 400);
                    if (TM && TM->tex)
                        P.tex(TM->tex, kx + WHOW, y2 + (KICK2_LINE - TM->tex->m_size.y / P.scale) / 2);
                }
                y2 += KICK2_LINE;
            } else {
                // level 2: the kicker is REPLACED by time + the bold name
                if (!KID.time.empty()) {
                    appendEsc(SB, KID.time);
                    const auto TM = cachedText(SB, v13On60(), T.header, (int)std::max(1.0, (W - TX - CARD_TEXT_INSET) * P.scale), linePx(T.header), 0, false, 400);
                    if (TM && TM->tex)
                        P.tex(TM->tex, kx, y2);
                }
                y2 += KICK2_LINE + KWHO2_MT;
                SB.clear();
                appendEsc(SB, KID.who);
                const auto WN = cachedText(SB, v13On(), T.title, (int)std::max(1.0, (W - TX - CARD_TEXT_INSET) * P.scale), linePx(T.title), 0, false, 500);
                if (WN && WN->tex)
                    P.tex(WN->tex, kx, y2);
                y2 += 18;
            }

            // the message lines (13/18 on82), the media glyph on the newest
            double mlineY = y2 + MSG_MT;
            for (size_t li = 0; li < KID.lines.size(); li++) {
                const double IND = (KID.media && li == 0) ? MINI_D + 4 : 0;
                if (IND > 0) {
                    const auto GLYPH = controlIcon(eControlIcon::MEDIA, (int)std::lround(MINI_D * P.scale), v13On60());
                    if (GLYPH)
                        P.texFit(GLYPH, CBox{kx, mlineY + 1, MINI_D, MINI_D}, 0);
                }
                SB.clear();
                appendEsc(SB, KID.lines[li]);
                const auto ML = cachedText(SB, v13On82(), T.body, (int)std::max(1.0, (W - TX - CARD_TEXT_INSET - IND) * P.scale), linePx(T.body), 0, false, 400);
                if (ML && ML->tex)
                    P.tex(ML->tex, kx + IND, mlineY);
                mlineY += 18 + MSG_GAP;
            }
            y2 = mlineY - MSG_GAP;

            if (KID.exp && IS_CONV && !KID.reply) {
                std::vector<CBox> kb;
                actionsOf(*N, acts);
                const double ah = layoutActions(P, T, acts, kb, TW);
                if (ah > 0) {
                    y2 += KACTS_MT;
                    paintActions(P, T, SCard::CHILD, N->id, group, KID.key, acts, kb, kx, y2, kidActsAbs.back());
                    y2 += ah;
                }
            }
            if (KID.reply && replyField && replySend) {
                y2 += REPLY_MT;
                paintReplyAt(P, T, N, kx, y2, TW - 11, *replyField, *replySend);
                y2 += REPLY_H + 8;
            }
            ky += KH; // advance to the next kid (layout accumulates identically)
        }

        if (open && hidden > 0) {
            P.lineH(CBox{box.x, ky, W, HAIR_H}, v13RowLine());
            ky += HAIR_H;
            auto& SB = scratch();
            SB += std::to_string(hidden);
            SB += " more";
            const auto MM = cachedText(SB, v13On60(), T.header, (int)std::max(1.0, (W - TX - KICK_RIGHT) * P.scale), linePx(T.header), 0, false, 400);
            if (MM && MM->tex)
                P.tex(MM->tex, box.x + TX, ky + KID_PAD + (KICK2_LINE - MM->tex->m_size.y / P.scale) / 2);
            ky += KID_PAD + KICK2_LINE + KID_PAD;
        }

        // ---- the expand affordance, top-right: the count pill, or the
        // ROM's circular chevron button ----
        {
            const int    CHIPN = BUNDLE ? (int)kids.size() : (IS_CONV && N->unreadCount > 1 ? (int)N->unreadCount : -1);
            const bool   CHIPHOV = hovered.kind == (BUNDLE ? (open ? SCard::GHEAD : SCard::DIGEST) : SCard::ROW) && hovered.id == N->id && hovered.group == group && hovered.part == 1;
            if (CHIPN < 0)
                paintChevronButton(P, box.x + W - CHEV_BTN_X - CHEV_BTN_D, box.y + CHEV_BTN_Y, open, CHIPHOV);
            else
                paintChip(P, T, box.x + W - CHIP_X - chipW(T, P.scale, CHIPN), box.y + CHIP_Y, open, CHIPN, CHIPHOV ? v13RaisedH() : v13PillBg(), v13PillFg());
        }

        // ---- hit boxes: layout state, identical on the warm and the draw
        // pass — input reads whatever the last pass left, so a warm pass may
        // never leave a partial card set (the panel would shadow the rows)
        SCard c;
        c.kind       = BUNDLE ? (open ? SCard::GHEAD : SCard::DIGEST) : SCard::ROW;
        c.box        = box;
        c.id         = N->id;
        c.group      = group;
        c.expandable = IS_CONV || kind == eCardKind::PLAIN; // urgent is collapsed for good
        c.manage.push_back({CBox{box.x + W - CHEV_HIT_W, box.y, CHEV_HIT_W, open ? CHEV_HIT_H_E : CHEV_HIT_H}, 1});
        c.manage.push_back({CBox{box.x, box.y, ICONCOL_W, ICONCOL_H}, 2});
        if (kind == eCardKind::PLAIN && open && actsH > 0)
            for (size_t i = 0; i < plainActsAbs.size(); i++)
                c.buttons.push_back({plainActsAbs[i], *acts[i].id});
        c.links = std::move(cardLinks);
        if (replyField && replySend && !replyField->empty()) {
            c.replyField = *replyField;
            c.replySend  = *replySend;
        }
        const CBox KID_REPLY_FIELD = c.replyField; // capture before the move
        const CBox KID_REPLY_SEND  = c.replySend;
        cards.push_back(std::move(c));

        for (size_t i = 0; i < kids.size(); i++) {
            const auto& KID  = kids[i];
            const CBox  KBOX = kidBoxes[i];
            SCard       k;
            k.kind       = SCard::CHILD;
            k.box        = KBOX;
            k.id         = KID.target; // bundle kid: the child's own identity
            k.group      = group;
            k.childKey   = KID.key;
            k.expandable = IS_CONV;
            if (IS_CONV)
                k.manage.push_back({CBox{KBOX.x + KBOX.w - KID_CHEV_HIT, KBOX.y, KID_CHEV_HIT, KID_CHEV_HIT}, 1});
            if (KID.reply) { // the field lives inside this kid's box
                k.replyField = KID_REPLY_FIELD;
                k.replySend  = KID_REPLY_SEND;
            }
            for (size_t bi = 0; bi < kidActsAbs[i].size(); bi++)
                k.buttons.push_back({kidActsAbs[i][bi], *acts[bi].id});
            cards.push_back(std::move(k));
        }
    }

    // children past the kid cap, for the "N more" line
    static int hiddenChildren(eCardKind kind, const SNotif& N, size_t shown) {
        if (kind != eCardKind::CONV)
            return 0;
        // ROM contract: one child row per message for ALL conversation types
        // (2026-08-26 audit: applies to both 1:1 and group, no per-sender bundling)
        return std::max<size_t>(0, N.messages.size() - shown);
    }

    void paintCard(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, bool more, eCardKind kind) {
        (void)more; // the hidden count is derived from the model, not the caller
        std::vector<SKid> kids;
        if (open && kind == eCardKind::CONV)
            convKids(*N, kids);
        CBox field, send;
        cardEngine(P, T, N, box, open, open ? (int)hiddenChildren(kind, *N, kids.size()) : 0, kind, kids, std::string{}, kind == eCardKind::CONV ? &field : nullptr,
                   kind == eCardKind::CONV ? &send : nullptr);
    }

    double measureCard(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, bool open, eCardKind kind) {
        SPaint M = P;
        M.warm   = true;
        paintCard(M, T, N, CBox{0, 0, w, 0}, open, false, kind);
        return s_measuredH;
    }

    void paintBundle(const SPaint& P, const SType& T, const SDisp& D, const CBox& box, bool open, const std::vector<double>& childH) {
        (void)childH; // the per-kid heights are the engine's own record
        std::vector<SKid> kids;
        if (open)
            bundleKids(D, kids);
        const int HIDDEN = open ? std::max(0, (int)D.items.size() - (int)kids.size()) : 0;
        cardEngine(P, T, D.items.front(), box, open, HIDDEN, eCardKind::BCHILD, kids, D.key, nullptr, nullptr);
    }

    double measureBundle(const SPaint& P, const SType& T, const SDisp& D, double w, bool open, const std::vector<double>& childH) {
        SPaint M = P;
        M.warm   = true;
        paintBundle(M, T, D, CBox{0, 0, w, 0}, open, childH);
        return s_measuredH;
    }

} // namespace NHyprnotify
