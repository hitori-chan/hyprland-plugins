// The cut a card's text takes before pango: never inside a tag, an entity
// or a codepoint, open tags closed.
#include "../notify/textcut.hpp"

#include "harness.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

bool test_textcut() {
    using NAwesome::Notify::cutToLines;
    constexpr size_t ANY = SIZE_MAX;

    // what fits is left alone
    AW_CHECK(cutToLines("short", false, 100, ANY).empty());
    AW_CHECK(cutToLines(std::string(50, 'a'), false, 50, ANY).empty());

    // plain text: a character budget
    AW_CHECK(cutToLines(std::string(1000, 'a'), false, 100, ANY) == std::string(100, 'a'));

    // line breaks: the cut keeps maxBreaks + 1 of them
    {
        std::string s;
        for (int i = 0; i < 600; i++)
            s += "a\n";
        AW_CHECK(cutToLines(s, false, 1000, 2) == "a\na\na\n");
    }

    // a codepoint is never split
    {
        std::string s;
        for (int i = 0; i < 100; i++)
            s += "\xc3\xa9"; // é
        const auto OUT = cutToLines(s, false, 10, ANY);
        AW_CHECK(OUT.size() == 20);
        AW_CHECK(OUT == s.substr(0, 20));
    }

    // markup: tags cost nothing, the open ones are closed in order
    AW_CHECK(cutToLines("<b>" + std::string(500, 'x') + "</b>", true, 10, ANY) == "<b>" + std::string(10, 'x') + "</b>");
    AW_CHECK(cutToLines("<span foreground=\"#ffffff\"><i>" + std::string(500, 'x') + "</i></span>", true, 3, ANY) == "<span foreground=\"#ffffff\"><i>xxx</i></span>");
    // a tag already closed is not closed again
    AW_CHECK(cutToLines("<b>ab</b>" + std::string(500, 'x'), true, 5, ANY) == "<b>ab</b>xxx");
    // an entity is one character and is never split
    {
        std::string s;
        for (int i = 0; i < 100; i++)
            s += "&amp;";
        AW_CHECK(cutToLines(s, true, 2, ANY) == "&amp;&amp;");
    }
    // plain text takes '&' and '<' as characters
    AW_CHECK(cutToLines(std::string(20, '&'), false, 4, ANY) == "&&&&");
    AW_CHECK(cutToLines(std::string(20, '<'), false, 4, ANY) == "<<<<");
    // malformed markup is left for the parse fallback
    AW_CHECK(cutToLines("<b" + std::string(500, 'x'), true, 10, ANY).empty());

    using NAwesome::Notify::capMarkupHead;
    // the head cap: within the bytes, never inside a tag or an entity,
    // the open tags closed inside the cap
    AW_CHECK(capMarkupHead("short", 10) == "short");
    {
        const std::string S = "<span foreground=\"#ffffff\">" + std::string(100, 'x') + "</span>";
        const auto        OUT = capMarkupHead(S, 40);
        AW_CHECK(OUT.size() <= 40);
        AW_CHECK(OUT == "<span foreground=\"#ffffff\">" + std::string(40 - 27 - 7, 'x') + "</span>");
    }
    AW_CHECK(capMarkupHead("ab<span foreground=\"#fff\">cd</span>", 10) == "ab"); // the tag does not fit whole
    AW_CHECK(capMarkupHead("a&amp;b&amp;c", 8) == "a&amp;b");                       // never half an entity
    AW_CHECK(capMarkupHead("\xc3\xa9\xc3\xa9\xc3\xa9", 5) == "\xc3\xa9\xc3\xa9");      // never half a codepoint
    // a deep nest: closed within the cap, every opener matched
    {
        std::string deep, want;
        for (int i = 0; i < 5000; i++)
            deep += "<b>";
        deep += "x";
        for (int i = 0; i < 5000; i++)
            deep += "</b>";
        for (int i = 0; i < 14; i++)
            want += "<b>";
        for (int i = 0; i < 14; i++)
            want += "</b>";
        AW_CHECK(capMarkupHead(deep, 100) == want);
    }

    return true;
}
