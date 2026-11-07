// The host-native plugin manager service (docs/MPC_VST3_HOST.md, M3): catalog, download, sha256, unzip, install into <root>/sdcard/Synths/<name>/.
// No external libraries: a small JSON reader, SHA-256, inflate and zip reader here; HTTPS through WinHTTP (Windows) or the system libcurl (Linux, dlopen).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pm {

struct Item {
    std::string id, name, kind, summary, author, version, url, sha256;
    long long size = 0;
};

bool http_get(const std::string& url, std::vector<uint8_t>& out, std::string& err);   // https://..., http://..., file://path or a plain path
std::string sha256_hex(const uint8_t* p, size_t n);
bool unzip(const std::vector<uint8_t>& zip, const std::string& dest, const std::string& strip_prefix, std::string& err);   // entries under strip_prefix go to dest

// catalog.json -> the downloadable items for `arch` ("aarch64" or "armv7"; falls back to the other when a plugin has only one)
bool load_catalog(const std::string& url, const std::string& arch, std::vector<Item>& items, std::string& err);
// Download, verify, unpack and install: <root>/sdcard/Synths/<folder>/ (the package's portable/<folder>, as install.sh copies it). Returns the folder name.
bool install(const Item& it, const std::string& root, std::string& folder, std::string& err);

}  // namespace pm
