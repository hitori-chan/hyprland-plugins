#include "common/icons.hpp"

#include <hyprgraphics/resource/AsyncResourceGatherer.hpp>
#include <hyprgraphics/resource/resources/ImageResource.hpp>
#include <hyprutils/memory/SharedPtr.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <unistd.h>

namespace {

    bool check(bool value, const char* message) {
        if (value)
            return true;
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    void touch(const std::filesystem::path& path) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path);
        file << "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
    }

    bool loadSvgAtSize(const std::filesystem::path& path, int px) {
        Hyprgraphics::CAsyncResourceGatherer gatherer;
        auto resource = Hyprutils::Memory::makeAtomicShared<Hyprgraphics::CImageResource>(path.string(), Hyprutils::Math::Vector2D{(double)px, (double)px});
        gatherer.enqueue(resource);
        gatherer.await(resource);
        return resource->m_ready.load() && resource->m_asset.cairoSurface && resource->m_asset.pixelSize.x == px && resource->m_asset.pixelSize.y == px;
    }

}

int main() {
    namespace fs = std::filesystem;

    const auto root = fs::temp_directory_path() / ("hypr-icons-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    touch(root / "icons/Adwaita/symbolic/status/display-brightness-symbolic.svg");
    touch(root / "icons/Adwaita/symbolic/devices/touchpad-disabled-symbolic.svg");
    touch(root / "icons/hicolor/scalable/apps/qbittorrent.svg");

    const std::string data = root.string();
    const std::string cfg  = (root / "config").string();
    setenv("XDG_DATA_HOME", data.c_str(), 1);
    setenv("XDG_DATA_DIRS", data.c_str(), 1);
    setenv("XDG_CONFIG_HOME", cfg.c_str(), 1);
    NHyprCommon::resetIconNameCache();

    bool ok = true;
    ok &= check(NHyprCommon::resolveIconName("display-brightness-symbolic", 44) ==
                    (root / "icons/Adwaita/symbolic/status/display-brightness-symbolic.svg").string(),
                "Adwaita symbolic status icon resolves");
    ok &= check(NHyprCommon::resolveIconName("touchpad-disabled-symbolic", 44) ==
                    (root / "icons/Adwaita/symbolic/devices/touchpad-disabled-symbolic.svg").string(),
                "Adwaita symbolic device icon resolves");
    const auto qbit = NHyprCommon::resolveIconName("qbittorrent", 44);
    ok &= check(qbit == (root / "icons/hicolor/scalable/apps/qbittorrent.svg").string(), "qBittorrent app icon resolves");
    ok &= check(NHyprCommon::isSvgIconPath(qbit) && NHyprCommon::isSvgIconPath("ICON.SVG") && !NHyprCommon::isSvgIconPath("icon.svgz"),
                "SVG path detection is exact and case-insensitive");
    ok &= check(loadSvgAtSize(qbit, 44), "themed SVG app icon loads with an explicit viewport");

    fs::remove_all(root, ec);
    if (!ok)
        return 1;
    std::cout << "icon_resolver_test: all checks passed\n";
    return 0;
}
