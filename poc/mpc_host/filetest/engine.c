/* Fixture for poc/mpc_host: file access. At create it reads /plugin/seed.txt, stats it, lists /plugin, globs /plugin/*.txt, writes and rereads
 * a file under MPC_HOST_ROOT (/scratch/out.txt) asks dladdr for its own path and sorts with qsort. A parameter reads back exactly what was set only if all of
 * that worked; otherwise it reads back 1 higher (so the host's state check finds no settable parameter and smoke.sh fails). */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <glob.h>
#include <link.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "engine.h"

typedef struct { int ok; double level, tone; } f_t;

/* dl_iterate_phdr: the walk is guest code calling this guest callback; it finds the module that holds self_test() through its program headers */
struct walk { int modules, found; };
static int self_test(void);
static int walk_cb(struct dl_phdr_info *info, size_t size, void *data) {
    struct walk *w = data;
    w->modules++;
    if (size < offsetof(struct dl_phdr_info, dlpi_phnum) + sizeof info->dlpi_phnum || !info->dlpi_phdr) return 0;
    for (int k = 0; k < info->dlpi_phnum; k++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[k];
        if (ph->p_type == PT_LOAD) {
            const uintptr_t lo = info->dlpi_addr + ph->p_vaddr, hi = lo + ph->p_memsz, me = (uintptr_t)self_test;
            if (me >= lo && me < hi) { w->found = 1; return 42; }   /* a non-zero return ends the walk and is returned by dl_iterate_phdr */
        }
    }
    return 0;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int self_test(void) {
    struct walk wk = {0, 0};
    if (dl_iterate_phdr(walk_cb, &wk) != 42 || !wk.found || wk.modules < 1) return 0;
    int v[] = {5, 2, 9, 1, 7, 3}, key = 7;   /* qsort and bsearch call back into guest code: they run from guest_helpers.so */
    qsort(v, 6, sizeof v[0], cmp_int);
    if (v[0] != 1 || v[5] != 9 || !bsearch(&key, v, 6, sizeof v[0], cmp_int)) return 0;
    char line[64] = {0};
    FILE *f = fopen("/plugin/seed.txt", "r");
    if (!f || !fgets(line, sizeof line, f) || atoi(line) != 42) return 0;
    fseek(f, 0, SEEK_END);
    if (ftell(f) != 3) { fclose(f); return 0; }
    fclose(f);
    struct stat st;
    if (stat("/plugin/seed.txt", &st) != 0 || st.st_size != 3 || !S_ISREG(st.st_mode)) return 0;
    int seen = 0; DIR *d = opendir("/plugin");
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d));) if (!strcmp(e->d_name, "seed.txt")) seen = 1;
    closedir(d);
    if (!seen) return 0;
    glob_t g; if (glob("/plugin/*.txt", 0, NULL, &g) != 0 || g.gl_pathc < 1 || strncmp(g.gl_pathv[0], "/plugin/", 8)) return 0;
    globfree(&g);
    mkdir("/scratch", 0755);
    f = fopen("/scratch/out.txt", "w");
    if (!f) return 0;
    fprintf(f, "7 %s\n", "seven"); fclose(f);
    f = fopen("/scratch/out.txt", "r"); int n = 0; char w[16] = {0};
    if (!f || fscanf(f, "%d %15s", &n, w) != 2 || n != 7 || strcmp(w, "seven")) { if (f) fclose(f); return 0; }
    fclose(f);
    if (fopen("/nonexistent/x", "r")) return 0;
    Dl_info di; if (!dladdr((void *)self_test, &di) || !di.dli_fname || strncmp(di.dli_fname, "/plugin/", 8)) return 0;
    return 1;
}

static void *create(const char *d) { (void)d; f_t *s = calloc(1, sizeof *s); s->ok = self_test(); return s; }
static void destroy(void *i) { free(i); }
static void midi(void *i, const uint8_t *m, int n) { (void)i; (void)m; (void)n; }
static void set_param(void *i, const char *k, const char *v) {
    f_t *s = i;
    if (!strcmp(k, "level")) s->level = atof(v) + (s->ok ? 0 : 1);
    else if (!strcmp(k, "tone")) s->tone = atof(v) + (s->ok ? 0 : 1);
    else if (!strcmp(k, "state")) sscanf(v, "level=%lf;tone=%lf", &s->level, &s->tone);
}
static int get_param(void *i, const char *k, char *b, int n) {
    f_t *s = i;
    if (!strcmp(k, "level")) return snprintf(b, n, "%g", s->level);
    if (!strcmp(k, "tone")) return snprintf(b, n, "%g", s->tone);
    if (!strcmp(k, "state")) return snprintf(b, n, "level=%g;tone=%g", s->level, s->tone);
    return 0;
}
static void render(void *i, int16_t *o, int f) { (void)i; memset(o, 0, (size_t)f * 4); }

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
