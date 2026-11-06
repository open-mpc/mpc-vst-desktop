/* libc functions that take a guest callback. A host thunk cannot call back into guest code, so these run as guest code: build.sh compiles
 * this for armhf (-nostdlib; malloc/free/memcpy resolve to the host's thunks) and mpc_host loads it before any plugin. */
typedef unsigned int size_t;
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
