#include "devicesim.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "installer.h"

namespace pm {
namespace fs = std::filesystem;

static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static bool spit(const std::string& p, const std::string& d) {
    std::error_code ec; fs::create_directories(fs::path(p).parent_path(), ec);
    std::ofstream f(p, std::ios::binary); f << d; return f.good();
}
// the single-quoted words of a shell command line, in order ('a b' 'c' -> a b, c)
static std::vector<std::string> qwords(const std::string& s) {
    std::vector<std::string> v;
    for (size_t i = 0; i < s.size(); i++) if (s[i] == '\'') { size_t j = s.find('\'', i + 1); if (j == std::string::npos) break; v.push_back(s.substr(i + 1, j - i - 1)); i = j; }
    return v;
}
// the word after the first "> " (an output redirection, quoted or not)
static std::string redirect(const std::string& cmd) {
    size_t a = cmd.find("> ");
    if (a == std::string::npos) return "";
    a += 2;
    if (cmd[a] == '\'') { size_t b = cmd.find('\'', a + 1); return b == std::string::npos ? "" : cmd.substr(a + 1, b - a - 1); }
    size_t b = cmd.find(' ', a);
    return cmd.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
static std::vector<uint8_t> read_bin(const std::string& p) { std::vector<uint8_t> b; std::string e; http_get(p, b, e); return b; }

void DeviceSim::log(const std::string& m) { std::ofstream f(root + "/tmp/openmpc-pm/devicesim.log", std::ios::app); f << m << "\n"; }

void DeviceSim::setup() {
    std::error_code ec;
    for (const char* d : {"data/Settings/MPC", "nvme/Synths", "usr/bin", "sys/fs/cgroup", "proc", "tmp/openmpc-pm", "var/volatile/tmp/filmstrips"}) fs::create_directories(fs::path(root) / d, ec);
    for (const char* t : {"systemctl", "systemd-run", "unzip", "sha256sum", "pidof"}) if (!fs::exists(fs::path(root) / "usr/bin" / t)) spit(root + "/usr/bin/" + t, "#!/bin/sh\nexit 0\n");
    spit(root + "/sys/fs/cgroup/cgroup.procs", "");
    spit(root + "/proc/mounts", "/dev/root / ext4 rw,relatime 0 0\n/dev/nvme0n1p1 /nvme exfat rw,relatime 0 0\n");
    if (!fs::exists(fs::path(root) / "data/Settings/MPC/MPC.settings")) write_settings();
}

// MPC.settings, the real format trimmed to what the manager reads: the content locations and the Gen2 plugin list (from each plugin folder's plugin-meta.xml)
void DeviceSim::write_settings() {
    std::string entries;
    std::error_code ec;
    const fs::path syn = fs::path(root) / kSynths;
    std::vector<fs::path> dirs;
    for (auto& d : fs::directory_iterator(syn, ec)) if (d.is_directory(ec)) dirs.push_back(d.path());
    std::sort(dirs.begin(), dirs.end());
    for (auto& d : dirs) {
        std::string meta = slurp((d / "plugin-meta.xml").string());
        if (meta.empty()) continue;
        for (size_t at; (at = meta.find("%payload-path%")) != std::string::npos; ) meta.replace(at, 14, std::string("/") + kSynths);
        while (!meta.empty() && (meta.back() == '\n' || meta.back() == '\r')) meta.pop_back();
        entries += meta + "\n";
    }
    spit(root + "/data/Settings/MPC/MPC.settings",
         std::string("<PROPERTIES>\n<VALUE name=\"SynthContentLocations\"><SynthContentLocations><Location>/") + kSynths + "</Location><Location>/usr/share/Akai/Content/Synths</Location></SynthContentLocations></VALUE>\n"
         "<VALUE name=\"pluginList-arm-64bit\"><KNOWNPLUGINS>\n" + entries + "</KNOWNPLUGINS></VALUE>\n</PROPERTIES>\n");
}

bool DeviceSim::remove_folder(const std::string& folder) {
    std::error_code ec;
    fs::remove_all(folder, ec);
    return !ec;
}

// install.sh without the shell: portable/<skin folder> into the Synths folder (the user's own files in an old install stay), %payload-path% in the presets, the entry in MPC.settings
bool DeviceSim::install_pkg(const std::string& pkgdir, const std::string& synths, std::string& err) {
    std::error_code ec;
    fs::path src;
    for (auto& f : fs::directory_iterator(fs::path(pkgdir) / "portable", ec)) if (f.is_directory(ec)) { src = f.path(); break; }
    if (src.empty()) { err = "this package is damaged: portable/<folder> is missing"; return false; }
    fs::path dst = fs::path(synths) / src.filename();
    fs::create_directories(dst.parent_path(), ec);
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) { err = "cannot copy to " + dst.string() + ": " + ec.message(); return false; }
    for (auto& f : fs::directory_iterator(dst / "Presets", ec)) {
        if (f.path().extension() != ".xpl") continue;
        std::string d = slurp(f.path().string());
        for (size_t at; (at = d.find("%payload-path%")) != std::string::npos; ) d.replace(at, 14, std::string("/") + kSynths);
        spit(f.path().string(), d);
    }
    write_settings();
    return true;
}

// apply.sh, read for the lines the manager writes ("item 'id' 'dir' act 'ver' sh '<pkg>/install.sh' ... -t '<synths>' && setstate 'id' 'ver' && rm -rf ...") and done directly
int DeviceSim::apply(mpchost::Guest& g, const std::string& script) {
    std::string sh = slurp(script), result;
    std::istringstream in(sh);
    std::string line, state_path, wk = fs::path(script).parent_path().string();
    auto guest_to_host = [&](const std::string& gp) { std::string h; return g.host_path(gp, h) ? h : gp; };
    while (std::getline(in, line)) {
        if (line.compare(0, 6, "STATE=") == 0) { auto q = qwords(line); if (!q.empty()) state_path = guest_to_host(q[0]); continue; }
        if (line.compare(0, 6, "item '") != 0) continue;
        auto q = qwords(line);   // id, dir, [act is bare], ver, script, ... synths
        if (q.size() < 4) continue;
        const std::string id = q[0], dir = q[1], ver = q[2];
        const bool is_install = line.find(" install '") != std::string::npos, is_remove = line.find(" remove '") != std::string::npos;
        std::string err;
        if (is_install && q.size() >= 4) {
            const std::string script_path = q[3];   // <pkgdir>/install.sh
            size_t tp = line.find("-t '");
            const std::string synths = tp == std::string::npos ? std::string("/") + kSynths : qwords(line.substr(tp))[0];
            const std::string pkgdir = guest_to_host(script_path.substr(0, script_path.rfind('/')));
            if (install_pkg(pkgdir, guest_to_host(synths), err)) result += id + "|ok|install " + ver + "\n";
            else result += id + "|fail|" + err + "\n";
            if (err.empty() && !state_path.empty()) {   // setstate
                std::string st = slurp(state_path), out;
                std::istringstream si(st); std::string l;
                while (std::getline(si, l)) if (l.compare(0, id.size() + 1, id + " ") != 0) out += l + "\n";
                spit(state_path, out + id + " " + ver + "\n");
            }
        } else if (is_remove) {
            if (!dir.empty() && remove_folder(guest_to_host(dir))) { write_settings(); result += id + "|ok|remove \n"; }
            else result += id + "|fail|could not remove the folder\n";
            if (!state_path.empty()) {
                std::string st = slurp(state_path), out; std::istringstream si(st); std::string l;
                while (std::getline(si, l)) if (l.compare(0, id.size() + 1, id + " ") != 0) out += l + "\n";
                spit(state_path, out);
            }
        }
        std::error_code ec;   // rm -rf <WORK>/id <WORK>/id.zip
        fs::remove_all(fs::path(wk) / id, ec); fs::remove(fs::path(wk) / (id + ".zip"), ec);
    }
    log("apply " + script + " result: " + result);
    spit(wk + "/result", result);
    std::error_code ec; fs::remove(script, ec);
    if (on_restart) on_restart();
    return 0;
}

int DeviceSim::run_sh(mpchost::Guest& g, const std::string& cmd) {
    auto H = [&](const std::string& gp) { std::string h; return g.host_path(gp, h) ? h : gp; };
    auto q = qwords(cmd);
    if (cmd.compare(0, 6, "echo '") == 0 && cmd.find("sha256sum -c -") != std::string::npos && !q.empty()) {   // echo 'SHA  PATH' | sha256sum -c -
        size_t sp = q[0].find("  ");
        if (sp == std::string::npos) return 1;
        auto b = read_bin(H(q[0].substr(sp + 2)));
        return !b.empty() && sha256_hex(b.data(), b.size()) == q[0].substr(0, sp) ? 0 : 1;
    }
    if (cmd.compare(0, 10, "unzip -p '") == 0 && q.size() >= 5) {   // unzip -p 'zip' '*/mpc-plugin.json' > 'json' 2>/dev/null; sha256sum 'zip' | cut -c1-64 > 'sha'
        auto z = read_bin(H(q[0]));
        std::vector<ZipEntry> es; std::string err;
        std::vector<uint8_t> js;
        if (zip_list(z, es, err)) for (auto& e : es) if (e.name.size() > 16 && e.name.compare(e.name.size() - 16, 16, "/mpc-plugin.json") == 0) { zip_read(z, e.name, js, err); break; }
        spit(H(q[2]), std::string(js.begin(), js.end()));
        spit(H(q[4]), sha256_hex(z.data(), z.size()) + "\n");
        return 0;
    }
    if (cmd.compare(0, 10, "unzip -l '") == 0 && !q.empty() && !redirect(cmd).empty()) {   // unzip -l 'zip' > 'listing' 2>&1  (Info-ZIP's format)
        auto z = read_bin(H(q[0]));
        std::vector<ZipEntry> es; std::string err, out;
        if (!zip_list(z, es, err)) { spit(H(redirect(cmd)), "unzip:  cannot find zipfile directory\n"); return 9; }
        out = "Archive:  " + q[0] + "\n  Length      Date    Time    Name\n---------  ---------- -----   ----\n";
        uint64_t total = 0;
        for (auto& e : es) { char b[64]; snprintf(b, sizeof b, "%9llu  2026-01-01 00:00   ", (unsigned long long)e.size); out += b + e.name + "\n"; total += e.size; }
        char b[96]; snprintf(b, sizeof b, "---------                     -------\n%9llu                     %zu files\n", (unsigned long long)total, es.size());
        spit(H(redirect(cmd)), out + b);
        return 0;
    }
    if (cmd.find("removing leading") != std::string::npos || cmd.find("t += $1") != std::string::npos) return 0;   // the zip listing checks: names were validated when listing, size is small
    if (cmd.compare(0, 8, "rm -rf '") == 0 && cmd.find("unzip -q -o '") != std::string::npos && q.size() >= 3) {   // rm -rf 'dir' && unzip -q -o 'zip' -d 'dest'
        std::error_code ec; fs::remove_all(H(q[0]), ec);
        std::string err;
        return unzip(read_bin(H(q[1])), H(q[2]), "", err) ? 0 : 1;
    }
    if (cmd.compare(0, 8, "echo $$ ") == 0 && cmd.find("apply.sh") != std::string::npos) {   // the fallback restart (no systemd-run)
        size_t a = cmd.find("/bin/sh "), b = cmd.find(" </dev/null");
        if (a != std::string::npos && b != std::string::npos) return apply(g, H(cmd.substr(a + 8, b - a - 8)));
    }
    log("unhandled: " + cmd);
    return 127;
}

int DeviceSim::spawn(mpchost::Guest& g, const std::vector<std::string>& argv) {
    if (argv.size() >= 3 && (argv[0] == "/bin/sh" || argv[0] == "sh") && argv[1] == "-c") return run_sh(g, argv[2]);
    if (!argv.empty() && argv[0] == "systemd-run") {   // systemd-run --unit=... --collect /bin/sh <apply.sh>: MPC "restarts" and the apply runs
        for (size_t i = 1; i + 1 < argv.size(); i++) if ((argv[i] == "/bin/sh" || argv[i] == "sh") && i + 1 < argv.size()) {
            std::string h;
            return g.host_path(argv[i + 1], h) ? apply(g, h) : 1;
        }
    }
    std::string all;
    for (auto& a : argv) all += a + " ";
    log("unhandled spawn: " + all);
    return -1000;
}

}  // namespace pm
