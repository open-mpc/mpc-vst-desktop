// The operating-system side of the guest's files, time and C locale tables, behind one interface so guest.cpp has no platform #ifdefs in its thunks.
// POSIX hosts use the C library directly; Windows uses the CRT (_stat64, _open ...) and std::filesystem. Guest flag values are the ARM / AArch64 Linux ones.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace mpchost::hostfs {

struct Stat { uint64_t dev = 0, ino = 0, size = 0, blocks = 0, rdev = 0; uint32_t mode = 0, nlink = 1, uid = 0, gid = 0, blksize = 4096; int64_t atime = 0, mtime = 0, ctime = 0; };

inline bool stat(const std::string& p, Stat& s, bool follow = true) {
#ifdef _WIN32
    struct _stat64 st; (void)follow;
    if (::_stat64(p.c_str(), &st) != 0) return false;
    s.blocks = ((uint64_t)st.st_size + 511) / 512; s.dev = (uint64_t)st.st_dev; s.rdev = (uint64_t)st.st_rdev; s.nlink = (uint32_t)st.st_nlink;
#else
    struct ::stat st;
    if ((follow ? ::stat(p.c_str(), &st) : ::lstat(p.c_str(), &st)) != 0) return false;
    s.blocks = (uint64_t)st.st_blocks; s.dev = (uint64_t)st.st_dev; s.rdev = (uint64_t)st.st_rdev; s.nlink = (uint32_t)st.st_nlink; s.ino = (uint64_t)st.st_ino;
    s.uid = st.st_uid; s.gid = st.st_gid; s.blksize = (uint32_t)st.st_blksize;
#endif
    s.mode = (uint32_t)st.st_mode; s.size = (uint64_t)st.st_size; s.atime = (int64_t)st.st_atime; s.mtime = (int64_t)st.st_mtime; s.ctime = (int64_t)st.st_ctime;
    return true;
}
inline bool fstat(int fd, Stat& s) {
#ifdef _WIN32
    struct _stat64 st; if (::_fstat64(fd, &st) != 0) return false;
    s.blocks = ((uint64_t)st.st_size + 511) / 512;
#else
    struct ::stat st; if (::fstat(fd, &st) != 0) return false;
    s.blocks = (uint64_t)st.st_blocks; s.ino = (uint64_t)st.st_ino; s.uid = st.st_uid; s.gid = st.st_gid; s.blksize = (uint32_t)st.st_blksize;
#endif
    s.mode = (uint32_t)st.st_mode; s.size = (uint64_t)st.st_size; s.atime = (int64_t)st.st_atime; s.mtime = (int64_t)st.st_mtime; s.ctime = (int64_t)st.st_ctime;
    return true;
}

// Linux (arm, aarch64) open flags as the guest passes them
inline int open(const std::string& path, uint32_t g, uint32_t mode) {
    const uint32_t acc = g & 3;
#ifdef _WIN32
    int fl = acc == 0 ? _O_RDONLY : acc == 1 ? _O_WRONLY : _O_RDWR;
    if (g & 0100) fl |= _O_CREAT;
    if (g & 0200) fl |= _O_EXCL;
    if (g & 01000) fl |= _O_TRUNC;
    if (g & 02000) fl |= _O_APPEND;
    return ::_open(path.c_str(), fl | _O_BINARY, mode & 0600);
#else
    int fl = acc == 0 ? O_RDONLY : acc == 1 ? O_WRONLY : O_RDWR;
    if (g & 0100) fl |= O_CREAT;
    if (g & 0200) fl |= O_EXCL;
    if (g & 01000) fl |= O_TRUNC;
    if (g & 02000) fl |= O_APPEND;
    if (g & 04000) fl |= O_NONBLOCK;
    if (g & 040000) fl |= O_DIRECTORY;
    if (g & 0100000) fl |= O_NOFOLLOW;
    return ::open(path.c_str(), fl, (mode_t)mode);
#endif
}
inline long long read(int fd, void* b, size_t n) {
#ifdef _WIN32
    return ::_read(fd, b, (unsigned)n);
#else
    return ::read(fd, b, n);
#endif
}
inline long long write(int fd, const void* b, size_t n) {
#ifdef _WIN32
    return ::_write(fd, b, (unsigned)n);
#else
    return ::write(fd, b, n);
#endif
}
inline long long lseek(int fd, long long off, int whence) {
#ifdef _WIN32
    return ::_lseeki64(fd, off, whence);
#else
    return ::lseek(fd, (off_t)off, whence);
#endif
}
inline int close(int fd) {
#ifdef _WIN32
    return ::_close(fd);
#else
    return ::close(fd);
#endif
}
inline int access(const std::string& p, int mode) {
#ifdef _WIN32
    return ::_access(p.c_str(), mode & 6);   // F_OK 0, W_OK 2, R_OK 4 (X_OK is not tested on Windows)
#else
    return ::access(p.c_str(), mode);
#endif
}
inline int fseek64(FILE* f, long long off, int whence) {
#ifdef _WIN32
    return ::_fseeki64(f, off, whence);
#else
    return ::fseeko(f, (off_t)off, whence);
#endif
}
inline long long ftell64(FILE* f) {
#ifdef _WIN32
    return ::_ftelli64(f);
#else
    return ::ftello(f);
#endif
}
inline int mkdir(const std::string& p) {   // 0 or an errno value
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return 17;   // EEXIST
    if (std::filesystem::create_directory(p, ec)) return 0;
    return 2;                                         // ENOENT: the parent is missing
}

// A directory listing taken when it is opened: names and DT_* types, "." and ".." first like readdir
struct Dir { std::vector<std::pair<std::string, uint8_t>> ents; size_t pos = 0; };
inline bool listdir(const std::string& p, Dir& d) {
    std::error_code ec;
    if (!std::filesystem::is_directory(p, ec)) return false;
    d.ents.push_back({".", 4}); d.ents.push_back({"..", 4});
    for (auto it = std::filesystem::directory_iterator(p, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        d.ents.push_back({it->path().filename().string(), (uint8_t)(it->is_directory(ec) ? 4 : it->is_regular_file(ec) ? 8 : 0)});
    return true;
}

// '*' and '?' wildcards inside one path component
inline bool wild(const char* pat, const char* s) {
    if (!*pat) return !*s;
    if (*pat == '*') { for (;; s++) { if (wild(pat + 1, s)) return true; if (!*s) return false; } }
    if (!*s) return false;
    return (*pat == '?' || *pat == *s) && wild(pat + 1, s + 1);
}
// glob for an absolute pattern with wildcards in any component; sorted like glob(3)
inline std::vector<std::string> glob(const std::string& pattern) {
    std::vector<std::string> comps, out;
    for (size_t b = 0; b <= pattern.size();) { size_t e = pattern.find('/', b); if (e == std::string::npos) e = pattern.size(); if (e > b) comps.push_back(pattern.substr(b, e - b)); b = e + 1; }
    const std::string base = pattern.rfind('/', 0) == 0 ? "" : std::string();   // "/x" -> root; a Windows "C:/x" has its drive as the first component
    auto walk = [&](auto&& self, const std::string& dir, size_t k) -> void {
        if (k == comps.size()) { std::error_code ec; if (std::filesystem::exists(dir.empty() ? "/" : dir, ec)) out.push_back(dir); return; }
        const std::string& c = comps[k];
        if (c.find_first_of("*?") == std::string::npos) { self(self, dir + "/" + c, k + 1); return; }
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dir.empty() ? "/" : dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            const std::string n = it->path().filename().string();
            if (n[0] == '.' && c[0] != '.') continue;   // as glob(3): wildcards do not match a leading dot
            if (wild(c.c_str(), n.c_str())) self(self, dir + "/" + n, k + 1);
        }
    };
    if (!comps.empty() && comps[0].size() == 2 && comps[0][1] == ':') walk(walk, comps[0], 1);   // a drive letter
    else walk(walk, base, 0);
    std::sort(out.begin(), out.end());
    return out;
}

// The "C" locale's ctype tables in glibc's layout (little-endian): index -128..255, so 384 entries, as returned by __ctype_b_loc & friends
inline void ctype_b(uint16_t t[384]) {
    for (int k = 0; k < 384; k++) {
        const int c = k - 128; uint16_t b = 0;
        if (c >= 0 && c < 128) {
            const bool up = c >= 'A' && c <= 'Z', lo = c >= 'a' && c <= 'z', dg = c >= '0' && c <= '9', sp = c == ' ' || (c >= 9 && c <= 13), pr = c >= 32 && c < 127;
            const bool al = up || lo, cn = c < 32 || c == 127, pu = pr && !al && !dg && c != ' ';
            if (up) b |= 0x100;
            if (lo) b |= 0x200;
            if (al) b |= 0x400;
            if (dg) b |= 0x800;
            if (dg || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) b |= 0x1000;
            if (sp) b |= 0x2000;
            if (pr) b |= 0x4000;
            if (pr && c != ' ') b |= 0x8000;
            if (c == ' ' || c == '\t') b |= 0x1;
            if (cn) b |= 0x2;
            if (pu) b |= 0x4;
            if (al || dg) b |= 0x8;
        }
        t[k] = b;
    }
}
inline void ctype_case(int32_t t[384], bool upper) {
    for (int k = 0; k < 384; k++) {
        const int c = k - 128;
        t[k] = c == -1 ? -1 : c < 0 ? c + 256 : upper ? ((c >= 'a' && c <= 'z') ? c - 32 : c) : ((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
}

// time
inline void realtime(int64_t& sec, int64_t& nsec) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    sec = ns / 1000000000; nsec = ns % 1000000000;
}
inline void monotonic(int64_t& sec, int64_t& nsec) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    sec = ns / 1000000000; nsec = ns % 1000000000;
}

}  // namespace mpchost::hostfs
