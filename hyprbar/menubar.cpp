// hyprbar/menubar.cpp — awesome's Mod+P launcher: .desktop apps, categories, prompt, completion, history

#include "common/lifecycle.hpp"
#include "common/queries.hpp"

#include "hyprbar.hpp"

namespace NHyprbar {

    // ---- menubar: awesome's Mod+P app launcher, drawn in the bar strip ----

    namespace Menubar {
        // The stock awesome categories (menubar/menu_gen.lua): an app files
        // under the first of its Categories= tokens that maps here. Categories
        // list alongside the apps; Enter on one drills into it.
        const SCategory CATEGORIES[] = {
            {"Accessories", "Utility", "applications-accessories"},  {"Development", "Development", "applications-development"},
            {"Education", "Education", "applications-science"},      {"Games", "Game", "applications-games"},
            {"Graphics", "Graphics", "applications-graphics"},       {"Internet", "Network", "applications-internet"},
            {"Multimedia", "AudioVideo", "applications-multimedia"}, {"Office", "Office", "applications-office"},
            {"Science", "Science", "applications-science"},          {"Settings", "Settings", "applications-utilities"},
            {"System Tools", "System", "applications-system"},
        };
        const int         NCATS = (int)(sizeof(CATEGORIES) / sizeof(*CATEGORIES));

        std::vector<SApp> apps; // parsed on first open, sorted by name
        static bool       parsed = false;

        bool              isOpen = false;
        std::string       typed;
        size_t            cursor     = 0;
        int               currentCat = -1;
        static int        prevSel    = 0; // selection restored when leaving the category

        // the filtered list: categories, then apps, then the trailing
        // "Exec: <query>" entry that runs the raw typed text as a command
        std::vector<SShown>              shown;
        int                              sel = 0, first = 0;
        PHLMONITORREF                    mon;

        static NHyprCommon::CHop         pendingExec, pendingOpen;

        // launch counts + prompt history, persisted like awesome's
        // ~/.cache/awesome/{menu_count_file,history_menu}
        static std::map<std::string, int> counts;
        static std::vector<std::string>   history;      // oldest first
        static int                        histSel = -1; // -1 = editing the live query
        static std::string                histLive;     // the live query parked while walking history
        static bool                       filesLoaded = false;
        constexpr size_t                  HISTORY_MAX = 50; // awful.prompt's history_max

        // Tab-completion cycle; survives only between consecutive Tabs
        static bool                     compActive = false;
        static std::vector<std::string> compList;
        static int                      compIdx   = 0;
        static size_t                   compStart = 0, compLen = 0;

        static std::vector<std::string> splitList(const std::string& s, char sep = ';') {
            std::vector<std::string> out;
            size_t                   p = 0;
            while (p < s.size()) {
                const auto N = s.find(sep, p);
                const auto T = s.substr(p, (N == std::string::npos ? s.size() : N) - p);
                if (!T.empty())
                    out.push_back(T);
                if (N == std::string::npos)
                    break;
                p = N + 1;
            }
            return out;
        }

        static std::filesystem::path cacheDir() {
            if (const char* XDG = std::getenv("XDG_CACHE_HOME"); XDG && *XDG)
                return std::filesystem::path{XDG} / "hyprbar";
            const char* HOME = std::getenv("HOME");
            return std::filesystem::path{HOME ? HOME : "/tmp"} / ".cache" / "hyprbar";
        }

        static void loadFiles() {
            if (filesLoaded)
                return;
            filesLoaded = true;
            std::string   line;
            std::ifstream C(cacheDir() / "menu_count_file"); // "name;count" lines
            while (C && std::getline(C, line)) {
                const auto SEP = line.rfind(';');
                if (SEP == std::string::npos)
                    continue;
                try {
                    counts[line.substr(0, SEP)] = std::stoi(line.substr(SEP + 1));
                } catch (...) {}
            }
            std::ifstream H(cacheDir() / "history_menu");
            while (H && std::getline(H, line))
                if (!line.empty())
                    history.push_back(line);
            if (history.size() > HISTORY_MAX)
                history.erase(history.begin(), history.end() - HISTORY_MAX);
        }

        static void saveCounts() {
            std::error_code ec;
            std::filesystem::create_directories(cacheDir(), ec);
            std::ofstream F(cacheDir() / "menu_count_file", std::ios::trunc);
            for (const auto& [N, C] : counts)
                F << N << ';' << C << '\n';
        }

        static void historyAdd(const std::string& q) {
            if (q.empty())
                return;
            std::erase(history, q); // re-running moves it to most recent
            history.push_back(q);
            if (history.size() > HISTORY_MAX)
                history.erase(history.begin());
            std::error_code ec;
            std::filesystem::create_directories(cacheDir(), ec);
            std::ofstream F(cacheDir() / "history_menu", std::ios::trunc);
            for (const auto& H : history)
                F << H << '\n';
        }

        // Exec= field codes: %c = the app name, %k = the .desktop path, %i =
        // "--icon <path>"; the file/url ones (%f/%u/...) a launcher has nothing
        // for and drops. "%%" is a literal percent.
        static std::string substFieldCodes(const std::string& exec, const std::string& name, const std::string& file, const std::string& iconPath) {
            std::string out;
            for (size_t i = 0; i < exec.size(); i++) {
                if (exec[i] != '%' || i + 1 >= exec.size()) {
                    out += exec[i];
                    continue;
                }
                switch (exec[++i]) {
                    case '%': out += '%'; break;
                    case 'c': out += name; break;
                    case 'k': out += file; break;
                    case 'i':
                        if (!iconPath.empty())
                            out += "--icon " + iconPath;
                        break;
                    default: break; // %f/%u/%F/%U and the deprecated codes: dropped
                }
            }
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            return out;
        }

        static std::vector<std::string> desktops; // XDG_CURRENT_DESKTOP entries, for OnlyShowIn/NotShowIn

        static void                     parseFile(const std::filesystem::path& path, const std::string& id, std::unordered_set<std::string>& seen) {
            if (!seen.insert(id).second)
                return; // an earlier (higher-precedence) dir already provides this id

            std::ifstream F(path);
            if (!F)
                return;

            SApp        app;
            std::string line, rawExec, onlyShowIn, notShowIn, categories;
            bool        inEntry = false, hidden = false, isApp = false;
            while (std::getline(F, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.starts_with("[")) {
                    if (inEntry)
                        break; // only [Desktop Entry] matters
                    inEntry = line == "[Desktop Entry]";
                    continue;
                }
                if (!inEntry)
                    continue;
                if (line.starts_with("Name=") && app.name.empty())
                    app.name = line.substr(5);
                else if (line.starts_with("Exec="))
                    rawExec = line.substr(5);
                else if (line.starts_with("Icon="))
                    app.icon = line.substr(5);
                else if (line.starts_with("Terminal=true"))
                    app.terminal = true;
                else if (line.starts_with("Type="))
                    isApp = line.substr(5) == "Application";
                else if ((line.starts_with("NoDisplay=") || line.starts_with("Hidden=")) && line.ends_with("=true"))
                    hidden = true;
                else if (line.starts_with("OnlyShowIn="))
                    onlyShowIn = line.substr(11);
                else if (line.starts_with("NotShowIn="))
                    notShowIn = line.substr(10);
                else if (line.starts_with("Categories="))
                    categories = line.substr(11);
            }
            if (!isApp || hidden || rawExec.empty())
                return;

            // honor OnlyShowIn/NotShowIn against this desktop (awesome checks
            // them against its wm_name)
            const auto HERE = [&](const std::string& list) {
                for (const auto& T : splitList(list))
                    if (std::find(desktops.begin(), desktops.end(), T) != desktops.end())
                        return true;
                return false;
            };
            if (!onlyShowIn.empty() && !HERE(onlyShowIn))
                return;
            if (HERE(notShowIn))
                return;

            if (app.name.empty())
                app.name = "[" + path.stem().string() + "]"; // awesome's nameless fallback

            // the first Categories= token that maps files the app there
            for (const auto& T : splitList(categories)) {
                for (int i = 0; app.category < 0 && i < NCATS; i++)
                    if (T == CATEGORIES[i].appType)
                        app.category = i;
                if (app.category >= 0)
                    break;
            }

            const std::string ICONPATH = rawExec.find("%i") != std::string::npos ? resolveIconPath(app.icon) : "";
            app.exec                   = substFieldCodes(rawExec, app.name, path.string(), ICONPATH);
            if (app.exec.empty())
                return;
            app.lname = lower(app.name);
            // matched like awesome: against the name AND the launch command line
            app.lexec = lower(app.terminal ? cfg.terminal->value() + " -e " + app.exec : app.exec);
            apps.push_back(std::move(app));
        }

        static void parseApps() {
            apps.clear();
            if (const char* XCD = std::getenv("XDG_CURRENT_DESKTOP"); XCD && *XCD)
                desktops = splitList(XCD, ':');
            else
                desktops = {"Hyprland"};

            std::vector<std::string> dirs; // user dirs first: their ids win
            const auto               addDir = [&](const std::string& d) {
                if (!d.empty() && std::find(dirs.begin(), dirs.end(), d) == dirs.end())
                    dirs.push_back(d);
            };
            const char* HOME = std::getenv("HOME");
            if (const char* XDH = std::getenv("XDG_DATA_HOME"); XDH && *XDH)
                addDir(std::string{XDH} + "/applications");
            else if (HOME)
                addDir(std::string{HOME} + "/.local/share/applications");
            if (HOME)
                addDir(std::string{HOME} + "/.local/share/flatpak/exports/share/applications");
            for (const auto& D : splitList(std::getenv("XDG_DATA_DIRS") ? std::getenv("XDG_DATA_DIRS") : "/usr/local/share:/usr/share", ':'))
                addDir(D + (D.back() == '/' ? "applications" : "/applications"));
            addDir("/var/lib/flatpak/exports/share/applications");

            std::unordered_set<std::string> seen;
            for (const auto& D : dirs) {
                std::error_code                               ec;
                std::filesystem::recursive_directory_iterator IT(D, std::filesystem::directory_options::skip_permission_denied, ec), END;
                for (; !ec && IT != END; IT.increment(ec)) { // recursive, like awesome
                    std::error_code ec2;
                    if (IT->path().extension() == ".desktop" && IT->is_regular_file(ec2))
                        parseFile(IT->path(), IT->path().lexically_relative(D).string(), seen);
                }
            }
            std::sort(apps.begin(), apps.end(), [](const auto& a, const auto& b) { return a.lname < b.lname; });
        }

        // awesome's menulist_update: case-insensitive substring match on the
        // name or the command line; prefix matches rank higher, and once a
        // query is typed, entries launched often rank first (launch counts).
        static void refilter() {
            shown.clear();
            const auto Q = lower(typed);

            struct SRank {
                SShown s;
                int    prio, weight;
            };
            static std::vector<SRank> R; // reused; the menubar is singular
            R.clear();

            const auto weightOf = [&](const std::string& name) -> int {
                if (Q.empty())
                    return 0; // like awesome: counts only reorder typed queries
                const auto IT = counts.find(name);
                return IT == counts.end() ? 0 : IT->second;
            };

            static const std::vector<std::string> LCATS = [] {
                std::vector<std::string> v;
                for (int i = 0; i < NCATS; i++)
                    v.push_back(lower(CATEGORIES[i].name));
                return v;
            }();

            if (currentCat < 0)
                for (int i = 0; i < NCATS; i++) {
                    const auto& LN = LCATS[i];
                    if (LN.find(Q) == std::string::npos)
                        continue;
                    R.push_back({{.cat = i}, LN.starts_with(Q) ? 3 : 2, weightOf(CATEGORIES[i].name)});
                }

            for (int i = 0; i < (int)apps.size(); i++) {
                const auto& A = apps[i];
                if (currentCat >= 0 && A.category != currentCat)
                    continue;
                if (A.lname.find(Q) == std::string::npos && A.lexec.find(Q) == std::string::npos)
                    continue;
                R.push_back({{.app = i}, A.lname.starts_with(Q) || A.lexec.starts_with(Q) ? 1 : 0, weightOf(A.name)});
            }

            std::stable_sort(R.begin(), R.end(), [](const SRank& a, const SRank& b) { return a.prio != b.prio ? a.prio > b.prio : a.weight > b.weight; });
            for (const auto& r : R)
                shown.push_back(r.s);
            shown.push_back({}); // "Exec: <query>", always last

            // awesome keeps the selection across query changes, only clamped
            sel   = std::clamp(sel, 0, (int)shown.size() - 1);
            first = std::min(first, sel);
        }

        void close() {
            if (!isOpen)
                return;
            barChanged(); // while still open: the damage must cover the prompt strip
            isOpen = false;
            typed.clear();
            cursor     = 0;
            currentCat = -1;
            histSel    = -1;
            compActive = false;
        }

        void open() {
            const auto M = Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr;
            if (!M)
                return;
            Menu::close();
            if (!parsed) {
                parseApps(); // a few hundred small file reads, once per session
                parsed = true;
            }
            loadFiles();
            typed.clear();
            cursor     = 0;
            currentCat = -1;
            sel = first = 0;
            histSel     = -1;
            compActive  = false;
            refilter();
            mon    = M;
            isOpen = true;
            barChanged();
        }

        static void enterCategory(int c) {
            currentCat = c;
            prevSel    = sel;
            typed.clear(); // awesome resets the query when drilling in
            cursor  = 0;
            histSel = -1;
            sel = first = 0;
            refilter();
            barChanged();
        }

        static void exitCategory() { // the typed query survives, like awesome
            currentCat = -1;
            refilter();
            sel   = std::clamp(prevSel, 0, (int)shown.size() - 1);
            first = 0;
            barChanged();
        }

        // run a shown entry, or the raw query for the Exec one
        static void launch(const SShown& S, bool forceTerminal) {
            std::string cmd = S.app >= 0 ? apps[S.app].exec : typed;
            while (!cmd.empty() && cmd.back() == ' ')
                cmd.pop_back();
            if (cmd.empty())
                return;
            if ((S.app >= 0 && apps[S.app].terminal) || forceTerminal)
                cmd = cfg.terminal->value() + " -e " + cmd;
            // most-launched sorts first next time (in-memory, immediate); raw
            // Exec one-offs are not counted — they'd grow the file unbounded
            if (S.app >= 0)
                counts[apps[S.app].name]++;
            // the disk writes ride the deferred hop with the spawn, off the key emission
            pendingExec.arm([cmd, hist = typed]() {
                saveCounts();
                historyAdd(hist);
                std::ignore = Config::Supplementary::executor()->spawn(cmd);
            });
        }

        // cursor movement over typed: byte offsets on UTF-8 boundaries
        static size_t prevChar(size_t p) {
            while (p > 0 && (typed[--p] & 0xC0) == 0x80) {}
            return p;
        }
        static size_t nextChar(size_t p) {
            if (p < typed.size())
                for (p++; p < typed.size() && (typed[p] & 0xC0) == 0x80; p++) {}
            return p;
        }
        static size_t prevWord(size_t p) {
            while (p > 0 && typed[p - 1] == ' ')
                p--;
            while (p > 0 && typed[p - 1] != ' ')
                p--;
            return p;
        }
        static size_t nextWord(size_t p) {
            while (p < typed.size() && typed[p] == ' ')
                p++;
            while (p < typed.size() && typed[p] != ' ')
                p++;
            return p;
        }

        // Tab completion (awful.completion.shell, natively): the word under
        // the cursor completes from $PATH executables when it is the command
        // word, from filenames otherwise; Tab cycles, Shift-Tab cycles back.
        static void complete(bool backward) {
            if (!compActive) {
                size_t start = cursor;
                while (start > 0 && typed[start - 1] != ' ')
                    start--;
                size_t end = cursor;
                while (end < typed.size() && typed[end] != ' ')
                    end++;
                const auto STEM = typed.substr(start, cursor - start);

                compList.clear();
                if (typed.find_first_not_of(' ') >= start) { // the command word
                    for (const auto& D : splitList(std::getenv("PATH") ? std::getenv("PATH") : "", ':')) {
                        std::error_code                     ec;
                        std::filesystem::directory_iterator IT(D, std::filesystem::directory_options::skip_permission_denied, ec), END;
                        for (; !ec && IT != END; IT.increment(ec)) {
                            std::error_code ec2;
                            const auto      ST = IT->status(ec2);
                            if (ec2 || !std::filesystem::is_regular_file(ST) ||
                                (ST.permissions() & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec)) ==
                                    std::filesystem::perms::none)
                                continue;
                            if (const auto N = IT->path().filename().string(); N.starts_with(STEM))
                                compList.push_back(N);
                        }
                    }
                } else { // a filename; the typed form (~ and all) is preserved
                    std::string expanded = STEM;
                    if (const char* HOME = std::getenv("HOME"); HOME && expanded.starts_with("~/"))
                        expanded = std::string{HOME} + expanded.substr(1);
                    const auto                          SLASH  = expanded.rfind('/');
                    const auto                          DIR    = SLASH == std::string::npos ? std::string{"."} : expanded.substr(0, SLASH + 1);
                    const auto                          BASE   = SLASH == std::string::npos ? expanded : expanded.substr(SLASH + 1);
                    const auto                          TSLASH = STEM.rfind('/');
                    const auto                          PRE    = TSLASH == std::string::npos ? std::string{} : STEM.substr(0, TSLASH + 1);

                    std::error_code                     ec;
                    std::filesystem::directory_iterator IT(DIR, std::filesystem::directory_options::skip_permission_denied, ec), END;
                    for (; !ec && IT != END; IT.increment(ec)) {
                        std::error_code ec2;
                        const auto      N = IT->path().filename().string();
                        if (!N.starts_with(BASE) || (!BASE.starts_with(".") && N.starts_with(".")))
                            continue;
                        compList.push_back(PRE + N + (IT->is_directory(ec2) ? "/" : ""));
                    }
                }
                std::sort(compList.begin(), compList.end());
                compList.erase(std::unique(compList.begin(), compList.end()), compList.end());
                if (compList.empty())
                    return;
                compStart  = start;
                compLen    = end - start;
                compIdx    = backward ? (int)compList.size() - 1 : 0;
                compActive = true;
            } else {
                const int N = (int)compList.size();
                compIdx     = ((compIdx + (backward ? -1 : 1)) % N + N) % N;
            }
            typed.replace(compStart, compLen, compList[compIdx]);
            compLen = compList[compIdx].size();
            cursor  = compStart + compLen;
            histSel = -1;
            refilter();
            barChanged();
        }

        // Up/Down (or C-p/C-n): walk the persisted prompt history
        static void histGo(int dir) {
            if (history.empty())
                return;
            if (histSel < 0) {
                if (dir > 0)
                    return;
                histLive = typed; // park the live query
                histSel  = (int)history.size() - 1;
            } else if (dir < 0) {
                histSel = std::max(histSel - 1, 0);
            } else if (++histSel >= (int)history.size()) {
                histSel = -1;
                typed   = histLive; // walked past the newest: live query back
            }
            if (histSel >= 0)
                typed = history[histSel];
            cursor = typed.size();
            refilter();
            barChanged();
        }

        // While open the prompt owns the keyboard: every key event is swallowed
        // before the keybind layer, the IME, and the apps see it. Swallowing is
        // modifier-safe — mods reach clients via the separate onKeyboardMod
        // path, never through these key events.
        void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
            // emissions precede the compositor's lock handling: a locked
            // session must never feed the prompt (typed keys would collect in
            // `typed` and Return would launch() the line — under a LOCK)
            if (NHyprCommon::sessionLocked()) {
                if (isOpen)
                    close();
                return;
            }

            if (!isOpen)
                return;

            // monitor gone: never keep an invisible prompt grabbing the
            // keyboard. Fullscreen deliberately does NOT close it — render
            // and pointer both keep the prompt alive above fullscreen.
            const auto M = mon.lock();
            if (!M) {
                close();
                return;
            }

            // Releases pass through UNTOUCHED: the keybind layer tracks
            // pressed keys (m_pressedKeys), and eating the release of the
            // very Mod+P that opened the prompt left its 'p' marked held —
            // the next Mod+P press wouldn't fire and the chord needed two
            // presses. A stray release is harmless everywhere else (clients
            // and the IME ignore a release whose press they never saw).
            if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED)
                return;

            info.cancelled = true;

            const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
            if (!KB || !KB->m_xkbState)
                return;

            const xkb_keycode_t KC   = e.keycode + 8;
            const xkb_keysym_t  SYM  = xkb_state_key_get_one_sym(KB->m_xkbState, KC);
            const bool          CTRL = xkb_state_mod_name_is_active(KB->m_xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) == 1;
            const bool          ALT  = xkb_state_mod_name_is_active(KB->m_xkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) == 1;

            if (SYM >= XKB_KEY_Shift_L && SYM <= XKB_KEY_Hyper_R)
                return; // a bare modifier: not an edit, must not end a Tab cycle
            if (SYM != XKB_KEY_Tab && SYM != XKB_KEY_ISO_Left_Tab)
                compActive = false;

            const auto edited = [&]() { // any edit detaches the history walk
                histSel = -1;
                refilter();
                barChanged();
            };
            const auto delBack = [&](size_t from) {
                typed.erase(from, cursor - from);
                cursor = from;
                edited();
            };

            switch (SYM) {
                case XKB_KEY_Escape:
                    if (currentCat >= 0)
                        exitCategory(); // Escape leaves the category first, like awesome
                    else
                        close();
                    return;

                case XKB_KEY_Return:
                case XKB_KEY_KP_Enter: {
                    if (CTRL)
                        sel = (int)shown.size() - 1; // C-Return: the Exec entry, the raw query
                    if (sel >= 0 && sel < (int)shown.size()) {
                        const auto S = shown[sel];
                        if (S.cat >= 0) {
                            enterCategory(S.cat);
                            return; // drilling in keeps the prompt open
                        }
                        launch(S, CTRL && ALT); // C-M-Return: the raw query in the terminal
                    }
                    close();
                    return;
                }

                case XKB_KEY_Left: // selection never wraps, like awesome
                    sel = std::max(sel - 1, 0);
                    barChanged();
                    return;
                case XKB_KEY_Right:
                    sel = std::min(sel + 1, (int)shown.size() - 1);
                    barChanged();
                    return;
                case XKB_KEY_Home:
                    sel = first = 0;
                    barChanged();
                    return;
                case XKB_KEY_End:
                    sel = (int)shown.size() - 1;
                    barChanged();
                    return;

                case XKB_KEY_Tab: // shell completion, never selection (awesome parity)
                    complete(false);
                    return;
                case XKB_KEY_ISO_Left_Tab: complete(true); return;

                case XKB_KEY_Up: histGo(-1); return;
                case XKB_KEY_Down: histGo(1); return;

                case XKB_KEY_Delete:
                    if (cursor < typed.size()) {
                        typed.erase(cursor, nextChar(cursor) - cursor);
                        edited();
                    }
                    return;

                case XKB_KEY_BackSpace:
                    if (CTRL || ALT)
                        delBack(prevWord(cursor));
                    else if (typed.empty() && currentCat >= 0)
                        exitCategory(); // BackSpace on an empty query backs out
                    else if (cursor > 0)
                        delBack(prevChar(cursor));
                    return;

                default: break;
            }

            if (CTRL && !ALT) {
                switch (SYM) {
                    case XKB_KEY_j: // awesome: C-j/C-k double Left/Right
                        sel = std::max(sel - 1, 0);
                        barChanged();
                        return;
                    case XKB_KEY_k:
                        sel = std::min(sel + 1, (int)shown.size() - 1);
                        barChanged();
                        return;
                    case XKB_KEY_p: histGo(-1); return;
                    case XKB_KEY_n: histGo(1); return;
                    case XKB_KEY_a:
                        cursor = 0;
                        barChanged();
                        return;
                    case XKB_KEY_e:
                        cursor = typed.size();
                        barChanged();
                        return;
                    case XKB_KEY_b:
                        cursor = prevChar(cursor);
                        barChanged();
                        return;
                    case XKB_KEY_f:
                        cursor = nextChar(cursor);
                        barChanged();
                        return;
                    case XKB_KEY_d:
                        if (cursor < typed.size()) {
                            typed.erase(cursor, nextChar(cursor) - cursor);
                            edited();
                        }
                        return;
                    case XKB_KEY_h:
                        if (cursor > 0)
                            delBack(prevChar(cursor));
                        return;
                    case XKB_KEY_u: delBack(0); return;
                    case XKB_KEY_w: delBack(prevWord(cursor)); return;
                    default: return; // unbound C-chords are swallowed, not typed
                }
            }

            if (ALT && !CTRL) {
                switch (SYM) {
                    case XKB_KEY_b:
                        cursor = prevWord(cursor);
                        barChanged();
                        return;
                    case XKB_KEY_f:
                        cursor = nextWord(cursor);
                        barChanged();
                        return;
                    case XKB_KEY_d:
                        typed.erase(cursor, nextWord(cursor) - cursor);
                        edited();
                        return;
                    default: return; // unbound M-chords are swallowed, not typed
                }
            }
            if (CTRL || ALT)
                return;

            char buf[16] = {};
            if (xkb_state_key_get_utf8(KB->m_xkbState, KC, buf, sizeof(buf)) > 0 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f) {
                const std::string S{buf};
                typed.insert(cursor, S);
                cursor += S.size();
                edited();
            }
        }

        // The launcher strip, a floating glass pill below the bar — synced to
        // the compact islands (h 30, inset 6, below-bar offset 34, pill).
        CBox stripBox;

        void render(const SPaint& PAINT) {
            if (!isOpen || mon.lock() != PAINT.mon)
                return;

            // one palette fetch per render: color() memoizes but still hashes per call
            const CHyprColor COLFG = color(cfg.colFg), COLACTIVEBG = color(cfg.colActiveBg), COLFOCUS = color(cfg.colFocus), COLACTIVE = color(cfg.colActive),
                             COLMUTED = color(cfg.colMuted);

            const CBox STRIP{PAINT.mb.x + 6, PAINT.mb.y + PAINT.h + 4, PAINT.mb.w - 12, PAINT.h};
            stripBox = STRIP;
            PAINT.glass(STRIP, (int)std::lround(STRIP.h / 2 * PAINT.scale));

            double px = STRIP.x + 12;

            // "run: " (or the drilled-into category) with the cursor in place —
            // a ▏ glyph: renderText takes plain text only (no pango markup),
            // so an inverse-video block cursor can't be drawn natively
            const std::string PLABEL = Menubar::currentCat >= 0 ? std::string{Menubar::CATEGORIES[Menubar::currentCat].name} + ": " : "run: ";
            const auto        PROMPT = textTex(PLABEL + Menubar::typed.substr(0, Menubar::cursor) + "▏" + Menubar::typed.substr(Menubar::cursor), COLFG, PAINT.pt);
            const double      PW     = PROMPT ? PROMPT->m_size.x / PAINT.scale : 0;
            PAINT.texIn(PROMPT, CBox{px, STRIP.y, PW, STRIP.h});
            px += PW + 8;
            PAINT.rect(CBox{px, STRIP.y + 7, 1, STRIP.h - 14}, color(cfg.colFrame)); // the prompt's right hairline
            px += 9;

            // the keyboard hint, right-aligned, out of the chips' way
            const auto HINT  = textTex("type to filter · ←→ · ⏎ run · esc", COLMUTED, std::max(6, (int)std::round(10.0 * PAINT.scale)));
            const double HW  = HINT ? HINT->m_size.x / PAINT.scale : 0;
            const double PXE = STRIP.x + STRIP.w - HW - 12 - 8;
            PAINT.texIn(HINT, CBox{STRIP.x + STRIP.w - HW - 12, STRIP.y, HW, STRIP.h});

            auto& SH = Menubar::shown;
            if (!SH.empty()) {
                if (Menubar::sel >= (int)SH.size())
                    Menubar::sel = (int)SH.size() - 1;
                if (Menubar::first > Menubar::sel)
                    Menubar::first = Menubar::sel;

                const auto entryName = [&](int i) -> std::string {
                    if (SH[i].app >= 0)
                        return Menubar::apps[SH[i].app].name;
                    if (SH[i].cat >= 0)
                        return Menubar::CATEGORIES[SH[i].cat].name;
                    return "Exec: " + Menubar::typed;
                };
                const auto entryIcon = [&](int i) -> SP<ITexture> {
                    if (SH[i].app >= 0)
                        return namedIcon(Menubar::apps[SH[i].app].icon);
                    if (SH[i].cat >= 0)
                        return namedIcon(Menubar::CATEGORIES[SH[i].cat].icon);
                    return nullptr;
                };

                // app chips: h 24, pad-x 11, 15px icon, gap 5; the icon cell is
                // always reserved — an entry whose icon doesn't resolve gets the
                // letter fallback instead of collapsing (rows keep their rhythm)
                constexpr double CHIP_H = 24, CHIP_PADX = 11, CHIP_GAP = 5, ICON = 15, ICON_GAP = 5;
                const double     CY     = STRIP.y + (STRIP.h - CHIP_H) / 2;
                const int        RCHIP  = (int)std::lround(CHIP_H / 2 * PAINT.scale);
                const auto       entryW = [&](int i) {
                    const auto T = textTex(entryName(i), COLFG, PAINT.pt);
                    return CHIP_PADX + ICON + ICON_GAP + (T ? T->m_size.x / PAINT.scale : 0) + CHIP_PADX + CHIP_GAP;
                };

                { // keep the selection on screen: page-jump to it when it won't fit
                    double w = 0;
                    for (int i = Menubar::first; i <= Menubar::sel; i++)
                        w += entryW(i);
                    if (px + w > PXE)
                        Menubar::first = Menubar::sel;
                }

                for (int i = Menubar::first; i < (int)SH.size(); i++) {
                    const auto   NAME = entryName(i);
                    const auto   ITEX = entryIcon(i);
                    const auto   WT   = textTex(NAME, COLFG, PAINT.pt);
                    const double W    = CHIP_PADX + ICON + ICON_GAP + (WT ? WT->m_size.x / PAINT.scale : 0) + CHIP_PADX;
                    if (px + W > PXE)
                        break;

                    CHyprColor fg = COLFG;
                    if (i == Menubar::sel) {
                        PAINT.rect(CBox{px, CY, W, CHIP_H}, COLACTIVEBG, RCHIP);
                        fg = COLFOCUS;
                    }

                    double tx = px + CHIP_PADX;
                    if (ITEX && ITEX->m_texID != 0) {
                        const auto P = PAINT.toPhys(CBox{tx, CY + (CHIP_H - ICON) / 2, ICON, ICON});
                        PAINT.tex(ITEX, P);
                    } else
                        PAINT.texIn(textTex(letterOf(NAME), COLACTIVE, PAINT.pt), CBox{tx, CY, ICON, CHIP_H});
                    tx += ICON + ICON_GAP;

                    const auto T = i == Menubar::sel ? textTex(NAME, fg, PAINT.pt) : WT;
                    if (T && T->m_texID != 0) {
                        const auto P = PAINT.toPhys(CBox{tx, CY, 1, CHIP_H});
                        CBox       b{P.x, P.y + (P.h - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y};
                        PAINT.tex(T, b.round());
                    }
                    px += W + CHIP_GAP;
                }
            }
        }
    }

    namespace Menubar {
        // hl.plugin.hyprbar.menubar(), deferred out of the Lua call
        void toggleDeferred() {
            pendingOpen.arm([]() {
                if (isOpen)
                    close();
                else
                    open();
            });
        }

        void exit() {
            close();
            apps.clear();
            parsed = false;
            counts.clear();
            history.clear();
            filesLoaded = false;
            compList.clear();
            pendingExec.reset();
            pendingOpen.reset();
        }
    } // namespace Menubar

} // namespace NHyprbar
