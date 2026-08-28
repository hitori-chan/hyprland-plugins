// Pins CAsyncFileIndex's symlink contract: packaged .desktop symlinks
// (LibreOffice ships this way) must yield FULL target contents — a link-length
// truncation parses as nothing and silently drops the app. Dangling links,
// oversized files, and NUL-bearing files are skipped, not framed.
#include "test.hpp"
#include "../common/fileindex.hpp"

#include <filesystem>
#include <fstream>
#include <stdlib.h>
#include <unistd.h>

using namespace NHyprCommon;

namespace {

    std::string bodyText(int pad) {
        // long enough that a link-length truncation cannot pass any check here
        std::string out = "[Desktop Entry]\nType=Application\nName=Symlinked\nExec=true\nIcon=icon\n#";
        return out + std::string(pad, '=') + "\n";
    }

    void put(const std::filesystem::path& path, const std::string& text) {
        std::ofstream F(path);
        F << text;
    }

} // namespace

int main() {
    NHyprTest::CSuite suite("fileindex-test");

    char dir[] = "/tmp/hypr-fileindex-test.XXXXXX";
    const char* made = mkdtemp(dir);
    if (!made) {
        std::cerr << "FAIL: mkdtemp\n";
        return EXIT_FAILURE;
    }
    const std::filesystem::path root = made;
    const int LINK_BYTES = 40; // shorter than the bodies below, like libreoffice-writer.desktop -> ...xdg/writer.desktop

    const auto REAL = root / "real.desktop";
    put(REAL, bodyText(200));

    const auto TARGET = root / "target-writer.desktop"; // name differs from the link: id stays the link-relative one
    const std::string BODY = bodyText(600);
    put(TARGET, BODY);
    const auto LINKED = root / "linked.desktop";
    HYPR_EXPECT(suite, symlink(TARGET.c_str(), LINKED.c_str()) == 0);

    const auto DANGLING = root / "dangling.desktop";
    HYPR_EXPECT(suite, symlink((root / "missing.desktop").c_str(), DANGLING.c_str()) == 0);

    put(root / "oversize.desktop", std::string(64 * 1024, 'x')); // above the request cap below

    { // a NUL byte must not reach a frame: bash strings cannot carry it
        std::ofstream B(root / "binary.desktop", std::ios::binary);
        B << std::string(100, 'a') << '\0' << std::string(100, 'b');
    }

    wl_event_loop* loop = wl_event_loop_create();
    HYPR_EXPECT(suite, loop != nullptr);

    CAsyncFileIndex index;
    HYPR_EXPECT(suite, index.init(loop));

    CAsyncFileIndex::SRequest request;
    request.generation   = 7;
    request.extensions   = {".desktop"};
    request.maxEntries   = 64;
    request.maxVisited   = 256;
    request.maxFileBytes = 16 * 1024; // the cap oversize.desktop must exceed
    request.recursive    = false;
    request.roots.push_back(root);

    const uint64_t GENERATION = request.generation;
    index.request(std::move(request));
    std::vector<CAsyncFileIndex::SEntry> entries;
    for (;;) {
        wl_event_loop_dispatch(loop, 100); // drive fd callbacks like the compositor does
        if (index.poll(GENERATION, entries, 16))
            break; // true: this generation is complete and fully consumed
    }

    std::filesystem::path linkedContents;
    bool sawReal = false, sawDangling = false, sawOversize = false, sawBinary = false;
    for (const auto& E : entries) {
        const auto NAME = E.path.filename().string();
        sawDangling |= NAME == "dangling.desktop";
        sawOversize |= NAME == "oversize.desktop";
        sawBinary |= NAME == "binary.desktop";
        if (NAME == "real.desktop") {
            sawReal = true;
            HYPR_EXPECT(suite, E.contents.size() == std::filesystem::file_size(E.path)); // deref'd size
        }
        if (NAME == "linked.desktop") {
            linkedContents = E.relative;
            HYPR_EXPECT(suite, E.contents.size() > LINK_BYTES); // NOT truncated to the link length
            HYPR_EXPECT(suite, E.contents == BODY);            // exactly the target text
        }
    }
    HYPR_EXPECT(suite, sawReal);
    HYPR_EXPECT(suite, !linkedContents.empty());
    HYPR_EXPECT(suite, !sawDangling); // bash -f rejects it before framing
    HYPR_EXPECT(suite, !sawOversize);
    HYPR_EXPECT(suite, !sawBinary); // length check drops it instead of corrupting the stream

    std::filesystem::remove_all(root);
    wl_event_loop_destroy(loop);
    return suite.finish();
}
