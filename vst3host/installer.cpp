#include "installer.h"
#include "json_mini.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace pm {
namespace fs = std::filesystem;

// ---- SHA-256 ----------------------------------------------------------------------------------------------------------
std::string sha256_hex(const uint8_t* d, size_t n) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rot = [](uint32_t x, int k) { return (x >> k) | (x << (32 - k)); };
    std::vector<uint8_t> m(d, d + n);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 7; i >= 0; i--) m.push_back((uint8_t)(bits >> (i * 8)));
    for (size_t o = 0; o < m.size(); o += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)m[o + i * 4] << 24 | m[o + i * 4 + 1] << 16 | m[o + i * 4 + 2] << 8 | m[o + i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rot(w[i - 15], 7) ^ rot(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rot(w[i - 2], 17) ^ rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rot(e, 6) ^ rot(e, 11) ^ rot(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rot(a, 2) ^ rot(a, 13) ^ rot(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = dd + t1; dd = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += dd; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65];
    for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
    return out;
}

// ---- inflate (RFC 1951) -----------------------------------------------------------------------------------------------
namespace {
struct Bits {
    const uint8_t *p, *end; uint32_t buf = 0; int cnt = 0; bool bad = false;
    int get(int n) {
        while (cnt < n) { if (p >= end) { bad = true; return 0; } buf |= (uint32_t)*p++ << cnt; cnt += 8; }
        int v = buf & ((1u << n) - 1); buf >>= n; cnt -= n; return v;
    }
};
struct Huff { uint16_t count[16], symbol[288]; };
void build(Huff& h, const uint8_t* len, int n) {
    memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; i++) h.count[len[i]]++;
    h.count[0] = 0;
    uint16_t offs[16] = {};
    for (int i = 1; i < 16; i++) offs[i] = offs[i - 1] + h.count[i - 1];
    for (int i = 0; i < n; i++) if (len[i]) h.symbol[offs[len[i]]++] = (uint16_t)i;
}
int decode(Bits& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= b.get(1);
        int c = h.count[l];
        if (code - c < first) return h.symbol[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
        if (b.bad) return -1;
    }
    return -1;
}
bool inflate(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    static const uint16_t lbase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint16_t lext[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint16_t dext[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    Bits b{src, src + n};
    int last;
    do {
        last = b.get(1);
        int type = b.get(2);
        if (b.bad) return false;
        if (type == 0) {
            b.buf = 0; b.cnt = 0;
            if (b.end - b.p < 4) return false;
            unsigned len = b.p[0] | b.p[1] << 8;
            b.p += 4;
            if ((size_t)(b.end - b.p) < len) return false;
            out.insert(out.end(), b.p, b.p + len); b.p += len;
        } else if (type == 1 || type == 2) {
            Huff lit, dist;
            if (type == 1) {
                uint8_t l[320];
                int i = 0;
                for (; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                build(lit, l, 288);
                for (i = 0; i < 30; i++) l[i] = 5;
                build(dist, l, 30);
            } else {
                int nlen = b.get(5) + 257, ndist = b.get(5) + 1, ncode = b.get(4) + 4;
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t l[320] = {};
                for (int i = 0; i < ncode; i++) l[order[i]] = (uint8_t)b.get(3);
                Huff cl; build(cl, l, 19);
                uint8_t ll[320] = {};
                int i = 0;
                while (i < nlen + ndist) {
                    int s = decode(b, cl);
                    if (s < 0) return false;
                    if (s < 16) ll[i++] = (uint8_t)s;
                    else {
                        int rep, v = 0;
                        if (s == 16) { if (!i) return false; v = ll[i - 1]; rep = 3 + b.get(2); }
                        else if (s == 17) rep = 3 + b.get(3);
                        else rep = 11 + b.get(7);
                        if (i + rep > nlen + ndist) return false;
                        while (rep--) ll[i++] = (uint8_t)v;
                    }
                }
                build(lit, ll, nlen); build(dist, ll + nlen, ndist);
            }
            for (;;) {
                int s = decode(b, lit);
                if (s < 0) return false;
                if (s < 256) out.push_back((uint8_t)s);
                else if (s == 256) break;
                else {
                    s -= 257;
                    if (s >= 29) return false;
                    int len = lbase[s] + b.get(lext[s]);
                    int ds = decode(b, dist);
                    if (ds < 0 || ds >= 30) return false;
                    size_t d = dbase[ds] + b.get(dext[ds]);
                    if (d > out.size()) return false;
                    for (int k = 0; k < len; k++) out.push_back(out[out.size() - d]);
                }
            }
        } else return false;
    } while (!last);
    return true;
}
uint32_t u16(const uint8_t* p) { return p[0] | p[1] << 8; }
uint32_t u32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
}  // namespace

bool zip_list(const std::vector<uint8_t>& z, std::vector<ZipEntry>& out, std::string& err) {
    out.clear();
    if (z.size() < 22) { err = "not a zip"; return false; }
    size_t e = z.size() - 22;
    while (e > 0 && u32(&z[e]) != 0x06054b50) { if (z.size() - e > 70000) { err = "not a zip"; return false; } e--; }
    if (u32(&z[e]) != 0x06054b50) { err = "not a zip"; return false; }
    unsigned count = u16(&z[e + 10]);
    size_t pos = u32(&z[e + 16]);
    for (unsigned i = 0; i < count; i++) {
        if (pos + 46 > z.size() || u32(&z[pos]) != 0x02014b50) { err = "damaged zip"; return false; }
        unsigned nl = u16(&z[pos + 28]), xl = u16(&z[pos + 30]), cl = u16(&z[pos + 32]);
        if (pos + 46 + nl > z.size()) { err = "damaged zip"; return false; }
        out.push_back({std::string((const char*)&z[pos + 46], nl), u32(&z[pos + 24])});
        pos += 46 + nl + xl + cl;
    }
    return true;
}
bool zip_read(const std::vector<uint8_t>& z, const std::string& name, std::vector<uint8_t>& out, std::string& err) {
    // reuse unzip(): extract just this entry into a scratch folder is heavier than reading it here, so walk the central directory again
    size_t e = z.size() < 22 ? 0 : z.size() - 22;
    while (e > 0 && u32(&z[e]) != 0x06054b50) e--;
    if (z.size() < 22 || u32(&z[e]) != 0x06054b50) { err = "not a zip"; return false; }
    unsigned count = u16(&z[e + 10]);
    size_t pos = u32(&z[e + 16]);
    for (unsigned i = 0; i < count && pos + 46 <= z.size(); i++) {
        unsigned method = u16(&z[pos + 10]), nl = u16(&z[pos + 28]), xl = u16(&z[pos + 30]), cl = u16(&z[pos + 32]);
        uint64_t csize = u32(&z[pos + 20]), usize = u32(&z[pos + 24]);
        size_t lho = u32(&z[pos + 42]);
        std::string n((const char*)&z[pos + 46], nl);
        pos += 46 + nl + xl + cl;
        if (n != name) continue;
        if (lho + 30 > z.size() || u32(&z[lho]) != 0x04034b50) { err = "damaged zip"; return false; }
        size_t data = lho + 30 + u16(&z[lho + 26]) + u16(&z[lho + 28]);
        if (data + csize > z.size() || usize > (64u << 20)) { err = "damaged zip"; return false; }
        out.clear();
        if (method == 0) out.assign(z.begin() + data, z.begin() + data + csize);
        else if (method == 8) { if (!inflate(&z[data], csize, out) || out.size() != usize) { err = "cannot inflate " + name; return false; } }
        else { err = "unsupported compression"; return false; }
        return true;
    }
    err = "no such entry: " + name;
    return false;
}

bool unzip(const std::vector<uint8_t>& z, const std::string& dest, const std::string& strip, std::string& err) {
    if (z.size() < 22) { err = "not a zip"; return false; }
    size_t e = z.size() - 22;
    while (e > 0 && u32(&z[e]) != 0x06054b50) { if (z.size() - e > 70000) { err = "not a zip"; return false; } e--; }
    if (u32(&z[e]) != 0x06054b50) { err = "not a zip"; return false; }
    unsigned count = u16(&z[e + 10]);
    size_t pos = u32(&z[e + 16]);
    fs::path root(dest);
    std::error_code ec;
    for (unsigned i = 0; i < count; i++) {
        if (pos + 46 > z.size() || u32(&z[pos]) != 0x02014b50) { err = "damaged zip"; return false; }
        unsigned method = u16(&z[pos + 10]);
        uint64_t csize = u32(&z[pos + 20]), usize = u32(&z[pos + 24]);
        unsigned nl = u16(&z[pos + 28]), xl = u16(&z[pos + 30]), cl = u16(&z[pos + 32]);
        size_t lho = u32(&z[pos + 42]);
        if (pos + 46 + nl > z.size()) { err = "damaged zip"; return false; }
        std::string name((const char*)&z[pos + 46], nl);
        pos += 46 + nl + xl + cl;
        if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos || name.find("..") != std::string::npos) { err = "unsafe file name in the zip: " + name; return false; }
        if (name.compare(0, strip.size(), strip) != 0) continue;
        std::string rel = name.substr(strip.size());
        if (rel.empty()) continue;
        fs::path out = root / rel;
        if (name.back() == '/') { fs::create_directories(out, ec); continue; }
        if (lho + 30 > z.size() || u32(&z[lho]) != 0x04034b50) { err = "damaged zip"; return false; }
        size_t data = lho + 30 + u16(&z[lho + 26]) + u16(&z[lho + 28]);
        if (data + csize > z.size() || usize > (800u << 20)) { err = "damaged zip"; return false; }
        std::vector<uint8_t> buf;
        if (method == 0) buf.assign(z.begin() + data, z.begin() + data + csize);
        else if (method == 8) { buf.reserve(usize); if (!inflate(&z[data], csize, buf) || buf.size() != usize) { err = "cannot inflate " + name; return false; } }
        else { err = "unsupported compression in " + name; return false; }
        fs::create_directories(out.parent_path(), ec);
        FILE* f = fopen(out.string().c_str(), "wb");
        if (!f) { err = "cannot write " + out.string(); return false; }
        fwrite(buf.data(), 1, buf.size(), f);
        fclose(f);
    }
    return true;
}

// ---- HTTP ---------------------------------------------------------------------------------------------------------------
static bool read_file(const std::string& p, std::vector<uint8_t>& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    uint8_t b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out.insert(out.end(), b, b + n);
    fclose(f);
    return true;
}
#ifdef _WIN32
static bool https_get(const std::string& url, std::vector<uint8_t>& out, std::string& err) {
    std::wstring w(url.begin(), url.end());
    URL_COMPONENTSW uc{}; wchar_t host[256], path[2048];
    uc.dwStructSize = sizeof uc; uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) { err = "bad URL"; return false; }
    HINTERNET s = WinHttpOpen(L"openmpc-host/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!s) { err = "WinHttpOpen"; return false; }
    HINTERNET c = WinHttpConnect(s, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", std::wstring(path, uc.dwUrlPathLength).c_str(), nullptr, nullptr, nullptr, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (r && WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
        if (code != 200) err = "HTTP " + std::to_string(code);
        else {
            ok = true;
            for (;;) {
                DWORD n = 0; uint8_t b[65536];
                if (!WinHttpReadData(r, b, sizeof b, &n)) { ok = false; err = "read failed"; break; }
                if (!n) break;
                out.insert(out.end(), b, b + n);
            }
        }
    } else err = "request failed (error " + std::to_string(GetLastError()) + ")";
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}
#else
static bool https_get(const std::string& url, std::vector<uint8_t>& out, std::string& err) {
    void* h = dlopen("libcurl.so.4", RTLD_NOW);
    if (!h) { err = "libcurl.so.4 not found"; return false; }
    typedef void* (*init_t)(); typedef int (*setopt_t)(void*, int, ...); typedef int (*perform_t)(void*); typedef void (*clean_t)(void*); typedef const char* (*str_t)(int);
    auto init = (init_t)dlsym(h, "curl_easy_init"); auto setopt = (setopt_t)dlsym(h, "curl_easy_setopt"); auto perform = (perform_t)dlsym(h, "curl_easy_perform");
    auto clean = (clean_t)dlsym(h, "curl_easy_cleanup"); auto strerr = (str_t)dlsym(h, "curl_easy_strerror");
    if (!init || !setopt || !perform || !clean || !strerr) { err = "libcurl incomplete"; return false; }
    void* c = init();
    if (!c) { err = "curl_easy_init"; return false; }
    struct W { static size_t f(char* p, size_t s, size_t n, void* ud) { auto* o = (std::vector<uint8_t>*)ud; o->insert(o->end(), p, p + s * n); return s * n; } };
    setopt(c, 10002, url.c_str()); setopt(c, 52, 1L); setopt(c, 45, 1L); setopt(c, 10018, "openmpc-host/0.1");
    setopt(c, 20011, &W::f); setopt(c, 10001, &out); setopt(c, 78, 15L);
    int rc = perform(c);
    if (rc) err = strerr(rc);
    clean(c);
    return rc == 0;
}
#endif
bool http_get(const std::string& url, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (url.compare(0, 7, "file://") == 0) { if (!read_file(url.substr(7), out)) { err = "cannot read " + url; return false; } return true; }
    if (url.compare(0, 4, "http") != 0) { if (!read_file(url, out)) { err = "cannot read " + url; return false; } return true; }
    return https_get(url, out, err);
}

bool load_catalog(const std::string& url, const std::string& arch, std::vector<Item>& items, std::string& err) {
    std::vector<uint8_t> buf;
    if (!http_get(url, buf, err)) return false;
    J root; P ps{(const char*)buf.data(), (const char*)buf.data() + buf.size()};
    if (!ps.val(root) || root.t != J::Obj) { err = "catalog.json is not valid"; return false; }
    const J* pl = root.get("plugins");
    if (!pl || pl->t != J::Arr) { err = "catalog.json has no plugins"; return false; }
    items.clear();
    for (const J& p : pl->a) {
        const J* vs = p.get("versions");
        if (!vs || vs->t != J::Arr) continue;
        std::string latest = p.str("latest");
        const J* pick = nullptr;
        for (const J& v : vs->a) {   // the catalog's "latest" if it names a stable version, else the first non-yanked stable one
            const J* y = v.get("yanked");
            if ((y && y->n) || v.str("channel") == "beta") continue;
            if (!pick || v.str("version") == latest) pick = &v;
            if (v.str("version") == latest) break;
        }
        if (!pick) continue;
        Item it;
        it.id = p.str("id"); it.name = p.str("name"); it.kind = p.str("kind"); it.summary = p.str("summary"); it.author = p.str("author"); it.version = pick->str("version");
        const J* as = pick->get("assets");
        const J* a = as ? as->get(arch.c_str()) : nullptr;
        if (!a && as) a = as->get(arch == "aarch64" ? "armv7" : "aarch64");
        if (a) { it.url = a->str("url"); it.sha256 = a->str("sha256"); const J* sz = a->get("size"); it.size = sz ? (long long)sz->n : 0; }
        else { it.url = pick->str("url"); it.sha256 = pick->str("sha256"); }
        if (it.id.empty() || it.url.empty() || it.sha256.size() != 64) continue;
        items.push_back(std::move(it));
    }
    return true;
}

bool parse_qlinks(const std::string& path, std::vector<QPage>& pages) {
    std::vector<uint8_t> buf; std::string err;
    pages.clear();
    if (!http_get(path, buf, err)) return false;
    J root; P ps{(const char*)buf.data(), (const char*)buf.data() + buf.size()};
    if (!ps.val(root) || root.t != J::Obj) return false;
    const J* sm = root.get("Screen Mode Q-Links");
    const J* map = sm ? sm->get("map") : nullptr;
    if (!map || map->t != J::Arr) return false;
    for (const J& e : map->a) {
        QPage q;
        for (int i = 0; i <= 16; i++) q.map[i] = -1;
        const J* t = e.get("Tab"); const J* st = e.get("SubTab");
        q.tab = t ? (int)t->n : 0; q.sub = st ? (int)st->n : 0;
        q.column = e.str("Bank Direction") != "Row";
        if (const J* ql = e.get("Q-Links")) for (auto& kv : ql->o) {
            int n = 0;
            if (sscanf(kv.first.c_str(), "Q-Link %d", &n) == 1 && n >= 1 && n <= 16 && kv.second.t == J::Num) q.map[n] = (int)kv.second.n;
        }
        pages.push_back(q);
    }
    return !pages.empty();
}

void name_pages(const std::string& path, std::vector<QPage>& pages) {
    std::vector<uint8_t> buf; std::string err;
    if (!http_get(path, buf, err)) return;
    J root; P ps{(const char*)buf.data(), (const char*)buf.data() + buf.size()};
    if (!ps.val(root)) return;
    const J* pd = root.get("pageData"); const J* tabs = pd ? pd->get("tabs") : nullptr;
    if (!tabs || tabs->t != J::Arr) return;
    for (QPage& q : pages) {
        int same = 0;
        for (const J& t : tabs->a) { const J* k = t.get("fnKeyIndex"); if (k && (int)k->n == q.tab - 1) same++; }
        for (const J& t : tabs->a) {
            const J* k = t.get("fnKeyIndex"); const J* sk = t.get("fnKeySubIndex");
            if (k && (int)k->n == q.tab - 1 && (sk ? (int)sk->n : 0) == q.sub - 1) { q.name = t.str("tabName"); if (same > 1) q.name += " " + std::to_string(q.sub); }
        }
    }
}

bool install(const Item& it, const std::string& root, std::string& folder, std::string& err) {
    std::vector<uint8_t> zip;
    if (!http_get(it.url, zip, err)) return false;
    if (sha256_hex(zip.data(), zip.size()) != it.sha256) { err = "the download does not match the catalog's sha256"; return false; }
    // the package: <top>/portable/<folder>/... (install.sh copies portable/<folder> into a Synths folder)
    fs::path tmp = fs::path(root) / "tmp" / ("pm-" + it.id);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (!unzip(zip, tmp.string(), "", err)) return false;
    fs::path src;
    for (auto& top : fs::directory_iterator(tmp, ec)) {
        fs::path port = top.path() / "portable";
        if (!fs::is_directory(port, ec)) continue;
        for (auto& f : fs::directory_iterator(port, ec)) if (fs::is_directory(f.path(), ec)) { src = f.path(); break; }
    }
    if (src.empty()) { err = "the package has no portable/<folder>"; fs::remove_all(tmp, ec); return false; }
    folder = src.filename().string();
    fs::path dst = fs::path(root) / "nvme" / "Synths" / folder;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);   // files the user added in the folder stay
    std::error_code ec2;
    fs::remove_all(tmp, ec2);
    if (ec) { err = "cannot copy to " + dst.string() + ": " + ec.message(); return false; }
    return true;
}

}  // namespace pm
