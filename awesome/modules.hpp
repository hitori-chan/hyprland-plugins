// awesome/modules.hpp — the module singletons, in input-priority order
// (the registration order main.cpp uses; the pipeline dispatches in this
// order, so this list IS the input contract).
#pragma once

#include "core/supervisor.hpp"

#include "shell.hpp"
#include "notify.hpp"
#include "windows.hpp"
#include "system.hpp"
