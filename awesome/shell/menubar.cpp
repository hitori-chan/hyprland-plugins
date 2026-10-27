// awesome/shell/menubar.cpp — awesome's Mod+P launcher: .desktop apps, categories, prompt, completion, history


#include "../core/bounded.hpp"
#include "../core/clipboard.hpp"
#include "../core/desktopindex.hpp"
#include "../core/fileindex.hpp"
#include "../core/keys.hpp"
#include "../core/persist.hpp"
#include "../core/state.hpp"


#include "../core/desktop_exec.hpp"
#include "shell/shell.hpp"

#include <hyprland/src/protocols/XDGActivation.hpp>

namespace NAwesome::Shell {

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

        std::vector<SApp> apps; // from the core's desktop index, sorted by name
        static uint64_t   appsVersion = 0; // the index version apps were built from
        static bool       appsBuilt   = false;
        static NAwesome::CAsyncFileIndex completionIndex;
        static SP<CEventLoopTimer>           indexPoll;
        static NAwesome::Keys::CRepeat       repeat; // the held-key repeat (see onKey)
        static uint64_t                      completionGeneration = 0;
        static uint64_t                      activationGeneration = 0;
        static NAwesome::CClipboardRead     clipboard;
        // the selected row across a rebuild, by identity (its index moves
        // when a refresh lands): an app by desktop id, a category, or the
        // Exec entry — Enter on "Exec:" must not launch the app slid there
        struct SSelectionKey {
            std::string appId;
            int         cat  = -1;
            bool        exec = false;
        };
        static std::optional<SSelectionKey> pendingSelection;

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

        static NAwesome::CHop         pendingExec, pendingOpen;

        // launch counts + prompt history: the core's unified state
        // (core/state.hpp), one shared in-memory copy
        static NAwesome::CRecentMap<int>& launchCounts = NAwesome::StateStore::inst().data().launches;
        static NAwesome::ListStore&  history      = NAwesome::StateStore::inst().data().history; // oldest first
        static int                  histSel = -1;       // -1 = editing the live query
        static std::string          histLive;           // the live query parked while walking history
        static bool                 filesLoaded = false;
        constexpr size_t                  QUERY_MAX   = 4096;

        // Tab-completion cycle; survives only between consecutive Tabs
        static bool                     compActive = false;
        static std::vector<std::string> compList;
        static int                      compIdx   = 0;
        static size_t                   compStart = 0, compLen = 0;
        static bool                     compScanning = false, compBackward = false, compHideDotfiles = false;
        static std::string              compPrefix;
        // a stalled mount in $PATH (or under a typed path) stalls the
        // helper: past this the Tab completes from what it gathered
        constexpr auto                        COMPLETION_DEADLINE = std::chrono::seconds(2);
        static std::chrono::steady_clock::time_point compDeadline;

        static void cancelCompletion();

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

        static std::optional<sdbus::ObjectPath> desktopObjectPath(const std::string& name) {
            if (name.empty() || name.front() == '.' || name.back() == '.')
                return std::nullopt;
            std::string path{"/"};
            for (const char C : name) {
                if (C == '.')
                    path += '/';
                else if (C == '-')
                    path += '_';
                else
                    path += C;
            }
            return sdbus::ObjectPath{path};
        }

        // the launcher's state is the core's unified state file (the
        // supervisor loads it before module inits); this is only the
        // idempotent guard for a lazy first use.
        static void loadFiles() {
            if (filesLoaded)
                return;
            filesLoaded = true;
            NAwesome::StateStore::inst().load();
        }

        static void saveCounts() {
            NAwesome::StateStore::inst().dirty(); // coalesced atomic write
        }

        static void historyAdd(const std::string& q) {
            if (history.remember(q)) // dedup + most-recent-last + the 50 bound
                NAwesome::StateStore::inst().dirty();
        }

        static std::vector<std::string> desktops; // XDG_CURRENT_DESKTOP entries, for OnlyShowIn/NotShowIn

        // an index entry the launcher lists: an application, shown here,
        // with something to run
        static void addApp(const NAwesome::SDesktopEntry& E) {
            if (E.malformed || !E.application || E.hidden || (!E.dbusActivatable && E.exec.empty()))
                return;

            // honor OnlyShowIn/NotShowIn against this desktop (awesome checks
            // them against its wm_name)
            const auto HERE = [&](const std::vector<std::string>& list) {
                for (const auto& T : list)
                    if (std::find(desktops.begin(), desktops.end(), T) != desktops.end())
                        return true;
                return false;
            };
            if (!E.onlyShowIn.empty() && !HERE(E.onlyShowIn))
                return;
            if (HERE(E.notShowIn))
                return;

            SApp app;
            app.desktopId       = E.id;
            app.name            = E.name.empty() ? "[" + std::filesystem::path{E.path}.stem().string() + "]" : E.name; // awesome's nameless fallback
            app.icon            = E.icon;
            app.terminal        = E.terminal;
            app.dbusActivatable = E.dbusActivatable;

            // the first Categories= token that maps files the app there
            for (const auto& T : E.categories) {
                for (int i = 0; app.category < 0 && i < NCATS; i++)
                    if (T == CATEGORIES[i].appType)
                        app.category = i;
                if (app.category >= 0)
                    break;
            }

            std::string plainExec; // the words as typed, unquoted: what a query matches
            if (!E.exec.empty()) {
                const auto WORDS = NAwesome::DesktopExec::expand(E.exec, app.name, E.path, app.icon);
                if (!WORDS && !app.dbusActivatable)
                    return;
                if (WORDS) {
                    app.exec = NAwesome::DesktopExec::shellCommand(*WORDS);
                    for (const auto& W : *WORDS)
                        plainExec.append(plainExec.empty() ? "" : " ").append(W);
                }
            }
            app.lname = lower(app.name);
            // matched like awesome: against the name AND the launch command
            // line — its plain words (the quoted form starts every word with
            // a quote, and the command-prefix rank never matched)
            app.lexec = lower(app.terminal && !plainExec.empty() ? std::string(cfg().getS("plugin:awesome:shell:terminal")) + " -e " + plainExec : plainExec);
            apps.push_back(std::move(app));
        }

        // apps from the index as it stands (a no-op while it has not
        // changed); the open launcher keeps its selection across a rebuild
        static void buildApps() {
            const auto& INDEX = NAwesome::CDesktopIndex::inst();
            if (appsBuilt && appsVersion == INDEX.version())
                return;
            appsBuilt   = true;
            appsVersion = INDEX.version();
            if (isOpen && sel >= 0 && sel < (int)shown.size()) {
                const auto& S    = shown[sel];
                pendingSelection = S.app >= 0 ? SSelectionKey{.appId = apps[S.app].desktopId} : SSelectionKey{.cat = S.cat, .exec = S.cat < 0};
            }
            if (const char* XCD = std::getenv("XDG_CURRENT_DESKTOP"); XCD && *XCD)
                desktops = splitList(XCD, ':');
            else
                desktops = {"Hyprland"};
            apps.clear();
            shown.clear(); // its entries index into apps: the caller refilters
            for (const auto& E : INDEX.entries())
                addApp(E);
            std::ranges::sort(apps, [](const auto& a, const auto& b) { return a.lname < b.lname; });
        }

        static void restoreSelection() {
            if (!pendingSelection)
                return;
            const auto KEY = *std::exchange(pendingSelection, std::nullopt);
            for (int i = 0; i < (int)shown.size(); i++) {
                const auto& S = shown[i];
                if (KEY.exec          ? S.app < 0 && S.cat < 0 :
                        !KEY.appId.empty() ? S.app >= 0 && apps[S.app].desktopId == KEY.appId :
                                             S.app < 0 && S.cat == KEY.cat) {
                    sel = i;
                    break;
                }
            }
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
                const int* C = launchCounts.find(name);
                return C ? *C : 0;
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
            repeat.stop();
            if (!isOpen)
                return;
            // damage NOW, while still open: the coalesced barChanged runs
            // after isOpen drops and damages the bar's height alone, leaving
            // the prompt band's old pixels on screen
            damageBars();
            barChanged();
            isOpen = false;
            clipboard.cancel();
            typed.clear();
            cursor     = 0;
            currentCat = -1;
            histSel    = -1;
            cancelCompletion();
            compActive = false;
            compList.clear();
        }

        void open() {
            const auto M = Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr;
            if (!M)
                return;
            Menu::close();
            // an app installed or removed since the last scan rescans
            // (the index swaps the new set in whole when it lands)
            NAwesome::CDesktopIndex::inst().refreshIfStale();
            buildApps();
            loadFiles();
            clipboard.cancel();
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
            promptChanged();
        }

        static void exitCategory() { // the typed query survives, like awesome
            currentCat = -1;
            refilter();
            sel   = std::clamp(prevSel, 0, (int)shown.size() - 1);
            first = 0;
            promptChanged();
        }

        // The launch is the user's gesture, but the Enter that made it was
        // swallowed: the app holds no input serial to earn its own
        // xdg-activation token, so an already-running app (a D-Bus activated
        // one, a single-instance one) could not raise itself. Hand it a
        // compositor-minted token, the way any launcher passes its own.
        static std::string launchToken() {
            return PROTO::activation ? PROTO::activation->mintToken() : std::string{};
        }

        // The command line with the token in its environment (the executor
        // runs it through /bin/sh -c, so an export covers a whole list)
        static std::string withToken(const std::string& cmd, const std::string& token) {
            if (token.empty())
                return cmd;
            const auto Q = NAwesome::DesktopExec::shellQuote(token);
            return "export XDG_ACTIVATION_TOKEN=" + Q + " DESKTOP_STARTUP_ID=" + Q + "; " + cmd;
        }

        static void activateDbus(const SApp& app) {
            const auto TOKEN = launchToken();
            const auto PATH  = desktopObjectPath(app.desktopId);
            if (!PATH || !Tray::bus.conn()) {
                if (!app.exec.empty())
                    std::ignore = Config::Supplementary::executor()->spawn(withToken(app.exec, TOKEN));
                return;
            }
            try {
                auto P = std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(*Tray::bus.conn(), sdbus::ServiceName{app.desktopId}, *PATH).release());
                auto DATA = std::map<std::string, sdbus::Variant>{};
                if (!TOKEN.empty()) {
                    DATA.emplace("activation-token", sdbus::Variant{TOKEN});
                    DATA.emplace("desktop-startup-id", sdbus::Variant{TOKEN});
                }
                const auto GENERATION = activationGeneration;
                P->callMethodAsync("Activate")
                    .onInterface("org.freedesktop.Application")
                    .withArguments(DATA)
                    .uponReplyInvoke([P, app, GENERATION](std::optional<sdbus::Error> error) {
                        if (GENERATION == activationGeneration && error && !app.exec.empty())
                            std::ignore = Config::Supplementary::executor()->spawn(withToken(app.exec, launchToken()));
                    });
                Tray::pollSoon();
            } catch (...) {
                if (!app.exec.empty())
                    std::ignore = Config::Supplementary::executor()->spawn(withToken(app.exec, TOKEN));
            }
        }

        // run a shown entry, or the raw query for the Exec one
        static void launch(const SShown& S, bool forceTerminal) {
            const SApp* APP = S.app >= 0 && S.app < (int)apps.size() ? &apps[S.app] : nullptr;
            if (APP && APP->dbusActivatable && !forceTerminal) {
                const SApp COPY = *APP;
                NAwesome::bumpCount(launchCounts, APP->name);
                pendingExec.arm([COPY, hist = typed]() {
                    saveCounts();
                    historyAdd(hist);
                    activateDbus(COPY);
                });
                return;
            }
            std::string cmd = APP ? APP->exec : typed;
            while (!cmd.empty() && cmd.back() == ' ')
                cmd.pop_back();
            if (cmd.empty())
                return;
            if ((APP && APP->terminal) || forceTerminal)
                cmd = std::string(cfg().getS("plugin:awesome:shell:terminal")) + " -e " + cmd;
            // most-launched sorts first next time (in-memory, immediate); raw
            // Exec one-offs are not counted — they'd grow the file unbounded
            if (APP)
                NAwesome::bumpCount(launchCounts, APP->name);
            // the disk writes ride the deferred hop with the spawn, off the key emission
            pendingExec.arm([cmd, hist = typed]() {
                saveCounts();
                historyAdd(hist);
                std::ignore = Config::Supplementary::executor()->spawn(withToken(cmd, launchToken()));
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

        static void cancelCompletion() {
            if (!compScanning)
                return;
            compScanning = false;
            completionIndex.cancel(++completionGeneration);
        }

        static void applyCompletion(bool backward) {
            if (compList.empty())
                return;
            if (compActive) {
                const int N = (int)compList.size();
                compIdx     = ((compIdx + (backward ? -1 : 1)) % N + N) % N;
            } else {
                compIdx    = backward ? (int)compList.size() - 1 : 0;
                compActive = true;
            }
            typed.replace(compStart, compLen, compList[compIdx]);
            compLen = compList[compIdx].size();
            cursor  = compStart + compLen;
            histSel = -1;
            refilter();
            promptChanged();
        }

        static void pollCompletion() {
            if (!compScanning)
                return;
            std::vector<NAwesome::CAsyncFileIndex::SEntry> entries;
            const bool COMPLETE = completionIndex.poll(completionGeneration, entries, 64);
            for (const auto& entry : entries) {
                const auto name = entry.path.filename().string();
                if (compHideDotfiles && name.starts_with("."))
                    continue;
                // the prompt runs through /bin/sh -c: a name lands as one
                // escaped word, or not at all (control/bidi characters)
                const auto WORD = NAwesome::DesktopExec::shellWord(name);
                if (!WORD)
                    continue;
                auto candidate = compPrefix + *WORD + (entry.directory ? "/" : "");
                if (typed.size() - compLen + candidate.size() > QUERY_MAX)
                    continue; // the prompt's bound holds for completions too
                compList.push_back(std::move(candidate));
            }
            bool done = COMPLETE;
            if (!done && std::chrono::steady_clock::now() > compDeadline) {
                completionIndex.cancel(++completionGeneration);
                done = true;
            }
            compScanning = !done;
            // the first completion waits for the whole scan (or its cap, or
            // its deadline): the first batch's first was not the list's, and
            // the cycle began mid-list
            if (done) {
                std::sort(compList.begin(), compList.end());
                compList.erase(std::unique(compList.begin(), compList.end()), compList.end());
                applyCompletion(compBackward); // it repaints; an empty list stays as typed
            }
        }

        // Tab completion (awful.completion.shell, natively): the word under
        // the cursor completes from $PATH executables when it is the command
        // word, from filenames otherwise; a command word typed as a path
        // (./run, ~/bin/x) completes from its directory, executables and
        // the directories to walk on, as the shell does. Enumeration is
        // worker-owned; the prompt only receives bounded batches through
        // pollCompletion().
        static void complete(bool backward) {
            if (compActive) {
                applyCompletion(backward);
                return;
            }
            if (compScanning) {
                compBackward = backward;
                return;
            }

            // the word under the cursor, an escaped space inside it
            const auto WORDBREAK = [](size_t i) { return typed[i] == ' ' && !NAwesome::DesktopExec::escapedAt(typed, i); };
            size_t     start     = cursor;
            while (start > 0 && !WORDBREAK(start - 1))
                start--;
            size_t end = cursor;
            while (end < typed.size() && !WORDBREAK(end))
                end++;
            const auto STEM = typed.substr(start, cursor - start); // as typed, escapes and all

            NAwesome::CAsyncFileIndex::SRequest request;
            request.generation = ++completionGeneration;
            request.maxEntries = 512;
            request.maxVisited = 8192;
            const bool COMMAND = typed.find_first_not_of(' ') >= start;
            if (COMMAND && NAwesome::DesktopExec::unescapeWord(STEM).find('/') == std::string::npos) { // a bare command word
                const char* PATH = std::getenv("PATH");
                for (const auto& dir : splitList(PATH ? PATH : "", ':'))
                    request.roots.emplace_back(dir);
                request.namePrefix = NAwesome::DesktopExec::unescapeWord(STEM);
                request.executable = true;
                compPrefix.clear();
                compHideDotfiles = false;
            } else { // a filename; the typed form (~ and all) is preserved
                std::string expanded = NAwesome::DesktopExec::unescapeWord(STEM);
                if (const char* HOME = std::getenv("HOME"); HOME && expanded.starts_with("~/"))
                    expanded = std::string{HOME} + expanded.substr(1);
                const auto SLASH = expanded.rfind('/');
                const auto DIR   = SLASH == std::string::npos ? std::string{"."} : expanded.substr(0, SLASH + 1);
                request.roots.emplace_back(DIR);
                request.namePrefix = SLASH == std::string::npos ? expanded : expanded.substr(SLASH + 1);
                request.directories = true;
                request.executable  = COMMAND;
                const auto TSLASH = STEM.rfind('/');
                compPrefix = TSLASH == std::string::npos ? std::string{} : STEM.substr(0, TSLASH + 1);
                compHideDotfiles = !request.namePrefix.starts_with(".");
            }
            if (request.roots.empty())
                return;
            compList.clear();
            compStart    = start;
            compLen      = end - start;
            compBackward = backward;
            compScanning = true;
            compDeadline = std::chrono::steady_clock::now() + COMPLETION_DEADLINE;
            completionIndex.request(std::move(request));
            if (indexPoll)
                indexPoll->updateTimeout(std::chrono::milliseconds(2));
        }

        // Up/Down (or C-p/C-n): walk the persisted prompt history
        static void histGo(int dir) {
            if (history.entries.empty())
                return;
            if (histSel < 0) {
                if (dir > 0)
                    return;
                histLive = typed; // park the live query
                histSel  = (int)history.entries.size() - 1;
            } else if (dir < 0) {
                histSel = std::max(histSel - 1, 0);
            } else if (++histSel >= (int)history.entries.size()) {
                histSel = -1;
                typed   = histLive; // walked past the newest: live query back
            }
            if (histSel >= 0)
                typed = history.entries[histSel];
            cursor = typed.size();
            refilter();
            promptChanged();
        }

        // ---- key repeat ----
        //
        // core/keys.hpp's repeat replays a held key's press through
        // handleKey; the keys that act once never start one.
        static void handleKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info);

        static void onRepeat(uint32_t keycode) {
            if (!isOpen) {
                repeat.stop();
                return;
            }
            Event::SCallbackInfo info;
            handleKey(IKeyboard::SKeyEvent{.keycode = keycode, .state = WL_KEYBOARD_KEY_STATE_PRESSED}, info);
        }

        // keys that act once: run, close, complete, paste
        static bool actsOnce(xkb_keysym_t sym, bool ctrl) {
            return sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_Escape || sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab || (ctrl && sym == XKB_KEY_v);
        }

        // While open the prompt owns the keyboard: key presses are swallowed
        // before the keybind layer, the IME, and the apps see it — except
        // Super chords, which stay the user's (Super+1, the prompt's own
        // Super+P toggle) and must run even with the strip open; a prompt
        // left open would otherwise strand workspace switching. Swallowing is
        // modifier-safe — mods reach clients via the separate onKeyboardMod
        // path, never through these key events.
        void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
            repeat.onKey(e);
            handleKey(e, info);
            if (e.state == WL_KEYBOARD_KEY_STATE_PRESSED && info.cancelled && isOpen) {
                const auto KB = NAwesome::Keys::keyboardFor(e.keycode);
                if (KB && KB->m_xkbState &&
                    !actsOnce(xkb_state_key_get_one_sym(KB->m_xkbState, e.keycode + 8),
                              xkb_state_mod_name_is_active(KB->m_xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) == 1))
                    repeat.start(KB, e.keycode);
            }
        }

        static void handleKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
            // emissions precede the compositor's lock handling: a locked
            // session must never feed the prompt (typed keys would collect in
            // `typed` and Return would launch() the line — under a LOCK)
            if (NAwesome::sessionLocked()) {
                if (isOpen)
                    close();
                return;
            }
            if (NAwesome::nativeInputCaptureActive())
                return;

            if (!isOpen)
                return;

            // monitor gone or disabled (a lid closed with the output
            // disabled keeps its object): never keep an invisible prompt
            // grabbing the keyboard. Fullscreen deliberately does NOT close
            // it — render and pointer both keep the prompt alive above
            // fullscreen.
            const auto M = mon.lock();
            if (!M || !M->m_enabled) {
                close();
                return;
            }

            // An exclusive-keyboard layer (a launcher or a power menu a Super
            // bind opened over the prompt) owns the keyboard natively: the
            // prompt steps aside instead of eating what is typed into it.
            if (NAwesome::Keys::exclusiveLayer()) {
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

            const auto KB = NAwesome::Keys::keyboardFor(e.keycode);
            if (!KB || !KB->m_xkbState)
                return;

            const xkb_keycode_t KC   = e.keycode + 8;
            const xkb_keysym_t  SYM  = xkb_state_key_get_one_sym(KB->m_xkbState, KC);
            const bool          CTRL = xkb_state_mod_name_is_active(KB->m_xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) == 1;
            const bool          ALT  = xkb_state_mod_name_is_active(KB->m_xkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) == 1;
            const bool          LOGO = xkb_state_mod_name_is_active(KB->m_xkbState, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) == 1;

            if (LOGO)
                return; // a Super chord is a user bind passing through — the chord runs (workspace switch,
                        // Super+P re-toggles the strip), the prompt keeps whatever state it has

            info.cancelled = true;

            if (SYM >= XKB_KEY_Shift_L && SYM <= XKB_KEY_Hyper_R)
                return; // a bare modifier: not an edit, must not end a Tab cycle
            if (SYM != XKB_KEY_Tab && SYM != XKB_KEY_ISO_Left_Tab) {
                compActive = false;
                cancelCompletion();
            }

            const auto edited = [&]() { // any edit detaches the history walk
                histSel = -1;
                refilter();
                promptChanged();
            };
            const auto delBack = [&](size_t from) {
                typed.erase(from, cursor - from);
                cursor = from;
                edited();
            };
            const auto paste = [&]() {
                const auto GENERATION = activationGeneration;
                clipboard.request(QUERY_MAX, [GENERATION](std::string text) {
                    if (!isOpen || GENERATION != activationGeneration)
                        return;
                    for (char& C : text)
                        if (C == '\n' || C == '\r' || C == '\t')
                            C = ' ';
                    // the line runs through /bin/sh: no control or bidi
                    // character may hide what the prompt shows
                    text = NAwesome::DesktopExec::typeableText(text);
                    if (typed.size() >= QUERY_MAX)
                        return;
                    text.resize(NAwesome::clipToUtf8Boundary(text, QUERY_MAX - typed.size()));
                    if (text.empty())
                        return;
                    // the scan and the cycle hold byte offsets into the query
                    compActive = false;
                    cancelCompletion();
                    typed.insert(cursor, text);
                    cursor += text.size();
                    histSel = -1;
                    refilter();
                    promptChanged();
                });
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
                    promptChanged();
                    return;
                case XKB_KEY_Right:
                    sel = std::min(sel + 1, (int)shown.size() - 1);
                    promptChanged();
                    return;
                case XKB_KEY_Home:
                    sel = first = 0;
                    promptChanged();
                    return;
                case XKB_KEY_End:
                    sel = (int)shown.size() - 1;
                    promptChanged();
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
                        promptChanged();
                        return;
                    case XKB_KEY_k:
                        sel = std::min(sel + 1, (int)shown.size() - 1);
                        promptChanged();
                        return;
                    case XKB_KEY_p: histGo(-1); return;
                    case XKB_KEY_n: histGo(1); return;
                    case XKB_KEY_a:
                        cursor = 0;
                        promptChanged();
                        return;
                    case XKB_KEY_e:
                        cursor = typed.size();
                        promptChanged();
                        return;
                    case XKB_KEY_b:
                        cursor = prevChar(cursor);
                        promptChanged();
                        return;
                    case XKB_KEY_f:
                        cursor = nextChar(cursor);
                        promptChanged();
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
                    case XKB_KEY_v: paste(); return;
                    default: return; // unbound C-chords are swallowed, not typed
                }
            }

            if (ALT && !CTRL) {
                switch (SYM) {
                    case XKB_KEY_b:
                        cursor = prevWord(cursor);
                        promptChanged();
                        return;
                    case XKB_KEY_f:
                        cursor = nextWord(cursor);
                        promptChanged();
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

            // the return is the length NEEDED: a longer keysym string was cut,
            // maybe mid-codepoint — typed whole or not at all
            char      buf[64] = {};
            const int N       = xkb_state_key_get_utf8(KB->m_xkbState, KC, buf, sizeof(buf));
            if (N > 0 && (size_t)N < sizeof(buf) && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f) {
                const std::string S{buf, (size_t)N};
                if (typed.size() + S.size() <= QUERY_MAX) {
                    typed.insert(cursor, S);
                    cursor += S.size();
                    edited();
                }
            }
        }

        // The prompt strip, drawn right BELOW the bar so the bar stays visible —
        // awesome's menubar is a separate wibox at the workarea top, under the
        // wibar; it never replaced it.
        void render(const SPaint& PAINT) {
            if (!isOpen || mon.lock() != PAINT.mon)
                return;

            // one palette fetch per render: color() memoizes but still hashes per call
            const CHyprColor COLBG = color(cfg().getColor("plugin:awesome:theme:col_glass")), COLFG = color(cfg().getColor("plugin:awesome:shell:col_fg")),
                             COLFOCUS = color(cfg().getColor("plugin:awesome:shell:col_focus"));

            // the bar's own glass, a hairline where it meets the bar
            const double MY = PAINT.mb.y + PAINT.h;
            PAINT.glass(CBox{PAINT.mb.x, MY, PAINT.mb.w, PAINT.h}, COLBG);
            PAINT.rect(CBox{PAINT.mb.x, MY, PAINT.mb.w, 1.0 / PAINT.scale}, NAwesome::tHairline());

            double px = PAINT.mb.x + 8;

            // "Run:" (or the drilled-into category), quiet and bold
            static std::string PROMPTBUF; // reused; the strip renders on the main thread only
            PROMPTBUF.assign(Menubar::currentCat >= 0 ? Menubar::CATEGORIES[Menubar::currentCat].name : "Run").append(":");
            const auto LABEL = textTex(PROMPTBUF, NAwesome::tSub(), PAINT.pt, 0, "", 600);
            const double LW  = textInkW(LABEL, PAINT.pt, PAINT.scale);
            PAINT.texIn(LABEL, CBox{px, MY, LW, PAINT.h});
            px += LW + 8;

            // the query around the caret, the caret a drawn accent bar
            // between its halves. A window of it, and a raster no wider than
            // the monitor: a 4 KiB query rasterized whole was tens of
            // thousands of px, past what GL can upload
            {
                constexpr size_t BEFORE = 160, AFTER = 96; // bytes either side of the cursor
                const auto&      Q      = Menubar::typed;
                const size_t     CUR    = std::min(Menubar::cursor, Q.size());
                size_t           from   = CUR > BEFORE ? CUR - BEFORE : 0;
                size_t           to     = std::min(Q.size(), CUR + AFTER);
                while (from > 0 && ((unsigned char)Q[from] & 0xC0) == 0x80)
                    from--;
                while (to < Q.size() && ((unsigned char)Q[to] & 0xC0) == 0x80)
                    to++;
                const int MAXPX = (int)(PAINT.mb.w * PAINT.scale);
                PROMPTBUF.assign(from > 0 ? "…" : "").append(Q, from, CUR - from);
                const auto HEAD = PROMPTBUF.empty() ? nullptr : textTex(PROMPTBUF, COLFG, PAINT.pt, MAXPX);
                PROMPTBUF.assign(Q, CUR, to - CUR).append(to < Q.size() ? "…" : "");
                const auto TAIL = PROMPTBUF.empty() ? nullptr : textTex(PROMPTBUF, COLFG, PAINT.pt, MAXPX);
                const auto PUT  = [&](const SP<ITexture>& t) {
                    if (!t)
                        return;
                    const double TW = textInkW(t, PAINT.pt, PAINT.scale);
                    PAINT.texIn(t, CBox{px, MY, TW, PAINT.h});
                    px += TW;
                };
                PUT(HEAD);
                constexpr double CARET_W = 1.5, CARET_H = 15;
                PAINT.rect(CBox{px, MY + (PAINT.h - CARET_H) / 2, CARET_W, CARET_H}, COLFOCUS);
                px += CARET_W;
                PUT(TAIL);
            }
            px += 18;

            auto& SH = Menubar::shown;
            if (!SH.empty()) {
                if (Menubar::sel >= (int)SH.size())
                    Menubar::sel = (int)SH.size() - 1;
                if (Menubar::first > Menubar::sel)
                    Menubar::first = Menubar::sel;

                // by reference: the row labels are already stored strings, and
                // this runs per visible row per frame (warm AND draw) — copying
                // them out was a handful of allocations per frame for nothing.
                // Only the trailing Exec row composes, into a reused buffer.
                static const std::vector<std::string> CATNAMES = [] {
                    std::vector<std::string> v;
                    for (int i = 0; i < NCATS; i++)
                        v.emplace_back(CATEGORIES[i].name);
                    return v;
                }();
                static std::string EXECNAME;
                const auto         entryName = [&](int i) -> const std::string& {
                    if (SH[i].app >= 0)
                        return Menubar::apps[SH[i].app].name;
                    if (SH[i].cat >= 0)
                        return CATNAMES[SH[i].cat];
                    EXECNAME.assign("Exec: ").append(Menubar::typed);
                    return EXECNAME;
                };
                const auto entryIcon = [&](int i) -> SP<ITexture> {
                    if (SH[i].app >= 0)
                        return namedIcon(Menubar::apps[SH[i].app].icon);
                    if (SH[i].cat >= 0)
                        return namedIcon(Menubar::CATEGORIES[SH[i].cat].icon);
                    return namedIcon("system-run");
                };

                // entry: [icon 16][6][text], 20 apart; the cell is always
                // reserved, even when no icon resolves, so row geometry
                // stays stable and names never jump
                constexpr double ICON = ICON_CELL, ICON_GAP = 6, SPACING = 20;
                const auto       cellW = [&](const SP<ITexture>& t) { return ICON + ICON_GAP + textInkW(t, PAINT.pt, PAINT.scale); };
                // an entry never outgrows half the strip (the Exec row
                // echoes the whole query)
                const int ENTRYMAX = (int)(PAINT.mb.w * PAINT.scale / 2);
                const double RIGHT = PAINT.mb.x + PAINT.mb.w - 8;

                { // keep the selection on screen: page-jump to it when it won't fit
                    // (the first overflow decides it: End from the top would
                    // otherwise raster every entry in one warm)
                    double w = 0;
                    for (int i = Menubar::first; i <= Menubar::sel; i++) {
                        w += cellW(textTex(entryName(i), COLFG, PAINT.pt, ENTRYMAX)) + (i > Menubar::first ? SPACING : 0);
                        if (px + w > RIGHT) {
                            Menubar::first = Menubar::sel;
                            break;
                        }
                    }
                }

                for (int i = Menubar::first; i < (int)SH.size(); i++) {
                    const auto&  NAME = entryName(i);
                    const auto   WT   = textTex(NAME, COLFG, PAINT.pt, ENTRYMAX);
                    const double W    = cellW(WT);
                    if (px + W > RIGHT)
                        break;

                    // the selection is the accent text, never a fill
                    if (const auto ITEX = entryIcon(i); ITEX && ITEX->m_texID != 0)
                        PAINT.texFit(ITEX, CBox{px, MY + (PAINT.h - ICON) / 2, ICON, ICON});
                    const auto T = i == Menubar::sel ? textTex(NAME, COLFOCUS, PAINT.pt, ENTRYMAX) : WT;
                    if (T && T->m_texID != 0) {
                        const auto P = PAINT.toPhys(CBox{px + ICON + ICON_GAP, MY, 1, PAINT.h});
                        CBox       b{P.x - textPad(PAINT.pt), P.y + (P.h - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y};
                        PAINT.texAt(T, b.round());
                    }
                    px += W + SPACING;
                }
            }
        }
    }

    namespace Menubar {
        void init() {
            if (!g_pEventLoopManager || !g_pCompositor || !completionIndex.init(g_pCompositor->m_wlEventLoop))
                return;
            ++activationGeneration;
            indexPoll = makeShared<CEventLoopTimer>(
                std::nullopt,
                [](SP<CEventLoopTimer> self, void*) {
                    pollCompletion();
                    self->updateTimeout(compScanning ? std::optional{std::chrono::milliseconds(16)} : std::nullopt);
                },
                nullptr);
            g_pEventLoopManager->addTimer(indexPoll);
            // the index changed (its first scan's batches, a refresh landing):
            // an open launcher lists it now, a closed one at its next open
            NAwesome::CDesktopIndex::inst().subscribe([]() {
                // the first scan's batches land once, at its end: rebuilding
                // and repainting the whole list per 32-file batch made a
                // launcher opened at login quadratic in the entries
                if (!isOpen || NAwesome::CDesktopIndex::inst().scanning())
                    return;
                buildApps();
                refilter();
                restoreSelection();
                promptChanged();
            });
            repeat.init(onRepeat);
        }

        // hl.plugin.awesome.menubar(), deferred out of the Lua call
        void toggleDeferred() {
            pendingOpen.arm([]() {
                if (isOpen)
                    close();
                else
                    open();
            });
        }

        void exit() {
            ++activationGeneration;
            close();
            clipboard.cancel();
            if (indexPoll && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(indexPoll);
            indexPoll.reset();
            repeat.teardown();
            completionIndex.exit();
            apps.clear();
            shown.clear(); // its entries index into apps
            desktops.clear();
            pendingSelection.reset();
            appsBuilt    = false;
            compScanning = false;
            launchCounts.clear();
            history.entries.clear();
            filesLoaded = false;
            compList.clear();
            compPrefix.clear();
            pendingExec.reset();
            pendingOpen.reset();
        }
    } // namespace Menubar

} // namespace NAwesome::Shell
