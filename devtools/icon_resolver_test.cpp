#include "common/icons.hpp"
#include "test.hpp"

#include <hyprgraphics/resource/AsyncResourceGatherer.hpp>
#include <hyprgraphics/resource/resources/ImageResource.hpp>
#include <hyprutils/memory/SharedPtr.hpp>

#include <filesystem>
#include <fstream>

#include <unistd.h>

namespace {

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
    NHyprTest::CSuite suite{"icon_resolver_test"};

    const auto        root = fs::temp_directory_path() / ("hypr-icons-" + std::to_string(::getpid()));
    std::error_code   ec;
    fs::remove_all(root, ec);
    touch(root / "icons/Adwaita/symbolic/status/display-brightness-symbolic.svg");
    touch(root / "icons/Adwaita/symbolic/devices/touchpad-disabled-symbolic.svg");
    touch(root / "icons/hicolor/scalable/apps/qbittorrent.svg");
    // single-size apps: nm-applet's notification set is 22x22 only, discord
    // ships 256x256 only, some apps 512x512 only — every one must resolve
    touch(root / "icons/hicolor/22x22/apps/nm-signal-50.png");
    touch(root / "icons/hicolor/256x256/apps/discord.png");
    touch(root / "icons/hicolor/512x512/apps/bigapp.png");
    // when several sizes exist, the one near the request wins, not the largest
    touch(root / "icons/hicolor/48x48/apps/duo.png");
    touch(root / "icons/hicolor/256x256/apps/duo.png");
    // a GTK theme in the breeze <context>/<size> layout
    touch(root / "config/gtk-3.0/settings.ini");
    { std::ofstream f(root / "config/gtk-3.0/settings.ini");
      f << "[Settings]\ngtk-icon-theme-name=faketheme\n"; }
    touch(root / "icons/faketheme/devices/22/fake-device.svg");

    const std::string data = root.string();
    const std::string cfg  = (root / "config").string();
    setenv("XDG_DATA_HOME", data.c_str(), 1);
    setenv("XDG_DATA_DIRS", data.c_str(), 1);
    setenv("XDG_CONFIG_HOME", cfg.c_str(), 1);
    NHyprCommon::resetIconNameCache();

    suite.expect(NHyprCommon::resolveIconName("display-brightness-symbolic", 44) == (root / "icons/Adwaita/symbolic/status/display-brightness-symbolic.svg").string(),
                 "Adwaita symbolic status icon resolves");
    suite.expect(NHyprCommon::resolveIconName("touchpad-disabled-symbolic", 44) == (root / "icons/Adwaita/symbolic/devices/touchpad-disabled-symbolic.svg").string(),
                 "Adwaita symbolic device icon resolves");
    const auto qbit = NHyprCommon::resolveIconName("qbittorrent", 44);
    suite.expect(qbit == (root / "icons/hicolor/scalable/apps/qbittorrent.svg").string(), "qBittorrent app icon resolves");
    suite.expect(NHyprCommon::isSvgIconPath(qbit) && NHyprCommon::isSvgIconPath("ICON.SVG") && !NHyprCommon::isSvgIconPath("icon.svgz"),
                 "SVG path detection is exact and case-insensitive");
    suite.expect(loadSvgAtSize(qbit, 44), "themed SVG app icon loads with an explicit viewport");

    suite.expect(NHyprCommon::resolveIconName("nm-signal-50", 44) == (root / "icons/hicolor/22x22/apps/nm-signal-50.png").string(),
                 "22x22-only icon resolves (the nm-applet set)");
    suite.expect(NHyprCommon::resolveIconName("discord", 44) == (root / "icons/hicolor/256x256/apps/discord.png").string(),
                 "256x256-only icon resolves (discord)");
    suite.expect(NHyprCommon::resolveIconName("bigapp", 44) == (root / "icons/hicolor/512x512/apps/bigapp.png").string(),
                 "512x512-only icon resolves");
    suite.expect(NHyprCommon::resolveIconName("duo", 44) == (root / "icons/hicolor/48x48/apps/duo.png").string(),
                 "near-size wins over larger when both exist");
    suite.expect(NHyprCommon::resolveIconName("fake-device", 44) == (root / "icons/faketheme/devices/22/fake-device.svg").string(),
                 "GTK theme breeze-layout (context/size) device icon resolves");

    fs::remove_all(root, ec);
    return suite.finish();
}
