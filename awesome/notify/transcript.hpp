// A chat card's words, pure and testable: the sender a legacy chat client
// leads its body with, one paragraph per message, and the window a card
// shows, fitted from the newest end.
#pragma once

#include "textcut.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NAwesome::Notify::Transcript {

    // U+2028 LINE SEPARATOR: a message's own line breaks stay inside its
    // paragraph, so the hanging indent marks every line that continues a
    // message and only a message's first line meets the margin
    inline constexpr std::string_view LINE_SEP = " ";

    struct SLead {
        std::string sender, text;
    };

    // The group form Telegram Desktop (and the XDG chat clients after it)
    // posts: the sender as a bold line of its own, "<b>Alice</b>\nmessage".
    // A line that goes on past the name ("<b>Bob</b> sent a photo") or a
    // name with markup inside is no bare sender.
    inline std::optional<SLead> senderLead(std::string_view body) {
        if (!body.starts_with("<b>"))
            return std::nullopt;
        const auto CLOSE = body.find("</b>");
        if (CLOSE == std::string_view::npos || CLOSE == 3)
            return std::nullopt;
        const auto NL = body.find('\n', CLOSE);
        if (NL == std::string_view::npos || body.find_first_not_of(" \t\r", CLOSE + 4) != NL)
            return std::nullopt;
        const auto NAME = body.substr(3, CLOSE - 3);
        if (NAME.find_first_of("<\n") != std::string_view::npos)
            return std::nullopt;
        return SLead{std::string{NAME}, std::string{body.substr(NL + 1)}};
    }

    // One message as one paragraph: "<b>Sender</b>: text" (a sender only in
    // a group), its line breaks turned to LINE_SEP and the blank ones at its
    // ends dropped. Sender and text are sanitized markup already.
    inline std::string paragraph(std::string_view sender, std::string_view text) {
        const auto FIRST = text.find_first_not_of("\r\n");
        const auto LAST  = text.find_last_not_of("\r\n");
        text             = FIRST == std::string_view::npos ? std::string_view{} : text.substr(FIRST, LAST - FIRST + 1);
        std::string out;
        out.reserve(sender.size() + text.size() + 16);
        if (!sender.empty()) {
            out += "<b>";
            out += sender;
            out += "</b>: ";
        }
        for (const char C : text) {
            if (C == '\n')
                out += LINE_SEP;
            else if (C != '\r')
                out += C;
        }
        return out;
    }

    // a paragraph read as one line: the collapsed row's
    inline std::string flatten(std::string s) {
        for (size_t at = 0; (at = s.find(LINE_SEP, at)) != std::string::npos;)
            s.replace(at, LINE_SEP.size(), " ");
        return s;
    }

    // the visible characters of sanitized markup: an entity is one, a tag
    // none
    inline size_t visibleChars(const std::string& s) {
        TextCut::SOpen open;
        size_t         n = 0;
        for (size_t i = 0; i < s.size();) {
            const bool TAG = s[i] == '<';
            if (!TextCut::step(s, i, open))
                break;
            n += !TAG;
        }
        return n;
    }

    // The head of a paragraph: its first `keep` visible characters, the
    // spaces before the cut dropped, an ellipsis for the rest and the tags
    // still open closed. A cut inside a word backs off to the space before
    // it while at least `minKeep` characters stay (text without spaces is
    // cut where it stands).
    inline std::string headCut(const std::string& s, size_t keep, size_t minKeep = std::string::npos) {
        TextCut::SOpen open, wordOpen;
        size_t         i = 0, seen = 0, wordAt = std::string::npos;
        bool           afterSpace = false;
        while (i < s.size()) {
            const bool TAG = s[i] == '<';
            if (!TAG && seen == keep)
                break;
            if (!TAG && s[i] == ' ' && seen >= minKeep) {
                wordAt   = i;
                wordOpen = open;
            }
            size_t j = i;
            if (!TextCut::step(s, j, open))
                break;
            if (!TAG)
                afterSpace = s[i] == ' ';
            seen += !TAG;
            i = j;
        }
        size_t next = i;
        while (next < s.size() && s[next] == '<') {
            const auto CLOSE = s.find('>', next);
            next             = CLOSE == std::string::npos ? s.size() : CLOSE + 1;
        }
        if (wordAt != std::string::npos && !afterSpace && next < s.size() && s[next] != ' ') {
            i    = wordAt;
            open = std::move(wordOpen);
        }
        std::string out = s.substr(0, i);
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        out += "…";
        for (auto it = open.tags.rbegin(); it != open.tags.rend(); ++it) {
            out += "</";
            out += it->name;
            out += '>';
        }
        return out;
    }

    // what a partly shown message keeps at least past its sender: fewer
    // characters say nothing
    inline constexpr size_t MIN_HEAD = 12;

    // The longest head of paragraph P that `ok` admits — its sender and at
    // least MIN_HEAD characters past it — or "" when none is.
    template <class Ok>
    std::string bestHead(const std::string& P, Ok&& ok) {
        const auto   SENDER = P.starts_with("<b>") ? P.find("</b>: ") : std::string::npos;
        const size_t LEAD   = SENDER == std::string::npos ? 0 : visibleChars(P.substr(0, SENDER + 6));
        const size_t TOTAL  = visibleChars(P);
        const size_t FLOOR  = LEAD + MIN_HEAD;
        size_t       lo = FLOOR, hi = TOTAL - 1, best = 0;
        while (lo <= hi && hi < TOTAL) {
            const size_t MID = lo + (hi - lo) / 2;
            if (ok(headCut(P, MID, FLOOR))) {
                best = MID;
                lo   = MID + 1;
            } else
                hi = MID - 1;
        }
        return best ? headCut(P, best, FLOOR) : std::string{};
    }

    // The window a chat card shows, fitted from the newest end. `paras` run
    // oldest first; `fits(markup)` says whether paragraphs joined by '\n'
    // lay out within the card's height. The newest paragraph is shown whole
    // (the layout ellipsizes its end when it alone overflows), then older
    // ones above it while they fit; the first one that does not keeps the
    // head that fits (bestHead) and ends the window. Never a message's
    // tail: a line that begins mid-sentence reads as a message of its own.
    template <class Fits>
    std::string fitTail(const std::vector<std::string>& paras, Fits&& fits) {
        if (paras.empty())
            return {};
        std::string shown = paras.back();
        for (size_t i = paras.size() - 1; i-- > 0;) {
            std::string cand = paras[i] + "\n" + shown;
            if (fits(cand)) {
                shown = std::move(cand);
                continue;
            }
            if (const auto HEAD = bestHead(paras[i], [&](const std::string& h) { return fits(h + "\n" + shown); }); !HEAD.empty())
                shown = HEAD + "\n" + shown;
            break;
        }
        return shown;
    }

    // The same window, counted: `lines(p)` is how many lines one paragraph
    // takes at the card's width, `budget` how many the card holds. Pango
    // lays out each paragraph on its own, so the lines add up — one small
    // layout a paragraph instead of the growing window's — and the window
    // is checked once, whole, with `fits`. A line taller than the font's (an
    // emoji's fallback font) fails that check; the exact fit then decides.
    template <class Lines, class Fits>
    std::string fitTail(const std::vector<std::string>& paras, size_t budget, Lines&& lines, Fits&& fits) {
        if (paras.empty())
            return {};
        std::string shown = paras.back();
        size_t      used  = lines(shown);
        if (used >= budget) // the newest fills the card alone: nothing above it fits
            return shown;
        for (size_t i = paras.size() - 1; i-- > 0 && used < budget;) {
            const size_t N = lines(paras[i]);
            if (used + N <= budget) {
                shown = paras[i] + "\n" + shown;
                used += N;
                continue;
            }
            const size_t ROOM = budget - used;
            if (const auto HEAD = bestHead(paras[i], [&](const std::string& h) { return lines(h) <= ROOM; }); !HEAD.empty())
                shown = HEAD + "\n" + shown;
            break;
        }
        return fits(shown) ? shown : fitTail(paras, fits);
    }

} // namespace NAwesome::Notify::Transcript
