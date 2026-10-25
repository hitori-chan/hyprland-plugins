// awesome/notify/stack.cpp — an app's cards stacked under the newest, as
// macOS's Notification Center draws them. COLLAPSED: the newest card over a
// deck of up to two layers, each 12 px narrower a side and 8 px lower than
// the one above it and a step darker — the depth says there is more, no
// count does. EXPANDED: Show Less and a close button right-aligned above the
// stack on glass of their own, then every card the page holds as itself
// (center.cpp lays those out), and "+N more" counting the rest.
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
        const auto  RIM = color(cfg().getColor("plugin:awesome:notify:col_frame"));
        const int   R   = (int)std::lround(DECK_ROUND * P.scale);
        const float RP  = rPow();
        // deepest first: each layer shows below the one over it, and the top
        // card's glass covers all of them but their bottom edges
        for (size_t d = deckLayers(count); d > 0; d--) {
            const double I = DECK_INSET * (double)d;
            const CBox   L{top.x + I, top.y + top.h + DECK_STEP * (double)d - DECK_H, top.w - 2 * I, DECK_H};
            P.shadow(CBox{L}.expand(DECK_DROP).translate(Vector2D{0.0, DECK_SINK}), R, RP, (int)DECK_DROP);
            P.glass(L, d == 1 ? tDeck1() : tDeck2(), R, RP);
            P.ring(L, RIM.modifyA(RIM.a * (1.f - 0.2f * (float)d)), R, RP);
        }
    }

    void paintStackHead(const SPaint& P, const SType& T, const SDisp& D, const CBox& row) {
        const auto   COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title"));
        const bool   HOV      = hovered.kind == SCard::SHEAD && hovered.group == D.key;
        const auto   LESS     = cachedText("Show Less", COLTITLE, T.meta, (int)std::lround(row.w * P.scale), -1, 0, false, 600);
        const auto   XG       = Glyph::tex(Glyph::eGlyph::DISMISS, COLTITLE, (int)std::lround(CLOSE_GLYPH * P.scale));
        const double CY       = row.y + (row.h - CAP_H) / 2;
        const CBox   XB{row.x + row.w - CAP_H, CY, CAP_H, CAP_H};
        const double LW = texW(LESS, P.scale) + 2 * CAP_PADX;
        const CBox   LB{XB.x - CAP_GAP - LW, CY, LW, CAP_H};
        paintChip(P, LB, HOV && hovered.part == 1);
        texCentered(P, LESS, LB);
        paintChip(P, XB, HOV && hovered.part == 2);
        if (XG)
            P.texIn(XG, XB);

        // the close button is a part of the header, not a card's CLOSE
        SCard card = recycledCard();
        card.kind  = SCard::SHEAD;
        card.box   = row;
        card.id    = D.items.front()->id; // the newest: the card menu's target
        card.group = D.key;
        card.less  = LB;
        card.close = XB;
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
