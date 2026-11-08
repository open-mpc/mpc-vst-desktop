// A stand-in MPC for the stock Plugin Manager (docs/MPC_VST3_HOST.md): the files it looks for (MPC.settings, a Synths folder, /proc/mounts, the tools it checks
// for), the few shell commands it runs (sha256sum, unzip, a restart through systemd-run) done natively, and the install/remove steps its apply.sh
// would do on a device. Everything lives under the plugin's root folder; nothing outside it is touched.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../poc/mpc_host/guest.h"

namespace pm {

struct DeviceSim {
    std::string root;                    // the plugin's "/" on the host
    std::function<void()> on_restart;    // the apply "restarted MPC": the host reloads the hosted plugin and rescans
    void setup();                        // creates the device's files under root (keeps what is there)
    int spawn(mpchost::Guest& g, const std::vector<std::string>& argv);   // the guest's posix_spawn: exit status, or -1000 for an unknown program
    void write_settings();               // MPC.settings from the plugin folders in /nvme/Synths
    static constexpr const char* kSynths = "nvme/Synths";   // the install location, relative to root
private:
    int run_sh(mpchost::Guest& g, const std::string& cmd);
    int apply(mpchost::Guest& g, const std::string& script_host_path);
    bool install_pkg(const std::string& pkgdir, const std::string& synths, std::string& err);
    bool remove_folder(const std::string& folder);
    void log(const std::string& m);
};

}  // namespace pm
