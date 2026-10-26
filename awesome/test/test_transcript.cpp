// awesome/test/test_transcript.cpp — a chat card's words: the legacy sender
// lead, one paragraph per message, the window fitted from the newest end.
#include "../notify/transcript.hpp"

#include "harness.hpp"

#include <string>
#include <vector>

namespace {
    using namespace NAwesome::Notify::Transcript;

    // a monospace stand-in for pango: `width` characters a line, a
    // LINE_SEP or '\n' starts a line, a paragraph takes one at least
    struct SGrid {
        size_t width, lines;
        size_t count(const std::string& s) const {
            size_t total = 0;
            for (size_t start = 0; start <= s.size();) {
                auto end = s.find('\n', start);
                if (end == std::string::npos)
                    end = s.size();
                const auto PARA = s.substr(start, end - start);
                for (size_t prev = 0;;) {
                    const auto AT    = PARA.find(LINE_SEP, prev);
                    const auto CHARS = visibleChars(PARA.substr(prev, AT == std::string::npos ? std::string::npos : AT - prev));
                    total += CHARS == 0 ? 1 : (CHARS + width - 1) / width;
                    if (AT == std::string::npos)
                        break;
                    prev = AT + LINE_SEP.size();
                }
                start = end + 1;
            }
            return total;
        }
        bool operator()(const std::string& s) const {
            return count(s) <= lines;
        }
    };

    // the window both ways — exact, and counted one paragraph at a time then
    // checked whole — which must agree wherever every line is one line tall
    std::string both(const std::vector<std::string>& P, const SGrid& G) {
        const auto EXACT   = fitTail(P, G);
        const auto COUNTED = fitTail(P, G.lines, [&](const std::string& s) { return G.count(s); }, G);
        return EXACT == COUNTED ? EXACT : "<counted and exact disagree>";
    }

    // a grid whose 😀 lines are two lines tall: the count says one
    struct STall : SGrid {
        size_t count(const std::string& s) const {
            size_t n = SGrid::count(s);
            for (size_t at = 0; (at = s.find("😀", at)) != std::string::npos; at++)
                n++;
            return n;
        }
        bool operator()(const std::string& s) const {
            return count(s) <= lines;
        }
    };
}

bool test_transcript() {
    // the lead: Telegram's bold sender line, nothing else
    {
        const auto L = senderLead("<b>Alice</b>\nanyone tried it?");
        AW_CHECK(L && L->sender == "Alice" && L->text == "anyone tried it?");
        AW_CHECK(senderLead("<b>Minh Nguy&#7877;n</b>\nPhoto")->sender == "Minh Nguy&#7877;n");
        AW_CHECK(!senderLead("<b>Bob</b> sent a photo\nnice")); // goes on past the name
        AW_CHECK(!senderLead("<b>Bob</b>"));                     // no message under it
        AW_CHECK(!senderLead("plain\ntext"));
        AW_CHECK(!senderLead("<b><i>Eve</i></b>\nhi")); // markup inside: not a bare name
        AW_CHECK(!senderLead("<b></b>\nhi"));
        AW_CHECK(senderLead("<b>Bob</b>  \r\nhi")->text == "hi");
    }

    // a paragraph: the sender bold, the message's breaks kept inside it
    {
        AW_CHECK(paragraph("Quang", "Packing list:\n- tent\n- snacks") == "<b>Quang</b>: Packing list: - tent - snacks");
        AW_CHECK(paragraph("", "are you coming tonight?") == "are you coming tonight?");
        AW_CHECK(paragraph("Bob", "\n\nhi\n\n") == "<b>Bob</b>: hi");
        AW_CHECK(paragraph("Bob", "a\r\nb") == "<b>Bob</b>: a b");
        AW_CHECK(flatten(paragraph("Quang", "list:\n- tent")) == "<b>Quang</b>: list: - tent");
    }

    // the head of a paragraph: whole characters, open tags closed
    {
        AW_CHECK(visibleChars("<b>Bob</b>: a&amp;b") == 8);
        AW_CHECK(headCut("<b>Bob</b>: also turn off", 10) == "<b>Bob</b>: also…");
        AW_CHECK(headCut("<i>abcdef</i>", 3) == "<i>abc…</i>");
        AW_CHECK(headCut("x&amp;y", 2) == "x&amp;…");
        AW_CHECK(headCut("ééé", 1) == "é…");
        // a word split at the cut backs off to its space, not below minKeep
        AW_CHECK(headCut("<b>Bob</b>: also turn off", 12, 5) == "<b>Bob</b>: also…");
        AW_CHECK(headCut("<b>Bob</b>: also turn off", 12, 11) == "<b>Bob</b>: also tu…");
        AW_CHECK(headCut("<b>Bob</b>: also turn off", 14, 5) == "<b>Bob</b>: also turn…"); // the cut meets a space
        AW_CHECK(headCut("<i>one two</i>three", 7, 0) == "<i>one…</i>");               // a tag does not end a word
        AW_CHECK(headCut("ありがとうございます", 4, 0) == "ありがと…");
    }

    // the window: the newest whole, older ones above while they fit
    {
        const std::vector<std::string> P = {paragraph("Alice", "first message here"), paragraph("Bob", "second"), paragraph("Alice", "third")};
        // everything fits
        AW_CHECK(both(P, SGrid{40, 8}) == P[0] + "\n" + P[1] + "\n" + P[2]);
        // two lines: the two newest
        AW_CHECK(both(P, SGrid{40, 2}) == P[1] + "\n" + P[2]);
        // one line: the newest alone
        AW_CHECK(both(P, SGrid{40, 1}) == P[2]);
        // no paragraphs, no window
        AW_CHECK(both({}, SGrid{40, 2}).empty());
    }
    // the newest overflowing alone is still the window (the layout cuts it)
    {
        const std::vector<std::string> P = {"old", std::string(200, 'x')};
        AW_CHECK(both(P, SGrid{40, 2}) == P[1]);
    }
    // the first older message that does not fit keeps its head
    {
        const std::string LONG = paragraph("Bob", "also turn off new_optimizations if your wallpaper is animated, otherwise the blur lags");
        const std::vector<std::string> P = {paragraph("Alice", "screenshot pls"), LONG, paragraph("Charlotte", "ok")};
        const auto OUT = both(P, SGrid{30, 3});
        AW_CHECK(OUT.ends_with("\n" + P[2]));
        AW_CHECK(OUT.starts_with("<b>Bob</b>: also turn off"));
        AW_CHECK(OUT.find("…") != std::string::npos);
        AW_CHECK(OUT.find("Alice") == std::string::npos); // nothing above the cut message
        AW_CHECK((SGrid{30, 3}(OUT)));
    }
    // a head too short to say anything is left out
    {
        const std::vector<std::string> P = {paragraph("Bob", std::string(100, 'y')), std::string(55, 'z')};
        AW_CHECK(both(P, SGrid{30, 2}) == P[1]); // the newest takes both lines
    }
    // a message's own lines count: the window holds whole messages
    {
        const std::vector<std::string> P = {paragraph("Linh", "Bus at 7:30"), paragraph("Quang", "Packing list:\n- tent\n- snacks\n- headlamp")};
        AW_CHECK(both(P, SGrid{40, 5}) == P[0] + "\n" + P[1]);
        AW_CHECK(both(P, SGrid{40, 4}) == P[1]);
    }
    // a line taller than the count: the counted window fails its check and
    // the exact fit decides
    {
        const std::vector<std::string> P = {paragraph("Ann", "one"), paragraph("Ben", "two 😀"), paragraph("Cy", "three")};
        const STall                    G{{40, 3}};
        const auto OUT = fitTail(P, 3, [&](const std::string& s) { return G.SGrid::count(s); }, G);
        AW_CHECK(OUT == P[1] + "\n" + P[2]);
        AW_CHECK(G(OUT));
    }
    return true;
}
