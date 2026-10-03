// awesome/test/test_store.cpp — admission, unified round-trip, hostility,
// and the one-time state migration. Temp state lives under
// /tmp/hypr-awesome-test.
#include "../core/state.hpp"

#include <filesystem>
#include <cstdlib>
#include <fstream>

#include "harness.hpp"

using namespace NAwesome;
namespace fs = std::filesystem;

static const fs::path TDIR = fs::path{"/tmp/hypr-awesome-test"};

bool test_store() {
    std::error_code ec;
    fs::remove_all(TDIR, ec);
    fs::create_directories(TDIR, ec);

    // ---- BoxStore admission ----
    {
        BoxStore S;
        AW_CHECK(S.remember("org.app.Main", {10, 20, 640, 400}));
        AW_CHECK(!S.remember("org.app.Main", {10, 20, 640, 400})); // unchanged
        AW_CHECK(S.remember("org.app.Main", {10, 20, 650, 410}));  // changed
        AW_CHECK(!S.remember("bad\tclass", {1, 1, 1, 1}));        // tab in class
        AW_CHECK(!S.remember("", {1, 1, 1, 1}));                  // empty
        AW_CHECK(!S.remember("x", {0, 0, -1, 1}));                // negative w
        AW_CHECK(S.rows.size() == 1);
        AW_CHECK((S.find("org.app.Main") != nullptr && *S.find("org.app.Main") == Box{10, 20, 650, 410}));
    }

    // ---- BoxStore read hostility ----
    {
        const auto P = TDIR / "hostile.tsv";
        {
            std::ofstream f(P, std::ios::binary);
            // valid row, class starting with a digit
            f << "1\t2\t3\t4\t0ad\n";
            // legacy position-only form (2 numbers)
            f << "5\t6\tlegacy.class\n";
            // NUL in the row: skipped
            f << "1\t2\t3\t4\tnul\0class\n";
            // five numbers: not a box row
            f << "1\t2\t3\t4\t5\ttoo.many\n";
            // no class
            f << "1\t2\t3\t4\n";
            // non-numeric garbage
            f << "x\ty\tz\tw\tbad\n";
        }
        const auto R = BoxStore::read(P);
        AW_CHECK(R.rows.size() == 2);
        AW_CHECK((R.find("0ad") != nullptr && *R.find("0ad") == Box{1, 2, 3, 4}));
        AW_CHECK((R.find("legacy.class") != nullptr && *R.find("legacy.class") == Box{5, 6, 0, 0}));

        // oversized file: rejected as a unit, not prefix-retained
        const auto O = TDIR / "oversized.tsv";
        {
            std::ofstream f(O, std::ios::binary);
            f << std::string(MAX_STORE_FILE_BYTES + 1, 'x');
        }
        AW_CHECK(BoxStore::read(O).rows.empty());
    }

    // ---- entry cap ----
    {
        BoxStore S;
        for (size_t i = 0; i < MAX_STORE_ENTRIES; i++)
            AW_CHECK(S.remember("k" + std::to_string(i), {(int)i, 0, 1, 1}));
        AW_CHECK(!S.remember("overflow", {0, 0, 1, 1}));
        // an existing key still updates at the cap
        AW_CHECK(S.remember("k0", {0, 0, 2, 2}));
    }

    // ---- ListStore: dedup, recency, caps ----
    {
        ListStore L;
        L.remember("b");
        L.remember("a");
        L.remember("b"); // re-run: moves to most recent
        AW_CHECK(L.entries.size() == 2 && L.entries[0] == "a" && L.entries[1] == "b");
        for (int i = 0; i < 60; i++)
            L.remember(std::string{"q"} + std::to_string(i));
        AW_CHECK(L.entries.size() == MAX_STORE_LIST_ENTRIES);
        AW_CHECK(L.entries.front() == "q10"); // the ten oldest were evicted
        // oversized entry: clipped at a codepoint boundary
        L.entries.clear();
        L.remember(std::string(MAX_STORE_STRING_BYTES + 3, '\xF0') + "ab");
        AW_CHECK(L.entries.size() == 1);
        AW_CHECK(L.entries[0].size() <= MAX_STORE_STRING_BYTES);
        // the tail "ab" lands the clip on a 4-byte boundary: 512/4 = 128 full
        // sequences, so the clip keeps 128 of them and drops the rest
        AW_CHECK(L.entries[0].size() == 4 * (MAX_STORE_STRING_BYTES / 4));
        AW_CHECK(!L.remember(""));
    }

    // ---- CountStore: bump + hostile read rows ----
    {
        CountStore C;
        AW_CHECK(C.bump("Firefox"));
        AW_CHECK(C.bump("Firefox"));
        AW_CHECK(!C.bump("bad\tname"));
        AW_CHECK(!C.bump(""));
        AW_CHECK(C.counts.size() == 1 && C.counts.at("Firefox") == 2);
        // a name with a semicolon: only the trailing number is the count
        const auto H = TDIR / "semi.tsv";
        {
            std::ofstream f(H, std::ios::binary);
            f << "org;app;7\n";    // name "org;app", count 7
            f << "no.count\n";     // no separator: skipped
            f << ";3\n";           // empty name: skipped
            f << "neg;-1\n";       // negative: skipped
            f << "huge;999999999\n"; // over the cap: skipped
            f << "ok;5\n";
        }
        const auto R2 = CountStore::read(H);
        AW_CHECK(R2.counts.size() == 2);
        AW_CHECK((R2.counts.count("org;app") && R2.counts.at("org;app") == 7));
        AW_CHECK((R2.counts.count("ok") && R2.counts.at("ok") == 5));
    }

    // ---- unified state.tsv: round-trip ----
    {
        AppState D;
        D.spot.remember("foot", {100, 100, 500, 400});
        D.spot.remember("0ad", {5, 6, 320, 200});
        D.windowed.remember("firefox", {303, 120, 1235, 745});
        D.launches.counts["Firefox"] = 7;
        D.launches.counts["org;app"] = 3;
        D.history.remember("first");
        D.history.remember("second query");

        const auto P = TDIR / "state-roundtrip.tsv";
        AW_CHECK(StateStore::writeUnified(P, D));
        AppState R;
        AW_CHECK(StateStore::readUnified(P, R));
        AW_CHECK(R.spot.rows == D.spot.rows);
        AW_CHECK(R.windowed.rows == D.windowed.rows);
        AW_CHECK(R.launches.counts == D.launches.counts);
        AW_CHECK(R.history.entries == D.history.entries);

        // absent file: false, out untouched
        AppState E;
        AW_CHECK(!StateStore::readUnified(TDIR / "nope-state.tsv", E));
        AW_CHECK(E.spot.rows.empty() && E.history.entries.empty());
    }

    // ---- unified state.tsv: hostility ----
    {
        const auto P = TDIR / "state-hostile.tsv";
        {
            std::ofstream f(P, std::ios::binary);
            f << "spot\t1\t2\t3\t4\tgood\n";
            f << "boguskind\t1\t2\t3\t4\tunknown\n"; // unknown kind: skipped
            f << "windowed\tx\ty\tz\tw\tbad\n";      // non-numeric: skipped
            f << "spot\t1\t2\t3\t4\n";               // no class: skipped
            f << "launches\tok;5\n";
            f << "launches\tno.count\n";             // no separator: skipped
            f << "history\tquery one\n";
            f << "history\tquery\twith\ttabs\n";     // tabs inside survive
            f << "no-tab-line\n";                    // no kind: skipped
            f << "spot\tnan\t2\t3\t4\tnanfoot\n";   // non-finite: skipped
        }
        AppState R;
        AW_CHECK(StateStore::readUnified(P, R));
        AW_CHECK(R.spot.rows.size() == 1 && R.spot.find("good") != nullptr);
        AW_CHECK(R.windowed.rows.empty());
        AW_CHECK(R.launches.counts.size() == 1 && R.launches.counts.at("ok") == 5);
        AW_CHECK(R.history.entries.size() == 2);
        AW_CHECK(R.history.entries[1] == "query\twith\ttabs");

        // per-kind caps: 1100 spot rows keep the first 1024; 80 history
        // rows keep the MOST RECENT 50
        const auto C = TDIR / "state-caps.tsv";
        {
            std::ofstream f(C, std::ios::binary);
            for (int i = 0; i < 1100; i++)
                f << "spot\t" << i << "\t0\t1\t1\tk" << i << "\n";
            for (int i = 0; i < 80; i++)
                f << "history\tq" << i << "\n";
        }
        AppState R2;
        AW_CHECK(StateStore::readUnified(C, R2));
        AW_CHECK(R2.spot.rows.size() == MAX_STORE_ENTRIES);
        AW_CHECK(R2.spot.contains("k0") && R2.spot.contains("k1023") && !R2.spot.contains("k1024"));
        AW_CHECK(R2.history.entries.size() == MAX_STORE_LIST_ENTRIES);
        AW_CHECK(R2.history.entries.front() == "q30" && R2.history.entries.back() == "q79");
    }

    // ---- the one-time migration (StateStore::load; runs once per process) ----
    {
        char* ORIG_STATE = std::getenv("XDG_STATE_HOME");
        char* ORIG_CACHE = std::getenv("XDG_CACHE_HOME");
        setenv("XDG_STATE_HOME", TDIR.c_str(), 1);
        setenv("XDG_CACHE_HOME", TDIR.c_str(), 1);

        const auto DIR  = stateDir();
        const auto OLD  = legacyStateDir();
        const auto SPOT = stateBase() / "hyprplace" / "lastspot.tsv";
        const auto WIN  = stateBase() / "hyprmax" / "windowed.tsv";
        const auto CNT  = cacheBase() / "hyprbar" / "menu_count_file";
        const auto HIST = cacheBase() / "hyprbar" / "history_menu";
        fs::create_directories(DIR, ec);
        fs::create_directories(OLD, ec);
        fs::create_directories(SPOT.parent_path(), ec);
        fs::create_directories(WIN.parent_path(), ec);
        fs::create_directories(CNT.parent_path(), ec);

        auto writeRow = [](const fs::path& P, std::string content) {
            std::ofstream f(P, std::ios::trunc);
            f << std::move(content);
            return (bool)f;
        };
        // current layout (newest): wins key-by-key
        AW_CHECK(writeRow(DIR / "windows-spot.tsv", "1\t2\t3\t4\tfoot\n5\t6\t320\t240\tcurrent.only\n"));
        AW_CHECK(writeRow(DIR / "windows-windowed.tsv", "10\t20\t800\t600\tcurff\n"));
        AW_CHECK(writeRow(DIR / "shell-launches.tsv", "Current;2\n"));
        AW_CHECK(writeRow(DIR / "shell-history.tsv", "current query\n"));
        // pre-rename layout: fills missing keys only
        AW_CHECK(writeRow(OLD / "windows-spot.tsv", "1\t2\t3\t4\tfoot\n9\t9\t100\t100\told.only\n"));
        AW_CHECK(writeRow(OLD / "windows-windowed.tsv", "30\t40\t900\t700\toldff\n"));
        AW_CHECK(writeRow(OLD / "shell-launches.tsv", "Old;9\n"));
        // the current history is live: the old one must not mix in
        AW_CHECK(writeRow(OLD / "shell-history.tsv", "stale query\n"));
        // the ancient per-module stores: last resort
        AW_CHECK(writeRow(SPOT, "7\t8\t4\t5\tancient.spot\n"));
        AW_CHECK(writeRow(WIN, "11\t22\t333\t444\tancient.win\n"));
        AW_CHECK(writeRow(CNT, "Ancient;1\n"));
        AW_CHECK(writeRow(HIST, "unused history\n"));

        StateStore::inst().load();
        const auto P = StateStore::path();
        AW_CHECK(fs::is_regular_file(P));
        auto& D = StateStore::inst().data();
        // current wins, pre-rename fills, ancient fills
        AW_CHECK((D.spot.find("foot") != nullptr && *D.spot.find("foot") == Box{1, 2, 3, 4}));
        AW_CHECK((D.spot.find("current.only") != nullptr && D.spot.find("old.only") != nullptr));
        AW_CHECK((D.spot.find("ancient.spot") != nullptr && *D.spot.find("ancient.spot") == Box{7, 8, 4, 5}));
        AW_CHECK(D.windowed.rows.size() == 3);
        AW_CHECK((D.windowed.find("ancient.win") != nullptr && *D.windowed.find("ancient.win") == Box{11, 22, 333, 444}));
        AW_CHECK(D.launches.counts.at("Current") == 2 && D.launches.counts.at("Old") == 9 && D.launches.counts.at("Ancient") == 1);
        AW_CHECK(D.history.entries.size() == 1 && D.history.entries[0] == "current query");

        // the sources are CONSUMED: the migration is one-time
        AW_CHECK(!fs::exists(DIR / "windows-spot.tsv") && !fs::exists(DIR / "windows-windowed.tsv") &&
                 !fs::exists(DIR / "shell-launches.tsv") && !fs::exists(DIR / "shell-history.tsv"));
        AW_CHECK(!fs::exists(OLD / "windows-spot.tsv") && !fs::exists(OLD)); // emptied dir removed
        AW_CHECK(!fs::exists(SPOT) && !fs::exists(stateBase() / "hyprplace"));
        AW_CHECK(!fs::exists(WIN) && !fs::exists(stateBase() / "hyprmax"));
        AW_CHECK(!fs::exists(CNT) && !fs::exists(HIST) && !fs::exists(cacheBase() / "hyprbar"));

        // a dirty() then flush() writes the unified file and it round-trips
        D.spot.remember("foot", {1, 2, 999, 999});
        StateStore::inst().dirty(); // arms the coalesced hop (no-op headless)
        StateStore::inst().flush(); // writes immediately
        AppState R;
        AW_CHECK(StateStore::readUnified(P, R));
        AW_CHECK((R.spot.find("foot") != nullptr && R.spot.find("foot")->h == 999));

        // idempotent: re-arming the legacy sources does nothing to the
        // live state file (load already ran)
        fs::create_directories(OLD, ec);
        AW_CHECK(writeRow(OLD / "windows-spot.tsv", "1\t2\t3\t4\tresurrected\n"));
        StateStore::inst().load();
        AW_CHECK(!StateStore::inst().data().spot.contains("resurrected"));

        if (ORIG_STATE)
            setenv("XDG_STATE_HOME", ORIG_STATE, 1);
        else
            unsetenv("XDG_STATE_HOME");
        if (ORIG_CACHE)
            setenv("XDG_CACHE_HOME", ORIG_CACHE, 1);
        else
            unsetenv("XDG_CACHE_HOME");
    }

    fs::remove_all(TDIR, ec);
    return true;
}
