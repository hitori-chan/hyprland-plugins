// awesome/core/store.cpp — see store.hpp.
#include "store.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace NAwesome::detail {

    std::string readBoundedFile(const fs::path& path, size_t maxBytes) {
        std::error_code statusError;
        if (!fs::is_regular_file(path, statusError))
            return {};
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return {};
        std::string contents(maxBytes + 1, '\0');
        f.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        const auto READ = static_cast<size_t>(f.gcount());
        if (READ > maxBytes)
            return {};
        contents.resize(READ);
        return contents;
    }

} // namespace NAwesome::detail

namespace NAwesome {

    std::filesystem::path stateDir() {
        const char* XDG  = std::getenv("XDG_STATE_HOME");
        const char* HOME = std::getenv("HOME");
        const auto  BASE = XDG && *XDG ? fs::path{XDG} : fs::path{HOME ? HOME : ""} / ".local" / "state";
        return BASE / "hyprland" / "plugin" / "awesome";
    }

    bool validStoreKey(std::string_view k) {
        if (k.empty() || k.size() > MAX_STORE_KEY_BYTES)
            return false;
        return k.find_first_of(std::string_view{"\t\r\n\0", 4}) == std::string_view::npos;
    }

    bool ListStore::remember(std::string_view entry) {
        // a line break would end the row early and smuggle the rest in as
        // a row of its own
        if (entry.empty() || entry.find_first_of(std::string_view{"\r\n\0", 3}) != std::string_view::npos)
            return false;
        BoundedString<MAX_STORE_STRING_BYTES> B;
        B.assignClipped(entry);
        const auto& E = B.str();
        if (E.empty() || (!entries.empty() && entries.back() == E))
            return false;
        // re-running an entry moves it to most recent
        std::erase(entries, E);
        entries.push_back(E);
        while (entries.size() > MAX_STORE_LIST_ENTRIES)
            entries.erase(entries.begin());
        return true;
    }

    bool bumpCount(CRecentMap<int>& counts, std::string_view name) {
        const int* C = counts.find(name);
        // at the cap the count stops, the recency still moves
        return counts.put(name, C ? std::min(*C + 1, MAX_LAUNCH_COUNT) : 1);
    }

} // namespace NAwesome
