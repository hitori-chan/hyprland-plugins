// common/icons.hpp — freedesktop icon-NAME resolution, shared by every
// plugin that shows themed icons (hyprnotify's cards, hyprbar's task chips
// and tray). One implementation: the GTK theme's size dirs (scalable first,
// then size proximity), then hicolor, then flat pixmaps. Inheritance beyond
// hicolor isn't followed — app icons live in hicolor in practice.
//
// Pure name -> path; rasterizing stays per plugin (each has its own texture
// rules and caches). Misses are cached too, so a nonexistent name never
// rescans the theme. Call resetIconNameCache() on config reload — it forgets
// the memoized GTK theme name along with the paths, since a theme switch is
// exactly what makes the old resolutions wrong.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NHyprCommon {

    inline bool iconIdentityPrefixMatch(std::string_view identifier, std::string_view candidate) {
        if (candidate.size() < 3 || candidate.size() >= identifier.size() || !identifier.starts_with(candidate))
            return false;
        const char boundary = identifier[candidate.size()];
        return boundary == '_' || boundary == '-';
    }

    // The XDG data dirs in precedence order: the per-user one first (it
    // overrides), then $XDG_DATA_DIRS. Every freedesktop lookup a plugin
    // does — icon themes, .desktop entries, pixmaps — walks this list.
    inline std::vector<std::string> xdgDataDirs() {
        std::vector<std::string> dirs;
        if (const char* X = getenv("XDG_DATA_HOME"); X && *X)
            dirs.push_back(X);
        else if (const char* H = getenv("HOME"); H && *H)
            dirs.push_back(std::string(H) + "/.local/share");
        std::string data = "/usr/local/share:/usr/share";
        if (const char* X = getenv("XDG_DATA_DIRS"); X && *X)
            data = X;
        for (size_t p = 0; p < data.size();) {
            const auto E = data.find(':', p);
            auto       D = data.substr(p, E == std::string::npos ? E : E - p);
            while (D.size() > 1 && D.back() == '/')
                D.pop_back();
            if (!D.empty())
                dirs.push_back(std::move(D));
            if (E == std::string::npos)
                break;
            p = E + 1;
        }
        return dirs;
    }

    // Where icon THEMES live: the data dirs' icons/ plus the legacy ~/.icons
    // (second, right after the per-user data dir — the spec's order).
    inline std::vector<std::string> xdgIconBases() {
        const auto               DATA = xdgDataDirs();
        std::vector<std::string> bases;
        bases.reserve(DATA.size() + 1);
        if (!DATA.empty())
            bases.push_back(DATA.front() + "/icons");
        if (const char* H = getenv("HOME"); H && *H)
            bases.push_back(std::string(H) + "/.icons");
        for (size_t i = 1; i < DATA.size(); i++)
            bases.push_back(DATA[i] + "/icons");
        return bases;
    }

    inline std::unordered_map<std::string, std::string>& iconNameCache() {
        static std::unordered_map<std::string, std::string> C;
        return C;
    }

    struct SThemeName {
        std::string value;
        bool        read = false;
    };
    inline SThemeName& iconThemeName() {
        static SThemeName T;
        return T;
    }

    inline void resetIconNameCache() {
        iconNameCache().clear();
        iconThemeName() = {};
    }

    // The GTK icon theme is this system's source of truth (Qt follows it). Read
    // gtk-icon-theme-name from settings.ini once; fall back to hicolor.
    inline std::string gtkIconThemeName() {
        auto& T = iconThemeName();
        if (T.read)
            return T.value;
        T.read = true;

        std::string cfgHome;
        if (const char* X = getenv("XDG_CONFIG_HOME"); X && *X)
            cfgHome = X;
        else if (const char* H = getenv("HOME"); H && *H)
            cfgHome = std::string(H) + "/.config";
        if (!cfgHome.empty()) {
            std::ifstream f(cfgHome + "/gtk-3.0/settings.ini");
            std::string   line;
            while (std::getline(f, line))
                if (const auto P = line.find("gtk-icon-theme-name"); P == 0) {
                    if (const auto EQ = line.find('='); EQ != std::string::npos) {
                        T.value = line.substr(EQ + 1);
                        T.value.erase(0, T.value.find_first_not_of(" \t"));
                        T.value.erase(T.value.find_last_not_of(" \t\r\n") + 1);
                    }
                    break;
                }
        }
        return T.value;
    }

    // Look for <dir>/name.ext directly and one category level down
    // (<dir>/<cat>/name.ext) — the freedesktop size dirs hold either.
    inline std::string findIconInDir(const std::string& dir, const std::string& name) {
        static const char* EXT[] = {".svg", ".png", ".xpm"};
        std::error_code    ec;
        for (const char* e : EXT)
            if (std::filesystem::exists(dir + "/" + name + e, ec))
                return dir + "/" + name + e;
        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::end(it); it.increment(ec)) {
            if (!it->is_directory(ec))
                continue;
            for (const char* e : EXT)
                if (std::filesystem::exists(it->path().string() + "/" + name + e, ec))
                    return it->path().string() + "/" + name + e;
        }
        return "";
    }

    // Resolve a freedesktop icon NAME to a file path via themed lookup; "" if
    // unresolved or if the string is already a path. Cached per name AND
    // size: the requested size leads sizeDirs below, so in a PNG-only theme
    // it CHOOSES the file — keyed on the name alone, whichever caller asked
    // first pinned the size for every later one (hyprnotify wants a card
    // icon at max_icon and an action icon at ~15px).
    inline std::string resolveIconName(const std::string& name, int sizePx) {
        if (name.empty() || name.find('/') != std::string::npos)
            return ""; // already a path, or nothing to resolve
        auto&      CACHE = iconNameCache();
        const auto KEY   = name + "\x1f" + std::to_string(sizePx);
        if (const auto IT = CACHE.find(KEY); IT != CACHE.end())
            return IT->second;

        const auto               bases = xdgIconBases();

        std::vector<std::string> themes;
        if (const auto GT = gtkIconThemeName(); !GT.empty()) {
            themes.push_back(GT);
            // a "-dark"/"-light" variant usually inherits its base — a cheap
            // approximation of index.theme inheritance
            for (const char* SUF : {"-dark", "-light", "-Dark", "-Light"})
                if (GT.ends_with(SUF))
                    themes.push_back(GT.substr(0, GT.size() - std::string(SUF).size()));
        }
        themes.push_back("hicolor");
        themes.push_back("Adwaita"); // the freedesktop-name last resorts
        themes.push_back("AdwaitaLegacy");

        // Adwaita stores symbolic marks as symbolic/<context>/name.svg,
        // unlike themes that use scalable/<context>/ or size/<context>/.
        // Treat symbolic as a size directory so findIconInDir also probes the
        // context below it.
        std::vector<std::string> sizeDirs = {"scalable", "symbolic"};
        for (const int S : {sizePx, 64, 48, 96, 128, 256, 72, 32, 24, 16})
            sizeDirs.push_back(std::to_string(S) + "x" + std::to_string(S));

        // breeze (and KDE themes generally) lay out <context>/<size> instead
        // of <size>x<size>/<context> — probe the common contexts too
        static const char*       CTXS[]   = {"status", "apps", "devices", "actions", "categories", "mimetypes", "legacy", "symbolic"};
        std::vector<std::string> ctxSizes = {"symbolic", "scalable"};
        for (const int S : {sizePx, 64, 48, 32, 24, 22, 16})
            ctxSizes.push_back(std::to_string(S));

        std::string found;
        for (const auto& THEME : themes) {
            for (const auto& BASE : bases) {
                const auto      TDIR = BASE + "/" + THEME;
                std::error_code ec;
                if (!std::filesystem::is_directory(TDIR, ec))
                    continue;
                for (const auto& SD : sizeDirs)
                    if (found = findIconInDir(TDIR + "/" + SD, name); !found.empty())
                        break;
                for (const char* CTX : CTXS) {
                    if (!found.empty())
                        break;
                    if (!std::filesystem::is_directory(TDIR + "/" + CTX, ec))
                        continue;
                    for (const auto& SZ : ctxSizes) {
                        static const char* EXT[] = {".svg", ".png"};
                        for (const char* E : EXT)
                            if (std::filesystem::exists(TDIR + "/" + CTX + "/" + SZ + "/" + name + E, ec)) {
                                found = TDIR + "/" + CTX + "/" + SZ + "/" + name + E;
                                break;
                            }
                        if (!found.empty())
                            break;
                    }
                }
                if (!found.empty())
                    break;
            }
            if (!found.empty())
                break;
        }
        if (found.empty())
            for (const auto& D : xdgDataDirs()) { // flat pixmaps, the pre-theme layout
                std::error_code ec;
                for (const char* e : {".svg", ".png", ".xpm"})
                    if (const auto P = D + "/pixmaps/" + name + e; std::filesystem::exists(P, ec)) {
                        found = P;
                        break;
                    }
                if (!found.empty())
                    break;
            }

        CACHE[KEY] = found;
        return found;
    }

} // namespace NHyprCommon
