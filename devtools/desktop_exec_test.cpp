#include "hyprbar/desktop_exec.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using NHyprbar::DesktopExec::expand;
    using NHyprbar::DesktopExec::unescapeList;
    using NHyprbar::DesktopExec::unescapeString;

    int failures = 0;

    void expect(bool value, std::string_view name) {
        if (value)
            return;
        std::cerr << "failed: " << name << '\n';
        failures++;
    }

    void expectString(const std::optional<std::string>& value, std::string_view expected, std::string_view name) {
        expect(value && *value == expected, name);
    }

    void expectWords(const std::optional<std::vector<std::string>>& value, std::vector<std::string> expected, std::string_view name) {
        expect(value && *value == expected, name);
    }

} // namespace

int main() {
    expectString(unescapeString(R"(A\sname\\with\nlines\tand\rreturns)"), "A name\\with\nlines\tand\rreturns", "ordinary escapes");
    expect(!unescapeString(R"(bad\;semicolon)"), "semicolon escape belongs to lists only");
    expect(!unescapeString(R"(bad\q)"), "unknown string escape is invalid");
    expect(!unescapeString("trailing\\"), "trailing string backslash is invalid");

    const auto LIST = unescapeList(R"(Hyprland;GNOME\;compat;two\swords;)" );
    expect(LIST && *LIST == std::vector<std::string>{"Hyprland", "GNOME;compat", "two words"}, "list escapes and separators");
    expect(!unescapeList(R"(Hyprland;bad\q;)"), "unknown list escape is invalid");

    expectWords(expand(R"(client --title "two words" %c %% %k)", "Name With Space", "/tmp/example.desktop", "example-icon"),
                {"client", "--title", "two words", "Name With Space", "%", "/tmp/example.desktop"}, "valid quoted arguments and scalar fields");
    expectWords(expand(R"(client %i)", "Name", "/tmp/example.desktop", "example-icon"), {"client", "--icon", "example-icon"}, "standalone icon field");
    expectWords(expand(R"(client %F)", "Name", "/tmp/example.desktop", ""), {"client"}, "empty supplied input field");
    expectWords(expand(R"(client \%c)", "Name", "/tmp/example.desktop", ""), {"client", "%c"}, "escaped field literal");

    expect(!expand(R"(client "%c")", "Name", "/tmp/example.desktop", ""), "field in quoted argument");
    expect(!expand(R"(client --files=%F)", "Name", "/tmp/example.desktop", ""), "multi-file field must stand alone");
    expect(!expand(R"(client --icon=%i)", "Name", "/tmp/example.desktop", ""), "icon field must stand alone");
    expect(!expand(R"(client %f %u)", "Name", "/tmp/example.desktop", ""), "only one file or URL field");
    expect(!expand(R"(client %z)", "Name", "/tmp/example.desktop", ""), "unknown field is invalid");
    expect(!expand(R"(client "bad\q")", "Name", "/tmp/example.desktop", ""), "invalid quoted exec escape");
    expect(!expand(R"(%c client)", "Name", "/tmp/example.desktop", ""), "field code cannot be executable");

    if (failures != 0)
        return EXIT_FAILURE;
    std::cout << "desktop_exec_test: all checks passed\n";
    return EXIT_SUCCESS;
}
