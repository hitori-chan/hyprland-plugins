// awesome/test/test_store.cpp — admission, round-trip, hostility,
// migration. Temp state lives under /tmp/hypr-awesome-test.
#include "../core/store.hpp"

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

    // ---- BoxStore round-trip ----
    {
        BoxStore S;
        AW_CHECK(S.remember("org.app.Main", {10, 20, 640, 400}));
        AW_CHECK(!S.remember("org.app.Main", {10, 20, 640, 400})); // unchanged
        AW_CHECK(S.remember("org.app.Main", {10, 20, 650, 410}));  // changed
        AW_CHECK(!S.remember("bad\tclass", {1, 1, 1, 1}));        // tab in class
        AW_CHECK(!S.remember("", {1, 1, 1, 1}));                  // empty
        AW_CHECK(!S.remember("x", {0, 0, -1, 1}));                // negative w
        const auto P = TDIR / "box.tsv";
        AW_CHECK(S.write(P));
        const auto R = BoxStore::read(P);
        AW_CHECK(R.rows.size() == 1);
        AW_CHECK((R.find("org.app.Main") != nullptr && *R.find("org.app.Main") == Box{10, 20, 650, 410}));
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

    // ---- ListStore: dedup, recency, caps, round-trip ----
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
        const auto P = TDIR / "list.tsv";
        AW_CHECK(L.write(P));
        const auto R = ListStore::read(P);
        AW_CHECK(R.entries == L.entries);
    }

    // ---- CountStore: bump, round-trip, hostile rows ----
    {
        CountStore C;
        AW_CHECK(C.bump("Firefox"));
        AW_CHECK(C.bump("Firefox"));
        AW_CHECK(!C.bump("bad\tname"));
        AW_CHECK(!C.bump(""));
        const auto P = TDIR / "counts.tsv";
        AW_CHECK(C.write(P));
        const auto R = CountStore::read(P);
        AW_CHECK(R.counts.size() == 1 && R.counts.at("Firefox") == 2);
        // a name with a semicolon: only the trailing number is the count
        const auto H = TDIR / "semi.tsv";
        {
            std::ofstream f(H, std::ios::binary);
            f << "org;app;7\n";   // name "org;app", count 7
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

    // ---- migration: legacy read once, fresh wins once live ----
    {
        const auto LEG = TDIR / "legacy-box.tsv";
        const auto FRESH = TDIR / "windows.tsv";
        AW_CHECK(BoxStore{}.write(LEG));
        BoxStore L;
        L.remember("org.old.App", {1, 2, 320, 200});
        AW_CHECK(L.write(LEG));
        AW_CHECK(migrateBoxStore(FRESH, LEG));
        const auto M = BoxStore::read(FRESH);
        AW_CHECK((M.find("org.old.App") != nullptr && *M.find("org.old.App") == Box{1, 2, 320, 200}));
        // fresh is now live: a different legacy must not clobber it
        BoxStore L2;
        L2.remember("org.other.App", {9, 9, 100, 100});
        AW_CHECK(L2.write(LEG));
        AW_CHECK(migrateBoxStore(FRESH, LEG));
        const auto M2 = BoxStore::read(FRESH);
        AW_CHECK(!M2.contains("org.other.App") && M2.contains("org.old.App"));
        // no legacy file: a no-op, fresh left absent
        AW_CHECK(migrateBoxStore(TDIR / "absent.tsv", TDIR / "nope.tsv"));
        AW_CHECK(!fs::exists(TDIR / "absent.tsv"));
    }

    // ---- state-dir rename: old "awesome" dir -> "hyprland/plugin/awesome"
    {
        char* ORIG = std::getenv("XDG_STATE_HOME");
        setenv("XDG_STATE_HOME", TDIR.c_str(), 1);
        const auto OLD = TDIR / "awesome";
        const auto NEW = TDIR / "hyprland" / "plugin" / "awesome";
        fs::create_directories(OLD, ec);
        BoxStore B;
        B.remember("org.app.Main", {5, 6, 320, 240});
        AW_CHECK(B.write(OLD / "windows-spot.tsv"));
        AW_CHECK(migrateStateDirRename());
        const auto M = BoxStore::read(NEW / "windows-spot.tsv");
        AW_CHECK((M.find("org.app.Main") != nullptr && *M.find("org.app.Main") == Box{5, 6, 320, 240}));
        // idempotent: a second run must not touch the live fresh store
        BoxStore B2;
        B2.remember("org.stale.App", {1, 1, 50, 50});
        AW_CHECK(B2.write(OLD / "windows-spot.tsv"));
        AW_CHECK(migrateStateDirRename());
        const auto M2 = BoxStore::read(NEW / "windows-spot.tsv");
        AW_CHECK(M2.contains("org.app.Main") && !M2.contains("org.stale.App"));
        // no legacy dir at all: a clean no-op
        fs::remove_all(OLD, ec);
        AW_CHECK(migrateStateDirRename());
        const auto M3 = BoxStore::read(NEW / "windows-spot.tsv");
        AW_CHECK(M3.rows.size() == M2.rows.size() && M3.contains("org.app.Main") && !M3.contains("org.stale.App"));
        if (ORIG)
            setenv("XDG_STATE_HOME", ORIG, 1);
        else
            unsetenv("XDG_STATE_HOME");
    }

    fs::remove_all(TDIR, ec);
    return true;
}
