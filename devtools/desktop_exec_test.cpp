#include "common/fileindex.hpp"
#include "common/icons.hpp"
#include "hyprbar/desktop_exec.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

    using NHyprbar::DesktopExec::expand;
    using NHyprbar::DesktopExec::MAX_DESKTOP_FILE_BYTES;
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

    void expectLocalizedDesktopAdmission() {
        const auto STAMP = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto DIR   = std::filesystem::temp_directory_path() / ("hyprbar-desktop-index-" + std::to_string(STAMP));
        std::filesystem::create_directory(DIR);
        struct SCleanup {
            std::filesystem::path path;
            ~SCleanup() {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } cleanup{DIR};

        std::string firefox = "[Desktop Entry]\nType=Application\nExec=/usr/lib/firefox/firefox %u\nName=Firefox\n";
        firefox.append(96 * 1024, '#');
        std::ofstream{DIR / "firefox.desktop", std::ios::binary} << firefox;

        std::string oversized(MAX_DESKTOP_FILE_BYTES + 1, '#');
        std::ofstream{DIR / "oversized.desktop", std::ios::binary} << oversized;

        NHyprCommon::CAsyncFileIndex index;
        NHyprCommon::CAsyncFileIndex::SRequest request;
        request.generation   = 1;
        request.roots        = {DIR};
        request.extensions   = {".desktop"};
        request.maxEntries   = 8;
        request.maxVisited   = 16;
        request.maxFileBytes = MAX_DESKTOP_FILE_BYTES;
        index.request(std::move(request));

        std::vector<NHyprCommon::CAsyncFileIndex::SEntry> entries;
        bool complete = false;
        for (const auto DEADLINE = std::chrono::steady_clock::now() + std::chrono::seconds(2); std::chrono::steady_clock::now() < DEADLINE && !complete;) {
            complete = index.poll(1, entries, 8);
            if (!complete)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(complete, "desktop index completes");
        expect(entries.size() == 1 && entries.front().path.filename() == "firefox.desktop" && entries.front().contents.size() == firefox.size(),
               "localized desktop file admitted and oversized file rejected");
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
    expectWords(expand("/usr/lib/firefox/firefox %u", "Firefox", "/usr/share/applications/firefox.desktop", "firefox"), {"/usr/lib/firefox/firefox"},
                "Firefox URL field without supplied URL");
    expectWords(expand(R"(client \%c)", "Name", "/tmp/example.desktop", ""), {"client", "%c"}, "escaped field literal");

    expect(!expand(R"(client "%c")", "Name", "/tmp/example.desktop", ""), "field in quoted argument");
    expect(!expand(R"(client --files=%F)", "Name", "/tmp/example.desktop", ""), "multi-file field must stand alone");
    expect(!expand(R"(client --icon=%i)", "Name", "/tmp/example.desktop", ""), "icon field must stand alone");
    expect(!expand(R"(client %f %u)", "Name", "/tmp/example.desktop", ""), "only one file or URL field");
    expect(!expand(R"(client %z)", "Name", "/tmp/example.desktop", ""), "unknown field is invalid");
    expect(!expand(R"(client "bad\q")", "Name", "/tmp/example.desktop", ""), "invalid quoted exec escape");
    expect(!expand(R"(%c client)", "Name", "/tmp/example.desktop", ""), "field code cannot be executable");

    expect(NHyprCommon::iconIdentityPrefixMatch("ente_status_icon_1", "ente"), "separator-bound SNI identity prefix");
    expect(!NHyprCommon::iconIdentityPrefixMatch("enterprise_status_icon_1", "ente"), "partial SNI identity prefix rejected");

    expectLocalizedDesktopAdmission();

    if (failures != 0)
        return EXIT_FAILURE;
    std::cout << "desktop_exec_test: all checks passed\n";
    return EXIT_SUCCESS;
}
