// awesome/notify.cpp — port of hyprnotify.
#include "notify.hpp"

namespace NAwesome::Notify {
    CModule& module() {
        static CModule M;
        return M;
    }
    void CModule::init() {}
    void CModule::teardown() {}
}
