// awesome/system.cpp — port of hyprosd + hyprpad.
#include "system.hpp"

namespace NAwesome::System {
    CModule& module() {
        static CModule M;
        return M;
    }
    void CModule::init() {}
    void CModule::teardown() {}
}
