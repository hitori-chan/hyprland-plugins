// awesome/notify/bundle.cpp — an app's cards folded into one: the two faces
// of a bundle, both one glass card. The DIGEST (folded): the app's icon,
// name and count with a down chevron, then the two newest cards a line
// each — enough to decide whether to open it. The GROUP (open): the same
// header with an up chevron and a ✕ that dismisses the lot, then every
// child the page holds as a compact card between hairlines (no icon: the
// header owns the identity), and "+N more" counting the rest.
//
// The texture rule as everywhere: textures are requested unconditionally,
// painting goes through the paint context, which no-ops inside a warm.

#include "ui.hpp"

namespace NAwesome::Notify {

    double digestH(const SType& T, size_t count, double scale) {
        (void)T;
        return GROUP_HEAD_H + (double)std::min<size_t>(2, count) * (bodyLineH(scale) + 4) + 8;
    }

    double groupChromeH(bool cut) {
        return GROUP_HEAD_H + (cut ? TAIL_H : 0);
    }

    // the header both faces share: icon, "App" and its count; the circles
    // at the right are the caller's (one chevron, or a chevron and a ✕)
    static void paintHeader(const SPaint& P, const SType& T, const SDisp& D, const CBox& row, double rightEdge) {
        const auto  COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title"));
        const auto& NEWEST   = D.items.front();
        prepareCard(NEWEST, P.scale);
        const auto& IDT = NEWEST->identTex && NEWEST->identTex->m_texID ? NEWEST->identTex : contentFace(*NEWEST, true);
        if (IDT)
            P.texFit(IDT, CBox{row.x + PADX, row.y + (row.h - GROUP_ICON) / 2, GROUP_ICON, GROUP_ICON}, (int)std::lround(5 * P.scale), rPow());

        const double TX    = row.x + PADX + GROUP_ICON + 10;
        const auto   COUNT = cachedText(std::to_string(D.items.size()), COLTITLE, T.small, 64, -1, 0, false, 600);
        const double PILLW = std::max(PILL_H, texW(COUNT, P.scale) + 10);
        auto&        NB    = scratch();
        appendEsc(NB, NEWEST->appName);
        const auto NAME = cachedText(NB, COLTITLE, T.title, std::max(1, (int)((rightEdge - 8 - PILLW - 6 - TX) * P.scale)), -1, 0, true, 600);
        double     tx   = TX;
        if (NAME && NAME->tex) {
            P.tex(NAME->tex, tx, row.y + (row.h - texH(NAME, P.scale)) / 2);
            tx += texW(NAME, P.scale) + 6;
        }
        const CBox PB{tx, row.y + (row.h - PILL_H) / 2, PILLW, PILL_H};
        P.rect(PB, tFill(), (int)std::lround(PILL_H / 2 * P.scale), 2.f);
        texCentered(P, COUNT, PB);
    }

    static void paintCircle(const SPaint& P, const CBox& c, Glyph::eGlyph g, bool hover) {
        P.rect(c, hover ? tFillHover() : tFill(), (int)std::lround(c.w / 2 * P.scale), 2.f);
        if (const auto GL = Glyph::tex(g, color(cfg().getColor("plugin:awesome:notify:col_title")), (int)std::lround(14 * P.scale)))
            P.texIn(GL, c);
    }

    void paintDigest(const SPaint& P, const SType& T, const SDisp& D, const CBox& box) {
        const auto COLBG = color(cfg().getColor("plugin:awesome:notify:col_bg")), COLRIM = color(cfg().getColor("plugin:awesome:notify:col_frame")),
                   COLFG = color(cfg().getColor("plugin:awesome:notify:col_fg"));
        P.frost(box, COLBG, COLRIM, rCard(P.scale), rPow());

        const bool HOV = hovered.kind == SCard::DIGEST && hovered.group == D.key;
        const CBox CHV{box.x + box.w - PADX - CIRCLE, box.y + (GROUP_HEAD_H - CIRCLE) / 2, CIRCLE, CIRCLE};
        paintCircle(P, CHV, Glyph::eGlyph::CHEVRON_DOWN, HOV);
        paintHeader(P, T, D, CBox{box.x, box.y, box.w, GROUP_HEAD_H}, CHV.x);

        // the two newest, a line each: "Title  newest line"
        const auto&  SUBHEX = hexOfCached(color(cfg().getColor("plugin:awesome:notify:col_kicker")));
        const double LH     = bodyLineH(P.scale);
        double       py     = box.y + GROUP_HEAD_H;
        for (size_t i = 0; i < std::min<size_t>(2, D.items.size()); i++) {
            const auto& N  = D.items[i];
            auto&       LB = scratch();
            LB += "<b>";
            LB += N->summary;
            LB += "</b>  <span foreground=\"";
            LB += SUBHEX;
            LB += "\">";
            LB += lastLine(N->body);
            LB += "</span>";
            const auto LN = cachedText(LB, COLFG, T.body, std::max(1, (int)((box.w - 2 * PADX) * P.scale)), -1, 0, true, 400);
            if (LN && LN->tex)
                P.tex(LN->tex, box.x + PADX, py + (LH - texH(LN, P.scale)) / 2);
            py += LH + 4;
        }

        SCard card = recycledCard();
        card.kind  = SCard::DIGEST;
        card.box   = box;
        card.id    = D.items.front()->id; // the newest: the card menu's target
        card.group = D.key;
        cards.push_back(std::move(card));
    }

    void paintGroup(const SPaint& P, const SType& T, const SDisp& D, const CBox& box, const std::vector<double>& childH) {
        const auto COLBG = color(cfg().getColor("plugin:awesome:notify:col_bg")), COLRIM = color(cfg().getColor("plugin:awesome:notify:col_frame")),
                   COLSUB = color(cfg().getColor("plugin:awesome:notify:col_kicker"));
        P.frost(box, COLBG, COLRIM, rCard(P.scale), rPow());

        const bool HHOV = hovered.kind == SCard::GHEAD && hovered.group == D.key;
        const CBox XB{box.x + box.w - PADX - CIRCLE, box.y + (GROUP_HEAD_H - CIRCLE) / 2, CIRCLE, CIRCLE};
        const CBox CHV{XB.x - 6 - CIRCLE, XB.y, CIRCLE, CIRCLE};
        paintCircle(P, CHV, Glyph::eGlyph::CHEVRON_UP, HHOV && hovered.part == 1);
        paintCircle(P, XB, Glyph::eGlyph::CLOSE, HHOV && hovered.part == 2);
        paintHeader(P, T, D, CBox{box.x, box.y, box.w, GROUP_HEAD_H}, CHV.x);
        {
            SCard card   = recycledCard();
            card.kind    = SCard::GHEAD;
            card.box     = CBox{box.x, box.y, box.w, GROUP_HEAD_H};
            card.id      = D.items.front()->id;
            card.group   = D.key;
            card.close   = XB;
            card.chevron = CHV;
            cards.push_back(std::move(card));
        }

        // the children the page holds, each readable whole; the budget
        // measured exactly these
        const double HAIR  = 1.0 / P.scale;
        double       cy    = box.y + GROUP_HEAD_H;
        const size_t SHOWN = std::min(childH.size(), D.items.size());
        for (size_t k = 0; k < SHOWN; k++) {
            P.rect(CBox{box.x + PADX, cy, box.w - 2 * PADX, HAIR}, tHairline());
            SCard card = recycledCard();
            renderCard(P, T, D.items[k], CBox{box.x, cy, box.w, 0}, SCardForm{.where = SCardForm::CHILD}, card);
            card.group = D.key;
            cards.push_back(std::move(card));
            cy += childH[k];
        }

        // the rest, counted: dismissing a shown child brings the next in
        if (SHOWN < D.items.size()) {
            P.rect(CBox{box.x + PADX, cy, box.w - 2 * PADX, HAIR}, tHairline());
            auto& MB = scratch();
            MB += "+";
            MB += std::to_string(D.items.size() - SHOWN);
            MB += " more";
            texCentered(P, cachedText(MB, COLSUB, T.meta, std::max(1, (int)((box.w - 2 * PADX) * P.scale)), -1, 0, false, 500), CBox{box.x, cy, box.w, TAIL_H});
        }
    }

} // namespace NAwesome::Notify
