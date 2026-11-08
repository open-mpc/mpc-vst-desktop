/* libc functions that take a guest callback. A host thunk cannot call back into guest code, so these run as guest code: build.sh compiles
 * this for armhf (-nostdlib; malloc/free/memcpy resolve to the host's thunks) and mpc_host loads it before any plugin. */
typedef __SIZE_TYPE__ size_t;
extern void *malloc(size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
typedef int (*cmp_t)(const void *, const void *);

/* stable merge sort, like glibc's qsort for the sizes plugins use */
void qsort(void *base, size_t n, size_t sz, cmp_t cmp) {
    if (n < 2 || !sz) return;
    char *a = base, *tmp = malloc(n * sz);
    if (!tmp) {   /* no memory: insertion sort in place */
        for (size_t i = 1; i < n; i++)
            for (size_t j = i; j > 0 && cmp(a + (j - 1) * sz, a + j * sz) > 0; j--)
                for (size_t k = 0; k < sz; k++) { char t = a[(j - 1) * sz + k]; a[(j - 1) * sz + k] = a[j * sz + k]; a[j * sz + k] = t; }
        return;
    }
    char *src = a, *dst = tmp;
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (cmp(src + j * sz, src + i * sz) < 0) { memcpy(dst + k * sz, src + j * sz, sz); j++; }
                else { memcpy(dst + k * sz, src + i * sz, sz); i++; }
                k++;
            }
            while (i < mid) { memcpy(dst + k * sz, src + i * sz, sz); i++; k++; }
            while (j < hi) { memcpy(dst + k * sz, src + j * sz, sz); j++; k++; }
        }
        char *t = src; src = dst; dst = t;
    }
    if (src != a) memcpy(a, src, n * sz);
    free(tmp);
}

void *bsearch(const void *key, const void *base, size_t n, size_t sz, cmp_t cmp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *p = (const char *)base + mid * sz;
        int c = cmp(key, p);
        if (c == 0) return (void *)p;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

/* A libcurl lookalike for guests that dlopen("libcurl.so.4") (the Plugin Manager): the host lends HTTP GET through __mpc_http_* thunks, only when the
 * embedding host provides it (Guest::hooks.http_get). Guest code, because the write and progress callbacks are guest functions. Easy interface only,
 * and only the options the manager sets (URL, write and progress callbacks, resume offset); the rest are accepted and ignored. */
#include <stdarg.h>
typedef __SIZE_TYPE__ sz_t;
extern int __mpc_http_open(const char *, long);
extern long __mpc_http_total(int);
extern long __mpc_http_read(int, void *, long);
extern int __mpc_http_close(int);
struct mpc_curl {
    char url[1024];
    sz_t (*wf)(char *, sz_t, sz_t, void *);
    void *wd;
    int (*xf)(void *, long long, long long, long long, long long);
    void *xd;
    long long from;
};
void *curl_easy_init(void) {
    struct mpc_curl *c = malloc(sizeof *c);
    if (c) { char *p = (char *)c; for (sz_t i = 0; i < sizeof *c; i++) p[i] = 0; }
    return c;
}
void curl_easy_cleanup(void *h) { free(h); }
int curl_easy_setopt(void *h, int opt, ...) {
    struct mpc_curl *c = h;
    va_list ap;
    va_start(ap, opt);
    if (opt == 10002) { const char *u = va_arg(ap, const char *); sz_t i = 0; for (; u && u[i] && i < sizeof c->url - 1; i++) c->url[i] = u[i]; c->url[i] = 0; }
    else if (opt == 20011) c->wf = va_arg(ap, void *);
    else if (opt == 10001) c->wd = va_arg(ap, void *);
    else if (opt == 20219) c->xf = va_arg(ap, void *);
    else if (opt == 10057) c->xd = va_arg(ap, void *);
    else if (opt == 30116) c->from = va_arg(ap, long long);
    va_end(ap);
    return 0;
}
int curl_easy_perform(void *h) {
    struct mpc_curl *c = h;
    int hd = __mpc_http_open(c->url, (long)c->from);
    if (hd < 0) return 6;   /* CURLE_COULDNT_RESOLVE_HOST: whatever went wrong, the catalog is out of reach */
    long total = __mpc_http_total(hd), done = 0, n;
    char *buf = malloc(16384);
    int rc = 0;
    if (!buf) { __mpc_http_close(hd); return 27; }
    while ((n = __mpc_http_read(hd, buf, 16384)) > 0) {
        if (c->wf && c->wf(buf, 1, (sz_t)n, c->wd) != (sz_t)n) { rc = 23; break; }
        done += n;
        if (c->xf && c->xf(c->xd, total - (long)c->from, done, 0, 0)) { rc = 42; break; }
    }
    free(buf);
    __mpc_http_close(hd);
    return rc;
}
const char *curl_easy_strerror(int rc) {
    return rc == 0 ? "No error" : rc == 6 ? "Could not resolve host" : rc == 23 ? "Failed writing received data" : rc == 42 ? "Operation was aborted by an application callback" : "Unknown error";
}
