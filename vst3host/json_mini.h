// A small JSON reader (just enough for catalog.json, Q-Links.json and a skin's TUI.json). Header-only.
#pragma once
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace pm {
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    std::string s; double n = 0;
    std::vector<J> a; std::vector<std::pair<std::string, J>> o;
    const J* get(const char* k) const { for (auto& kv : o) if (kv.first == k) return &kv.second; return nullptr; }
    std::string str(const char* k) const { const J* j = get(k); return j && j->t == Str ? j->s : ""; }
};
struct P {
    const char* p; const char* e;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool val(J& j, int depth = 0) {
        ws();
        if (p >= e || depth > 64) return false;
        if (*p == '{') {
            j.t = J::Obj; p++; ws();
            if (p < e && *p == '}') { p++; return true; }
            for (;;) {
                ws(); J k; if (!str(k)) return false; ws();
                if (p >= e || *p++ != ':') return false;
                J v; if (!val(v, depth + 1)) return false;
                j.o.emplace_back(std::move(k.s), std::move(v)); ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[') {
            j.t = J::Arr; p++; ws();
            if (p < e && *p == ']') { p++; return true; }
            for (;;) {
                J v; if (!val(v, depth + 1)) return false;
                j.a.push_back(std::move(v)); ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') return str(j);
        if (!strncmp(p, "true", 4)) { j.t = J::Bool; j.n = 1; p += 4; return true; }
        if (!strncmp(p, "false", 5)) { j.t = J::Bool; p += 5; return true; }
        if (!strncmp(p, "null", 4)) { p += 4; return true; }
        char* end; j.n = strtod(p, &end);
        if (end == p) return false;
        j.t = J::Num; p = end; return true;
    }
    bool str(J& j) {
        if (p >= e || *p != '"') return false;
        j.t = J::Str; p++;
        for (; p < e && *p != '"'; p++) {
            if (*p != '\\') { j.s += *p; continue; }
            if (++p >= e) return false;
            switch (*p) {
                case 'n': j.s += '\n'; break; case 't': j.s += '\t'; break; case 'r': j.s += '\r'; break;
                case 'u': {
                    if (e - p < 5) return false;
                    unsigned cp = (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16); p += 4;
                    if (cp < 0x80) j.s += (char)cp;
                    else if (cp < 0x800) { j.s += (char)(0xC0 | cp >> 6); j.s += (char)(0x80 | (cp & 63)); }
                    else { j.s += (char)(0xE0 | cp >> 12); j.s += (char)(0x80 | ((cp >> 6) & 63)); j.s += (char)(0x80 | (cp & 63)); }
                    break;
                }
                default: j.s += *p;
            }
        }
        if (p >= e) return false;
        p++; return true;
    }
};


}  // namespace pm
