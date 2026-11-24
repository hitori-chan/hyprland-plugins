// The pointer's scroll as the drawn surfaces read it: whole notches out of a
// wheel's detents or a touchpad's travel (the bar), and the speed a
// touchpad's finger lifts at (the center's fling).
#pragma once

#include <hyprland/src/devices/IPointer.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace Awesome::Wheel {

    // Whole notches, signed like the axis (negative = up): awesome hears
    // button 4/5 once per wheel detent and once per 15 units of touchpad
    // travel (libinput's X11 scroll distance), never once per event. A
    // high-resolution wheel sends a detent as several partial v120 events
    // and a touchpad swipe as dozens of small deltas; neither steps per
    // event. The remainder resets on a direction flip, a source change and
    // a touchpad's lift (its axis stop: delta 0). One per surface: each
    // keeps its own remainder.
    class CNotches {
      public:
        int step(const IPointer::SAxisEvent& e) {
            if (e.source != m_source) {
                m_source = e.source;
                m_acc    = 0;
            }
            const bool   WHEEL = e.source == WL_POINTER_AXIS_SOURCE_WHEEL;
            const double V     = WHEEL ? (e.deltaDiscrete != 0 ? (double)e.deltaDiscrete : e.delta * 8.0) : e.delta;
            const double UNIT  = WHEEL ? 120.0 : 15.0;
            if (V == 0.0 || (m_acc != 0.0 && (m_acc < 0) != (V < 0)))
                m_acc = 0;
            m_acc += V;
            const int N = (int)(m_acc / UNIT);
            m_acc -= N * UNIT;
            return N;
        }

      private:
        double                 m_acc    = 0;
        wl_pointer_axis_source m_source = WL_POINTER_AXIS_SOURCE_WHEEL;
    };

    // A finger's last travel, by the events' own times: its lift leaves at
    // the speed of the last 100 ms (px/s), none if it rested 50 ms before
    // lifting or moved once.
    class CFling {
      public:
        void add(uint32_t ms, double px) {
            m_s[m_head] = {ms, px};
            m_head      = (m_head + 1) % m_s.size();
            m_n         = std::min(m_n + 1, m_s.size());
        }
        void clear() {
            m_n = 0;
        }
        double lift(uint32_t ms) {
            const size_t N = m_n;
            m_n            = 0;
            if (N < 2)
                return 0;
            const auto AT = [&](size_t k) -> const SSample& { return m_s[(m_head + m_s.size() - 1 - k) % m_s.size()]; }; // 0: the newest
            if (ms - AT(0).ms > 50)
                return 0;
            // the oldest sample's travel came before its time: the span
            // starts there and counts what followed
            double   px     = 0;
            uint32_t oldest = AT(0).ms;
            for (size_t k = 0; k < N && AT(0).ms - AT(k).ms <= 100; k++) {
                px += k > 0 ? AT(k - 1).px : 0;
                oldest = AT(k).ms;
            }
            const uint32_t SPAN = AT(0).ms - oldest;
            return SPAN >= 5 ? px * 1000.0 / SPAN : 0;
        }

      private:
        struct SSample {
            uint32_t ms = 0;
            double   px = 0;
        };
        std::array<SSample, 8> m_s{};
        size_t                 m_n = 0, m_head = 0;
    };

} // namespace Awesome::Wheel
