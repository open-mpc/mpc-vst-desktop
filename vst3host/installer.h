// The host-native plugin manager service (docs/MPC_VST3_HOST.md, M3): catalog, download, sha256, unzip, install into <root>/nvme/Synths/<name>/.
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

struct ZipEntry { std::string name; uint64_t size = 0; };
bool zip_list(const std::vector<uint8_t>& zip, std::vector<ZipEntry>& out, std::string& err);                          // every entry, as `unzip -l` lists them
bool zip_read(const std::vector<uint8_t>& zip, const std::string& name, std::vector<uint8_t>& out, std::string& err);   // one entry's contents

// catalog.json -> the downloadable items for `arch` ("aarch64" or "armv7"; falls back to the other when a plugin has only one)
bool load_catalog(const std::string& url, const std::string& arch, std::vector<Item>& items, std::string& err);
// Download, verify, unpack and install: <root>/sdcard/Synths/<folder>/ (the package's portable/<folder>, as install.sh copies it). Returns the folder name.
bool install(const Item& it, const std::string& root, std::string& folder, std::string& err);

// A skin's Q-Links.json: per page (Tab, SubTab) which hosted parameter each of the 16 Q-Link keys (numbered 1..16, bottom row first) drives; -1 for none.
struct QPage { int tab = 0, sub = 0; bool column = true; int map[17]; std::string name; };
bool parse_qlinks(const std::string& path, std::vector<QPage>& pages);
// Names the pages from the skin's TUI.json: Q-Links "Tab t, SubTab s" is the tab with fnKeyIndex t-1 and fnKeySubIndex s-1 (its tabName, e.g. "SEQ A").
void name_pages(const std::string& tui_json, std::vector<QPage>& pages);

}  // namespace pm
