/* types.c -- type construction and interning.
 *
 * Types are interned, so the checker can compare them with `==` for every
 * case except arrays-of-arrays built through different paths (handled by
 * ty_array caching on the element pointer).
 */
#include "cub.h"

static Type *mk(TypeKind k) { Type *t = cx_alloc(sizeof(Type)); t->kind = k; return t; }

#define SINGLETON(fn, kind)                       \
    Type *fn(void) {                              \
        static Type *t = NULL;                    \
        if (!t) t = mk(kind);                     \
        return t;                                 \
    }

SINGLETON(ty_err,   TY_ERR)
SINGLETON(ty_void,  TY_VOID)
SINGLETON(ty_int,   TY_INT)
SINGLETON(ty_float, TY_FLOAT)
SINGLETON(ty_bool,  TY_BOOL)
SINGLETON(ty_str,   TY_STR)

static Vec array_cache;

Type *ty_array(Type *elem) {
    for (int i = 0; i < array_cache.len; i++) {
        Type *t = array_cache.items[i];
        if (t->elem == elem) return t;
    }
    Type *t = mk(TY_ARRAY);
    t->elem = elem;
    vec_push(&array_cache, t);
    return t;
}

static Vec map_cache;

Type *ty_map(Type *key, Type *val) {
    for (int i = 0; i < map_cache.len; i++) {
        Type *t = map_cache.items[i];
        if (t->key == key && t->elem == val) return t;
    }
    Type *t = mk(TY_MAP);
    t->key = key;
    t->elem = val;
    vec_push(&map_cache, t);
    return t;
}

/* `T?` and `T!` share a shape -- a value that may not be there -- and
 * differ in whether a failure carries a reason.  Neither nests: the parser
 * refuses `T??`, so `elem` is always a plain type. */
static Vec maybe_cache;

static Type *mk_maybe(TypeKind kind, Type *inner) {
    for (int i = 0; i < maybe_cache.len; i++) {
        Type *t = maybe_cache.items[i];
        if (t->kind == kind && t->elem == inner) return t;
    }
    Type *t = mk(kind);
    t->elem = inner;
    vec_push(&maybe_cache, t);
    return t;
}

Type *ty_opt(Type *inner)  { return mk_maybe(TY_OPT, inner); }
Type *ty_fail(Type *inner) { return mk_maybe(TY_FAIL, inner); }

bool ty_is_maybe(Type *t) { return t && (t->kind == TY_OPT || t->kind == TY_FAIL); }

/* `(int, string) -> bool`.  Interned like the rest, so two spellings of
 * the same shape are the same Type. */
static Vec fn_cache;

Type *ty_fn(Vec *params, Type *ret) {
    for (int i = 0; i < fn_cache.len; i++) {
        Type *t = fn_cache.items[i];
        if (t->elem != ret || t->fparams.len != params->len) continue;
        bool same = true;
        for (int j = 0; j < params->len; j++)
            if (t->fparams.items[j] != params->items[j]) { same = false; break; }
        if (same) return t;
    }
    Type *t = mk(TY_FN);
    t->elem = ret;
    for (int i = 0; i < params->len; i++) vec_push(&t->fparams, params->items[i]);
    vec_push(&fn_cache, t);
    return t;
}

Type *ty_named(TypeKind k, char *name) {
    Type *t = mk(k);
    t->name = name;
    return t;
}

bool ty_same(Type *a, Type *b) {
    if (!a || !b) return false;
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    if (a->kind == TY_ARRAY || a->kind == TY_OPT || a->kind == TY_FAIL)
        return ty_same(a->elem, b->elem);
    if (a->kind == TY_FN) {
        if (a->fparams.len != b->fparams.len) return false;
        for (int i = 0; i < a->fparams.len; i++)
            if (!ty_same(a->fparams.items[i], b->fparams.items[i])) return false;
        return ty_same(a->elem, b->elem);
    }
    if (a->kind == TY_MAP)   return ty_same(a->key, b->key) && ty_same(a->elem, b->elem);
    if (a->kind == TY_STRUCT || a->kind == TY_ENUM || a->kind == TY_CLASS)
        return strcmp(a->name, b->name) == 0;
    return true;
}

/* A value of a subclass may be used wherever the parent is expected. */
bool ty_assignable(Type *from, Type *to) {
    if (ty_same(from, to)) return true;
    if (!from || !to) return false;
    if (from->kind == TY_CLASS && to->kind == TY_CLASS) {
        for (ClassDef *c = from->cdef; c; c = c->base)
            if (c == to->cdef) return true;
    }
    /* A plain value goes where a `T?` or `T!` is wanted -- widening, never
     * the other way, so a value that may be missing is never mistaken for
     * one that is there. */
    if (ty_is_maybe(to) && !ty_is_maybe(from))
        return ty_assignable(from, to->elem);
    return false;
}

bool ty_is_num(Type *t) { return t && (t->kind == TY_INT || t->kind == TY_FLOAT); }

const char *ty_show(Type *t) {
    if (!t) return "?";
    switch (t->kind) {
    case TY_ERR:    return "?";
    case TY_VOID:   return "void";
    case TY_INT:    return "int";
    case TY_FLOAT:  return "float";
    case TY_BOOL:   return "bool";
    case TY_STR:    return "string";
    case TY_ARRAY:  return cx_fmt("[%s]", ty_show(t->elem));
    case TY_MAP:    return cx_fmt("[%s: %s]", ty_show(t->key), ty_show(t->elem));
    case TY_FN: {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "(");
        for (int i = 0; i < t->fparams.len; i++)
            buf_printf(&b, "%s%s", i ? ", " : "", ty_show(t->fparams.items[i]));
        buf_printf(&b, ") -> %s", ty_show(t->elem));
        return b.data;
    }
    case TY_OPT:    return cx_fmt("%s?", ty_show(t->elem));
    case TY_FAIL:   return cx_fmt("%s!", ty_show(t->elem));
    case TY_STRUCT:
    case TY_ENUM:
    case TY_CLASS:  return t->name;
    }
    return "?";
}

const char *ty_mangle(Type *t) {
    switch (t->kind) {
    case TY_VOID:   return "void";
    case TY_INT:    return "int";
    case TY_FLOAT:  return "float";
    case TY_BOOL:   return "bool";
    case TY_STR:    return "string";
    case TY_ARRAY:  return cx_fmt("arr_%s", ty_mangle(t->elem));
    case TY_MAP:    return cx_fmt("map_%s_%s", ty_mangle(t->key), ty_mangle(t->elem));
    case TY_STRUCT: return cx_fmt("s_%s", t->name);
    case TY_ENUM:   return cx_fmt("e_%s", t->name);
    case TY_CLASS:  return cx_fmt("c_%s", t->name);
    case TY_FN: {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "fn");
        for (int i = 0; i < t->fparams.len; i++)
            buf_printf(&b, "_%s", ty_mangle(t->fparams.items[i]));
        buf_printf(&b, "_to_%s", ty_mangle(t->elem));
        return b.data;
    }
    case TY_OPT:    return cx_fmt("opt_%s", ty_mangle(t->elem));
    case TY_FAIL:   return cx_fmt("fail_%s", ty_mangle(t->elem));
    default:        return "err";
    }
}
