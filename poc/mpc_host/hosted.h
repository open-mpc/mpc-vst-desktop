// One MPC VST2 plugin (an armhf or aarch64 .so) running under the emulator, behind the calls a plugin host needs. Used by the command-line driver (main.cpp)
// and by the VST3 module (vst3host/). Not thread-safe: the caller serialises calls (the emulator is one CPU).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "guest.h"

namespace mpchost {

class HostedPlugin {
public:
    static constexpr int kBlock = 128;          // MPC plugins are driven in 128-frame blocks at 44100 Hz
    static constexpr double kRate = 44100.0;

    struct Config {
        std::string root;                       // the plugin's "/" (MPC_HOST_ROOT); empty: the environment variable
        std::string support_dir;                // where guest_helpers*.so and guestlibs/ are; empty: next to the running program
        std::vector<std::string> libs;          // extra guest libraries to load first; empty: guestlibs/<arch>/ (or MPC_HOST_LIBS)
        Guest::Hooks hooks;                     // services lent to the guest (HTTP, spawn); empty: none
    };

    HostedPlugin();
    ~HostedPlugin();
    bool open(const std::string& so_path, const Config& cfg, std::string& error);   // load and create the plugin; call start() next
    void start();                               // effOpen, sample rate, block size, mains on
    bool is64() const;

    std::string name();                         // effGetEffectName
    std::string vendor();
    int num_params() const;
    int num_outputs() const;
    int num_programs() const;
    std::string param_name(int i);              // effGetParamName / Label / Display
    std::string param_label(int i);
    std::string param_display(int i);
    void set_param(int i, float v);             // normalised 0..1
    float get_param(int i);

    void midi(uint8_t status, uint8_t d1, uint8_t d2, int delta_frames = 0);   // delivered with the next process() block
    void set_transport(double tempo, double ppq, bool playing);
    void process(float* l, float* r);           // one block of kBlock frames (the plugin's output; its input is silence)

    std::vector<uint8_t> get_chunk();           // the plugin's state, empty if it has none
    bool set_chunk(const std::vector<uint8_t>& c);

    // The plugin changed a parameter by itself (audioMasterAutomate) / asked for a redraw (audioMasterUpdateDisplay): collected, then taken by the caller.
    std::vector<std::pair<int, float>> take_automation();
    bool take_display_dirty();

    Guest& guest();                             // for stats and tests
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace mpchost
