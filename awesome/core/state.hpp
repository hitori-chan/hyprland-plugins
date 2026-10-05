// awesome/core/state.hpp — the plugin's single persistent state.
//
// One file, one load, one save path:
//   $XDG_STATE_HOME/hyprland/plugin/awesome/state.tsv
// A version line, then typed tab-separated rows, each kind oldest first
// (the least recently remembered row is the first to go at a bound):
//   awesome-state 2
//   spot     <x> <y> <class>          (where the app's last window closed)
//   windowed <x> <y> <w> <h> <class>  (its last windowed box: the un-max
//                                      restore of a window born maximized)
//   launches <count> <name>           (launcher ranking, by .desktop Name)
//   history  <query>                  (launcher queries, newest last)
// Positions are relative to the origin of the monitor the window was on:
// a spot remembered on one monitor lands at the same place on whichever
// monitor the app opens on next, never clamped against a far edge. The
// key (class, name) is the row's last field: app ids contain spaces and
// colons, never tabs.
//
// load() runs once, before any module inits. A file of an unknown (newer)
// version, or one that exists but cannot be read, blocks every later
// write: the user's memory is never replaced by a session that could not
// see it. The version-1 layout (no version line, size-carrying spots, a
// "name;count" launch row) is read once and rewritten as version 2.
//
// Modules reach their stores through this object — the documented
// cross-module seam for anything persisted.
#pragma once

#include "persist.hpp"
#include "store.hpp"

namespace NAwesome {

    struct AppState {
        CRecentMap<Box> spot;     // w, h unused (0): the client picks its size
        CRecentMap<Box> windowed;
        CRecentMap<int> launches;
        ListStore       history;
    };

    class StateStore {
      public:
        static StateStore& inst();

        AppState&       data();
        const AppState& data() const;

        // Idempotent; must run before any module inits (supervisor does).
        void load();

        // Coalesced atomic save (one write per burst, out of the
        // dirtying event); flush() writes immediately (teardown).
        void dirty();
        void flush();

        static std::filesystem::path path();
        // false: absent, unreadable, or a version this build does not know
        static bool                  read(const std::filesystem::path& path, AppState& out);
        static bool                  write(const std::filesystem::path& path, const AppState& data);

      private:
        StateStore();
        AppState m_data{};
        Saver    m_saver;
        bool     m_loaded       = false;
        bool     m_writeBlocked = false;
    };

} // namespace NAwesome
