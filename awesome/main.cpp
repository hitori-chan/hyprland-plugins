// awesome/main.cpp — the one plugin entrypoint.
//
// One .so, one version, one deploy unit. The modules register themselves
// into the supervisor in input-priority order (the old hyprpm load-order
// contract, now in code): shell, notify, windows, system. The hash check
// is the fork's own cross-build guard; the layout matching is the real
// protection, this is the fast, loud half.
#include "core/config.hpp"
#include "core/jobs.hpp"
#include "core/supervisor.hpp"
#include "core/version.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>

#include <stdexcept>
#include <string>

#include "modules.hpp"

namespace {
    HANDLE PHANDLE = nullptr;

    static SP<IPC::Socket1::SCommand> ctlCmd;

    // `hyprctl awesome <verb>`: the first module to answer owns it.
    // The prefix command hands us the FULL request ("awesome count");
    // the verb is everything after the command word.
    static std::string ctlDispatch(const std::string& request) {
        const auto SP = request.find(' ');
        const auto verb = (SP == std::string::npos) ? std::string{} : request.substr(SP + 1);
        if (verb.empty())
            return "usage: awesome <count|center|state|badge|topline|clear|…>";
        const NAwesome::IModule* const MODULES[] = {&NAwesome::Shell::module(), &NAwesome::Notify::module(), &NAwesome::Windows::module(), &NAwesome::System::module()};
        for (const auto* M : MODULES)
            if (const auto R = M->handleCtl(verb))
                return *R;
        return "unknown request";
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[awesome] Version mismatch: rebuild the plugin against the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[awesome] version mismatch");
    }

    NAwesome::cfg().init(PHANDLE);
    NAwesome::supervisor().registerModule(&NAwesome::Shell::module());
    NAwesome::supervisor().registerModule(&NAwesome::Notify::module());
    NAwesome::supervisor().registerModule(&NAwesome::Windows::module());
    NAwesome::supervisor().registerModule(&NAwesome::System::module());
    NAwesome::supervisor().start(PHANDLE);

    // the lockscreen bell reads count; the gate reads state
    ctlCmd = HyprlandAPI::registerHyprCtlCommand(PHANDLE, IPC::Socket1::SCommand{.name = "awesome", .match = IPC::Socket1::COMMAND_MATCH_PREFIX,
                                                                                  .handler = [](const IPC::Socket1::SRequest& request) { return IPC::Socket1::SResponse{ctlDispatch(request.command)}; }});

    return {NAwesome::NAME, "the awesome shell, unified", NAwesome::AUTHOR, NAwesome::VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    NAwesome::supervisor().stop(); // listeners, hops, then modules in reverse
    if (ctlCmd)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, ctlCmd);
    ctlCmd.reset();
    NAwesome::Jobs::inst().teardown(); // children after the modules that armed them
    NAwesome::cfg().exit(PHANDLE);
}
