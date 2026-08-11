// hyprnotify/row.cpp — one shade row, and the two faces of an app bundle.
//
// A ROW IS ITS BANNER. Collapsed it is a title, an age and the newest body
// line; open it is the whole card — header, title, body, progress, the
// sender's action buttons and (armed) the reply field. The two states share
// one function on purpose: the collapsed form must never say something the
// open one contradicts, and center.cpp measures with exactly
// the code that later paints.
//
// A BUNDLE has two faces made of the same parts: the DIGEST (folded — one
// app row with a count pill and two preview lines) and the GROUP (expanded —
// one identity-bearing header followed by text-only children). Children are
// intentionally text-only: the header already owns the app identity.
//
// Everything here obeys the texture rule (crash class 4): cachedText is
// requested UNCONDITIONALLY, and only the painting is gated on P.warm — the
// SPaint calls no-op inside a warm by themselves.

#include "ui.hpp"

namespace NHyprnotify {

    struct SMessageLine {
        std::string        participant;
        const SCachedText* sender     = nullptr;
        const SCachedText* body       = nullptr;
        double             senderY    = 0;
        double             bodyY      = 0;
        double             avatarY    = 0;
        bool               groupStart = false;
    };

    static CBox paintExpandPill(const SPaint& P, const SType& T, const CBox& row, std::string_view count, bool expanded, bool hover) {
        const auto   COLFG = color(cfg.colFg);
        const auto   LABEL = count.empty() ? nullptr : cachedText(std::string{count}, COLFG, T.small, 64, -1, 0, false, 600);
        const double GLYPH = 16;
        const double GAP   = LABEL ? 2 : 0;
        const double W     = std::max(PILL_W, texW(LABEL, P.scale) + GAP + GLYPH + 8);
        const CBox   PILL{row.x + row.w - ROW_PADX - W, row.y + ROW_PADT + (ROW_ICON - PILL_H) / 2, W, PILL_H};
        const CBox   HIT{PILL.x + (PILL.w - std::max(PILL_HIT, PILL.w)) / 2, PILL.y + (PILL.h - PILL_HIT) / 2, std::max(PILL_HIT, PILL.w), PILL_HIT};
        const auto   ICON = controlIcon(expanded ? eControlIcon::EXPAND_LESS : eControlIcon::EXPAND_MORE, (int)std::lround(GLYPH * P.scale), COLFG);
        if (!P.warm) {
            P.rect(PILL, hover ? stateLayer() : surfaceHigh(), (int)std::lround(PILL_H / 2 * P.scale));
            double x = PILL.x + 4;
            if (LABEL && LABEL->tex) {
                P.tex(LABEL->tex, x, PILL.y + (PILL.h - LABEL->tex->m_size.y / P.scale) / 2);
                x += texW(LABEL, P.scale) + GAP;
            }
            if (ICON)
                P.texFit(ICON, CBox{x, PILL.y + (PILL.h - GLYPH) / 2, GLYPH, GLYPH});
        }
        return HIT;
    }

    double renderRow(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, bool more, const SRowStyle& ST, SCard& card, bool child) {
        const auto COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker), COLACC = color(cfg.colHighlight);
        const CHyprColor COLBODY = COLFG.modifyA(COLFG.a * 0.92);
        const auto       AGE     = ageString(N->arrived);
        const auto&      SUBHEX  = hexOfCached(COLSUB);
        const float      RP      = rPow();

        // warmGate.warming, not P.warm: measureRow runs this whole function with
        // P.warm forced on to suppress painting, and a build there would be a
        // build inside a render (crash class 4). Only the real warm may create.
        if (warmGate.warming && (!child || (open && N->conversation && N->conversationKind == "group"))) {
            if (!child)
                ensureIconTex(*N, (int)std::lround(std::max(ST.iconPx, (double)cfg.maxIcon->value()) * P.scale));
            ensureConversationIcons(*N, (int)std::lround(std::max(ST.iconPx, (double)cfg.maxIcon->value()) * P.scale));
        }
        if (warmGate.warming)
            for (auto& IM : N->bodyImages)
                ensureBodyImage(IM, (int)std::lround(BODYIMG_H * P.scale));

        const bool   LEADICON = !child && hasLeadIcon(*N);
        const double ICONW    = LEADICON ? ST.iconPx : 0;
        const double TX       = box.x + ROW_PADX + (ICONW > 0 ? ICONW + ROW_ICON_GAP : 0);
        const double MIN_TEXT_W = 1.0 / std::max(P.scale, 0.01);
        const double TEXTW      = std::max(MIN_TEXT_W, box.x + box.w - ROW_PADX - TX - (!child && more ? CONTENT_END : 0));
        const int    TEXTWPX  = std::max(1, (int)std::floor(TEXTW * P.scale));
        const double BODYX    = TX;
        const double BODYW    = TEXTW;
        const int    BODYWPX  = TEXTWPX;

        // Timed silence indicator: when the app is under a timed mute, show a
        // clock icon and the time remaining in the header line. Android shows
        // this in the notification row's metadata area.
        const int64_t SILENCE_REMAINING = Policy::silenceRemaining(N->appKey);
        const bool    TIMED_SILENCE     = SILENCE_REMAINING > 0;

        double       th = 0;
        const double TY = box.y + ROW_PADT;

        if (!open) {
            // collapsed: bold "title • age" + the newest body line (+progress)
            auto& SB = scratch();
            SB += titleForDisplay(*N);
            SB += " <span foreground=\"";
            SB += SUBHEX;
            SB += "\">• ";
            if (TIMED_SILENCE) {
                SB += "Silent ";
                SB += shortDuration(SILENCE_REMAINING);
                SB += " • ";
            }
            SB += AGE;
            SB += "</span>";
            const auto LINE = cachedText(SB, COLTITLE, T.title, TEXTWPX, -1, 0, true, 600);
            const auto B1S  = lastLine(bodyForDisplay(*N));
            const auto B1   = B1S.empty() ? nullptr : cachedText(B1S, COLBODY, T.body, TEXTWPX, -1, 0, true, 400);
            th              = texH(LINE, P.scale) + (B1 ? 2 + texH(B1, P.scale) : 0) + (N->progress >= 0 ? PROGRESS_GAP + PROGRESS_H : 0);
            if (!P.warm) {
                if (LINE)
                    P.tex(LINE->tex, TX, TY);
                double yy = TY + texH(LINE, P.scale) + 2;
                if (B1) {
                    P.tex(B1->tex, TX, yy);
                    yy += texH(B1, P.scale);
                }
                if (N->progress >= 0) {
                    yy += PROGRESS_GAP;
                    paintProgress(P, TX, yy, TEXTW, N->progress, N->urgency >= 2);
                }
            }
        } else {
            // expanded: age/header line, title, body, progress, then the card's
            // own actions in Notify order. The PRIMARY gets no button here —
            // the row body fires it, exactly as the banner does.
            const int KICKWPX = TEXTWPX;

            auto& KB = scratch();
            if (ST.headerHasApp) {
                appendEsc(KB, N->appName);
                KB += " • ";
            }
            if (TIMED_SILENCE) {
                KB += "Silent ";
                KB += shortDuration(SILENCE_REMAINING);
                KB += " • ";
            }
            KB += AGE;
            const auto KICK  = cachedText(KB, COLSUB, T.header, KICKWPX, -1, 0, true, 500);
            const auto& TITLETEXT = titleForDisplay(*N);
            const auto  TITLE     = TITLETEXT.empty() ? nullptr : cachedText(TITLETEXT, COLTITLE, T.title, TEXTWPX, -1, 0, true, 600);
            // a merged chat is a transcript, so it gets Android's MessagingStyle
            // depth (~7 messages) where an ordinary card gets four lines
            const int  CAPL = (int)std::lround(T.body * 1.35 * (N->conversation ? 7 : 4));
            // linkCol collects the <a href> rects: without them a body click
            // meant for a URL would fire the card's primary instead
            const auto COLLINK = color(cfg.colLink);
            const auto& BODYTEXT = bodyForDisplay(*N);
            const bool                       MESSAGE_ROWS = N->conversation && N->conversationKind == "group" && !N->messages.empty();
            const auto                       BODY = MESSAGE_ROWS || BODYTEXT.empty() ? nullptr : cachedText(BODYTEXT, COLBODY, T.body, BODYWPX, CAPL, 1.1f, true, 400, &COLLINK);

            static std::vector<SMessageLine> messageLines;
            messageLines.clear();
            double messageH = 0;
            double messageX = TX;
            double messageW = TEXTW;
            if (MESSAGE_ROWS) {
                constexpr double MESSAGE_ICON = 24;
                messageX                      = box.x + ROW_PADX + (child ? MESSAGE_ICON + 8 : ROW_ICON + ROW_ICON_GAP);
                messageW                      = std::max(MIN_TEXT_W, box.x + box.w - ROW_PADX - messageX - (!child && more ? CONTENT_END : 0));
                const int    MESSAGE_WPX      = std::max(1, (int)std::floor(messageW * P.scale));
                std::string  previous;
                const size_t START = Pixel::presentedMessageStart(N->messages);
                for (size_t i = START; i < N->messages.size(); i++) {
                    const auto& M = N->messages[i];
                    if (M.text.empty())
                        continue;
                    const auto KEY       = Pixel::participantKey(M.senderId, M.senderName);
                    const bool NEW_GROUP = previous.empty() || KEY != previous;
                    if (NEW_GROUP && !messageLines.empty())
                        messageH += 14;
                    else if (!messageLines.empty())
                        messageH += 6;

                    SMessageLine LINE;
                    LINE.participant = KEY;
                    LINE.groupStart  = NEW_GROUP;
                    LINE.avatarY     = messageH;
                    if (NEW_GROUP && !M.senderName.empty()) {
                        auto& NAME = scratch();
                        appendEsc(NAME, M.senderName);
                        LINE.sender  = cachedText(NAME, COLSUB, T.header, MESSAGE_WPX, -1, 0, true, 500);
                        LINE.senderY = messageH;
                        messageH += texH(LINE.sender, P.scale) + 2;
                    }
                    LINE.body  = cachedText(M.text, COLBODY, T.body, MESSAGE_WPX, (int)std::lround(T.body * 1.35 * 2), 1.1f, true, 400, &COLLINK);
                    LINE.bodyY = messageH;
                    messageH += texH(LINE.body, P.scale);
                    messageLines.push_back(std::move(LINE));
                    previous = KEY;
                }
            }

            // The reply affordance is a chip among the buttons until it is
            // armed, and then the field takes a row of its own instead.
            const bool               ARMED    = ST.canReply && replyArmedOn(N->id);
            static const std::string REPLY_ID = "inline-reply", REPLY_LBL = "Reply";
            static std::vector<std::pair<const std::string*, const std::string*>> btnSrc; // reused
            btnSrc.clear();
            if (ST.canReply && N->canReply && !ARMED)
                btnSrc.emplace_back(&REPLY_ID, N->replyActionText.empty() ? &REPLY_LBL : &N->replyActionText);
            for (const auto& A : N->actions)
                btnSrc.emplace_back(&A.id, &A.label);

            static std::vector<CBox>               btnBoxes; // reused; main thread only
            static std::vector<const SCachedText*> btnLbls;
            btnBoxes.clear();
            btnLbls.clear();
            const double ACTIONW = BODYW;
            double btnH = 0;
            {
                double bx = 0, rowY = 0;
                for (const auto& [BID, BLBL] : btnSrc) {
                    auto& LB = scratch();
                    appendEsc(LB, *BLBL);
                    const auto   LBL = cachedText(LB, COLACC, T.action, BODYWPX, -1, 0, true, 600);
                    const double BW  = std::min(ACTIONW, texW(LBL, P.scale) + 2 * BTN_PADX);
                    if (bx > 0 && bx + BW > ACTIONW + 0.5) {
                        bx = 0;
                        rowY += BTN_H + BTN_GAP;
                    }
                    btnBoxes.push_back(CBox{bx, rowY, BW, BTN_H});
                    btnLbls.push_back(LBL);
                    bx += BW + BTN_GAP;
                }
                btnH = btnBoxes.empty() ? 0 : rowY + BTN_H;
            }
            const double ACTIONH = btnH;

            const double KH = texH(KICK, P.scale), TH = texH(TITLE, P.scale), BH = MESSAGE_ROWS ? messageH : texH(BODY, P.scale);
            th = KH + (KH > 0 ? HEAD_GAP : 0) + TH + (TH > 0 && BH > 0 ? TITLE_GAP : 0) + BH + (N->progress >= 0 ? PROGRESS_GAP + PROGRESS_H : 0) +
                (ACTIONH > 0 ? BTN_ROW_GAP + ACTIONH : 0) + (ARMED ? BTN_ROW_GAP + BTN_H : 0);

            double yy = TY;
            if (!P.warm && KICK)
                P.tex(KICK->tex, TX, yy);
            yy += KH + (KH > 0 ? HEAD_GAP : 0);
            if (!P.warm && TITLE)
                P.tex(TITLE->tex, TX, yy);
            yy += TH;
            if (TH > 0 && BH > 0)
                yy += TITLE_GAP;
            if (MESSAGE_ROWS) {
                const double MESSAGE_TOP = yy;
                for (const auto& LINE : messageLines) {
                    if (!P.warm) {
                        if (LINE.groupStart)
                            paintParticipantAvatar(P, *N, LINE.participant, CBox{child ? box.x + ROW_PADX : box.x + ROW_PADX + 8, MESSAGE_TOP + LINE.avatarY, 24, 24});
                        if (LINE.sender && LINE.sender->tex)
                            P.tex(LINE.sender->tex, messageX, MESSAGE_TOP + LINE.senderY);
                        if (LINE.body && LINE.body->tex)
                            P.tex(LINE.body->tex, messageX, MESSAGE_TOP + LINE.bodyY);
                    }
                    if (LINE.body)
                        for (const auto& L : LINE.body->links)
                            card.links.push_back({CBox{messageX + L.rel.x / P.scale, MESSAGE_TOP + LINE.bodyY + L.rel.y / P.scale, L.rel.w / P.scale, L.rel.h / P.scale}, L.href});
                }
                yy += messageH;
            } else {
                if (!P.warm && BODY)
                    P.tex(BODY->tex, BODYX, yy);
                if (BODY) // hit rects in both modes, like the buttons: physical -> global logical
                    for (const auto& L : BODY->links)
                        card.links.push_back({CBox{BODYX + L.rel.x / P.scale, yy + L.rel.y / P.scale, L.rel.w / P.scale, L.rel.h / P.scale}, L.href});
                yy += BH;
            }
            if (N->progress >= 0) {
                yy += PROGRESS_GAP;
                paintProgress(P, BODYX, yy, BODYW, N->progress, N->urgency >= 2);
                yy += PROGRESS_H;
            }
            if (ACTIONH > 0) {
                yy += BTN_ROW_GAP;
                // The label starts at the content column while the target
                // keeps equal optical padding on both sides.
                const double BX0 = BODYX - BTN_PADX;
                for (size_t i = 0; i < btnBoxes.size(); i++) {
                    const CBox BOX{BX0 + btnBoxes[i].x, yy + btnBoxes[i].y, btnBoxes[i].w, btnBoxes[i].h};
                    if (!P.warm) {
                        if (hovered.id == N->id && hovered.btn == (int)i)
                            P.rect(BOX, stateLayer(), (int)std::lround(BTN_H / 2 * P.scale));
                        if (btnLbls[i] && btnLbls[i]->tex)
                            P.tex(btnLbls[i]->tex, BOX.x + BTN_PADX, BOX.y + (BOX.h - btnLbls[i]->tex->m_size.y / P.scale) / 2);
                    }
                    card.buttons.push_back({BOX, *btnSrc[i].first});
                }
                yy += ACTIONH;
            }

            // ---- the armed inline-reply field ----
            if (ARMED) {
                yy += BTN_ROW_GAP;
                const auto&  TXT   = replyText();
                static const std::string SEND_DEFAULT  = "Send";
                const auto&              SENDTEXT      = N->replySubmitText.empty() ? SEND_DEFAULT : N->replySubmitText;
                const auto               SLBL_DISABLED = cachedText(SENDTEXT, COLSUB.modifyA(0.38f), T.action, BODYWPX, -1, 0, false, 600);
                const auto               SLBL_REST     = cachedText(SENDTEXT, COLACC, T.action, BODYWPX, -1, 0, false, 600);
                const auto               SLBL_FILLED   = cachedText(SENDTEXT, onHighlight(), T.action, BODYWPX, -1, 0, false, 600);
                const double             SENDW = std::min(BODYW / 2, std::max({texW(SLBL_DISABLED, P.scale), texW(SLBL_REST, P.scale), texW(SLBL_FILLED, P.scale)}) + 2 * BTN_PADX);
                const CBox   FB{BODYX, yy, std::max(40.0, BODYW - SENDW - BTN_GAP), BTN_H};
                const CBox   SB{BODYX + BODYW - SENDW, yy, SENDW, BTN_H};
                const int    RB = (int)std::lround(BTN_H / 2 * P.scale);

                // The complete line stays in one bounded texture and slides
                // behind the field clip. Prefix textures are measurement-only:
                // they place the caret and selection on UTF-8 boundaries.
                constexpr int REPLY_TEX_CAP = 32768;
                const auto    ENT           = TXT.empty() ? nullptr : cachedText(TXT, COLFG, T.action, REPLY_TEX_CAP, -1, 0, false, 400);
                const size_t  CURSOR        = std::min(replyCursor(), TXT.size());
                const auto [SEL0, SEL1]     = replySelection();
                const auto CUR              = CURSOR == 0 ? nullptr : cachedText(TXT.substr(0, CURSOR), COLFG, T.action, REPLY_TEX_CAP, -1, 0, false, 400);
                const auto S0               = SEL0 == 0 ? nullptr : cachedText(TXT.substr(0, std::min(SEL0, TXT.size())), COLFG, T.action, REPLY_TEX_CAP, -1, 0, false, 400);
                const auto S1               = SEL1 == 0 ? nullptr : cachedText(TXT.substr(0, std::min(SEL1, TXT.size())), COLFG, T.action, REPLY_TEX_CAP, -1, 0, false, 400);
                const auto PH  = TXT.empty() ? cachedText(N->replyPlaceholder.empty() ? "Type a reply..." : N->replyPlaceholder, COLSUB, T.action,
                                                         std::max(1, (int)((FB.w - 2 * BTN_PADX) * P.scale)), -1, 0, false, 400) :
                                               nullptr;
                if (!P.warm) {
                    P.rect(FB, surfaceHigh(), RB);
                    P.ring(FB, COLACC, RB, RP); // armed: the field wears the accent
                    const CBox CLIP{FB.x + BTN_PADX, FB.y + 2, std::max(1.0, FB.w - 2 * BTN_PADX), FB.h - 4};
                    double     cx = CLIP.x;
                    if (ENT && ENT->tex) {
                        const double TOTALW = texW(ENT, P.scale), CURW = texW(CUR, P.scale);
                        const double SCROLL = std::clamp(CURW - CLIP.w + 2.0, 0.0, std::max(0.0, TOTALW - CLIP.w));
                        if (SEL0 != SEL1) {
                            const double X0 = CLIP.x + texW(S0, P.scale) - SCROLL;
                            const double X1 = CLIP.x + texW(S1, P.scale) - SCROLL;
                            const double L  = std::max(CLIP.x, std::min(X0, X1));
                            const double R  = std::min(CLIP.x + CLIP.w, std::max(X0, X1));
                            if (R > L)
                                P.rect(CBox{L, FB.y + 4, R - L, FB.h - 8}, stateLayer(), 2);
                        }
                        P.texClipped(ENT->tex, CLIP.x - SCROLL, FB.y + (FB.h - ENT->tex->m_size.y / P.scale) / 2, CLIP);
                        cx = CLIP.x + CURW - SCROLL;
                    } else if (PH && PH->tex)
                        P.texClipped(PH->tex, CLIP.x, FB.y + (FB.h - PH->tex->m_size.y / P.scale) / 2, CLIP);
                    P.rect(CBox{std::clamp(cx + 1, CLIP.x, CLIP.x + CLIP.w - 1.5), FB.y + 5, 1.5, FB.h - 10}, COLACC, 0);

                    const bool SHOV = hovered.id == N->id && hovered.part == 4;
                    P.rect(SB, TXT.empty() ? surfaceHigh() : SHOV ? color(cfg.colHighlight) : stateLayer(), RB);
                    const auto* SLBL = TXT.empty() ? SLBL_DISABLED : SHOV ? SLBL_FILLED : SLBL_REST;
                    if (SLBL && SLBL->tex)
                        P.tex(SLBL->tex, SB.x + (SB.w - SLBL->tex->m_size.x / P.scale) / 2, SB.y + (SB.h - SLBL->tex->m_size.y / P.scale) / 2);
                }
                card.replyField = FB;
                card.replySend  = SB;
            }

        }

        const double ROWH = std::max(th, ICONW) + ROW_PADT + ROW_PADB;

        if (!P.warm && LEADICON) {
            // collapsed rows center the icon; expanded top-pin it
            const double IY = open ? box.y + ROW_PADT : box.y + (ROWH - ICONW) / 2;
            paintIconColumn(P, *N, CBox{box.x + ROW_PADX, IY, ICONW, ICONW}, ST.withBadge && N->conversation, RP);
        }
        card.box        = CBox{box.x, box.y, box.w, ROWH};
        card.id         = N->id;
        card.kind       = child ? SCard::CHILD : SCard::ROW;
        card.expandable = more;
        if (!child && more) {
            const auto COUNT     = N->conversation && N->unreadCount > 0 ? std::to_string(N->unreadCount) : std::string{};
            const bool HOV       = hovered.kind == SCard::ROW && hovered.id == N->id && hovered.part == 5;
            card.expandButton    = paintExpandPill(P, T, card.box, COUNT, open, HOV);
            card.expansionButton = true;
        }
        return ROWH;
    }

    // measure without painting: same code, a paint context that draws nothing
    // (cachedText still resolves through the real warm gate)
    double measureRow(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, bool open, const SRowStyle& ST, bool child) {
        SPaint MP = P;
        MP.warm   = true; // paints nothing; it does NOT license a texture build
        SCard scratch;
        return renderRow(MP, T, N, CBox{0, 0, w, 0}, open, true, ST, scratch, child);
    }

    // ---- the three things a shade slot can hold ----

    void paintSingle(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, bool more) {
        const bool HOV = hovered.kind == SCard::ROW && hovered.id == N->id && hovered.btn < 0 && hovered.part == 0;
        P.rect(box, HOV ? stateLayer() : surface(), rRow(P.scale), rPow());
        SCard card;
        renderRow(P, T, N, CBox{box.x, box.y, box.w, 0}, open, more, ROW_SINGLE, card, false);
        cards.push_back(std::move(card));
    }

    double digestH(const SType& T, size_t count, double scale) {
        return ROW_PADT + std::max(ROW_ICON, (double)T.title / scale + 2) + std::min<size_t>(2, count) * ((double)T.body / scale * 1.35 + 3) + ROW_PADB;
    }

    double groupHeadH() {
        return ROW_PADT + CHILD_ICON + ROW_PADB;
    }

    double snoozeRowH() {
        return SNOOZE_H;
    }

    static std::string snoozeDurationLabel(int64_t seconds) {
        seconds = std::max<int64_t>(seconds, 0);
        if (seconds == 0)
            return "now";
        if (seconds % 3600 == 0) {
            const auto HOURS = seconds / 3600;
            return std::to_string(HOURS) + (HOURS == 1 ? " hour" : " hours");
        }
        if (seconds % 60 == 0)
            return std::to_string(seconds / 60) + " min";
        return std::to_string(seconds) + " sec";
    }

    // Android's timed mute durations: 15 minutes, 1 hour, 2 hours, Forever.
    // These appear directly under a staged Silent choice.
    static constexpr std::array<std::pair<const char*, int64_t>, 4> SILENCE_DURATIONS = {
        {{"15 minutes", 15 * 60}, {"1 hour", 60 * 60}, {"2 hours", 2 * 60 * 60}, {"Forever", 0}}
    };

    std::vector<SMenuEntry> menuEntries(const SP<SNotif>& N, bool bundle) {
        std::vector<SMenuEntry> out;
        const auto              SELECTED = centerManageMode();
        if (!bundle && !N->conversationId.empty())
            out.push_back({eControlIcon::PRIORITY, "Priority", "Show at the top and alert", eManageEntryKind::ALERTING, Policy::eAlertingMode::PRIORITY, 0,
                           SELECTED == Policy::eAlertingMode::PRIORITY});
        out.push_back({eControlIcon::NOTIFICATION_ALERT, "Default", "May ring or vibrate based on device settings", eManageEntryKind::ALERTING, Policy::eAlertingMode::DEFAULT, 0,
                       SELECTED == Policy::eAlertingMode::DEFAULT});
        const bool SILENT_STAGED = SELECTED == Policy::eAlertingMode::SILENT;
        out.push_back({eControlIcon::NOTIFICATION_SILENT, "Silent", "Show quietly without a banner or sound", eManageEntryKind::ALERTING, Policy::eAlertingMode::SILENT, 0,
                       SILENT_STAGED});
        // Android's timed mutes: staging Silent reveals the durations directly
        // under it. Each is its own immediate verb — picking one commits the
        // silence and closes the panel, so Done stays the persistent choice.
        if (SILENT_STAGED)
            for (const auto& [LABEL, SECONDS] : SILENCE_DURATIONS)
                out.push_back({eControlIcon::SNOOZE, LABEL, "", eManageEntryKind::SILENCE_TIMED, Policy::eAlertingMode::SILENT, SECONDS, false});

        if (!bundle && !Model::vanishes(N)) {
            const auto SECONDS = std::max<int64_t>(cfg.snoozeSeconds->value(), 0);
            out.push_back({eControlIcon::SNOOZE, "Snooze " + snoozeDurationLabel(SECONDS), "Hide and alert again later", eManageEntryKind::SNOOZE,
                           Policy::eAlertingMode::DEFAULT, 0, false});
        }
        return out;
    }

    double managePanelH(const SP<SNotif>& N, const std::string& group) {
        const auto EN   = menuEntries(N, !group.empty());
        double     ROWS = EN.empty() ? 0 : (EN.size() - 1) * MENU_ROW_GAP;
        for (const auto& E : EN)
            ROWS += E.selected ? MENU_SELECTED_H : MENU_ROW_H;
        return MANAGE_PADT + MANAGE_HEAD + MANAGE_SECTION_GAP + ROWS + MANAGE_SECTION_GAP + MANAGE_ACTION_H + MANAGE_PADB;
    }

    void paintManagePanel(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, const std::string& group) {
        const auto  COLFG = color(cfg.colFg), COLSUB = color(cfg.colKicker), COLACC = color(cfg.colHighlight);
        const float RP = rPow();
        P.rect(box, surface(), rRow(P.scale), RP);

        SCard card;
        card.kind = SCard::MANAGE;
        card.id   = N->id;
        card.box  = box;
        card.group = group;

        // The held notification remains identifiable while its body becomes
        // management. Completion lives in the bottom action row, matching the
        // Pixel surface instead of adding a second close affordance.
        if (warmGate.warming) {
            ensureIconTex(*N, (int)std::lround(cfg.maxIcon->value() * P.scale));
            ensureConversationIcons(*N, (int)std::lround(cfg.maxIcon->value() * P.scale));
        }
        if (hasLeadIcon(*N))
            paintIconColumn(P, *N, CBox{box.x + MANAGE_PADT, box.y + MANAGE_PADT, ROW_ICON, ROW_ICON}, N->conversation, RP);

        const double TX = box.x + MANAGE_PADT + ROW_ICON + ROW_ICON_GAP;
        const int    TW = std::max(1, (int)((box.x + box.w - MANAGE_PADT - TX) * P.scale));
        const auto&  BODYTEXT = bodyForDisplay(*N);
        const auto&  DISPLAY_TITLE = titleForDisplay(*N);
        const auto   TOP           = cachedText(DISPLAY_TITLE.empty() ? (N->appName.empty() ? "Notification" : N->appName) : DISPLAY_TITLE, COLFG, T.title, TW, -1, 0, true, 600);
        const auto   MID = BODYTEXT.empty() ? nullptr : cachedText(lastLine(BODYTEXT), COLFG, T.body, TW, -1, 0, true, 400);
        const auto   APP           = N->appName.empty() || DISPLAY_TITLE.empty() ? nullptr : cachedText(N->appName, COLSUB, T.header, TW, -1, 0, false, 500);
        const double HH  = texH(TOP, P.scale) + (MID ? 1 + texH(MID, P.scale) : 0) + (APP ? 1 + texH(APP, P.scale) : 0);
        double       hy  = box.y + MANAGE_PADT + std::max(0.0, (MANAGE_HEAD - HH) / 2);
        if (!P.warm) {
            if (TOP && TOP->tex) {
                P.tex(TOP->tex, TX, hy);
                hy += texH(TOP, P.scale) + 1;
            }
            if (MID && MID->tex) {
                P.tex(MID->tex, TX, hy);
                hy += texH(MID, P.scale) + 1;
            }
            if (APP && APP->tex)
                P.tex(APP->tex, TX, hy);
        }

        double     y  = box.y + MANAGE_PADT + MANAGE_HEAD + MANAGE_SECTION_GAP;
        const auto EN = menuEntries(N, !group.empty());
        for (size_t i = 0; i < EN.size(); i++) {
            const auto& E = EN[i];
            const double RH = E.selected ? MENU_SELECTED_H : MENU_ROW_H;
            const CBox   RB{box.x + MANAGE_PADT, y, box.w - 2 * MANAGE_PADT, RH};
            // part codes 16.. address the entries: one rect per row, so a new
            // verb costs an entry and not a member
            const bool HOV = hovered.kind == SCard::MANAGE && hovered.id == N->id && hovered.part == (uint8_t)(16 + i);
            const double IX = RB.x + 10, TX2 = IX + MENU_GLYPH_D + MENU_GLYPH_GAP;
            const int    LW = std::max(1, (int)((RB.x + RB.w - 10 - TX2) * P.scale));
            const auto   G  = controlIcon(E.icon, (int)std::lround(MENU_GLYPH_D * P.scale), COLFG);
            const auto   L  = cachedText(E.label, COLFG, T.body, LW, -1, 0, false, 600);
            const auto   D  = E.selected ? cachedText(E.description, COLSUB, T.small, LW, -2, 1.05f, false, 400) : nullptr;
            if (!P.warm) {
                if (E.selected || HOV)
                    P.rect(RB, stateLayer(), (int)std::lround(12 * P.scale), RP);
                P.ring(RB, E.selected ? COLSUB.modifyA(0.66f) : COLSUB.modifyA(0.42f), (int)std::lround(12 * P.scale), RP);
                if (G)
                    P.texFit(G, CBox{IX, RB.y + (RB.h - MENU_GLYPH_D) / 2, MENU_GLYPH_D, MENU_GLYPH_D});
                const double COPYH = texH(L, P.scale) + (D ? 2 + texH(D, P.scale) : 0);
                double       ty2   = RB.y + (RB.h - COPYH) / 2;
                if (L && L->tex) {
                    P.tex(L->tex, TX2, ty2);
                    ty2 += texH(L, P.scale) + 2;
                }
                if (D && D->tex)
                    P.tex(D->tex, TX2, ty2);
            }
            card.manage.push_back({RB, (uint8_t)(16 + i)});
            y += RH + MENU_ROW_GAP;
        }

        y += MANAGE_SECTION_GAP - (EN.empty() ? 0 : MENU_ROW_GAP);
        const auto   DISMISS = cachedText("Dismiss", COLACC, T.action, 128, -1, 0, false, 600);
        const auto   DONE    = cachedText("Done", onHighlight(), T.action, 96, -1, 0, false, 600);
        const double DW      = std::max(86.0, texW(DISMISS, P.scale) + 24);
        const double OKW     = std::max(72.0, texW(DONE, P.scale) + 24);
        const CBox   DB{box.x + MANAGE_PADT, y, DW, MANAGE_ACTION_H};
        const CBox   OK{box.x + box.w - MANAGE_PADT - OKW, y, OKW, MANAGE_ACTION_H};
        const bool   DHOV = hovered.kind == SCard::MANAGE && hovered.id == N->id && hovered.part == MANAGE_DISMISS_PART;
        const bool   OHOV = hovered.kind == SCard::MANAGE && hovered.id == N->id && hovered.part == MANAGE_DONE_PART;
        if (!P.warm) {
            if (DHOV)
                P.rect(DB, stateLayer(), (int)std::lround(MANAGE_ACTION_H / 2 * P.scale));
            P.ring(DB, COLACC, (int)std::lround(MANAGE_ACTION_H / 2 * P.scale), RP);
            P.rect(OK, COLACC, (int)std::lround(MANAGE_ACTION_H / 2 * P.scale));
            if (OHOV)
                P.ring(OK, onHighlight(), (int)std::lround(MANAGE_ACTION_H / 2 * P.scale), RP);
            if (DISMISS && DISMISS->tex)
                P.tex(DISMISS->tex, DB.x + (DB.w - DISMISS->tex->m_size.x / P.scale) / 2, DB.y + (DB.h - DISMISS->tex->m_size.y / P.scale) / 2);
            if (DONE && DONE->tex)
                P.tex(DONE->tex, OK.x + (OK.w - DONE->tex->m_size.x / P.scale) / 2, OK.y + (OK.h - DONE->tex->m_size.y / P.scale) / 2);
        }
        card.manage.push_back({DB, MANAGE_DISMISS_PART});
        card.manage.push_back({OK, MANAGE_DONE_PART});

        cards.push_back(std::move(card));
    }

    // The undo row — Android's "Snoozed for 1 hour · Undo", in the slot the
    // card just held. Keep the notification identity at the top-left and expose
    // one textual Undo action at the right; snooze duration is configured from
    // the hold menu, so this transient row has no second clock control.
    void paintSnoozeRow(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box) {
        const auto  COLSUB = color(cfg.colKicker), COLACC = color(cfg.colHighlight);
        const float RP     = rPow();
        P.rect(box, surface(), rRow(P.scale), RP);

        if (warmGate.warming) {
            ensureIconTex(*N, (int)std::lround(cfg.maxIcon->value() * P.scale));
            ensureConversationIcons(*N, (int)std::lround(cfg.maxIcon->value() * P.scale));
        }

        SCard card;
        card.kind = SCard::SNOOZE;
        card.id   = N->id;
        card.box  = box;

        const bool HASICON = hasLeadIcon(*N);
        if (HASICON) {
            const CBox ICON{box.x + ROW_PADX, box.y + (SNOOZE_H - CHILD_ICON) / 2, CHILD_ICON, CHILD_ICON};
            paintIconColumn(P, *N, ICON, N->conversation, RP);
        }
        const double TX = box.x + ROW_PADX + (HASICON ? CHILD_ICON + ROW_ICON_GAP : 0);

        const auto   UND  = cachedText("Undo", COLACC, T.action, 96, -1, 0, false, 600);
        const double UNDW = texW(UND, P.scale) + 2 * BTN_PADX;
        const CBox   UB{box.x + box.w - ROW_PADX - UNDW, box.y + (SNOOZE_H - BTN_H) / 2, UNDW, BTN_H};
        const bool   UHOV = hovered.kind == SCard::SNOOZE && hovered.id == N->id && hovered.part == 8;
        if (!P.warm) {
            if (UHOV)
                P.rect(UB, stateLayer(), (int)std::lround(BTN_H / 2 * P.scale));
            if (UND && UND->tex)
                P.tex(UND->tex, UB.x + BTN_PADX, UB.y + (UB.h - UND->tex->m_size.y / P.scale) / 2);
        }
        card.manage.push_back({UB, 8});

        auto& LB = scratch();
        LB += "Snoozed ";
        appendEsc(LB, Model::snoozeLabel(N));
        const int  LW  = std::max(1, (int)std::floor((UB.x - MANAGE_GAP - TX) * P.scale));
        const auto LBL = cachedText(LB, COLSUB, T.body, LW, -1, 0, true, 500);
        if (!P.warm) {
            if (LBL && LBL->tex)
                P.tex(LBL->tex, TX, box.y + (SNOOZE_H - LBL->tex->m_size.y / P.scale) / 2);
        }

        cards.push_back(std::move(card));
    }

    // The folded bundle: the app's identity, a count pill, and the two newest
    // cards previewed a line each — enough to decide whether to open it.
    void paintDigest(const SPaint& P, const SType& T, const SDisp& D, const CBox& box) {
        const auto  COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker);
        const auto& SUBHEX = hexOfCached(COLSUB);
        const float RP     = rPow();

        const auto& NEWEST = D.items.front();
        const bool  HOV    = hovered.kind == SCard::DIGEST && hovered.group == D.key && hovered.part == 0;
        P.rect(box, HOV ? stateLayer() : surface(), rRow(P.scale), RP);

        if (warmGate.warming)
            ensureIconTex(*NEWEST, (int)std::lround(cfg.maxIcon->value() * P.scale));
        const auto& IDT = NEWEST->identTex;
        if (IDT)
            P.texFit(IDT, CBox{box.x + ROW_PADX, box.y + ROW_PADT, ROW_ICON, ROW_ICON}, (int)std::lround(ROW_ICON * 10.0 / 44.0 * P.scale), RP);

        const double TX    = box.x + ROW_PADX + ROW_ICON + ROW_ICON_GAP;
        const double PILLX = box.x + box.w - ROW_PADX - CONTENT_END;

        auto& DB = scratch();
        appendEsc(DB, NEWEST->appName);
        DB += " <span foreground=\"";
        DB += SUBHEX;
        DB += "\">• ";
        DB += std::to_string(D.items.size());
        DB += " • ";
        DB += ageString(NEWEST->arrived);
        DB += "</span>";
        const auto SUMLINE = cachedText(DB, COLTITLE, T.title, std::max(1, (int)((PILLX - 8 - TX) * P.scale)), -1, 0, true, 600);
        if (!P.warm && SUMLINE)
            P.tex(SUMLINE->tex, TX, box.y + ROW_PADT + (ROW_ICON - texH(SUMLINE, P.scale)) / 2);

        // <=2 preview lines, indented into the text column
        double       py   = box.y + ROW_PADT + std::max(ROW_ICON, (double)T.title / P.scale + 2);
        const size_t PREV = std::min(D.items.size(), Pixel::maxVisibleChildren(D.classified, Pixel::eExpansion::COLLAPSED));
        for (size_t i = 0; i < PREV; i++) {
            const auto& N = D.items[i];
            const double LH = (double)T.body / P.scale * 1.35;
            py += 3;
            double px = TX;
            auto& PBUF = scratch();
            PBUF += "<b>";
            PBUF += titleForDisplay(*N);
            PBUF += "</b>  <span foreground=\"";
            PBUF += SUBHEX;
            PBUF += "\">";
            PBUF += lastLine(bodyForDisplay(*N));
            PBUF += "</span>";
            const auto LN = cachedText(PBUF, COLFG, T.body, std::max(1, (int)((box.x + box.w - ROW_PADX - px) * P.scale)), -1, 0, true, 400);
            if (!P.warm && LN)
                P.tex(LN->tex, px, py + (LH - texH(LN, P.scale)) / 2);
            py += LH;
        }

        SCard card;
        card.kind            = SCard::DIGEST;
        card.box             = box;
        card.group           = D.key;
        card.expandable      = true;
        card.expansionButton = true;
        const bool PILL_HOV  = hovered.kind == SCard::DIGEST && hovered.group == D.key && hovered.part == 5;
        card.expandButton    = paintExpandPill(P, T, box, std::to_string(D.items.size()), false, PILL_HOV);
        cards.push_back(std::move(card));
    }

    // The expanded bundle: a header that owns the app's identity and count,
    // then every child as a text-only row. Right-click or hold-menu Dismiss
    // handles removing the whole bundle; no trailing close icon is needed.
    void paintGroup(const SPaint& P, const SType& T, const SDisp& D, const CBox& box, const std::vector<double>& childH) {
        const auto   COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker);
        const auto& SUBHEX = hexOfCached(COLSUB);
        const float RP     = rPow();

        const auto&  NEWEST = D.items.front();
        const bool   HHOV   = hovered.kind == SCard::GHEAD && hovered.group == D.key && hovered.part == 0;
        const double HEADRH = groupHeadH();
        // One outer surface owns the group's silhouette, so the final child's
        // bottom corners inherit the full notification radius. Header/child
        // state layers keep the small connected radius inside that outline.
        P.rect(box, surface(), rRow(P.scale), RP);
        if (HHOV)
            P.rect(CBox{box.x, box.y, box.w, HEADRH}, stateLayer(), rJoint(P.scale), RP);

        if (warmGate.warming)
            ensureIconTex(*NEWEST, (int)std::lround(cfg.maxIcon->value() * P.scale));
        const auto& IDT = NEWEST->identTex;
        if (IDT)
            P.texFit(IDT, CBox{box.x + ROW_PADX, box.y + ROW_PADT, CHILD_ICON, CHILD_ICON}, (int)std::lround(CHILD_ICON * 10.0 / 44.0 * P.scale), RP);

        const double PILLX = box.x + box.w - ROW_PADX - CONTENT_END;

        const double TX = box.x + ROW_PADX + CHILD_ICON + ROW_ICON_GAP;
        auto&        HB = scratch();
        appendEsc(HB, NEWEST->appName);
        HB += " <span foreground=\"";
        HB += SUBHEX;
        HB += "\">• ";
        HB += std::to_string(D.items.size());
        HB += " • ";
        HB += ageString(NEWEST->arrived);
        HB += "</span>";
        const auto HL = cachedText(HB, COLTITLE, T.title, std::max(1, (int)((PILLX - 8 - TX) * P.scale)), -1, 0, true, 600);
        if (!P.warm && HL)
            P.tex(HL->tex, TX, box.y + ROW_PADT + (CHILD_ICON - texH(HL, P.scale)) / 2);

        {
            SCard card;
            card.kind  = SCard::GHEAD;
            card.box   = CBox{box.x, box.y, box.w, HEADRH};
            card.group = D.key;
            card.expandable      = true;
            card.expansionButton = true;
            const bool PILL_HOV  = hovered.kind == SCard::GHEAD && hovered.group == D.key && hovered.part == 5;
            card.expandButton    = paintExpandPill(P, T, card.box, std::to_string(D.items.size()), true, PILL_HOV);
            cards.push_back(std::move(card));
        }

        // the children, each fully readable (no third fold state)
        double cy = box.y + HEADRH;
        const size_t VISIBLE = std::min(D.items.size(), childH.size());
        for (size_t k = 0; k < VISIBLE; k++) {
            const auto& N = D.items[k];
            cy += CHILD_GAP;
            const double CH2  = childH[k];
            const bool   CHOV = hovered.kind == SCard::CHILD && hovered.id == N->id && hovered.btn < 0 && hovered.part == 0;
            // Keep the ROM's 0.5dp layout gap, but center a drawable hairline
            // over it. At scale 1 the raw gap rounds to zero physical pixels,
            // which Hyprland rejects as invalid render geometry.
            const double DIVIDER_H = std::max(CHILD_GAP, 1.0 / std::max(P.scale, 0.01));
            P.rect(CBox{box.x, cy - (CHILD_GAP + DIVIDER_H) / 2, box.w, DIVIDER_H}, color(cfg.colFrame), 0);
            if (CHOV)
                P.rect(CBox{box.x, cy, box.w, CH2}, stateLayer(), rJoint(P.scale), RP);
            SCard card;
            card.group = D.key;
            renderRow(P, T, N, CBox{box.x, cy, box.w, 0}, true, false, ROW_CHILD, card, true);
            cards.push_back(std::move(card));
            cy += CH2;
        }
    }

} // namespace NHyprnotify
