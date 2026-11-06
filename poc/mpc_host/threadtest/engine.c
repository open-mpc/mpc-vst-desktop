/* Fixture for poc/mpc_host: every parameter write is handed to a worker thread (mutex + condition variable) and only takes effect
 * when the worker has run. A value that reads back therefore proves the host scheduled the worker. "state" is applied directly. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

typedef struct {
    pthread_mutex_t mu; pthread_cond_t cv; pthread_t worker;
    int quit, pending; char key[16]; double val;
    double level, tone;
} th_t;

static void apply(th_t *s, const char *k, double v) { if (!strcmp(k, "level")) s->level = v; else if (!strcmp(k, "tone")) s->tone = v; }

static void *work(void *arg) {
    th_t *s = arg;
    pthread_mutex_lock(&s->mu);
    for (;;) {
        while (!s->pending && !s->quit) pthread_cond_wait(&s->cv, &s->mu);
        if (s->quit) break;
        apply(s, s->key, s->val);
        s->pending = 0;
    }
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

static void *create(const char *d) {
    (void)d; th_t *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->mu, NULL); pthread_cond_init(&s->cv, NULL);
    pthread_create(&s->worker, NULL, work, s);
    return s;
}
static void destroy(void *i) {
    th_t *s = i;
    pthread_mutex_lock(&s->mu); s->quit = 1; pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->mu);
    pthread_join(s->worker, NULL);
    free(s);
}
static void midi(void *i, const uint8_t *m, int n) { (void)i; (void)m; (void)n; }
static void set_param(void *i, const char *k, const char *v) {
    th_t *s = i;
    pthread_mutex_lock(&s->mu);
    if (!strcmp(k, "state")) { sscanf(v, "level=%lf;tone=%lf", &s->level, &s->tone); }
    else { snprintf(s->key, sizeof s->key, "%s", k); s->val = atof(v); s->pending = 1; pthread_cond_signal(&s->cv); }
    pthread_mutex_unlock(&s->mu);
}
static int get_param(void *i, const char *k, char *b, int n) {
    th_t *s = i; int len = 0;
    pthread_mutex_lock(&s->mu);
    if (!strcmp(k, "level")) len = snprintf(b, n, "%g", s->level);
    else if (!strcmp(k, "tone")) len = snprintf(b, n, "%g", s->tone);
    else if (!strcmp(k, "state")) len = snprintf(b, n, "level=%g;tone=%g", s->level, s->tone);
    pthread_mutex_unlock(&s->mu);
    return len;
}
static void render(void *i, int16_t *o, int f) { (void)i; memset(o, 0, (size_t)f * 4); }

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
