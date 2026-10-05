// awesome/test/test_desktop_exec.cpp — the freedesktop Exec= grammar:
// unescaping, the quote/escape tokenizer, and field-code expansion. This
// builds the argv of spawned processes, so the grammar's rejection paths
// are as important as its happy paths.
#include "../core/desktop_exec.hpp"

#include "harness.hpp"

namespace DE = NAwesome::DesktopExec;

static bool wordsEqual(const std::optional<std::vector<std::string>>& got, std::vector<std::string> want) {
    if (!got)
        return false;
    return *got == want;
}

bool test_desktop_exec() {
    // ---- unescapeString (Name, Icon, ...) ----
    AW_CHECK(DE::unescapeString("plain").value() == "plain");
    AW_CHECK(DE::unescapeString("a\\sb").value() == "a b");
    AW_CHECK(DE::unescapeString("a\\nb\\tb\\rb").value() == "a\nb\tb\rb");
    AW_CHECK(DE::unescapeString("a\\\\b").value() == "a\\b");
    AW_CHECK(!DE::unescapeString("trailing\\").has_value()); // dangling escape
    AW_CHECK(!DE::unescapeString("a\\;b").has_value()); // ';' escape is list-only
    AW_CHECK(!DE::unescapeString("a\\qb").has_value()); // unknown escape

    // ---- unescapeList (string-list values) ----
    AW_CHECK(*DE::unescapeList("a;b") == (std::vector<std::string>{"a", "b"}));
    AW_CHECK(*DE::unescapeList("a\\;b;c") == (std::vector<std::string>{"a;b", "c"})); // ';' escape is list-only
    AW_CHECK(DE::unescapeList("").value().empty());
    AW_CHECK(*DE::unescapeList("a;b;") == (std::vector<std::string>{"a", "b"})); // trailing ';'
    AW_CHECK(!DE::unescapeList("a;\\").has_value()); // dangling escape at end of item
    AW_CHECK(!DE::unescapeList("a\\qb").has_value()); // unknown escape

    // ---- unescapeExec: the string pass, before the quoting grammar ----
    AW_CHECK(DE::unescapeExec("a\\sb") == "a b");
    AW_CHECK(DE::unescapeExec("a\\\\b") == "a\\b");
    AW_CHECK(DE::unescapeExec("\"q\\\"x\"") == "\"q\\\"x\""); // an unknown escape stays for tokens()
    AW_CHECK(DE::unescapeExec("dangling\\") == "dangling\\");
    // Wine's generated entry: four backslashes are one literal, an escaped
    // escape-space keeps "Start Menu" one argument
    AW_CHECK(wordsEqual(DE::words(DE::unescapeExec("wine C:\\\\\\\\windows\\\\\\\\start.exe /Unix /home/u/Start\\\\ Menu/x.lnk")),
                        {"wine", "C:\\windows\\start.exe", "/Unix", "/home/u/Start Menu/x.lnk"}));

    // ---- tokens / words (the Exec line) ----
    AW_CHECK(wordsEqual(DE::words("/usr/bin/term -o flag"), {"/usr/bin/term", "-o", "flag"}));
    AW_CHECK(wordsEqual(DE::words("app \"arg with space\""), {"app", "arg with space"}));
    AW_CHECK(wordsEqual(DE::words("app \"a\\\"b\""), {"app", "a\"b"})); // escaped quote inside quotes
    AW_CHECK(wordsEqual(DE::words("app a\\ b"), {"app", "a b"}));       // escaped space outside quotes
    AW_CHECK(wordsEqual(DE::words("app spaced\\tmore  out"), {"app", "spacedtmore", "out"})); // exec-escape: literal t
    AW_CHECK(!DE::words("app \"unclosed").has_value());
    AW_CHECK(!DE::words("app \"a\\ b\"").has_value()); // bad escape inside quotes
    AW_CHECK(!DE::words("app\\").has_value());         // dangling escape
    AW_CHECK(DE::words("   ").value().empty());        // whitespace-only line

    // ---- expand: the executable token must be literal ----
    AW_CHECK(wordsEqual(DE::expand("app", "N", "/f", ""), {"app"}));
    AW_CHECK(!DE::expand("%c", "N", "/f", "").has_value());        // a field cannot name the process
    AW_CHECK(!DE::expand("/bin/%c", "N", "/f", "").has_value());
    AW_CHECK(!DE::expand("%c extra", "N", "/f", "").has_value());

    // ---- expand: scalars ----
    AW_CHECK(wordsEqual(DE::expand("app %c %k", "N", "/f.desktop", ""), {"app", "N", "/f.desktop"}));
    AW_CHECK(wordsEqual(DE::expand("app %c", "My Name", "/f", ""), {"app", "My Name"}));
    // a literal percent survives; %% collapses to one; an unknown code rejects
    AW_CHECK(wordsEqual(DE::expand("app 100%%", "N", "/f", ""), {"app", "100%"}));
    AW_CHECK(!DE::expand("app 50%z", "N", "/f", "").has_value());

    // ---- expand: input fields (the menubar passes no file or URL) ----
    AW_CHECK(wordsEqual(DE::expand("app %f", "N", "/f", ""), {"app"}));                    // whole-arg drops
    AW_CHECK(wordsEqual(DE::expand("app --open %f", "N", "/f", ""), {"app", "--open"}));   // the option stays
    AW_CHECK(wordsEqual(DE::expand("app -f%f", "N", "/f", ""), {"app", "-f"}));            // embedded: empty
    AW_CHECK(wordsEqual(DE::expand("app %u", "N", "/f", ""), {"app"}));
    AW_CHECK(wordsEqual(DE::expand("app %F", "N", "/f", ""), {"app"}));
    AW_CHECK(!DE::expand("app %f %F", "N", "/f", "").has_value()); // two input fields

    // ---- expand: %i ----
    AW_CHECK(wordsEqual(DE::expand("app %i", "N", "/f", "term"), {"app", "--icon", "term"}));
    AW_CHECK(wordsEqual(DE::expand("app %i", "N", "/f", ""), {"app"})); // empty icon: bare drop
    AW_CHECK(!DE::expand("app -i%i", "N", "/f", "term").has_value());   // %i must own its argument

    // ---- expand: deprecated fields expand to empty ----
    AW_CHECK(wordsEqual(DE::expand("app %d", "N", "/f", ""), {"app"}));
    AW_CHECK(wordsEqual(DE::expand("app %D %n %N %v %m", "N", "/f", ""), {"app"}));
    AW_CHECK(wordsEqual(DE::expand("app -x%d", "N", "/f", ""), {"app", "-x"})); // embedded: empty

    // ---- expand: rejection paths (these build an argv) ----
    AW_CHECK(!DE::expand("app %z", "N", "/f", "").has_value()); // unknown field code
    AW_CHECK(!DE::expand("app %", "N", "/f", "").has_value());  // dangling field
    AW_CHECK(!DE::expand("app \"%c\"", "N", "/f", "").has_value()); // field inside a quoted argument
    AW_CHECK(!DE::expand("\"%c\"", "N", "/f", "").has_value());
    AW_CHECK(!DE::expand("", "N", "/f", "").has_value()); // empty line

    // ---- shell quoting ----
    AW_CHECK(DE::shellQuote("plain") == "'plain'");
    AW_CHECK(DE::shellQuote("it's") == "'it'\\''s'");
    AW_CHECK(DE::shellQuote("a b\"c") == "'a b\"c'");
    AW_CHECK(DE::shellCommand({"a", "b c"}) == "'a' 'b c'");

    return true;
}
