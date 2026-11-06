// Fixture for poc/mpc_host: C++ with exceptions, std::string and std::map. A parameter above 0.5 is "rejected" by a throw two frames
// deep and caught in set_param; the value is stored either way, so a value reads back only if unwinding works (an uncaught
// or mishandled exception ends in std::terminate and the host stops). Needs MPC_HOST_LIBS (libgcc_s, libstdc++).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
extern "C" {
#include "engine.h"
}

namespace {
struct Inst { std::map<std::string, double> v; int caught = 0; };

void check(double x) { if (x > 0.5) throw std::out_of_range("above 0.5: " + std::to_string(x)); }
void validate(double x) { check(x); }

void *create(const char *) { return new Inst; }
void destroy(void *i) { delete static_cast<Inst *>(i); }
void midi(void *, const uint8_t *, int) {}
void set_param(void *i, const char *k, const char *val) {
    auto *s = static_cast<Inst *>(i);
    if (!strcmp(k, "state")) {
        double a = 0, b = 0;
        sscanf(val, "level=%lf;tone=%lf", &a, &b);
        s->v["level"] = a; s->v["tone"] = b;
        return;
    }
    const double x = atof(val);
    try { validate(x); } catch (const std::exception &e) { s->caught++; }
    s->v[k] = x;
}
int get_param(void *i, const char *k, char *b, int n) {
    auto *s = static_cast<Inst *>(i);
    if (!strcmp(k, "state")) return snprintf(b, n, "level=%g;tone=%g", s->v["level"], s->v["tone"]);
    if (!strcmp(k, "level") || !strcmp(k, "tone")) return snprintf(b, n, "%g", s->v[k]);
    return 0;
}
void render(void *, int16_t *o, int f) { memset(o, 0, (size_t)f * 4); }
const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, nullptr};
}  // namespace

extern "C" const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
