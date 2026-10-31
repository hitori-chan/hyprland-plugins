#include "store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace NAwesome::detail {

    std::string readBoundedFile(const fs::path& path, size_t maxBytes) {
        // open first, then check what was opened: a FIFO swapped in after a
        // path check would block the open, and the compositor with it
        const int FD = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
        if (FD < 0)
            return {};
        std::string contents;
        struct stat st {};
        if (fstat(FD, &st) == 0 && S_ISREG(st.st_mode)) {
            contents.resize(maxBytes + 1);
            size_t got = 0;
            while (got < contents.size()) {
                const auto N = ::read(FD, contents.data() + got, contents.size() - got);
                if (N < 0 && errno == EINTR)
                    continue;
                if (N < 0)
                    got = contents.size(); // an error reads as too large: rejected
                if (N <= 0)
                    break;
                got += (size_t)N;
            }
            contents.resize(got > maxBytes ? 0 : got);
        }
        close(FD);
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

    std::string storeLabel(std::string_view s) {
        std::string out{s.substr(0, clipToUtf8Boundary(s, MAX_STORE_LABEL_BYTES))};
        std::ranges::replace_if(out, [](char c) { return c == '\t' || c == '\r' || c == '\n' || c == '\0'; }, ' ');
        return out;
    }

    bool validStoreLabel(std::string_view s) {
        return s.size() <= MAX_STORE_LABEL_BYTES && s.find_first_of(std::string_view{"\t\r\n\0", 4}) == std::string_view::npos;
    }

    bool ListStore::remember(std::string_view entry) {
        // a line break would end the row early and smuggle the rest in as
        // a row of its own
        if (entry.empty() || entry.find_first_of(std::string_view{"\r\n\0", 3}) != std::string_view::npos)
            return false;
        const std::string E{entry.substr(0, clipToUtf8Boundary(entry, MAX_STORE_STRING_BYTES))};
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
