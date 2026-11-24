// The pointer's scroll as the drawn surfaces read it: whole notches out of a
// wheel's detents or a touchpad's travel.
#pragma once

#include <hyprland/src/devices/IPointer.hpp>


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

} // namespace Awesome::Wheel
