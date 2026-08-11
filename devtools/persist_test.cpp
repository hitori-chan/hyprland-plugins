#include "common/persist.hpp"
#include "test.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/stat.h>

namespace {

    struct STempDir {
        std::filesystem::path path;
        STempDir() {
            const auto STAMP = std::chrono::steady_clock::now().time_since_epoch().count();
            path             = std::filesystem::temp_directory_path() / ("hypr-persist-test-" + std::to_string(STAMP));
            std::filesystem::create_directory(path);
        }
        ~STempDir() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    void write(const std::filesystem::path& path, std::string_view contents) {
        std::ofstream file{path, std::ios::binary | std::ios::trunc};
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

} // namespace

int main() {
    using namespace NHyprCommon;
    NHyprTest::CSuite suite{"persist_test"};

    STempDir          temp;
    const auto        state = temp.path / "state.tsv";

    write(state, "1\t2\t300\t200\tapp one\n9\t8\tlegacy app\n3\t4\t320\t240\tapp one\n0\t0\t-1\t2\tbad\n");
    auto store = readBoxTsv(state);
    suite.expect(store.size() == 2, "ordinary and legacy rows admitted");
    suite.expect(store.at("app one") == CBox{3, 4, 320, 240}, "later duplicate replaces earlier row");
    suite.expect(store.at("legacy app") == CBox{9, 8, 0, 0}, "legacy position row retained");

    std::string nulRow = "1\t2\t3\t4\ttruncated";
    nulRow.push_back('\0');
    nulRow += "suffix\n5\t6\t7\t8\tafter-nul\n";
    write(state, nulRow);
    store = readBoxTsv(state);
    suite.expect(store.size() == 1 && store.contains("after-nul"), "embedded NUL row rejected without truncation");

    const std::string maxClass(MAX_BOX_STORE_CLASS_BYTES, 'a');
    const std::string longClass(MAX_BOX_STORE_CLASS_BYTES + 1, 'b');
    write(state, "1\t2\t3\t4\t" + maxClass + "\n1\t2\t3\t4\t" + longClass + "\n");
    store = readBoxTsv(state);
    suite.expect(store.size() == 1 && store.contains(maxClass), "class key boundary enforced");

    std::string overlong = "1\t2\t3\t4\t" + std::string(MAX_BOX_STORE_LINE_BYTES, 'x') + "\n5\t6\t7\t8\tafter\n";
    write(state, overlong);
    store = readBoxTsv(state);
    suite.expect(store.size() == 1 && store.contains("after"), "overlong row skipped without hiding following row");

    std::string tooManyRows;
    for (size_t i = 0; i < MAX_BOX_STORE_ROWS; ++i)
        tooManyRows += "1\t2\t3\t4\tduplicate\n";
    tooManyRows += "1\t2\t3\t4\tafter-row-limit\n";
    write(state, tooManyRows);
    store = readBoxTsv(state);
    suite.expect(store.size() == 1 && !store.contains("after-row-limit"), "row scan limit enforced");

    std::string tooManyEntries;
    for (size_t i = 0; i <= MAX_BOX_STORE_ENTRIES; ++i)
        tooManyEntries += "1\t2\t3\t4\tapp-" + std::to_string(i) + "\n";
    write(state, tooManyEntries);
    store = readBoxTsv(state);
    suite.expect(store.size() == MAX_BOX_STORE_ENTRIES && !store.contains("app-" + std::to_string(MAX_BOX_STORE_ENTRIES)), "retained-entry limit enforced");

    write(state, std::string(MAX_BOX_STORE_FILE_BYTES + 1, 'x'));
    suite.expect(readBoxTsv(state).empty(), "oversized file rejected as a unit");

    const auto fifo = temp.path / "state.fifo";
    suite.expect(::mkfifo(fifo.c_str(), 0600) == 0, "FIFO fixture created");
    suite.expect(readBoxTsv(fifo).empty(), "non-regular state rejected without opening");

    SBoxStore runtime;
    suite.expect(rememberBox(runtime, "app", CBox{1, 2, 3, 4}), "runtime insertion admitted");
    suite.expect(!rememberBox(runtime, "app", CBox{1, 2, 3, 4}), "unchanged runtime row ignored");
    suite.expect(rememberBox(runtime, "app", CBox{2, 3, 4, 5}), "runtime replacement admitted");
    suite.expect(!rememberBox(runtime, longClass, CBox{1, 2, 3, 4}), "oversized runtime class rejected");

    return suite.finish();
}
