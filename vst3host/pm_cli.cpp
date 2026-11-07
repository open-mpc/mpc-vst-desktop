// Command-line front end for the host-native plugin manager service (testing it without a DAW).
//   pm_cli list  [--catalog <url|file>] [--arch aarch64|armv7] [filter]
//   pm_cli install <id> <root> [--catalog <url|file>] [--arch ...]
#include <cstdio>
#include <cstring>
#include "installer.h"
int main(int argc, char** argv) {
    std::string cat = "https://open-mpc.github.io/catalog.json", arch = "aarch64", cmd = argc > 1 ? argv[1] : "", id, root, filter;
    std::vector<std::string> pos;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--catalog") && i + 1 < argc) cat = argv[++i];
        else if (!strcmp(argv[i], "--arch") && i + 1 < argc) arch = argv[++i];
        else pos.push_back(argv[i]);
    }
    std::vector<pm::Item> items; std::string err;
    if (cmd != "list" && cmd != "install") { fprintf(stderr, "usage: pm_cli list [filter] | install <id> <root> [--catalog X] [--arch A]\n"); return 2; }
    if (!pm::load_catalog(cat, arch, items, err)) { fprintf(stderr, "catalog: %s\n", err.c_str()); return 1; }
    if (cmd == "list") {
        for (auto& it : items) if (pos.empty() || it.id.find(pos[0]) != std::string::npos || it.name.find(pos[0]) != std::string::npos)
            printf("%-28s %-8s %-12s %s\n", it.id.c_str(), it.version.c_str(), it.kind.c_str(), it.name.c_str());
        return 0;
    }
    if (pos.size() < 2) return 2;
    for (auto& it : items) if (it.id == pos[0]) {
        std::string folder;
        if (!pm::install(it, pos[1], folder, err)) { fprintf(stderr, "install: %s\n", err.c_str()); return 1; }
        printf("installed %s %s into %s/sdcard/Synths/%s\n", it.name.c_str(), it.version.c_str(), pos[1].c_str(), folder.c_str());
        return 0;
    }
    fprintf(stderr, "no such plugin: %s\n", pos[0].c_str());
    return 1;
}
