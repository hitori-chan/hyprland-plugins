// The cursor theme reader, headless: an XCursor file's nearest size and its
// frames, bounds on a bad file, and a shape found through an older X name,
// a theme's Inherits and "default". `make -C vclick test`.
#include "theme.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

using namespace VClick::Theme;
namespace fs = std::filesystem;

static int pass = 0, fail = 0;

static void check(bool ok, const char* what) {
    if (ok)
        ++pass;
    else {
        ++fail;
        std::printf("  FAIL  %s\n", what);
    }
}

struct SImg {
    uint32_t nominal, w, h, hotX, hotY, delay, argb;
};

static void put(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        s += static_cast<char>(v >> (8 * i) & 0xff);
}

// an XCursor file of these images, each a solid argb
static std::string xcursor(const std::vector<SImg>& imgs) {
    std::string out, body;
    put(out, 0x72756358);
    put(out, 16);
    put(out, 0x10000);
    put(out, imgs.size());
    uint32_t at = 16 + 12 * imgs.size();
    for (const auto& i : imgs) {
        put(out, 0xfffd0002);
        put(out, i.nominal);
        put(out, at + body.size());
        for (const uint32_t V : {36u, 0xfffd0002u, i.nominal, 1u, i.w, i.h, i.hotX, i.hotY, i.delay})
            put(body, V);
        for (uint32_t p = 0; p < i.w * i.h; ++p)
            put(body, i.argb);
    }
    return out + body;
}

static void write(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

int main() {
    char       tmpl[] = "/tmp/vclick-theme-XXXXXX";
    const auto ROOT   = fs::path{mkdtemp(tmpl)};

    // two sizes: 24 px as two frames of an animation, 48 px as one
    write(ROOT / "anim", xcursor({{24, 24, 24, 3, 4, 50, 0xff102030}, {48, 48, 48, 6, 8, 0, 0xff405060}, {24, 24, 24, 3, 4, 70, 0x80000000}}));
    const auto A = readFile(ROOT / "anim", 32);
    check(A && A->nominal == 24 && A->frames.size() == 2, "the nearest nominal size (32 -> 24), its two frames");
    check(A && A->frames[0].w == 24 && A->frames[0].hotX == 3 && A->frames[0].hotY == 4 && A->frames[0].delayMs == 50 && A->frames[1].delayMs == 70,
          "  each frame's size, hotspot and delay");
    check(A && A->frames[0].argb.size() == 576 && A->frames[0].argb[0] == 0xff102030 && A->frames[1].argb[575] == 0x80000000, "  their pixels, as ARGB");
    const auto B = readFile(ROOT / "anim", 40);
    check(B && B->nominal == 48 && B->frames.size() == 1 && B->frames[0].w == 48, "40 -> 48");
    const auto T = readFile(ROOT / "anim", 36);
    check(T && T->nominal == 48, "a tie (36: 24 or 48) takes the larger");

    // bad files
    write(ROOT / "notx", "hello world, not a cursor");
    check(!readFile(ROOT / "notx", 24), "not an XCursor file: an error");
    const auto FULL = xcursor({{24, 24, 24, 0, 0, 0, 0xffffffff}});
    write(ROOT / "short", FULL.substr(0, FULL.size() - 10));
    check(!readFile(ROOT / "short", 24), "a truncated file: an error");
    write(ROOT / "huge", xcursor({{24, 300, 2, 0, 0, 0, 0xffffffff}}));
    check(!readFile(ROOT / "huge", 24), "an image over 256 px: refused");
    write(ROOT / "hot", xcursor({{24, 8, 8, 99, 99, 0, 0xffffffff}}));
    const auto H = readFile(ROOT / "hot", 24);
    check(H && H->frames[0].hotX == 7 && H->frames[0].hotY == 7, "a hotspot outside the image: clamped into it");
    check(!readFile(ROOT / "missing", 24), "a missing file: an error");

    // finding a shape: an older X name, an inherited theme, "default"
    const auto D1 = ROOT / "d1", D2 = ROOT / "d2";
    write(D1 / "child/cursors/text", FULL);
    write(D1 / "child/index.theme", "[Icon Theme]\nName=child\nInherits = parent ; other\n");
    write(D2 / "parent/cursors/left_ptr", FULL);
    write(D2 / "default/cursors/wait", FULL);
    write(D2 / "loop/index.theme", "[Icon Theme]\nInherits=loop\n");
    const std::vector<std::string> DIRS{D1, D2};
    check(VClick::Theme::find("child", "text", DIRS) == (D1 / "child/cursors/text").string(), "a shape in the theme itself");
    check(VClick::Theme::find("child", "default", DIRS) == (D2 / "parent/cursors/left_ptr").string(), "'default' as left_ptr, in the theme it inherits");
    check(VClick::Theme::find("child", "wait", DIRS) == (D2 / "default/cursors/wait").string(), "a shape no theme in the chain has: from 'default'");
    check(VClick::Theme::find("child", "zoom-in", DIRS).empty(), "a shape nowhere: none");
    check(VClick::Theme::find("loop", "pointer", DIRS).empty(), "a theme inheriting itself ends");
    check(VClick::Theme::find("../d1/child", "text", DIRS).empty(), "a theme name is one path component");

    // the search path
    setenv("XCURSOR_PATH", "/a:~/b::/c", 1);
    setenv("HOME", "/home/u", 1);
    check(searchPath() == std::vector<std::string>{"/a", "/home/u/b", "/c"}, "XCURSOR_PATH, ~ expanded, empty parts skipped");
    unsetenv("XCURSOR_PATH");
    unsetenv("XDG_DATA_HOME");
    setenv("XDG_DATA_DIRS", "/x:/y", 1);
    check(searchPath() == std::vector<std::string>{"/home/u/.local/share/icons", "/home/u/.icons", "/x/icons", "/y/icons", "/usr/share/pixmaps"}, "else the XDG icon dirs");

    fs::remove_all(ROOT);
    std::printf("%s: %d of %d checks passed\n", fail ? "FAIL" : "ok", pass, pass + fail);
    return fail ? 1 : 0;
}
