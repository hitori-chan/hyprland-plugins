// awesome/test/test_store.cpp — admission, recency bounds, the state
// file's round-trip, hostility, and the load's write block. Temp state lives under /tmp/hypr-awesome-test.
#include "../core/state.hpp"

#include <filesystem>
#include <cstdlib>
#include <fstream>

#include "harness.hpp"

using namespace NAwesome;
namespace fs = std::filesystem;

static const fs::path TDIR = fs::path{"/tmp/hypr-awesome-test"};

static bool writeFile(const fs::path& p, const std::string& contents) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
    return (bool)f;
}

bool test_store() {
    std::error_code ec;
    fs::remove_all(TDIR, ec);
    fs::create_directories(TDIR, ec);

    // ---- keyed admission ----
    {
        CRecentMap<Box> S;
        AW_CHECK(S.put("org.app.Main", {10, 20, 640, 400}));
        AW_CHECK(!S.put("org.app.Main", {10, 20, 640, 400})); // unchanged, already newest
        AW_CHECK(S.put("org.app.Main", {10, 20, 650, 410}));  // changed
        AW_CHECK(!S.put("bad\tclass", {1, 1, 1, 1}));         // tab in the key
        AW_CHECK(!S.put("", {1, 1, 1, 1}));                   // empty
        AW_CHECK(!S.put(std::string(MAX_STORE_KEY_BYTES + 1, 'k'), {1, 1, 1, 1}));
        AW_CHECK(!S.put("x", {0, 0, -1, 1}));                 // negative w
        AW_CHECK(S.size() == 1);
        AW_CHECK((S.find("org.app.Main") != nullptr && *S.find("org.app.Main") == Box{10, 20, 650, 410}));
        // an unchanged value that is not the newest still moves to newest
        AW_CHECK(S.put("other", {1, 1, 1, 1}));
        AW_CHECK(S.put("org.app.Main", {10, 20, 650, 410}));
        AW_CHECK(S.begin()->first == "other");
    }

    // ---- the bound evicts the least recently remembered ----
    {
        CRecentMap<Box> S;
        for (size_t i = 0; i < MAX_STORE_ENTRIES; i++)
            AW_CHECK(S.put("k" + std::to_string(i), {(int)i, 0, 1, 1}));
        AW_CHECK(S.put("k0", {0, 0, 2, 2})); // k0 is now the newest: k1 is the oldest
        AW_CHECK(S.put("overflow", {0, 0, 1, 1}));
        AW_CHECK(S.size() == MAX_STORE_ENTRIES);
        AW_CHECK(S.contains("overflow") && S.contains("k0") && !S.contains("k1") && S.contains("k2"));
        S.eraseIf([](const std::string& k, const Box&) { return k == "k2"; });
        AW_CHECK(!S.contains("k2") && S.size() == MAX_STORE_ENTRIES - 1);
        // a move keeps the index valid
        CRecentMap<Box> M = std::move(S);
        AW_CHECK(M.contains("overflow") && M.put("k3", {9, 9, 9, 9}) && M.find("k3")->x == 9);
    }

    // ---- ListStore: dedup, recency, caps ----
    {
        ListStore L;
        L.remember("b");
        L.remember("a");
        AW_CHECK(L.remember("b")); // re-run: moves to most recent
        AW_CHECK(!L.remember("b")); // already the most recent
        AW_CHECK(L.entries.size() == 2 && L.entries[0] == "a" && L.entries[1] == "b");
        AW_CHECK(!L.remember("two\nlines") && !L.remember("cr\r") && !L.remember(""));
        for (int i = 0; i < 60; i++)
            L.remember(std::string{"q"} + std::to_string(i));
        AW_CHECK(L.entries.size() == MAX_STORE_LIST_ENTRIES);
        AW_CHECK(L.entries.front() == "q10"); // the ten oldest were evicted
        // oversized entry: clipped at a codepoint boundary
        L.entries.clear();
        L.remember(std::string(MAX_STORE_STRING_BYTES + 3, '\xF0') + "ab");
        AW_CHECK(L.entries.size() == 1);
        AW_CHECK(L.entries[0].size() == 4 * (MAX_STORE_STRING_BYTES / 4));
    }

    // ---- launch counts ----
    {
        CRecentMap<int> C;
        AW_CHECK(bumpCount(C, "Firefox"));
        AW_CHECK(bumpCount(C, "Firefox"));
        AW_CHECK(!bumpCount(C, "bad\tname"));
        AW_CHECK(!bumpCount(C, ""));
        AW_CHECK(C.size() == 1 && *C.find("Firefox") == 2);
        AW_CHECK(C.put("capped", MAX_LAUNCH_COUNT) && !C.put("over", MAX_LAUNCH_COUNT + 1));
        AW_CHECK(bumpCount(C, "Firefox") && bumpCount(C, "capped") && *C.find("capped") == MAX_LAUNCH_COUNT);
    }

    // ---- round-trip, order included ----
    {
        AppState D;
        D.spot.put("foot", {100, 100, 0, 0});
        D.spot.put("0ad", {-5, 6, 0, 0}); // a class starting with a digit
        D.windowed.put("firefox", {303, 120, 1235, 745});
        D.launches.put("Firefox", 7);
        D.launches.put("org;app", 3);
        D.history.remember("first");
        D.history.remember("second\tquery");

        const auto P = TDIR / "state-roundtrip.tsv";
        AW_CHECK(StateStore::write(P, D));
        AppState R;
        AW_CHECK(StateStore::read(P, R));
        AW_CHECK(R.spot == D.spot && R.windowed == D.windowed && R.launches == D.launches);
        AW_CHECK(R.history.entries == D.history.entries);

        AppState E;
        AW_CHECK(!StateStore::read(TDIR / "nope-state.tsv", E));
        AW_CHECK(E.spot.empty() && E.history.entries.empty());
    }

    // ---- hostility ----
    {
        const auto P = TDIR / "state-hostile.tsv";
        AW_CHECK(writeFile(P, std::string{"spot\t1\t2\tgood\n"
                                          "boguskind\t1\t2\tunknown\n"
                                          "windowed\tx\ty\tz\tw\tbad\n"   // non-numeric
                                          "windowed\t1\t2\t3\t4\n"        // no class
                                          "spot\t1\t\t2\tgap\n"           // an empty field is not skipped
                                          "spot\tnan\t2\tnanfoot\n"       // non-finite
                                          "spot\t1\t \n"                  // a short row must not borrow
                                          "45\tnext\n"                    // the next row's number
                                          "launches\t5\tok\n"
                                          "launches\tno.count\n"
                                          "launches\t99999999\thuge\n"
                                          "history\tquery one\n"
                                          "history\tquery\twith\ttabs\n"
                                          "no-tab-line\n"
                                          "spot\t1\t2\tnul"} + std::string(1, '\0') + "class\n"));
        AppState R;
        AW_CHECK(StateStore::read(P, R));
        AW_CHECK(R.spot.size() == 1 && R.spot.contains("good"));
        AW_CHECK(R.windowed.empty());
        AW_CHECK(R.launches.size() == 1 && *R.launches.find("ok") == 5);
        AW_CHECK(R.history.entries.size() == 2 && R.history.entries[1] == "query\twith\ttabs");

        // over the bound a read keeps the MOST RECENT rows (the file's tail)
        std::string caps;
        for (int i = 0; i < 1100; i++)
            caps += "spot\t" + std::to_string(i) + "\t0\tk" + std::to_string(i) + "\n";
        for (int i = 0; i < 80; i++)
            caps += "history\tq" + std::to_string(i) + "\n";
        const auto C = TDIR / "state-caps.tsv";
        AW_CHECK(writeFile(C, caps));
        AppState R2;
        AW_CHECK(StateStore::read(C, R2));
        AW_CHECK(R2.spot.size() == MAX_STORE_ENTRIES);
        AW_CHECK(!R2.spot.contains("k75") && R2.spot.contains("k76") && R2.spot.contains("k1099"));
        AW_CHECK(R2.history.entries.size() == MAX_STORE_LIST_ENTRIES);
        AW_CHECK(R2.history.entries.front() == "q30" && R2.history.entries.back() == "q79");

        // oversized: rejected as a unit, not prefix-retained
        const auto O = TDIR / "state-oversized.tsv";
        AW_CHECK(writeFile(O, "spot\t1\t2\tfoot\n" + std::string(MAX_STATE_FILE_BYTES, 'x')));
        AppState R3;
        AW_CHECK(!StateStore::read(O, R3) && R3.spot.empty());
    }

    // ---- load (runs once per process): an unreadable file blocks writes ----
    {
        char* ORIG_STATE = std::getenv("XDG_STATE_HOME");
        setenv("XDG_STATE_HOME", TDIR.c_str(), 1);
        const auto P = StateStore::path();
        fs::create_directories(P.parent_path(), ec);
        AW_CHECK(writeFile(P, "spot\t1\t2\t3\t4\tfoot\n" + std::string(MAX_STATE_FILE_BYTES, 'x')));
        const auto SIZE = fs::file_size(P, ec);
        StateStore::inst().load();
        AW_CHECK(StateStore::inst().writeBlocked());
        AW_CHECK(StateStore::inst().data().spot.empty());
        StateStore::inst().data().spot.put("fresh", {1, 2, 0, 0});
        StateStore::inst().dirty(); // arms the coalesced hop (no-op headless)
        StateStore::inst().flush();
        AW_CHECK(fs::file_size(P, ec) == SIZE); // the user's file stands
        AW_CHECK(!fs::exists(P.string() + ".tmp"));

        if (ORIG_STATE)
            setenv("XDG_STATE_HOME", ORIG_STATE, 1);
        else
            unsetenv("XDG_STATE_HOME");
    }

    fs::remove_all(TDIR, ec);
    return true;
}
