/* util.c -- allocation, buffers, vectors, and diagnostics. */
#include "cub.h"
#include <unistd.h>

void *cx_alloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) { fprintf(stderr, "cubc: out of memory\n"); exit(1); }
    return p;
}

char *cx_strdup(const char *s) { return cx_strndup(s, strlen(s)); }

char *cx_strndup(const char *s, size_t n) {
    char *p = cx_alloc(n + 1);
    memcpy(p, s, n);
    return p;
}

char *cx_fmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *p = cx_alloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

void buf_init(Buf *b) { b->data = cx_alloc(256); b->len = 0; b->cap = 256; }

static void buf_grow(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    while (b->cap < b->len + extra + 1) b->cap *= 2;
    char *nd = cx_alloc(b->cap);
    memcpy(nd, b->data, b->len);
    free(b->data);
    b->data = nd;
}

void buf_putc(Buf *b, char c) { buf_grow(b, 1); b->data[b->len++] = c; b->data[b->len] = 0; }

void buf_puts(Buf *b, const char *s) {
    size_t n = strlen(s);
    buf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    buf_grow(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

void vec_push(Vec *v, void *p) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        void **ni = cx_alloc(sizeof(void *) * (size_t)v->cap);
        if (v->items) { memcpy(ni, v->items, sizeof(void *) * (size_t)v->len); free(v->items); }
        v->items = ni;
    }
    v->items[v->len++] = p;
}

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */
/* ------------------------------------------------------------------ */

Source *g_source  = NULL;
int     cub_errors = 0;

static const char *C_RED, *C_BLUE, *C_BOLD, *C_CYAN, *C_OFF;

static void colors_init(void) {
    static bool done = false;
    if (done) return;
    done = true;
    bool tty = isatty(2) && getenv("NO_COLOR") == NULL;
    C_RED = tty ? "\033[1;31m" : "";
    C_BLUE = tty ? "\033[1;34m" : "";
    C_BOLD = tty ? "\033[1m"    : "";
    C_CYAN = tty ? "\033[1;36m" : "";
    C_OFF = tty ? "\033[0m"    : "";
}

/* Print the offending source line with a caret under the column. */
static void show_line(int line, int col) {
    if (!g_source || !g_source->text || line <= 0) return;
    const char *p = g_source->text;
    for (int l = 1; l < line && *p; p++)
        if (*p == '\n') l++;
    if (!*p) return;
    const char *e = p;
    while (*e && *e != '\n') e++;

    fprintf(stderr, "  %s%4d |%s %.*s\n", C_BLUE, line, C_OFF, (int)(e - p), p);
    fprintf(stderr, "  %s     |%s ", C_BLUE, C_OFF);
    for (int i = 1; i < col && p + i - 1 < e; i++)
        fputc(p[i - 1] == '\t' ? '\t' : ' ', stderr);
    fprintf(stderr, "%s^%s\n", C_RED, C_OFF);
}

void err_at(int line, int col, const char *fmt, ...) {
    colors_init();
    cub_errors++;
    const char *path = g_source ? g_source->path : "<input>";
    fprintf(stderr, "%s%s:%d:%d:%s %serror:%s ", C_BOLD, path, line, col, C_OFF, C_RED, C_OFF);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    show_line(line, col);
}

void err_help(const char *fmt, ...) {
    colors_init();
    fprintf(stderr, "  %shelp:%s ", C_CYAN, C_OFF);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void fatal(const char *fmt, ...) {
    colors_init();
    fprintf(stderr, "%scubc:%s %serror:%s ", C_BOLD, C_OFF, C_RED, C_OFF);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void stop_if_errors(void) {
    if (cub_errors == 0) return;
    colors_init();
    fprintf(stderr, "%scubc:%s stopped after %d error%s.\n",
            C_BOLD, C_OFF, cub_errors, cub_errors == 1 ? "" : "s");
    exit(1);
}
