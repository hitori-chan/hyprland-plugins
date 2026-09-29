// awesome/shell.cpp — port of hyprbar (chrome).
#include "shell.hpp"

namespace NAwesome::Shell {
    CModule& module() {
        static CModule M;
        return M;
    }
    void CModule::init() {}
    void CModule::teardown() {}
}
