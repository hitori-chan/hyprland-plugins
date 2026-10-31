// The freedesktop Exec= grammar: unescaping, the quote/escape tokenizer,
// and field-code expansion. This builds the argv of spawned processes, so
// the grammar's rejection paths are as important as its happy paths.
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

    // ---- the launcher's typed line: completed names are one sh word ----
    AW_CHECK(DE::shellWord("plain-name_1.2.mp4").value() == "plain-name_1.2.mp4");
    AW_CHECK(DE::shellWord("x$(id).mp4").value() == "x\\$\\(id\\).mp4"); // no substitution survives
    AW_CHECK(DE::shellWord("a b").value() == "a\\ b");
    AW_CHECK(DE::shellWord("it's;&|<>`\"*?").value() == "it\\'s\\;\\&\\|\\<\\>\\`\\\"\\*\\?");
    AW_CHECK(DE::shellWord("~home").value() == "\\~home"); // no tilde expansion
    AW_CHECK(DE::shellWord("a=b").value() == "a\\=b");     // never an assignment
    AW_CHECK(DE::shellWord("ünïcødé").value() == "ünïcødé");
    AW_CHECK(!DE::shellWord("new\nline").has_value());        // a newline ends the command
    AW_CHECK(!DE::shellWord("tab\there").has_value());
    AW_CHECK(!DE::shellWord("c1\xc2\x85").has_value());       // NEL (C1)
    AW_CHECK(!DE::shellWord("bidi\xe2\x80\xae").has_value()); // RLO
    AW_CHECK(!DE::shellWord("iso\xe2\x81\xa6").has_value());  // LRI
    AW_CHECK(!DE::shellWord("bad\xff").has_value());           // malformed UTF-8
    AW_CHECK(!DE::shellWord("cut\xe2\x80").has_value());      // truncated sequence
    AW_CHECK(DE::unescapeWord("a\\ b") == "a b");
    AW_CHECK(DE::unescapeWord(DE::shellWord("x$(id) y").value()) == "x$(id) y"); // round trip
    AW_CHECK(DE::unescapeWord("trailing\\") == "trailing\\");
    AW_CHECK(DE::escapedAt("a\\ b", 2));
    AW_CHECK(!DE::escapedAt("a\\\\ b", 3)); // an escaped backslash, then a real space
    AW_CHECK(!DE::escapedAt("a b", 1));
    AW_CHECK(DE::typeableText("ok\x01\xe2\x80\xaeyes\xff") == "okyes");
    AW_CHECK(DE::typeableText("ünï") == "ünï");
    // a malformed byte goes alone: what follows it is read again
    AW_CHECK(DE::typeableText("caf\xe9 au") == "caf au");
    AW_CHECK(DE::typeableText("\xff" "abc") == "abc");
    AW_CHECK(DE::typeableText("a\xf5\x80\x80\x80" "b") == "ab");
    // overlong, surrogate and line-separator forms are refused
    AW_CHECK(!DE::typeableName("\xc0\xaf"));
    AW_CHECK(!DE::typeableName("\xe0\x80\xaf"));
    AW_CHECK(!DE::typeableName("\xed\xa0\x80"));
    AW_CHECK(!DE::typeableName("a\xe2\x80\xa8" "b"));
    AW_CHECK(DE::typeableName("\xf0\x9f\x98\x80"));

    // ---- parseEntry: the [Desktop Entry] group ----
    {
        const auto E = DE::parseEntry("# a comment\n[Desktop Entry]\r\nType = Application\nName=Files\nName[de]=Dateien\nName=Second\n"
                                      "Exec=thunar %U\nIcon=org.xfce.thunar\nStartupWMClass=Thunar\nCategories=System;FileManager;\n"
                                      "Terminal=false\nNoDisplay=false\n[Desktop Action window]\nName=Other\nExec=other\n");
        AW_CHECK(E.has_value());
        AW_CHECK(E->application);                                  // space around '=' is ignored
        AW_CHECK(E->name == "Files");                              // the first wins; a localized key is not the key
        AW_CHECK(E->exec == "thunar %U");                          // field codes stay for the expander
        AW_CHECK(E->icon == "org.xfce.thunar" && E->wmClass == "Thunar");
        AW_CHECK(E->categories == (std::vector<std::string>{"System", "FileManager"}));
        AW_CHECK(!E->hidden && !E->terminal && !E->malformed);
    }
    AW_CHECK(!DE::parseEntry("[Desktop Action x]\nName=x\n").has_value()); // no entry group
    AW_CHECK(DE::parseEntry("[Desktop Entry]\nHidden=true\n")->hidden);
    AW_CHECK(DE::parseEntry("[Desktop Entry]\nName=bad\\q\n")->malformed); // an unknown escape
    AW_CHECK(DE::parseEntry("[Desktop Entry]\nIcon=x\n#Icon=y\n")->icon == "x");
    AW_CHECK(!DE::parseEntry("[Desktop Entry]\nType=Link\n")->application);

    return true;
}
