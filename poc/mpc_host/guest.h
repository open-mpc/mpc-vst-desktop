// Prototype: run an armv7 (armhf) MPC VST2 .so on another CPU. See docs/MPC_HOST.md.
// One guest: a 4 GB address space, one dynarmic A32 CPU, one plugin. Single-threaded.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mpchost {

using gaddr = uint32_t;   // a guest (32-bit ARM) address

class Guest {
public:
    explicit Guest(bool a64 = false);   // false: armhf (A32), true: aarch64 (A64)
    ~Guest();
    // Loads the .so, applies relocations and runs its constructors. False on error (see error()).
    bool load(const std::string& path);
    bool is64() const;
    const std::vector<std::string>& stubs_called() const;   // unsupported imports called so far (needs MPC_HOST_LAX=1 set before the first call)
    const std::string& error() const { return err_; }
    const std::vector<std::string>& missing() const { return missing_; }   // imports with no thunk (called = abort)

    gaddr symbol(const char* name) const;   // a defined dynamic symbol, 0 if none

    // Calls a guest function (armhf AAPCS: ints in r0-r3 then the stack, floats in s0-s15).
    uint32_t call(gaddr fn, const std::vector<uint32_t>& ints, const std::vector<float>& floats = {});
    float call_f(gaddr fn, const std::vector<uint32_t>& ints, const std::vector<float>& floats = {});

    // Guest memory.
    uint8_t* host(gaddr a) const { return mem_ + a; }
    template <class T> T* ptr(gaddr a) const { return reinterpret_cast<T*>(mem_ + a); }
    uint32_t rd32(gaddr a) const;
    void wr32(gaddr a, uint32_t v);
    gaddr alloc(uint32_t size);        // guest heap (malloc)
    void dealloc(gaddr p);
    gaddr strdup(const char* s);

    // The guest's view of the VST host callback: a guest function pointer that runs `fn` on the host.
    using HostCallback = uint32_t (*)(Guest&, uint32_t eff, int32_t op, int32_t idx, int32_t val, uint32_t ptr, float opt);
    gaddr host_callback(HostCallback fn);

    // Services the embedding host may lend a guest (the VST3 gives the Plugin Manager a device to run on): HTTP GET for a libcurl lookalike, and what
    // posix_spawn runs (the argument vector; the exit status). Unset: libcurl is not found and spawning fails, as for any sandboxed guest.
    struct Hooks {
        std::function<bool(const std::string& url, std::vector<uint8_t>& body, std::string& error)> http_get;
        std::function<int(Guest&, const std::vector<std::string>& argv)> spawn;
    } hooks;
    bool host_path(const std::string& guest_path, std::string& host);   // where a guest path lives on the host (MPC_HOST_ROOT, /plugin/)

    struct Impl;
private:
    bool load64(const std::string& path);
    uint8_t* mem_ = nullptr;
    Impl* impl_ = nullptr;
    std::string err_;
    std::vector<std::string> missing_;
    friend struct Impl;
};

}  // namespace mpchost
