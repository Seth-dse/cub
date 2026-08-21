/* cub_rt.h -- the Cub runtime.
 *
 * Every program cubc emits carries a copy of this file, so the generated C
 * is standalone: one .c file, no libraries, no include paths.
 *
 * Two value shapes matter here:
 *
 *   CubStr   text.  Immutable, NUL-terminated, and passed by value.
 *   CubArr   an array.  A pointer to a heap object, so arrays are shared
 *            when assigned -- the same model as Python lists or Java arrays.
 *
 * Every heap value is recorded in one registry and released when the
 * program ends, so a Cub program never leaks and never frees twice.
 */
#ifndef CUB_RT_H
#define CUB_RT_H

/* `nanosleep`, `clock_gettime`, and `struct timespec` are POSIX rather
 * than ISO C.  Compiling with -std=c99 makes glibc hide everything that
 * is not standard C, so we have to ask for POSIX before any header is
 * read -- which is why this sits above the includes.  macOS is laxer and
 * exposes them either way, which is how this went unnoticed. */
#if !defined(_WIN32)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE 1
#  endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/resource.h>
#endif

typedef struct { const char *data; int64_t len; } CubStr;

typedef struct CubArrS {
    char   *data;
    int64_t len, cap, esz;
} *CubArr;

/* A map is an open-addressed hash table.  Keys are text or whole numbers;
 * values are stored in their own small block so any type fits. */
typedef struct {
    int     state;        /* 0 free, 1 in use, 2 vacated */
    CubStr  skey;
    int64_t ikey;
    void   *value;
} CubMapSlot;

typedef struct CubMapS {
    CubMapSlot *slots;
    int64_t     cap, len;
    int64_t     vsz;
    int         str_key;
} *CubMap;

/* ---------------- allocation registry ---------------- */

enum { CUB_MEM_RAW = 0, CUB_MEM_ARR = 1, CUB_MEM_MAP = 2 };

typedef struct { void *p; int kind; } CubAlloc;

static CubAlloc *cub_allocs = NULL;
static int64_t   cub_nallocs = 0, cub_allocs_cap = 0;

static void cub_oom(void) {
    fprintf(stderr, "\nRuntime error: out of memory\n");
    exit(70);
}

static void cub_track(void *p, int kind) {
    if (cub_nallocs == cub_allocs_cap) {
        int64_t nc = cub_allocs_cap ? cub_allocs_cap * 2 : 256;
        CubAlloc *na = (CubAlloc *)realloc(cub_allocs, (size_t)nc * sizeof(CubAlloc));
        if (!na) cub_oom();
        cub_allocs = na;
        cub_allocs_cap = nc;
    }
    cub_allocs[cub_nallocs].p = p;
    cub_allocs[cub_nallocs].kind = kind;
    cub_nallocs++;
}

static void *cub_alloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) cub_oom();
    cub_track(p, CUB_MEM_RAW);
    return p;
}

static void cub_rt_shutdown(void) {
    for (int64_t i = 0; i < cub_nallocs; i++) {
        if (cub_allocs[i].kind == CUB_MEM_MAP) {
            CubMap m = (CubMap)cub_allocs[i].p;
            for (int64_t j = 0; j < m->cap; j++)
                if (m->slots[j].state == 1) free(m->slots[j].value);
            free(m->slots);
            free(m);
        } else if (cub_allocs[i].kind == CUB_MEM_ARR) {
            CubArr a = (CubArr)cub_allocs[i].p;
            free(a->data);
            free(a);
        } else {
            free(cub_allocs[i].p);
        }
    }
    free(cub_allocs);
    cub_allocs = NULL;
    cub_nallocs = cub_allocs_cap = 0;
}

/* ---------------- errors ---------------- */

static void cub_panic_at(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "\nRuntime error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (file) fprintf(stderr, "\n  at %s:%d\n", file, line);
    else      fprintf(stderr, "\n");
    cub_rt_shutdown();
    exit(70);
}

/* ---------------- arithmetic ---------------- */

/* Signed overflow is undefined behaviour in C, and an optimiser is allowed
 * to assume it never happens -- which would quietly delete an overflow
 * check written in Cub.  So every int `+`, `-`, and `*` comes through here
 * and stops the program with a message, exactly as division by zero does.
 *
 * On GCC and Clang the builtins compile to the add-and-check-the-flag pair
 * the hardware already provides, so the guard costs a branch that is never
 * taken. */
#define CUB_INT_RANGE "an int runs from -9223372036854775808 to 9223372036854775807"

#if defined(__GNUC__) || defined(__clang__)
#  define CUB_OVERFLOW(op, a, b, out) __builtin_##op##_overflow((a), (b), (out))
#endif

static int64_t cub_add_int(int64_t a, int64_t b, const char *f, int l) {
#ifdef CUB_OVERFLOW
    int64_t r;
    if (!CUB_OVERFLOW(add, a, b, &r)) return r;
#else
    if (!((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)))
        return a + b;
#endif
    cub_panic_at(f, l, "%lld + %lld does not fit in an int; " CUB_INT_RANGE,
                 (long long)a, (long long)b);
    return 0;
}

static int64_t cub_sub_int(int64_t a, int64_t b, const char *f, int l) {
#ifdef CUB_OVERFLOW
    int64_t r;
    if (!CUB_OVERFLOW(sub, a, b, &r)) return r;
#else
    if (!((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)))
        return a - b;
#endif
    cub_panic_at(f, l, "%lld - %lld does not fit in an int; " CUB_INT_RANGE,
                 (long long)a, (long long)b);
    return 0;
}

static int64_t cub_mul_int(int64_t a, int64_t b, const char *f, int l) {
#ifdef CUB_OVERFLOW
    int64_t r;
    if (!CUB_OVERFLOW(mul, a, b, &r)) return r;
#else
    if (a == 0 || b == 0) return 0;
    if (!(a > INT64_MAX / b || a < INT64_MIN / b ||
          (a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)))
        return a * b;
#endif
    cub_panic_at(f, l, "%lld * %lld does not fit in an int; " CUB_INT_RANGE,
                 (long long)a, (long long)b);
    return 0;
}

static int64_t cub_neg_int(int64_t a, const char *f, int l) {
    if (a == INT64_MIN)
        cub_panic_at(f, l, "negating %lld does not fit in an int; " CUB_INT_RANGE,
                     (long long)a);
    return -a;
}

/* ---------------- the stack ---------------- */

/* C gives a program no way to recover from running out of stack: the
 * hardware faults and the process dies without a word.  So each Cub
 * function checks, on the way in, that there is still room beneath it --
 * one comparison against a floor worked out at startup, leaving enough
 * headroom for this message to be printed. */
static uintptr_t cub_stack_floor = 0;      /* 0 disables the check */
static volatile char *cub_stack_probe_at;

#if defined(__GNUC__) || defined(__clang__)
#  define CUB_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#  define CUB_NOINLINE __declspec(noinline)
#else
#  define CUB_NOINLINE
#endif

/* Inlined, this would share a frame with its caller and report the wrong
 * direction, so it is deliberately kept as a real call. */
CUB_NOINLINE static void cub_stack_probe(void) {
    char here;
    cub_stack_probe_at = &here;
}

static void cub_stack_init(void) {
    char base;
    size_t limit = 0;

#if defined(_WIN32)
    limit = 1u << 20;                       /* the usual 1 MB default */
#else
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
        rl.rlim_cur > 0 && rl.rlim_cur < (rlim_t)SIZE_MAX)
        limit = (size_t)rl.rlim_cur;
    else
        limit = 8u << 20;
#endif

    /* Everything here assumes the stack grows downward.  Check rather than
     * assume: where it grows upward, leave the guard switched off instead
     * of stopping perfectly good programs. */
    cub_stack_probe();
    if ((uintptr_t)cub_stack_probe_at >= (uintptr_t)&base) return;

    size_t margin = 64u * 1024;
    if (limit <= margin * 2) return;        /* too small to guard usefully */
    cub_stack_floor = (uintptr_t)&base - (uintptr_t)(limit - margin);
}

static void cub_stack_check(const char *name, const char *f, int l) {
    char here;
    if (cub_stack_floor && (uintptr_t)&here < cub_stack_floor)
        cub_panic_at(f, l, "`%s` ran out of stack space, so the calls were "
                           "nested too deeply to finish", name);
}

/* ---------------- values that may not be there ---------------- */

/* `T?` and `T!` become a struct per inner type.  The ones the library
 * itself hands back are declared here so a runtime function can return
 * one directly; cubc generates the rest. */
#define CUB_MAYBE(name, T)                              \
    typedef struct { bool ok; T value; CubStr err; } name

static CubStr cub_str_lit(const char *s, int64_t n);

CUB_MAYBE(CubMaybe_int, int64_t);
CUB_MAYBE(CubMaybe_float, double);
CUB_MAYBE(CubMaybe_string, CubStr);
CUB_MAYBE(CubMaybe_arr_string, struct CubArrRec *);
typedef struct { bool ok; CubStr err; } CubMaybe_void;

/* ---------------- text ---------------- */

static CubStr cub_str_lit(const char *s, int64_t n) { CubStr r; r.data = s; r.len = n; return r; }

static CubStr cub_str_new(const char *s, int64_t n) {
    char *p = (char *)cub_alloc((size_t)n + 1);
    if (n) memcpy(p, s, (size_t)n);
    p[n] = 0;
    CubStr r; r.data = p; r.len = n;
    return r;
}

static CubStr cub_str_concat(CubStr a, CubStr b) {
    char *p = (char *)cub_alloc((size_t)(a.len + b.len) + 1);
    memcpy(p, a.data, (size_t)a.len);
    memcpy(p + a.len, b.data, (size_t)b.len);
    p[a.len + b.len] = 0;
    CubStr r; r.data = p; r.len = a.len + b.len;
    return r;
}

static bool cub_str_eq(CubStr a, CubStr b) {
    return a.len == b.len && memcmp(a.data, b.data, (size_t)a.len) == 0;
}

static int cub_str_cmp(CubStr a, CubStr b) {
    int64_t n = a.len < b.len ? a.len : b.len;
    int c = n ? memcmp(a.data, b.data, (size_t)n) : 0;
    if (c) return c;
    return a.len < b.len ? -1 : (a.len > b.len ? 1 : 0);
}

static CubStr cub_str_from_int(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    return cub_str_new(buf, n);
}

static CubStr cub_str_from_bool(bool v) {
    return v ? cub_str_lit("true", 4) : cub_str_lit("false", 5);
}

static CubStr cub_str_from_float(double v) {
    char buf[40];
    if (isnan(v)) return cub_str_lit("nan", 3);
    if (isinf(v)) return v > 0 ? cub_str_lit("inf", 3) : cub_str_lit("-inf", 4);
    int n = snprintf(buf, sizeof buf, "%.15g", v);
    /* keep floats visibly float: 2 prints as 2.0 */
    bool plain = true;
    for (int i = 0; i < n; i++)
        if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') { plain = false; break; }
    if (plain && n + 2 < (int)sizeof buf) { buf[n++] = '.'; buf[n++] = '0'; buf[n] = 0; }
    return cub_str_new(buf, n);
}

static void cub_write(CubStr s) { fwrite(s.data, 1, (size_t)s.len, stdout); }
static void cub_println(CubStr s) { cub_write(s); fputc('\n', stdout); }

/* ---------------- text operations ---------------- */

static int64_t cub_str_len(CubStr s) { return s.len; }

static CubStr cub_str_upper(CubStr s) {
    CubStr r = cub_str_new(s.data, s.len);
    char *p = (char *)r.data;
    for (int64_t i = 0; i < r.len; i++) p[i] = (char)toupper((unsigned char)p[i]);
    return r;
}

static CubStr cub_str_lower(CubStr s) {
    CubStr r = cub_str_new(s.data, s.len);
    char *p = (char *)r.data;
    for (int64_t i = 0; i < r.len; i++) p[i] = (char)tolower((unsigned char)p[i]);
    return r;
}

static CubStr cub_str_trim(CubStr s) {
    int64_t a = 0, b = s.len;
    while (a < b && isspace((unsigned char)s.data[a])) a++;
    while (b > a && isspace((unsigned char)s.data[b - 1])) b--;
    return cub_str_new(s.data + a, b - a);
}

static int64_t cub_str_find(CubStr s, CubStr sub) {
    if (sub.len == 0) return 0;
    if (sub.len > s.len) return -1;
    for (int64_t i = 0; i + sub.len <= s.len; i++)
        if (memcmp(s.data + i, sub.data, (size_t)sub.len) == 0) return i;
    return -1;
}

static bool cub_str_contains(CubStr s, CubStr sub)     { return cub_str_find(s, sub) >= 0; }

static bool cub_str_starts_with(CubStr s, CubStr p) {
    return p.len <= s.len && memcmp(s.data, p.data, (size_t)p.len) == 0;
}

static bool cub_str_ends_with(CubStr s, CubStr p) {
    return p.len <= s.len && memcmp(s.data + s.len - p.len, p.data, (size_t)p.len) == 0;
}

static CubStr cub_str_slice(CubStr s, int64_t lo, int64_t hi) {
    if (lo < 0) lo = 0;
    if (hi > s.len) hi = s.len;
    if (lo >= hi) return cub_str_lit("", 0);
    return cub_str_new(s.data + lo, hi - lo);
}

static CubStr cub_str_repeat(CubStr s, int64_t n) {
    if (n <= 0 || s.len == 0) return cub_str_lit("", 0);
    char *p = (char *)cub_alloc((size_t)(s.len * n) + 1);
    for (int64_t i = 0; i < n; i++) memcpy(p + i * s.len, s.data, (size_t)s.len);
    p[s.len * n] = 0;
    CubStr r; r.data = p; r.len = s.len * n;
    return r;
}

static CubStr cub_str_replace(CubStr s, CubStr from, CubStr to) {
    if (from.len == 0 || from.len > s.len) return s;
    int64_t count = 0;
    for (int64_t i = 0; i + from.len <= s.len; ) {
        if (memcmp(s.data + i, from.data, (size_t)from.len) == 0) { count++; i += from.len; }
        else i++;
    }
    if (!count) return s;
    int64_t nlen = s.len + count * (to.len - from.len);
    char *p = (char *)cub_alloc((size_t)nlen + 1);
    int64_t w = 0;
    for (int64_t i = 0; i < s.len; ) {
        if (i + from.len <= s.len && memcmp(s.data + i, from.data, (size_t)from.len) == 0) {
            memcpy(p + w, to.data, (size_t)to.len);
            w += to.len;
            i += from.len;
        } else {
            p[w++] = s.data[i++];
        }
    }
    p[nlen] = 0;
    CubStr r; r.data = p; r.len = nlen;
    return r;
}

static CubStr cub_str_char_at(CubStr s, int64_t i, const char *f, int l) {
    if (i < 0 || i >= s.len)
        cub_panic_at(f, l, "position %lld is outside text of length %lld",
                     (long long)i, (long long)s.len);
    return cub_str_new(s.data + i, 1);
}

static int64_t cub_str_code_at(CubStr s, int64_t i, const char *f, int l) {
    if (i < 0 || i >= s.len)
        cub_panic_at(f, l, "position %lld is outside text of length %lld",
                     (long long)i, (long long)s.len);
    return (int64_t)(unsigned char)s.data[i];
}

static CubStr cub_str_from_code(int64_t c, const char *f, int l) {
    if (c < 0 || c > 255)
        cub_panic_at(f, l, "%lld is not a character code between 0 and 255", (long long)c);
    char b = (char)c;
    return cub_str_new(&b, 1);
}

/* ---------------- arrays ---------------- */

static CubArr cub_arr_new(int64_t esz, int64_t cap) {
    CubArr a = (CubArr)malloc(sizeof(struct CubArrS));
    if (!a) cub_oom();
    a->len = 0;
    a->cap = cap > 0 ? cap : 0;
    a->esz = esz;
    a->data = a->cap ? (char *)malloc((size_t)(a->cap * esz)) : NULL;
    if (a->cap && !a->data) cub_oom();
    cub_track(a, CUB_MEM_ARR);
    return a;
}

static CubArr cub_arr_lit(int64_t esz, int64_t n, const void *items) {
    CubArr a = cub_arr_new(esz, n);
    if (n) memcpy(a->data, items, (size_t)(n * esz));
    a->len = n;
    return a;
}

static void cub_arr_reserve(CubArr a, int64_t need) {
    if (need <= a->cap) return;
    int64_t nc = a->cap ? a->cap * 2 : 8;
    while (nc < need) nc *= 2;
    char *nd = (char *)realloc(a->data, (size_t)(nc * a->esz));
    if (!nd) cub_oom();
    a->data = nd;
    a->cap = nc;
}

static void *cub_arr_at(CubArr a, int64_t i, const char *f, int l) {
    if (i < 0 || i >= a->len) {
        if (a->len == 0)
            cub_panic_at(f, l, "position %lld is outside the array, which is empty",
                         (long long)i);
        cub_panic_at(f, l, "position %lld is outside the array, whose positions are 0 to %lld",
                     (long long)i, (long long)(a->len - 1));
    }
    return a->data + i * a->esz;
}

static void cub_arr_push(CubArr a, const void *v) {
    cub_arr_reserve(a, a->len + 1);
    memcpy(a->data + a->len * a->esz, v, (size_t)a->esz);
    a->len++;
}

static void *cub_arr_pop(CubArr a, const char *f, int l) {
    if (a->len == 0) cub_panic_at(f, l, "there is nothing to pop: the array is empty");
    a->len--;
    return a->data + a->len * a->esz;   /* still valid until the next push */
}

static void cub_arr_remove(CubArr a, int64_t i, const char *f, int l) {
    cub_arr_at(a, i, f, l);
    memmove(a->data + i * a->esz, a->data + (i + 1) * a->esz, (size_t)((a->len - i - 1) * a->esz));
    a->len--;
}

static void cub_arr_insert(CubArr a, int64_t i, const void *v, const char *f, int l) {
    if (i < 0 || i > a->len)
        cub_panic_at(f, l, "cannot insert at position %lld in an array of length %lld",
                     (long long)i, (long long)a->len);
    cub_arr_reserve(a, a->len + 1);
    memmove(a->data + (i + 1) * a->esz, a->data + i * a->esz, (size_t)((a->len - i) * a->esz));
    memcpy(a->data + i * a->esz, v, (size_t)a->esz);
    a->len++;
}

static CubArr cub_arr_slice(CubArr a, int64_t lo, int64_t hi) {
    if (lo < 0) lo = 0;
    if (hi > a->len) hi = a->len;
    if (lo >= hi) return cub_arr_new(a->esz, 0);
    return cub_arr_lit(a->esz, hi - lo, a->data + lo * a->esz);
}

static void cub_arr_reverse(CubArr a) {
    char *tmp = (char *)malloc((size_t)a->esz);
    if (!tmp) cub_oom();
    for (int64_t i = 0, j = a->len - 1; i < j; i++, j--) {
        memcpy(tmp, a->data + i * a->esz, (size_t)a->esz);
        memcpy(a->data + i * a->esz, a->data + j * a->esz, (size_t)a->esz);
        memcpy(a->data + j * a->esz, tmp, (size_t)a->esz);
    }
    free(tmp);
}

static bool cub_arr_contains_mem(CubArr a, const void *v) {
    for (int64_t i = 0; i < a->len; i++)
        if (memcmp(a->data + i * a->esz, v, (size_t)a->esz) == 0) return true;
    return false;
}

static bool cub_arr_contains_float(CubArr a, double v) {
    for (int64_t i = 0; i < a->len; i++)
        if (*(double *)(a->data + i * a->esz) == v) return true;
    return false;
}

static bool cub_arr_contains_str(CubArr a, CubStr v) {
    for (int64_t i = 0; i < a->len; i++)
        if (cub_str_eq(*(CubStr *)(a->data + i * a->esz), v)) return true;
    return false;
}

static int cub_cmp_int(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int cub_cmp_float(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int cub_cmp_str(const void *a, const void *b) {
    return cub_str_cmp(*(const CubStr *)a, *(const CubStr *)b);
}

static void cub_sort_int(CubArr a)   { qsort(a->data, (size_t)a->len, (size_t)a->esz, cub_cmp_int); }
static void cub_sort_float(CubArr a) { qsort(a->data, (size_t)a->len, (size_t)a->esz, cub_cmp_float); }
static void cub_sort_str(CubArr a)   { qsort(a->data, (size_t)a->len, (size_t)a->esz, cub_cmp_str); }

static CubArr cub_str_split(CubStr s, CubStr sep) {
    CubArr out = cub_arr_new((int64_t)sizeof(CubStr), 4);
    if (sep.len == 0) {                       /* split into single characters */
        for (int64_t i = 0; i < s.len; i++) {
            CubStr piece = cub_str_new(s.data + i, 1);
            cub_arr_push(out, &piece);
        }
        return out;
    }
    int64_t start = 0;
    for (int64_t i = 0; i + sep.len <= s.len; ) {
        if (memcmp(s.data + i, sep.data, (size_t)sep.len) == 0) {
            CubStr piece = cub_str_new(s.data + start, i - start);
            cub_arr_push(out, &piece);
            i += sep.len;
            start = i;
        } else i++;
    }
    CubStr piece = cub_str_new(s.data + start, s.len - start);
    cub_arr_push(out, &piece);
    return out;
}

static CubStr cub_str_join(CubArr a, CubStr sep) {
    if (a->len == 0) return cub_str_lit("", 0);
    int64_t total = sep.len * (a->len - 1);
    for (int64_t i = 0; i < a->len; i++) total += ((CubStr *)a->data)[i].len;
    char *p = (char *)cub_alloc((size_t)total + 1);
    int64_t w = 0;
    for (int64_t i = 0; i < a->len; i++) {
        if (i) { memcpy(p + w, sep.data, (size_t)sep.len); w += sep.len; }
        CubStr e = ((CubStr *)a->data)[i];
        memcpy(p + w, e.data, (size_t)e.len);
        w += e.len;
    }
    p[total] = 0;
    CubStr r; r.data = p; r.len = total;
    return r;
}

/* ---------------- objects ---------------- */

static void *cub_obj_new(size_t n) {
    void *p = cub_alloc(n);
    memset(p, 0, n);
    return p;
}

/* A field that holds an object starts out empty.  Reading one before it is
 * set stops the program here rather than wandering into freed memory. */
static void *cub_obj_ck(void *p, const char *cls, const char *f, int l) {
    if (!p) cub_panic_at(f, l, "there is no %s here yet", cls);
    return p;
}

/* ---------------- maps ---------------- */

static uint64_t cub_hash_str(CubStr s) {
    uint64_t h = 1469598103934665603ULL;          /* FNV-1a */
    for (int64_t i = 0; i < s.len; i++) {
        h ^= (unsigned char)s.data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t cub_hash_int(int64_t v) {
    uint64_t x = (uint64_t)v + 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static CubMap cub_map_new(int64_t vsz, int str_key) {
    CubMap m = (CubMap)malloc(sizeof(struct CubMapS));
    if (!m) cub_oom();
    m->cap = 16;
    m->len = 0;
    m->vsz = vsz;
    m->str_key = str_key;
    m->slots = (CubMapSlot *)calloc((size_t)m->cap, sizeof(CubMapSlot));
    if (!m->slots) cub_oom();
    cub_track(m, CUB_MEM_MAP);
    return m;
}

static bool cub_key_eq(CubMap m, CubMapSlot *s, CubStr sk, int64_t ik) {
    return m->str_key ? cub_str_eq(s->skey, sk) : s->ikey == ik;
}

static int64_t cub_map_slot(CubMap m, CubStr sk, int64_t ik) {
    uint64_t h = m->str_key ? cub_hash_str(sk) : cub_hash_int(ik);
    int64_t i = (int64_t)(h & (uint64_t)(m->cap - 1));
    int64_t first_free = -1;
    for (int64_t probe = 0; probe < m->cap; probe++) {
        CubMapSlot *s = &m->slots[i];
        if (s->state == 0) return first_free >= 0 ? first_free : i;
        if (s->state == 2) { if (first_free < 0) first_free = i; }
        else if (cub_key_eq(m, s, sk, ik)) return i;
        i = (i + 1) & (m->cap - 1);
    }
    return first_free;
}

static void cub_map_grow(CubMap m) {
    int64_t old_cap = m->cap;
    CubMapSlot *old = m->slots;
    m->cap *= 2;
    m->slots = (CubMapSlot *)calloc((size_t)m->cap, sizeof(CubMapSlot));
    if (!m->slots) cub_oom();
    m->len = 0;
    for (int64_t i = 0; i < old_cap; i++) {
        if (old[i].state != 1) continue;
        int64_t j = cub_map_slot(m, old[i].skey, old[i].ikey);
        m->slots[j] = old[i];
        m->len++;
    }
    free(old);
}

static void *cub_map_put(CubMap m, CubStr sk, int64_t ik) {
    if ((m->len + 1) * 10 >= m->cap * 7) cub_map_grow(m);
    int64_t i = cub_map_slot(m, sk, ik);
    CubMapSlot *s = &m->slots[i];
    if (s->state == 1) return s->value;
    s->state = 1;
    s->skey = sk;
    s->ikey = ik;
    s->value = malloc((size_t)m->vsz);
    if (!s->value) cub_oom();
    memset(s->value, 0, (size_t)m->vsz);
    m->len++;
    return s->value;
}

static void *cub_map_find(CubMap m, CubStr sk, int64_t ik) {
    int64_t i = cub_map_slot(m, sk, ik);
    if (i < 0) return NULL;
    CubMapSlot *s = &m->slots[i];
    return (s->state == 1 && cub_key_eq(m, s, sk, ik)) ? s->value : NULL;
}

static void *cub_map_at(CubMap m, CubStr sk, int64_t ik, const char *f, int l) {
    void *v = cub_map_find(m, sk, ik);
    if (!v) {
        if (m->str_key) cub_panic_at(f, l, "this map has no key \"%s\"", sk.data);
        else            cub_panic_at(f, l, "this map has no key %lld", (long long)ik);
    }
    return v;
}

static void *cub_map_get(CubMap m, CubStr sk, int64_t ik, void *fallback) {
    void *v = cub_map_find(m, sk, ik);
    return v ? v : fallback;
}

static bool cub_map_has(CubMap m, CubStr sk, int64_t ik) {
    return cub_map_find(m, sk, ik) != NULL;
}

static void cub_map_del(CubMap m, CubStr sk, int64_t ik) {
    int64_t i = cub_map_slot(m, sk, ik);
    if (i < 0) return;
    CubMapSlot *s = &m->slots[i];
    if (s->state != 1 || !cub_key_eq(m, s, sk, ik)) return;
    free(s->value);
    s->value = NULL;
    s->state = 2;
    m->len--;
}

static void cub_map_clear(CubMap m) {
    for (int64_t i = 0; i < m->cap; i++)
        if (m->slots[i].state == 1) { free(m->slots[i].value); m->slots[i].value = NULL; }
    memset(m->slots, 0, (size_t)m->cap * sizeof(CubMapSlot));
    m->len = 0;
}

static CubMap cub_map_lit(int64_t vsz, int str_key, int n, ...) {
    CubMap m = cub_map_new(vsz, str_key);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        CubStr sk = cub_str_lit("", 0);
        int64_t ik = 0;
        if (str_key) sk = va_arg(ap, CubStr);
        else         ik = va_arg(ap, int64_t);
        void *src = va_arg(ap, void *);
        memcpy(cub_map_put(m, sk, ik), src, (size_t)vsz);
    }
    va_end(ap);
    return m;
}

static CubArr cub_map_keys(CubMap m) {
    CubArr out = cub_arr_new(m->str_key ? (int64_t)sizeof(CubStr) : (int64_t)sizeof(int64_t),
                             m->len);
    for (int64_t i = 0; i < m->cap; i++) {
        if (m->slots[i].state != 1) continue;
        if (m->str_key) cub_arr_push(out, &m->slots[i].skey);
        else            cub_arr_push(out, &m->slots[i].ikey);
    }
    return out;
}

static CubArr cub_map_values(CubMap m) {
    CubArr out = cub_arr_new(m->vsz, m->len);
    for (int64_t i = 0; i < m->cap; i++)
        if (m->slots[i].state == 1) cub_arr_push(out, m->slots[i].value);
    return out;
}

/* ---------------- conversion ---------------- */

/* Casting a double that will not fit into an int64_t is undefined, so the
 * range is checked first.  The bound is written as a power of two because
 * INT64_MAX itself is not representable as a double. */
static int64_t cub_int_of_float(double v, const char *f, int l) {
    if (v != v)
        cub_panic_at(f, l, "`nan` cannot be turned into an int");
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0))
        cub_panic_at(f, l, "%g does not fit in an int; " CUB_INT_RANGE, v);
    return (int64_t)v;
}

static CubMaybe_int cub_int_of_str(CubStr s) {
    CubMaybe_int r;
    r.ok = false;
    r.value = 0;
    r.err = cub_str_lit("", 0);

    CubStr t = cub_str_trim(s);
    if (t.len == 0) {
        r.err = cub_str_lit("there is no whole number in empty text", 38);
        return r;
    }
    errno = 0;
    char *end = NULL;
    long long v = strtoll(t.data, &end, 10);
    if (end != t.data + t.len) {
        r.err = cub_str_concat(cub_str_concat(
                    cub_str_lit("cannot read a whole number from \"", 33), t),
                    cub_str_lit("\"", 1));
        return r;
    }
    if (errno == ERANGE) {
        r.err = cub_str_concat(cub_str_concat(
                    cub_str_lit("\"", 1), t),
                    cub_str_lit("\" does not fit in an int", 24));
        return r;
    }
    r.ok = true;
    r.value = (int64_t)v;
    return r;
}

static CubMaybe_float cub_float_of_str(CubStr s) {
    CubMaybe_float r;
    r.ok = false;
    r.value = 0.0;
    r.err = cub_str_lit("", 0);

    CubStr t = cub_str_trim(s);
    if (t.len == 0) {
        r.err = cub_str_lit("there is no number in empty text", 32);
        return r;
    }
    char *end = NULL;
    double v = strtod(t.data, &end);
    if (end != t.data + t.len) {
        r.err = cub_str_concat(cub_str_concat(
                    cub_str_lit("cannot read a number from \"", 27), t),
                    cub_str_lit("\"", 1));
        return r;
    }
    r.ok = true;
    r.value = v;
    return r;
}

/* ---------------- numbers ---------------- */

static int64_t cub_abs_int(int64_t v)   { return v < 0 ? -v : v; }
static double  cub_abs_float(double v)  { return v < 0 ? -v : v; }
static int64_t cub_min_int(int64_t a, int64_t b)  { return a < b ? a : b; }
static int64_t cub_max_int(int64_t a, int64_t b)  { return a > b ? a : b; }
static double  cub_min_float(double a, double b)  { return a < b ? a : b; }
static double  cub_max_float(double a, double b)  { return a > b ? a : b; }
static double  cub_round(double v)      { return floor(v + 0.5); }

/* ---------------- arithmetic with guards ---------------- */

static int64_t cub_div_int(int64_t a, int64_t b, const char *f, int l) {
    if (b == 0) cub_panic_at(f, l, "cannot divide %lld by zero", (long long)a);
    if (b == -1 && a == INT64_MIN) cub_panic_at(f, l, "this division overflows");
    return a / b;
}

static int64_t cub_mod_int(int64_t a, int64_t b, const char *f, int l) {
    if (b == 0) cub_panic_at(f, l, "cannot take the remainder of %lld by zero", (long long)a);
    if (b == -1) return 0;
    return a % b;
}

/* ---------------- files, input, misc ---------------- */

/* Why a file could not be used, in the same words a person would. */
static CubStr cub_file_trouble(const char *what, CubStr path) {
    CubStr r = cub_str_concat(cub_str_lit(what, (int64_t)strlen(what)),
                              cub_str_lit(" \"", 2));
    r = cub_str_concat(cub_str_concat(r, path), cub_str_lit("\": ", 3));
    const char *why = strerror(errno);
    return cub_str_concat(r, cub_str_lit(why, (int64_t)strlen(why)));
}

static CubMaybe_string cub_read_file(CubStr path) {
    CubMaybe_string r;
    r.ok = false;
    r.value = cub_str_lit("", 0);
    r.err = cub_str_lit("", 0);

    errno = 0;
    FILE *fp = fopen(path.data, "rb");
    if (!fp) { r.err = cub_file_trouble("cannot open", path); return r; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) { fclose(fp); r.err = cub_file_trouble("cannot read", path); return r; }
    char *p = (char *)cub_alloc((size_t)n + 1);
    size_t got = fread(p, 1, (size_t)n, fp);
    fclose(fp);
    p[got] = 0;
    r.ok = true;
    r.value.data = p;
    r.value.len = (int64_t)got;
    return r;
}

static CubMaybe_void cub_write_file(CubStr path, CubStr body) {
    CubMaybe_void r;
    r.ok = false;
    r.err = cub_str_lit("", 0);
    errno = 0;
    FILE *fp = fopen(path.data, "wb");
    if (!fp) { r.err = cub_file_trouble("cannot write", path); return r; }
    fwrite(body.data, 1, (size_t)body.len, fp);
    fclose(fp);
    r.ok = true;
    return r;
}

static CubStr cub_input(void) {
    size_t cap = 128, len = 0;
    char *buf = (char *)cub_alloc(cap);
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (len + 2 > cap) {
            size_t nc = cap * 2;
            char *nb = (char *)cub_alloc(nc);
            memcpy(nb, buf, len);
            buf = nb;
            cap = nc;
        }
        buf[len++] = (char)c;
    }
    if (len && buf[len - 1] == '\r') len--;
    buf[len] = 0;
    CubStr r; r.data = buf; r.len = (int64_t)len;
    return r;
}

static int64_t cub_rand_int(int64_t lo, int64_t hi, const char *f, int l) {
    if (lo > hi) cub_panic_at(f, l, "rand_int needs a low value no greater than the high one");
    return lo + (int64_t)(rand() % (hi - lo + 1));
}

static void cub_rand_seed(int64_t s) { srand((unsigned)s); }

static int64_t cub_time_ms(void) {
    return (int64_t)((double)clock() * 1000.0 / (double)CLOCKS_PER_SEC);
}

static void cub_assert(bool ok, CubStr msg, const char *f, int l) {
    if (!ok) cub_panic_at(f, l, "%s", msg.len ? msg.data : "an assertion failed");
}

/* ---------------- standard library ---------------- */

static int     cub_argc = 0;
static char  **cub_argv = NULL;

static CubArr cub_args(void) {
    CubArr out = cub_arr_new((int64_t)sizeof(CubStr), cub_argc);
    for (int i = 0; i < cub_argc; i++) {
        CubStr s = cub_str_new(cub_argv[i], (int64_t)strlen(cub_argv[i]));
        cub_arr_push(out, &s);
    }
    return out;
}

/* Which machine this is running on, as plain text. */
static CubStr cub_platform(void) {
#if defined(__APPLE__)
    return cub_str_lit("macos", 5);
#elif defined(_WIN32)
    return cub_str_lit("windows", 7);
#elif defined(__linux__)
    return cub_str_lit("linux", 5);
#else
    return cub_str_lit("unknown", 7);
#endif
}

static CubStr cub_env(CubStr name) {
    const char *v = getenv(name.data);
    return v ? cub_str_new(v, (int64_t)strlen(v)) : cub_str_lit("", 0);
}

static void cub_exit(int64_t code) {
    fflush(stdout);
    cub_rt_shutdown();
    exit((int)code);
}

static void cub_eprint(CubStr s) {
    fflush(stdout);
    fwrite(s.data, 1, (size_t)s.len, stderr);
    fputc('\n', stderr);
}

static void cub_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
#endif
}

static int64_t cub_clock_ms(void) {
#if defined(_WIN32)
    return (int64_t)GetTickCount64();
#elif defined(CLOCK_REALTIME)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
    return (int64_t)time(NULL) * 1000;      /* whole seconds, but portable */
#endif
}

/* ---- numbers ---- */

static int64_t cub_sign_int(int64_t v)  { return v > 0 ? 1 : (v < 0 ? -1 : 0); }
static int64_t cub_sign_float(double v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

static int64_t cub_clamp_int(int64_t v, int64_t lo, int64_t hi, const char *f, int l) {
    if (lo > hi) cub_panic_at(f, l, "clamp needs a low value no greater than the high one");
    return v < lo ? lo : (v > hi ? hi : v);
}

static double cub_clamp_float(double v, double lo, double hi, const char *f, int l) {
    if (lo > hi) cub_panic_at(f, l, "clamp needs a low value no greater than the high one");
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool cub_is_nan(double v) { return isnan(v) != 0; }
static bool cub_is_inf(double v) { return isinf(v) != 0; }

static double cub_rand_float(void) { return (double)rand() / ((double)RAND_MAX + 1.0); }

/* ---- text ---- */

static CubStr cub_str_pad(CubStr s, int64_t width, CubStr fill, int at_start) {
    if (fill.len == 0 || s.len >= width) return s;
    int64_t need = width - s.len;
    char *p = (char *)cub_alloc((size_t)width + 1);
    int64_t w = 0;
    if (at_start) {
        while (w < need) { p[w] = fill.data[w % fill.len]; w++; }
        memcpy(p + w, s.data, (size_t)s.len);
    } else {
        memcpy(p, s.data, (size_t)s.len);
        w = s.len;
        while (w < width) { p[w] = fill.data[(w - s.len) % fill.len]; w++; }
    }
    p[width] = 0;
    CubStr r; r.data = p; r.len = width;
    return r;
}

static CubStr cub_str_trim_start(CubStr s) {
    int64_t a = 0;
    while (a < s.len && isspace((unsigned char)s.data[a])) a++;
    return cub_str_new(s.data + a, s.len - a);
}

static CubStr cub_str_trim_end(CubStr s) {
    int64_t b = s.len;
    while (b > 0 && isspace((unsigned char)s.data[b - 1])) b--;
    return cub_str_new(s.data, b);
}

static CubStr cub_str_capitalize(CubStr s) {
    if (s.len == 0) return s;
    CubStr r = cub_str_new(s.data, s.len);
    char *p = (char *)r.data;
    p[0] = (char)toupper((unsigned char)p[0]);
    for (int64_t i = 1; i < r.len; i++) p[i] = (char)tolower((unsigned char)p[i]);
    return r;
}

static CubArr cub_str_lines(CubStr s) {
    CubArr out = cub_arr_new((int64_t)sizeof(CubStr), 4);
    int64_t start = 0;
    for (int64_t i = 0; i <= s.len; i++) {
        if (i == s.len || s.data[i] == '\n') {
            int64_t end = i;
            if (end > start && s.data[end - 1] == '\r') end--;
            if (i == s.len && start == i && s.len > 0) break;   /* no empty tail */
            CubStr piece = cub_str_new(s.data + start, end - start);
            cub_arr_push(out, &piece);
            start = i + 1;
        }
    }
    return out;
}

static int64_t cub_str_count(CubStr s, CubStr sub) {
    if (sub.len == 0 || sub.len > s.len) return 0;
    int64_t n = 0;
    for (int64_t i = 0; i + sub.len <= s.len; ) {
        if (memcmp(s.data + i, sub.data, (size_t)sub.len) == 0) { n++; i += sub.len; }
        else i++;
    }
    return n;
}

static int64_t cub_str_last_index_of(CubStr s, CubStr sub) {
    if (sub.len == 0) return s.len;
    if (sub.len > s.len) return -1;
    for (int64_t i = s.len - sub.len; i >= 0; i--)
        if (memcmp(s.data + i, sub.data, (size_t)sub.len) == 0) return i;
    return -1;
}

#define CUB_STR_ALL(name, test)                                     \
    static bool name(CubStr s) {                                    \
        if (s.len == 0) return false;                               \
        for (int64_t i = 0; i < s.len; i++)                         \
            if (!(test((unsigned char)s.data[i]))) return false;     \
        return true;                                                \
    }
CUB_STR_ALL(cub_is_digit, isdigit)
CUB_STR_ALL(cub_is_alpha, isalpha)
CUB_STR_ALL(cub_is_alnum, isalnum)
CUB_STR_ALL(cub_is_space, isspace)
CUB_STR_ALL(cub_is_upper, isupper)
CUB_STR_ALL(cub_is_lower, islower)
#undef CUB_STR_ALL

/* ---- arrays ---- */

static int64_t cub_sum_int(CubArr a) {
    int64_t t = 0;
    for (int64_t i = 0; i < a->len; i++) t += *(int64_t *)(a->data + i * a->esz);
    return t;
}

static double cub_sum_float(CubArr a) {
    double t = 0;
    for (int64_t i = 0; i < a->len; i++) t += *(double *)(a->data + i * a->esz);
    return t;
}

static CubArr cub_arr_copy(CubArr a) { return cub_arr_lit(a->esz, a->len, a->data); }

static CubArr cub_arr_concat(CubArr a, CubArr b) {
    CubArr out = cub_arr_new(a->esz, a->len + b->len);
    if (a->len) memcpy(out->data, a->data, (size_t)(a->len * a->esz));
    if (b->len) memcpy(out->data + a->len * a->esz, b->data, (size_t)(b->len * b->esz));
    out->len = a->len + b->len;
    return out;
}

static void cub_arr_clear(CubArr a) { a->len = 0; }

static void cub_arr_swap(CubArr a, int64_t i, int64_t j, const char *f, int l) {
    void *x = cub_arr_at(a, i, f, l);
    void *y = cub_arr_at(a, j, f, l);
    if (x == y) return;
    char *tmp = (char *)malloc((size_t)a->esz);
    if (!tmp) cub_oom();
    memcpy(tmp, x, (size_t)a->esz);
    memcpy(x, y, (size_t)a->esz);
    memcpy(y, tmp, (size_t)a->esz);
    free(tmp);
}

static void cub_arr_shuffle(CubArr a) {
    for (int64_t i = a->len - 1; i > 0; i--) {
        int64_t j = (int64_t)(rand() % (int)(i + 1));
        cub_arr_swap(a, i, j, NULL, 0);
    }
}

static int64_t cub_arr_index_mem(CubArr a, const void *v) {
    for (int64_t i = 0; i < a->len; i++)
        if (memcmp(a->data + i * a->esz, v, (size_t)a->esz) == 0) return i;
    return -1;
}

static int64_t cub_arr_index_float(CubArr a, double v) {
    for (int64_t i = 0; i < a->len; i++)
        if (*(double *)(a->data + i * a->esz) == v) return i;
    return -1;
}

static int64_t cub_arr_index_str(CubArr a, CubStr v) {
    for (int64_t i = 0; i < a->len; i++)
        if (cub_str_eq(*(CubStr *)(a->data + i * a->esz), v)) return i;
    return -1;
}

static int64_t cub_arr_count_mem(CubArr a, const void *v) {
    int64_t n = 0;
    for (int64_t i = 0; i < a->len; i++)
        if (memcmp(a->data + i * a->esz, v, (size_t)a->esz) == 0) n++;
    return n;
}

static int64_t cub_arr_count_float(CubArr a, double v) {
    int64_t n = 0;
    for (int64_t i = 0; i < a->len; i++)
        if (*(double *)(a->data + i * a->esz) == v) n++;
    return n;
}

static int64_t cub_arr_count_str(CubArr a, CubStr v) {
    int64_t n = 0;
    for (int64_t i = 0; i < a->len; i++)
        if (cub_str_eq(*(CubStr *)(a->data + i * a->esz), v)) n++;
    return n;
}

#define CUB_EXTREME(name, ctype_, cmp)                                        \
    static ctype_ name(CubArr a, const char *f, int l) {                      \
        if (a->len == 0) cub_panic_at(f, l, "there is no value here: "        \
                                            "the array is empty");            \
        ctype_ best = *(ctype_ *)a->data;                                     \
        for (int64_t i = 1; i < a->len; i++) {                                \
            ctype_ v = *(ctype_ *)(a->data + i * a->esz);                     \
            if (cmp) best = v;                                                \
        }                                                                     \
        return best;                                                          \
    }
CUB_EXTREME(cub_min_of_int,   int64_t, v < best)
CUB_EXTREME(cub_max_of_int,   int64_t, v > best)
CUB_EXTREME(cub_min_of_float, double,  v < best)
CUB_EXTREME(cub_max_of_float, double,  v > best)
CUB_EXTREME(cub_min_of_str,   CubStr,  cub_str_cmp(v, best) < 0)
CUB_EXTREME(cub_max_of_str,   CubStr,  cub_str_cmp(v, best) > 0)
#undef CUB_EXTREME

/* ---- files ---- */

static bool cub_file_exists(CubStr path) {
    FILE *fp = fopen(path.data, "rb");
    if (!fp) return false;
    fclose(fp);
    return true;
}

static CubMaybe_void cub_append_file(CubStr path, CubStr body) {
    CubMaybe_void r;
    r.ok = false;
    r.err = cub_str_lit("", 0);
    errno = 0;
    FILE *fp = fopen(path.data, "ab");
    if (!fp) { r.err = cub_file_trouble("cannot add to", path); return r; }
    fwrite(body.data, 1, (size_t)body.len, fp);
    fclose(fp);
    r.ok = true;
    return r;
}

static CubArr cub_str_lines(CubStr s);

static CubMaybe_arr_string cub_read_lines(CubStr path) {
    CubMaybe_arr_string r;
    r.ok = false;
    r.value = NULL;
    r.err = cub_str_lit("", 0);

    CubMaybe_string body = cub_read_file(path);
    if (!body.ok) { r.err = body.err; return r; }
    r.ok = true;
    r.value = cub_str_lines(body.value);
    return r;
}

static CubMaybe_void cub_delete_file(CubStr path) {
    CubMaybe_void r;
    r.ok = false;
    r.err = cub_str_lit("", 0);
    errno = 0;
    if (remove(path.data) != 0) {
        r.err = cub_file_trouble("cannot delete", path);
        return r;
    }
    r.ok = true;
    return r;
}

static void cub_rt_init(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand((unsigned)time(NULL));
    cub_argc = argc;
    cub_argv = argv;
}

#endif /* CUB_RT_H */
