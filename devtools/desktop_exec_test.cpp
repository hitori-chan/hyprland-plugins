#include "common/fileindex.hpp"
#include "common/icons.hpp"
#include "hyprbar/desktop_exec.hpp"
#include "test.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace {

    using NHyprbar::DesktopExec::expand;
    using NHyprbar::DesktopExec::classifyField;
    using NHyprbar::DesktopExec::EFieldKind;
    using NHyprbar::DesktopExec::EScalarSource;
    using NHyprbar::DesktopExec::MAX_DESKTOP_FILE_BYTES;
    using NHyprbar::DesktopExec::unescapeList;
    using NHyprbar::DesktopExec::unescapeString;

    NHyprTest::CSuite suite{"desktop_exec_test"};

    void              expect(bool value, std::string_view name) {
        suite.expect(value, name);
    }

    void expectString(const std::optional<std::string>& value, std::string_view expected, std::string_view name) {
        expect(value && *value == expected, name);
    }

    void expectWords(const std::optional<std::vector<std::string>>& value, std::vector<std::string> expected, std::string_view name) {
        expect(value && *value == expected, name);
    }

    void expectField(char code, EFieldKind kind, EScalarSource source, std::string_view name) {
        const auto FIELD = classifyField(code);
        expect(FIELD && FIELD->kind == kind && FIELD->scalarSource == source, name);
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

        wl_event_loop* const                    LOOP = wl_event_loop_create();
        NHyprCommon::CAsyncFileIndex           index;
        expect(LOOP && index.init(LOOP), "desktop index binds a Wayland event loop");
        NHyprCommon::CAsyncFileIndex::SRequest request;
        request.generation   = 1;
        request.roots        = {DIR};
        request.extensions   = {".desktop"};
        request.maxEntries   = 8;
        request.maxVisited   = 16;
        request.maxFileBytes = MAX_DESKTOP_FILE_BYTES;
        index.request(std::move(request));

        std::vector<NHyprCommon::CAsyncFileIndex::SEntry> entries;
        bool                                              complete = false;
        for (const auto DEADLINE = std::chrono::steady_clock::now() + std::chrono::seconds(2); std::chrono::steady_clock::now() < DEADLINE && !complete;) {
            wl_event_loop_dispatch(LOOP, 10);
            complete = index.poll(1, entries, 8);
        }
        expect(complete, "desktop index completes");
        expect(entries.size() == 1 && entries.front().path.filename() == "firefox.desktop" && entries.front().contents.size() == firefox.size(),
               "localized desktop file admitted and oversized file rejected");
        index.exit();
        wl_event_loop_destroy(LOOP);
    }

    void expectBoundedIndexCancellation() {
        const auto STAMP = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto DIR   = std::filesystem::temp_directory_path() / ("hyprbar-desktop-index-bounds-" + std::to_string(STAMP));
        std::filesystem::create_directory(DIR);
        struct SCleanup {
            std::filesystem::path path;
            ~SCleanup() {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } cleanup{DIR};

        for (size_t i = 0; i < 48; ++i)
            std::ofstream{DIR / ("indexed-" + std::to_string(i) + ".desktop")} << "[Desktop Entry]\nType=Application\nName=Indexed\n";
        const auto TOOL = DIR / "tool-launcher";
        std::ofstream{TOOL} << "#!/bin/sh\n";
        std::filesystem::permissions(TOOL, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
        std::filesystem::create_directory(DIR / "folder-completion");

        wl_event_loop* const          LOOP = wl_event_loop_create();
        NHyprCommon::CAsyncFileIndex index;
        expect(LOOP && index.init(LOOP), "bounded desktop index binds a Wayland event loop");

        NHyprCommon::CAsyncFileIndex::SRequest request;
        request.generation   = 7;
        request.roots        = {DIR};
        request.extensions   = {".desktop"};
        request.maxEntries   = 12;
        request.maxVisited   = 64;
        request.maxFileBytes = MAX_DESKTOP_FILE_BYTES;
        index.request(std::move(request));

        std::vector<NHyprCommon::CAsyncFileIndex::SEntry> entries;
        bool                                              complete = false;
        for (const auto DEADLINE = std::chrono::steady_clock::now() + std::chrono::seconds(2); std::chrono::steady_clock::now() < DEADLINE && !complete;) {
            wl_event_loop_dispatch(LOOP, 10);
            std::vector<NHyprCommon::CAsyncFileIndex::SEntry> batch;
            complete = index.poll(7, batch, 3);
            entries.insert(entries.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
        }
        expect(complete, "desktop index completes after bounded polls");
        expect(entries.size() == 12, "desktop index enforces entry admission across bounded polls");

        request.generation = 8;
        request.roots      = {DIR};
        request.extensions.clear();
        request.namePrefix = "tool-";
        request.maxEntries = 4;
        request.maxVisited = 64;
        request.maxFileBytes = 0;
        request.recursive    = false;
        request.executable = true;
        request.directories = false;
        index.request(std::move(request));
        std::vector<NHyprCommon::CAsyncFileIndex::SEntry> commands;
        complete = false;
        for (const auto DEADLINE = std::chrono::steady_clock::now() + std::chrono::seconds(2); std::chrono::steady_clock::now() < DEADLINE && !complete;) {
            wl_event_loop_dispatch(LOOP, 10);
            complete = index.poll(8, commands, 4);
        }
        expect(complete && commands.size() == 1 && commands.front().path.filename() == "tool-launcher" && !commands.front().directory,
               "desktop index admits executable command completion without recursion");

        request.generation  = 9;
        request.roots       = {DIR};
        request.extensions.clear();
        request.namePrefix  = "folder-";
        request.maxEntries  = 4;
        request.maxVisited  = 64;
        request.maxFileBytes = 0;
        request.recursive    = false;
        request.executable  = false;
        request.directories = true;
        index.request(std::move(request));
        std::vector<NHyprCommon::CAsyncFileIndex::SEntry> folders;
        complete = false;
        for (const auto DEADLINE = std::chrono::steady_clock::now() + std::chrono::seconds(2); std::chrono::steady_clock::now() < DEADLINE && !complete;) {
            wl_event_loop_dispatch(LOOP, 10);
            complete = index.poll(9, folders, 4);
        }
        expect(complete && folders.size() == 1 && folders.front().path.filename() == "folder-completion" && folders.front().directory,
               "desktop index retains directory completion semantics");

        request.generation   = 10;
        request.roots        = {DIR};
        request.extensions   = {".desktop"};
        request.namePrefix.clear();
        request.maxEntries   = 48;
        request.maxVisited   = 64;
        request.maxFileBytes = MAX_DESKTOP_FILE_BYTES;
        request.recursive    = false;
        request.executable   = false;
        request.directories  = false;
        index.request(std::move(request));
        index.cancel(11);
        std::vector<NHyprCommon::CAsyncFileIndex::SEntry> cancelled;
        expect(index.poll(11, cancelled, 8) && cancelled.empty() && !index.active(11), "desktop index cancellation closes an active helper without stale records");
        index.exit();
        wl_event_loop_destroy(LOOP);
    }

} // namespace

int main() {
    struct SFieldCase {
        char             code;
        EFieldKind       kind;
        EScalarSource    source;
        std::string_view name;
    };
    constexpr std::array FIELD_CASES{
        SFieldCase{'%', EFieldKind::LITERAL, EScalarSource::NONE, "literal field classification"},
        SFieldCase{'c', EFieldKind::SCALAR, EScalarSource::NAME, "name field classification"},
        SFieldCase{'k', EFieldKind::SCALAR, EScalarSource::DESKTOP_FILE, "desktop-file field classification"},
        SFieldCase{'f', EFieldKind::SINGLE_INPUT, EScalarSource::NONE, "single-file field classification"},
        SFieldCase{'u', EFieldKind::SINGLE_INPUT, EScalarSource::NONE, "single-URL field classification"},
        SFieldCase{'F', EFieldKind::LIST_INPUT, EScalarSource::NONE, "file-list field classification"},
        SFieldCase{'U', EFieldKind::LIST_INPUT, EScalarSource::NONE, "URL-list field classification"},
        SFieldCase{'i', EFieldKind::ICON, EScalarSource::NONE, "icon field classification"},
        SFieldCase{'d', EFieldKind::DEPRECATED_EMPTY, EScalarSource::NONE, "deprecated %d classification"},
        SFieldCase{'D', EFieldKind::DEPRECATED_EMPTY, EScalarSource::NONE, "deprecated %D classification"},
        SFieldCase{'n', EFieldKind::DEPRECATED_EMPTY, EScalarSource::NONE, "deprecated %n classification"},
        SFieldCase{'N', EFieldKind::DEPRECATED_EMPTY, EScalarSource::NONE, "deprecated %N classification"},
        SFieldCase{'v', EFieldKind::DEPRECATED_EMPTY, EScalarSource::NONE, "deprecated %v classification"},
        SFieldCase{'m', EFieldKind::DEPRECATED_EMPTY, EScalarSource::NONE, "deprecated %m classification"},
    };
    for (const auto& FIELD : FIELD_CASES)
        expectField(FIELD.code, FIELD.kind, FIELD.source, FIELD.name);
    expect(!classifyField('z'), "unknown field classification");

    expectString(unescapeString(R"(A\sname\\with\nlines\tand\rreturns)"), "A name\\with\nlines\tand\rreturns", "ordinary escapes");
    expect(!unescapeString(R"(bad\;semicolon)"), "semicolon escape belongs to lists only");
    expect(!unescapeString(R"(bad\q)"), "unknown string escape is invalid");
    expect(!unescapeString("trailing\\"), "trailing string backslash is invalid");

    const auto LIST = unescapeList(R"(Hyprland;GNOME\;compat;two\swords;)");
    expect(LIST && *LIST == std::vector<std::string>{"Hyprland", "GNOME;compat", "two words"}, "list escapes and separators");
    expect(!unescapeList(R"(Hyprland;bad\q;)"), "unknown list escape is invalid");

    expectWords(expand(R"(client --title "two words" %c %% %k)", "Name With Space", "/tmp/example.desktop", "example-icon"),
                {"client", "--title", "two words", "Name With Space", "%", "/tmp/example.desktop"}, "valid quoted arguments and scalar fields");
    expectWords(expand(R"(client %i)", "Name", "/tmp/example.desktop", "example-icon"), {"client", "--icon", "example-icon"}, "standalone icon field");
    expectWords(expand(R"(client %F)", "Name", "/tmp/example.desktop", ""), {"client"}, "empty supplied input field");
    expectWords(expand(R"(client --file=%f --old=%d)", "Name", "/tmp/example.desktop", ""), {"client", "--file=", "--old="},
                "embedded empty input and deprecated fields preserve arguments");
    expectWords(expand(R"(client %d)", "Name", "/tmp/example.desktop", ""), {"client"}, "standalone deprecated field removes argument");
    expectWords(expand("/usr/lib/firefox/firefox %u", "Firefox", "/usr/share/applications/firefox.desktop", "firefox"), {"/usr/lib/firefox/firefox"},
                "Firefox URL field without supplied URL");
    expectWords(expand("env LD_PRELOAD=/usr/lib/spotify-adblock.so spotify --uri=%U", "Spotify (adblock)", "/usr/share/applications/spotify-adblock.desktop", "spotify-client"),
                {"env", "LD_PRELOAD=/usr/lib/spotify-adblock.so", "spotify", "--uri="}, "embedded Spotify URL list field without supplied URLs");
    expectWords(expand(R"(client \%c)", "Name", "/tmp/example.desktop", ""), {"client", "%c"}, "escaped field literal");

    expect(!expand(R"(client "%c")", "Name", "/tmp/example.desktop", ""), "field in quoted argument");
    expect(!expand(R"(client --icon=%i)", "Name", "/tmp/example.desktop", ""), "icon field must stand alone");
    expect(!expand(R"(client %f %u)", "Name", "/tmp/example.desktop", ""), "only one file or URL field");
    expect(!expand(R"(client --file=%f%F)", "Name", "/tmp/example.desktop", ""), "multiple embedded input fields rejected");
    expect(!expand(R"(client %z)", "Name", "/tmp/example.desktop", ""), "unknown field is invalid");
    expect(!expand(R"(client "bad\q")", "Name", "/tmp/example.desktop", ""), "invalid quoted exec escape");
    expect(!expand(R"(%c client)", "Name", "/tmp/example.desktop", ""), "field code cannot be executable");

    expect(NHyprCommon::iconIdentityPrefixMatch("ente_status_icon_1", "ente"), "separator-bound SNI identity prefix");
    expect(!NHyprCommon::iconIdentityPrefixMatch("enterprise_status_icon_1", "ente"), "partial SNI identity prefix rejected");

    expectLocalizedDesktopAdmission();
    expectBoundedIndexCancellation();

    return suite.finish();
}
