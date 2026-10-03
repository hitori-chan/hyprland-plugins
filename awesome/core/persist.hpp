// awesome/core/persist.hpp — the store glue: the coalesced atomic write
// (one drain per burst, flushed at exit) and the state paths.
#pragma once

#include "hop.hpp"
#include "store.hpp"

#include <filesystem>
#include <functional>
#include <memory>

namespace NAwesome {

    // Coalesce store writes out of the event that dirtied them: a burst of
    // window closes must be one write, not one per close, and the write
    // never runs inside the emission that armed it.
    class Saver {
      public:
        explicit Saver(std::function<void()> write) : m_write(std::move(write)) {}
        ~Saver() {
            m_write = nullptr; // a late hop must not run the flush after the store is gone
        }

        void dirty() {
            if (m_queued)
                return; // one drain coalesces a burst — re-arming would cancel it
            m_queued = true;
            m_hop.arm([this]() {
                m_queued = false;
                if (m_write)
                    m_write();
            });
        }
        void flush() {
            if (!m_queued)
                return;
            m_queued  = false;
            m_hop.reset();
            if (m_write)
                m_write();
        }

      private:
        std::function<void()> m_write;
        CHop                  m_hop;
        bool                  m_queued = false;
    };

} // namespace NAwesome
