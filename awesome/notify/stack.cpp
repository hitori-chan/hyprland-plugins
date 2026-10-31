// An app's cards stacked under the newest. COLLAPSED: the newest card over
// a deck of up to two layers, each 12 px narrower a side and 8 px lower
// than the one above it and a step darker — the depth says there is more,
// no count does. EXPANDED: the app's name and Show Less above the stack,
// then every card the page holds as itself (center.cpp lays those out), and
// "+N more" counting the rest. The silent fold is drawn here too: the
// ranking's quiet tail folded into one row.
//
// The texture rule as everywhere: textures are requested unconditionally,
// painting goes through the paint context, which no-ops inside a warm.

#include "ui.hpp"

namespace NAwesome::Notify {

    // the deck's own drop, as its board draws it (0 6px 14px)
    static constexpr double DECK_DROP = 14, DECK_SINK = 6;

    size_t deckLayers(size_t count) {
        return std::min(DECK_LAYERS, count > 0 ? count - 1 : 0);
    }

    double deckDepth(size_t count) {
        return DECK_STEP * (double)deckLayers(count);
    }

    void paintDeck(const SPaint& P, const CBox& top, size_t count) {
        const auto  RIM = color(cfg().getColor("plugin:awesome:theme:col_rim")), GLASS = color(cfg().getColor("plugin:awesome:theme:col_glass"));
        const int   R   = (int)std::lround(DECK_ROUND * P.scale);
        const float RP  = rPow();
        // deepest first: each layer shows below the one over it, and the top
        // card's glass covers all of them but their bottom edges
        for (size_t d = deckLayers(count); d > 0; d--) {
            const double I = DECK_INSET * (double)d;
            const CBox   L{top.x + I, top.y + top.h + DECK_STEP * (double)d - DECK_H, top.w - 2 * I, DECK_H};
            P.glass(L, GLASS, R, RP);
            P.shadow(CBox{L}.expand(DECK_DROP).translate(Vector2D{0.0, DECK_SINK}), R, RP, (int)DECK_DROP, tShadow(), L);
            P.ring(L, RIM.modifyA(RIM.a * (1.f - 0.2f * (float)d)), R, RP);
        }
    }

    void paintStackHead(const SPaint& P, const SType& T, const SDisp& D, const CBox& row) {
        const auto   COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title"));
        const auto&  N        = *D.items.front();
        const bool   HOV      = hovered.kind == SCard::SHEAD && hovered.group == D.key && hovered.part == 1;
        const auto   LESS     = cachedText("Show Less", COLTITLE, T.meta, (int)std::lround(row.w * P.scale), -1, 0, false, 600);
        const double LW       = texW(LESS, P.scale) + 2 * CAP_PADX;
        const CBox   LB{row.x + row.w - LW, row.y + (row.h - CAP_H) / 2, LW, CAP_H};
        // the app names its stack, bare on the desktop as the heading is
        const int    NAMEPX = std::max(1, (int)std::floor((row.w - 4 - LW - 8) * P.scale));
        if (const auto NAME = cachedText(N.appName.empty() ? N.appKey : N.appName, COLTITLE, T.title, NAMEPX, -1, 0, false, 600, nullptr, false, eShadow::DESKTOP))
            texAt(P, NAME, row.x + 4, row.y + (row.h - texH(NAME, P.scale)) / 2);
        paintChip(P, LB, HOV);
        texCentered(P, LESS, LB);

        SCard card = recycledCard();
        card.kind  = SCard::SHEAD;
        card.box   = row;
        card.id    = N.id; // the newest: the card menu's target
        card.group = D.key;
        card.less  = LB;
        cards.push_back(std::move(card));
    }

    void paintSilentFold(const SPaint& P, const SType& T, const SDisp& D, const CBox& row) {
        const auto COLBG = color(cfg().getColor("plugin:awesome:theme:col_glass")), COLRIM = color(cfg().getColor("plugin:awesome:theme:col_rim")),
                   COLFG = color(cfg().getColor("plugin:awesome:notify:col_fg")), COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title")),
                   COLSUB = color(cfg().getColor("plugin:awesome:notify:col_kicker"));
        constexpr double ICON = 24, STEP = 16, PADL = 10, PADR = 14, GAP = 10, CHEV = 10;

        // the apps, newest first: two lend their icons, the first three
        // their names
        static std::vector<const SNotif*> apps; // reused; main thread only
        apps.clear();
        for (const auto& N : D.items)
            if (std::ranges::none_of(apps, [&](const SNotif* A) { return A->appKey == N->appKey; })) {
                if (apps.size() < 2)
                    prepareCard(N, P.scale);
                apps.push_back(N.get());
            }
        const size_t count = apps.size();
        const auto   name  = [](const SNotif* N) -> const std::string& { return N->appName.empty() ? N->appKey : N->appName; };
        auto&      MB   = scratch();
        MB += std::to_string(D.items.size());
        MB += " silent from ";
        MB += name(apps[0]);
        if (count == 2) {
            MB += " and ";
            MB += name(apps[1]);
        } else if (count >= 3) {
            MB += ", ";
            MB += name(apps[1]);
            MB += " and ";
            if (count == 3)
                MB += name(apps[2]);
            else {
                MB += std::to_string(count - 2);
                MB += " more";
            }
        }

        const bool   HOV = hovered.kind == SCard::SILENT;
        const auto   CHEVRON = Glyph::tex(Glyph::eGlyph::CHEVRON_DOWN, COLSUB, (int)std::lround(CHEV * P.scale));
        const auto   CHEVRONHOV = Glyph::tex(Glyph::eGlyph::CHEVRON_DOWN, COLTITLE, (int)std::lround(CHEV * P.scale)); // both faces warm: a hover only damages
        const size_t FACES = std::min<size_t>(count, 2);
        const double TX    = row.x + PADL + ICON + STEP * (double)(FACES - 1) + GAP;
        const double CX    = row.x + row.w - PADR - CHEV;
        const auto   TEXT  = cachedText(MB, COLFG, T.body, std::max(1, (int)std::floor((CX - GAP - TX) * P.scale)), -1, 0, false, 400);

        P.frost(row, COLBG, COLRIM, (int)std::lround(FOLD_H / 2 * P.scale), 2.f);
        const double IY = row.y + (row.h - ICON) / 2;
        const int    IR = (int)std::lround(6 * P.scale);
        for (size_t i = 0; i < FACES; i++) {
            const auto& TEX = apps[i]->identTex;
            if (!TEX || TEX->m_texID == 0)
                continue;
            const CBox B{row.x + PADL + STEP * (double)i, IY, ICON, ICON};
            if (i > 0) // the second over the first's edge, on a ring of the glass's own dark
                P.rect(CBox{B}.expand(1.5), COLBG.modifyA(1.f), (int)std::lround(7.5 * P.scale), 2.f);
            P.texFit(TEX, B, IR, 2.f);
        }
        if (TEXT && TEXT->tex)
            texAt(P, TEXT, TX, row.y + (row.h - texH(TEXT, P.scale)) / 2);
        if (const auto& G = HOV ? CHEVRONHOV : CHEVRON)
            P.texIn(G, CBox{CX, row.y, CHEV, row.h});

        SCard card = recycledCard();
        card.kind  = SCard::SILENT;
        card.box   = row;
        card.group = D.key;
        cards.push_back(std::move(card));
    }

    void paintStackTail(const SPaint& P, const SType& T, size_t more, const CBox& row) {
        auto& MB = scratch();
        MB += "+";
        MB += std::to_string(more);
        MB += " more";
        const auto   TX = cachedText(MB, color(cfg().getColor("plugin:awesome:notify:col_title")), T.meta, (int)std::lround(row.w * P.scale), -1, 0, false, 600);
        const double W  = texW(TX, P.scale) + 2 * CAP_PADX;
        const CBox   B{row.x + (row.w - W) / 2, row.y + (row.h - CAP_H) / 2, W, CAP_H};
        paintChip(P, B, false);
        texCentered(P, TX, B);
    }

} // namespace NAwesome::Notify
