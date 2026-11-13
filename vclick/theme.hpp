// The cursor theme's images for the shapes apps name (cursor-shape-v1):
// one XCursor file read for one shape at one size, found as libXcursor
// finds it (the icon dirs, the theme, what it inherits, then "default").
// Pure file reading with no compositor in it, so test/theme.cpp checks it
// headless.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace VClick::Theme {
    // bounds on what a file may make vclick read: larger is refused
    constexpr int MAX_SIDE   = 256; // px, an image's width or height
    constexpr int MAX_FRAMES = 64;  // an animation's
    constexpr int MAX_TOC    = 1024;
    constexpr int MAX_DEPTH  = 8; // themes inheriting themes

    struct SFrame {
        int                   w = 0, h = 0, hotX = 0, hotY = 0;
        uint32_t              delayMs = 0;
        std::vector<uint32_t> argb; // premultiplied ARGB, w * h, rows top down
    };

    struct SImage {
        int                 nominal = 0; // the file's nominal size these frames are
        std::vector<SFrame> frames;      // one, or an animation's
    };

    // the frames of the nominal size nearest `size` in one XCursor file
    std::expected<SImage, std::string> readFile(const std::string& path, int size);

    // the icon dirs, in order: XCURSOR_PATH, else $XDG_DATA_HOME/icons,
    // ~/.icons, each $XDG_DATA_DIRS/icons, /usr/share/pixmaps
    std::vector<std::string> searchPath();

    // the file that draws a shape: its own name or an older X name for it,
    // in the theme, what it inherits, then "default"; empty: none has it
    std::string find(std::string_view theme, std::string_view shape, const std::vector<std::string>& dirs);
}
