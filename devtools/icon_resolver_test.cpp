#include "common/icons.hpp"

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

}

int main() {
    namespace fs = std::filesystem;

    const auto root = fs::temp_directory_path() / ("hypr-icons-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    touch(root / "icons/Adwaita/symbolic/status/display-brightness-symbolic.svg");
    touch(root / "icons/Adwaita/symbolic/devices/touchpad-disabled-symbolic.svg");

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

    fs::remove_all(root, ec);
    if (!ok)
        return 1;
    std::cout << "icon_resolver_test: all checks passed\n";
    return 0;
}
