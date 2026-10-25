// awesome/test/test_iconcache.cpp — the icon theme cache reader: a cache
// gtk-update-icon-cache wrote resolves as the walk would (when the tool is
// installed), a stale one falls back to the walk, and a corrupt one answers
// nothing. Temp themes live in a private /tmp/hypr-awesome-test-icons.*
// (the sweep runs several harnesses at once).
#include "../core/icons.hpp"

#include "harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace NAwesome;
namespace fs = std::filesystem;

static void touch(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream{p} << "x";
}

static std::string find(const fs::path& theme, const std::string& name, int px) {
    bool        walked = false;
    std::string found;
    findInThemeDir(theme.string(), name, iconSearch(px), walked, found);
    return found;
}

static bool cacheTool() {
    return std::system("command -v gtk-update-icon-cache >/dev/null 2>&1") == 0;
}

bool test_iconcache() {
    char tmpl[] = "/tmp/hypr-awesome-test-icons.XXXXXX";
    if (!AW_CHECK(mkdtemp(tmpl) != nullptr))
        return false;
    const fs::path  ROOT = tmpl;
    std::error_code ec;
    const auto      T = ROOT / "theme";
    touch(T / "48x48/apps/a.png");
    touch(T / "scalable/apps/a.svg");
    touch(T / "apps/22/b.svg");
    touch(T / "22x22/status/c.png");

    if (cacheTool()) {
        AW_CHECK(std::system(("gtk-update-icon-cache -f -t -q -i " + T.string()).c_str()) == 0);
        iconThemeDirs().clear();
        AW_CHECK(iconThemeDir(T.string()).kind == SIconThemeDir::CACHED);
        AW_CHECK(find(T, "a", 48) == (T / "scalable/apps/a.svg").string()); // a size dir's scalable first
        AW_CHECK(find(T, "b", 22) == (T / "apps/22/b.svg").string());       // the context layout
        AW_CHECK(find(T, "c", 22) == (T / "22x22/status/c.png").string());
        AW_CHECK(find(T, "missing", 48).empty());
        // a listed file gone since: the next-ranked one that is there
        fs::remove(T / "scalable/apps/a.svg", ec);
        AW_CHECK(find(T, "a", 48) == (T / "48x48/apps/a.png").string());
        // a dir newer than its cache: GTK's rule, walked instead
        const auto LATER = fs::last_write_time(T / "icon-theme.cache", ec) + std::chrono::seconds(10);
        fs::last_write_time(T, LATER, ec);
        iconThemeDirs().clear();
        AW_CHECK(iconThemeDir(T.string()).kind == SIconThemeDir::WALKED);
        AW_CHECK(find(T, "c", 22) == (T / "22x22/status/c.png").string());
    } else
        std::printf("    skip: gtk-update-icon-cache is not installed\n");

    // a corrupt cache: one bucket whose chain points at itself, and a name
    // offset past the end — it opens, answers nothing, and ends
    const auto C = ROOT / "corrupt";
    fs::create_directories(C, ec);
    {
        const uint8_t BYTES[] = {0, 1, 0, 0,   0, 0, 0, 12, 0, 0, 0, 20, // v1.0, hash at 12, dirs at 20
                                 0, 0, 0, 1,   0, 0, 0, 24,              // one bucket -> the node at 24
                                 0, 0, 0, 0,                             // no dirs
                                 0, 0, 0, 24,  0, 0, 0xff, 0, 0, 0, 0, 0}; // next = itself, name far out
        std::ofstream{C / "icon-theme.cache", std::ios::binary}.write((const char*)BYTES, sizeof BYTES);
    }
    CIconThemeCache bad;
    AW_CHECK(bad.open(C.string()));
    int seen = 0;
    bad.images("anything", [&](const std::string&, uint16_t) { seen++; });
    AW_CHECK(seen == 0);
    // too short to hold a header
    const auto S = ROOT / "short";
    fs::create_directories(S, ec);
    std::ofstream{S / "icon-theme.cache"} << "abc";
    CIconThemeCache shortCache;
    AW_CHECK(!shortCache.open(S.string()));

    fs::remove_all(ROOT, ec);
    return true;
}
