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

Type *ty_named(TypeKind k, char *name) {
    Type *t = mk(k);
    t->name = name;
    return t;
}

bool ty_same(Type *a, Type *b) {
    if (!a || !b) return false;
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    if (a->kind == TY_ARRAY) return ty_same(a->elem, b->elem);
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
    default:        return "err";
    }
}
