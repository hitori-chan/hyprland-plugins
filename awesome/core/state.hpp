// awesome/core/state.hpp — the plugin's single persistent state.
//
// One file, one load, one save path:
//   $XDG_STATE_HOME/hyprland/plugin/awesome/state.tsv
// with typed tab-separated rows, fixed order:
//   spot    <x> <y> <w> <h> <class>   (spawn placement, last closed box)
//   windowed <x> <y> <w> <h> <appID>  (un-max restore, last windowed box)
//   launches <name>;<count>           (launcher ranking)
//   history <query>                   (launcher query history, newest last)
//
// load() runs once, before any module inits. When state.tsv is absent it
// migrates, newest first — the four-file layout in stateDir(), the
// pre-rename layout in legacyStateDir(), the ancient per-module stores
// (stateBase()/hyprplace, stateBase()/hyprmax, cacheBase()/hyprbar) —
// merges key-by-key (newer wins, older fills only missing keys) and then
// CONSUMES the sources (the migration is one-time; the plugin has exactly
// one state file from then on). The consume is best-effort and only runs
// once state.tsv itself is safely written.
//
// Modules reach their stores through this object — the documented
// cross-module seam for anything persisted.
#pragma once

#include "persist.hpp"
#include "store.hpp"

namespace NAwesome {

    // The four module stores, one in-memory copy.
    struct AppState {
        BoxStore   spot;
        BoxStore   windowed;
        CountStore launches;
        ListStore  history;
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
        static bool                  readUnified(const std::filesystem::path& path, AppState& out);
        static bool                  writeUnified(const std::filesystem::path& path, const AppState& data);

      private:
        StateStore();
        AppState m_data{};
        Saver    m_saver;
        bool     m_loaded = false;
    };

} // namespace NAwesome
