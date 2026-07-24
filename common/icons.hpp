// common/icons.hpp — freedesktop icon-NAME resolution, shared by every
// plugin that shows themed icons (hyprnotify's cards, hyprbar's task chips
// and tray). One implementation: the GTK theme's size dirs (scalable first,
// then size proximity), then hicolor, then flat pixmaps. Inheritance beyond
// hicolor isn't followed — app icons live in hicolor in practice.
//
// Pure name -> path; rasterizing stays per plugin (each has its own texture
// rules and caches). Misses are cached too, so a nonexistent name never
// rescans the theme. Call resetIconNameCache() on config reload.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace NHyprCommon {

    inline std::unordered_map<std::string, std::string>& iconNameCache() {
        static std::unordered_map<std::string, std::string> C;
        return C;
    }

    inline void resetIconNameCache() {
        iconNameCache().clear();
    }

    // The GTK icon theme is this system's source of truth (Qt follows it). Read
    // gtk-icon-theme-name from settings.ini once; fall back to hicolor.
    inline std::string gtkIconThemeName() {
        static std::string theme;
        static bool        done = false;
        if (done)
            return theme;
        done = true;

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
                        theme = line.substr(EQ + 1);
                        theme.erase(0, theme.find_first_not_of(" \t"));
                        theme.erase(theme.find_last_not_of(" \t\r\n") + 1);
                    }
                    break;
                }
        }
        return theme;
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
    // unresolved or if the string is already a path. Cached per name.
    inline std::string resolveIconName(const std::string& name, int sizePx) {
        if (name.empty() || name.find('/') != std::string::npos)
            return ""; // already a path, or nothing to resolve
        auto& CACHE = iconNameCache();
        if (const auto IT = CACHE.find(name); IT != CACHE.end())
            return IT->second;

        std::vector<std::string> bases;
        if (const char* X = getenv("XDG_DATA_HOME"); X && *X)
            bases.push_back(std::string(X) + "/icons");
        else if (const char* H = getenv("HOME"); H && *H)
            bases.push_back(std::string(H) + "/.local/share/icons");
        if (const char* H = getenv("HOME"); H && *H)
            bases.push_back(std::string(H) + "/.icons");
        std::string dataDirs = "/usr/local/share:/usr/share";
        if (const char* X = getenv("XDG_DATA_DIRS"); X && *X)
            dataDirs = X;
        for (size_t p = 0; p < dataDirs.size();) {
            const auto E = dataDirs.find(':', p);
            const auto D = dataDirs.substr(p, E == std::string::npos ? E : E - p);
            if (!D.empty())
                bases.push_back(D + "/icons");
            if (E == std::string::npos)
                break;
            p = E + 1;
        }

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

        std::vector<std::string> sizeDirs = {"scalable"};
        for (const int S : {sizePx, 64, 48, 96, 128, 256, 72, 32, 24, 16})
            sizeDirs.push_back(std::to_string(S) + "x" + std::to_string(S));

        // breeze (and KDE themes generally) lay out <context>/<size> instead
        // of <size>x<size>/<context> — probe the common contexts too
        static const char* CTXS[]  = {"status", "apps", "devices", "actions", "categories", "mimetypes", "legacy", "symbolic"};
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
            for (const char* e : {".svg", ".png", ".xpm"}) {
                std::error_code ec;
                if (std::filesystem::exists(std::string("/usr/share/pixmaps/") + name + e, ec)) {
                    found = std::string("/usr/share/pixmaps/") + name + e;
                    break;
                }
            }

        CACHE[name] = found;
        return found;
    }

} // namespace NHyprCommon
