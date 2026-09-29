// awesome/core/bounded.hpp — externally-sized values, bounded by
// construction.
//
// Every byte that crosses a wire (bus strings, action payloads, store
// rows, image metadata) enters the plugin through one of these types, so
// the bound is a property of the type, not a discipline each caller
// remembers. BoundedString clips at UTF-8 codepoint boundaries for
// display text; assignStrict is the all-or-nothing admission for opaque
// identifiers, where clipping would change identity (a clipped
// conversation-id would merge two different chats).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NAwesome {

    // Codepoint length for a lead byte; 0 for a continuation byte or an
    // invalid lead.
    inline constexpr unsigned utf8SeqLen(unsigned char lead) {
        if (lead < 0x80)
            return 1;
        if ((lead & 0xE0) == 0xC0)
            return 2;
        if ((lead & 0xF0) == 0xE0)
            return 3;
        if ((lead & 0xF8) == 0xF0)
            return 4;
        return 0;
    }

    // Largest prefix of s of at most cap bytes that ends on a codepoint
    // boundary (the contract is over well-formed UTF-8; malformed input is
    // preserved up to the last lead position, never extended). Walk back
    // from the cap over continuation bytes: the lead position n is the last
    // boundary before the sequence the cap cuts, whether that sequence is
    // complete or truncated at the input's end — the lead byte itself is
    // outside the prefix, so nothing is half-emitted.
    inline size_t clipToUtf8Boundary(std::string_view s, size_t cap) {
        if (s.size() <= cap)
            return s.size();
        size_t n = cap;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
            --n;
        return n;
    }

    // A string that can never hold more than N bytes. There is no
    // growth API: the only writers are the two admissions below.
    template <size_t N>
    class BoundedString {
      public:
        static constexpr size_t CAP = N;

        // Display text: clip at a codepoint boundary. False when input was
        // clipped.
        template <typename S>
        bool assignClipped(S&& sv) {
            std::string_view v{sv};
            m_data.assign(v.data(), clipToUtf8Boundary(v, N));
            m_rejected = false;
            m_clipped  = v.size() > N;
            return !m_clipped;
        }

        // Opaque identity: all-or-nothing. Rejected input leaves the
        // string empty and flagged.
        template <typename S>
        bool assignStrict(S&& sv) {
            std::string_view v{sv};
            m_clipped = false;
            if (v.size() > N) {
                m_data.clear();
                m_rejected = true;
                return false;
            }
            m_data.assign(v);
            m_rejected = false;
            return true;
        }

        [[nodiscard]] bool clipped() const {
            return m_clipped;
        }
        [[nodiscard]] bool rejected() const {
            return m_rejected;
        }
        [[nodiscard]] std::string_view view() const {
            return m_data;
        }
        [[nodiscard]] const std::string& str() const {
            return m_data;
        }
        [[nodiscard]] explicit operator bool() const {
            return !m_data.empty();
        }

      private:
        std::string m_data;
        bool        m_clipped  = false;
        bool        m_rejected = false;
    };

    // A FIFO that can never hold more than N entries. Eviction policy is
    // explicit at the call: tryPush rejects at the bound (bounded work
    // queues), pushEvict returns the oldest (model caps — the notification
    // overflow is this call). Front-erase is O(N); at the model's N (tens
    // to low hundreds) that is a non-event against the per-arrival cost.
    template <typename T, size_t N>
    class BoundedQueue {
      public:
        static constexpr size_t CAP = N;

        [[nodiscard]] bool full() const {
            return m_items.size() == N;
        }
        [[nodiscard]] size_t size() const {
            return m_items.size();
        }
        [[nodiscard]] bool empty() const {
            return m_items.empty();
        }
        [[nodiscard]] const T& front() const {
            return m_items.front();
        }
        [[nodiscard]] const T& back() const {
            return m_items.back();
        }

        bool tryPush(T v) {
            if (m_items.size() == N)
                return false;
            m_items.push_back(std::move(v));
            return true;
        }

        // Pushes unconditionally; returns the evicted oldest (value-
        // initialized when nothing was evicted).
        T pushEvict(T v) {
            T evicted{};
            if (m_items.size() == N) {
                evicted = std::move(m_items.front());
                m_items.erase(m_items.begin());
            }
            m_items.push_back(std::move(v));
            return evicted;
        }

        bool pop(T& out) {
            if (m_items.empty())
                return false;
            out       = std::move(m_items.front());
            m_items.erase(m_items.begin());
            return true;
        }

        void clear() {
            m_items.clear();
        }
        auto begin() {
            return m_items.begin();
        }
        auto end() {
            return m_items.end();
        }

      private:
        std::vector<T> m_items;
    };

} // namespace NAwesome
