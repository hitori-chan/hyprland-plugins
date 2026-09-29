// awesome/windows.cpp — port of hyprmax + hyprclick + hyprplace + hyprsnap
// (the bar's minimize/restore STATE operations land here too).
#include "windows.hpp"

namespace NAwesome::Windows {
    CModule& module() {
        static CModule M;
        return M;
    }
    void CModule::init() {}
    void CModule::teardown() {}
}
