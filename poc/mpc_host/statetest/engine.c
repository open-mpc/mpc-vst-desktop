/* Fixture for poc/mpc_host: an engine with two settable parameters and a "state" chunk (key=value text), so the host's
 * setParameter/getParameter and effGetChunk/effSetChunk calls have something to round-trip. Silent output. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

typedef struct { double level, tone; } st_t;

static void *create(const char *d) { (void)d; return calloc(1, sizeof(st_t)); }
static void destroy(void *i) { free(i); }
static void midi(void *i, const uint8_t *m, int n) { (void)i; (void)m; (void)n; }
static void set_param(void *i, const char *k, const char *v) {
    st_t *s = i;
    if (!strcmp(k, "level")) s->level = atof(v);
    else if (!strcmp(k, "tone")) s->tone = atof(v);
    else if (!strcmp(k, "state")) sscanf(v, "level=%lf;tone=%lf", &s->level, &s->tone);
}
static int get_param(void *i, const char *k, char *b, int n) {
    st_t *s = i;
    if (!strcmp(k, "level")) return snprintf(b, n, "%g", s->level);
    if (!strcmp(k, "tone")) return snprintf(b, n, "%g", s->tone);
    if (!strcmp(k, "state")) return snprintf(b, n, "level=%g;tone=%g", s->level, s->tone);
    return 0;
}
static void render(void *i, int16_t *o, int f) { (void)i; memset(o, 0, (size_t)f * 4); }

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
