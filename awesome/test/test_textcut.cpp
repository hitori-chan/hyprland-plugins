// awesome/test/test_textcut.cpp — the cut a card's text takes before pango:
// never inside a tag, an entity or a codepoint, open tags closed.
#include "../notify/textcut.hpp"

#include "harness.hpp"

#include <cstdint>

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
    return true;
}
