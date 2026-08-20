/* check.c -- name resolution and type checking.
 *
 * Cub is statically typed with no implicit conversions: an int never turns
 * into a float behind your back.  The checker's other job is to produce
 * messages a beginner can act on, so most errors carry a `help:` line and,
 * where a name is misspelled, a suggestion.
 */
#include "cub.h"
#include <math.h>

/* ------------------------------------------------------------------ */
/* scopes                                                              */
/* ------------------------------------------------------------------ */

typedef struct Scope {
    struct Scope *parent;
    Vec           syms;      /* VarSym* */
} Scope;

static Program *prog;
static Scope   *scope;
static FnDecl  *cur_fn;
static ClassDef *cur_class;
static FnDecl   *cur_method;
static int      loop_depth;
static int      uid;
static Vec      named_types;   /* Type* for every declared struct/enum */

static void push_scope(void) {
    Scope *s = cx_alloc(sizeof(Scope));
    s->parent = scope;
    scope = s;
}

static void pop_scope(void) { scope = scope->parent; }

static VarSym *lookup_local(const char *name) {
    for (int i = 0; i < scope->syms.len; i++) {
        VarSym *v = scope->syms.items[i];
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

static VarSym *lookup(const char *name) {
    for (Scope *s = scope; s; s = s->parent)
        for (int i = 0; i < s->syms.len; i++) {
            VarSym *v = s->syms.items[i];
            if (strcmp(v->name, name) == 0) return v;
        }
    return NULL;
}

static VarSym *declare(const char *name, Type *t, bool is_mut, bool global, int line, int col) {
    VarSym *old = lookup_local(name);
    if (old) {
        err_at(line, col, "`%s` is already declared in this scope", name);
        err_help("pick a different name, or assign to the existing one with `%s = ...`", name);
    }
    VarSym *v = cx_alloc(sizeof(VarSym));
    v->name = cx_strdup(name);
    v->type = t;
    v->is_mut = is_mut;
    v->is_global = global;
    v->cname = global ? cx_fmt("cubg_%s_%d", name, ++uid) : cx_fmt("cubv_%s_%d", name, ++uid);
    vec_push(&scope->syms, v);
    return v;
}

/* ------------------------------------------------------------------ */
/* "did you mean" suggestions                                          */
/* ------------------------------------------------------------------ */

static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 64 || lb > 64) return 99;
    int prev[65], cur_[65];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur_[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int m = prev[j] + 1;
            if (cur_[j - 1] + 1 < m) m = cur_[j - 1] + 1;
            if (prev[j - 1] + cost < m) m = prev[j - 1] + cost;
            cur_[j] = m;
        }
        for (int j = 0; j <= lb; j++) prev[j] = cur_[j];
    }
    return prev[lb];
}

static const char *best_match(const char *name, Vec *candidates) {
    const char *best = NULL;
    int bestd = 99;
    for (int i = 0; i < candidates->len; i++) {
        const char *c = candidates->items[i];
        int d = edit_distance(name, c);
        if (d < bestd) { bestd = d; best = c; }
    }
    int len = (int)strlen(name);
    int limit = len <= 4 ? 1 : (len <= 8 ? 2 : 3);
    return (best && bestd <= limit) ? best : NULL;
}

extern const char *builtin_names[];   /* defined in builtins table below */

static void suggest_name(const char *name) {
    Vec cands = {0};
    for (Scope *s = scope; s; s = s->parent)
        for (int i = 0; i < s->syms.len; i++)
            vec_push(&cands, ((VarSym *)s->syms.items[i])->name);
    for (int i = 0; i < prog->fns.len; i++)
        vec_push(&cands, ((FnDecl *)prog->fns.items[i])->name);
    for (int i = 0; builtin_names[i]; i++)
        vec_push(&cands, (void *)builtin_names[i]);
    const char *m = best_match(name, &cands);
    if (m) err_help("did you mean `%s`?", m);
}

/* ------------------------------------------------------------------ */
/* built-in table                                                      */
/* ------------------------------------------------------------------ */

const char *builtin_names[] = {
    "print", "write", "len", "push", "pop", "remove", "insert", "str",
    "int", "float", "abs", "min", "max", "sqrt", "pow", "floor", "ceil",
    "round", "rand_int", "rand_seed", "input", "upper", "lower", "trim",
    "split", "join", "find", "slice", "contains", "starts_with",
    "ends_with", "replace", "repeat", "char_at", "code_at", "from_code",
    "sort", "reverse", "read_file", "write_file", "panic", "assert",
    "time_ms", "has", "get", "keys", "values", "clear", "sin", "cos", "tan",
    "asin", "acos", "atan", "atan2", "log", "log10", "exp", "sign", "clamp",
    "is_nan", "is_inf", "rand_float", "pad_start", "pad_end", "trim_start",
    "trim_end", "lines", "chars", "count", "index_of", "last_index_of",
    "capitalize", "is_digit", "is_alpha", "is_alnum", "is_space",
    "is_upper", "is_lower", "sum", "copy", "concat", "shuffle", "swap",
    "min_of", "max_of", "eprint", "exit", "args", "env", "sleep_ms",
    "clock_ms", "file_exists", "append_file", "delete_file", "read_lines",
    NULL
};

int builtin_lookup(const char *name) {
    for (int i = 0; builtin_names[i]; i++)
        if (strcmp(builtin_names[i], name) == 0) return i;
    return BI_NONE;
}

/* ------------------------------------------------------------------ */
/* type resolution                                                     */
/* ------------------------------------------------------------------ */

static Type *find_named(const char *name) {
    for (int i = 0; i < named_types.len; i++) {
        Type *t = named_types.items[i];
        if (strcmp(t->name, name) == 0) return t;
    }
    return NULL;
}

static Type *resolve_type(Type *t, int line, int col) {
    if (!t) return ty_err();
    if (t->kind == TY_ARRAY) return ty_array(resolve_type(t->elem, line, col));
    if (t->kind == TY_MAP)
        return ty_map(resolve_type(t->key, line, col), resolve_type(t->elem, line, col));
    if (t->kind == TY_STRUCT && t->sdef == NULL && t->name) {
        Type *found = find_named(t->name);
        if (found) return found;
        err_at(line, col, "there is no type named `%s`", t->name);
        Vec cands = {0};
        for (int i = 0; i < named_types.len; i++)
            vec_push(&cands, ((Type *)named_types.items[i])->name);
        vec_push(&cands, (void *)"int");
        vec_push(&cands, (void *)"float");
        vec_push(&cands, (void *)"bool");
        vec_push(&cands, (void *)"string");
        const char *m = best_match(t->name, &cands);
        if (m) err_help("did you mean `%s`?", m);
        else   err_help("declare it with `type %s = struct { ... }`", t->name);
        return ty_err();
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* classes                                                             */
/* ------------------------------------------------------------------ */

/* Find a method by name, starting at `c` and walking up to its parents. */
static FnDecl *find_method(ClassDef *c, const char *name) {
    for (; c; c = c->base)
        for (int i = 0; i < c->methods.len; i++) {
            FnDecl *m = c->methods.items[i];
            if (strcmp(m->name, name) == 0) return m;
        }
    return NULL;
}

/* Find a field, returning the class that declares it. */
static ClassDef *find_field(ClassDef *c, const char *name, Type **out) {
    for (; c; c = c->base)
        for (int i = 0; i < c->fnames.len; i++)
            if (strcmp((char *)c->fnames.items[i], name) == 0) {
                if (out) *out = c->ftypes.items[i];
                return c;
            }
    return NULL;
}

static void collect_members(ClassDef *c, Vec *fields, Vec *methods) {
    for (; c; c = c->base) {
        for (int i = 0; i < c->fnames.len; i++) vec_push(fields, c->fnames.items[i]);
        for (int i = 0; i < c->methods.len; i++) {
            FnDecl *m = c->methods.items[i];
            if (!m->is_init) vec_push(methods, m->name);
        }
    }
}

/* ------------------------------------------------------------------ */
/* expressions                                                         */
/* ------------------------------------------------------------------ */

static Type *check_expr(Expr *e, Type *hint);
static void  check_stmts(Vec *body);

static bool is_err(Type *t) { return !t || t->kind == TY_ERR; }

static void want(Expr *e, Type *got, Type *expect_, const char *ctx) {
    if (is_err(got) || is_err(expect_)) return;
    if (ty_assignable(got, expect_)) return;
    err_at(e->line, e->col, "%s expects %s, but this is %s",
           ctx, ty_show(expect_), ty_show(got));
    if (expect_->kind == TY_FLOAT && got->kind == TY_INT)
        err_help("turn the int into a float with `float(x)`, or write it as `%s.0`",
                 e->kind == EX_INT ? "1" : "x");
    else if (expect_->kind == TY_INT && got->kind == TY_FLOAT)
        err_help("round it down to an int with `int(x)`");
    else if (expect_->kind == TY_STR)
        err_help("turn a value into text with `str(x)`");
    else if (expect_->kind == TY_CLASS && got->kind == TY_CLASS)
        err_help("`%s` is not built on `%s`", got->name, expect_->name);
}

static FnDecl *find_fn(const char *name) {
    for (int i = 0; i < prog->fns.len; i++) {
        FnDecl *f = prog->fns.items[i];
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

static bool arity(Expr *e, int lo, int hi, const char *name) {
    int n = e->args.len;
    if (n >= lo && (hi < 0 || n <= hi)) return true;
    if (hi < 0)
        err_at(e->line, e->col, "`%s` takes at least %d argument%s, but got %d",
               name, lo, lo == 1 ? "" : "s", n);
    else if (lo == hi)
        err_at(e->line, e->col, "`%s` takes %d argument%s, but got %d",
               name, lo, lo == 1 ? "" : "s", n);
    else
        err_at(e->line, e->col, "`%s` takes %d to %d arguments, but got %d", name, lo, hi, n);
    return false;
}

static Type *arg_type(Expr *e, int i) {
    return (i < e->args.len) ? ((Expr *)e->args.items[i])->type : ty_err();
}

static Expr *arg(Expr *e, int i) { return (Expr *)e->args.items[i]; }

static bool primitive_elem(Type *t) {
    return t->kind == TY_INT || t->kind == TY_FLOAT || t->kind == TY_STR ||
           t->kind == TY_BOOL || t->kind == TY_ENUM;
}

static Type *check_builtin(Expr *e, int bi) {
    const char *name = builtin_names[bi];
    for (int i = 0; i < e->args.len; i++) {
        Type *at_ = check_expr(arg(e, i), NULL);
        if (at_ && at_->kind == TY_VOID) {
            err_at(arg(e, i)->line, arg(e, i)->col,
                   "this call produces no value, so it cannot be an argument");
        }
    }
    e->builtin = bi;

    switch (bi) {
    case BI_PRINT: case BI_WRITE:
        return ty_void();

    case BI_LEN:
        if (!arity(e, 1, 1, name)) return ty_int();
        if (!is_err(arg_type(e, 0)) && arg_type(e, 0)->kind != TY_STR &&
            arg_type(e, 0)->kind != TY_ARRAY && arg_type(e, 0)->kind != TY_MAP) {
            err_at(e->line, e->col, "`len` works on text, arrays, and maps, not on %s",
                   ty_show(arg_type(e, 0)));
        }
        return ty_int();

    case BI_PUSH: {
        if (!arity(e, 2, 2, name)) return ty_void();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_void();
        if (a->kind != TY_ARRAY) {
            err_at(e->line, e->col, "`push` adds to an array, but this is %s", ty_show(a));
            return ty_void();
        }
        want(arg(e, 1), arg_type(e, 1), a->elem, "this array");
        return ty_void();
    }

    case BI_POP: {
        if (!arity(e, 1, 1, name)) return ty_err();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_err();
        if (a->kind != TY_ARRAY) {
            err_at(e->line, e->col, "`pop` removes from an array, but this is %s", ty_show(a));
            return ty_err();
        }
        return a->elem;
    }

    case BI_REMOVE: {
        if (!arity(e, 2, 2, name)) return ty_void();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_void();
        if (a->kind == TY_MAP) {
            want(arg(e, 1), arg_type(e, 1), a->key, "this map's key");
            return ty_void();
        }
        if (a->kind != TY_ARRAY)
            err_at(e->line, e->col, "`remove` works on arrays and maps, but this is %s",
                   ty_show(a));
        want(arg(e, 1), arg_type(e, 1), ty_int(), "the position");
        return ty_void();
    }

    case BI_INSERT: {
        if (!arity(e, 3, 3, name)) return ty_void();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_void();
        if (a->kind != TY_ARRAY) {
            err_at(e->line, e->col, "`insert` works on arrays, but this is %s", ty_show(a));
            return ty_void();
        }
        want(arg(e, 1), arg_type(e, 1), ty_int(), "the position");
        want(arg(e, 2), arg_type(e, 2), a->elem, "this array");
        return ty_void();
    }

    case BI_STR:
        if (!arity(e, 1, 1, name)) return ty_str();
        return ty_str();

    case BI_INT: {
        if (!arity(e, 1, 1, name)) return ty_int();
        Type *a = arg_type(e, 0);
        if (!is_err(a) && a->kind != TY_FLOAT && a->kind != TY_STR &&
            a->kind != TY_BOOL && a->kind != TY_INT)
            err_at(e->line, e->col, "`int` cannot convert %s", ty_show(a));
        return ty_int();
    }

    case BI_FLOAT: {
        if (!arity(e, 1, 1, name)) return ty_float();
        Type *a = arg_type(e, 0);
        if (!is_err(a) && a->kind != TY_INT && a->kind != TY_STR && a->kind != TY_FLOAT)
            err_at(e->line, e->col, "`float` cannot convert %s", ty_show(a));
        return ty_float();
    }

    case BI_ABS: {
        if (!arity(e, 1, 1, name)) return ty_int();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_int();
        if (!ty_is_num(a)) {
            err_at(e->line, e->col, "`abs` works on numbers, not on %s", ty_show(a));
            return ty_int();
        }
        return a;
    }

    case BI_MIN: case BI_MAX: {
        if (!arity(e, 2, 2, name)) return ty_int();
        Type *a = arg_type(e, 0), *b = arg_type(e, 1);
        if (is_err(a) || is_err(b)) return ty_int();
        if (!ty_is_num(a)) {
            err_at(e->line, e->col, "`%s` compares numbers, not %s", name, ty_show(a));
            return ty_int();
        }
        want(arg(e, 1), b, a, cx_fmt("`%s`", name));
        return a;
    }

    case BI_SQRT: case BI_FLOOR: case BI_CEIL: case BI_ROUND:
        if (!arity(e, 1, 1, name)) return ty_float();
        want(arg(e, 0), arg_type(e, 0), ty_float(), cx_fmt("`%s`", name));
        return ty_float();

    case BI_POW:
        if (!arity(e, 2, 2, name)) return ty_float();
        want(arg(e, 0), arg_type(e, 0), ty_float(), "`pow`");
        want(arg(e, 1), arg_type(e, 1), ty_float(), "`pow`");
        return ty_float();

    case BI_RAND_INT:
        if (!arity(e, 2, 2, name)) return ty_int();
        want(arg(e, 0), arg_type(e, 0), ty_int(), "`rand_int`");
        want(arg(e, 1), arg_type(e, 1), ty_int(), "`rand_int`");
        return ty_int();

    case BI_RAND_SEED:
        if (!arity(e, 1, 1, name)) return ty_void();
        want(arg(e, 0), arg_type(e, 0), ty_int(), "`rand_seed`");
        return ty_void();

    case BI_INPUT:
        arity(e, 0, 0, name);
        return ty_str();

    case BI_TIME_MS:
        arity(e, 0, 0, name);
        return ty_int();

    case BI_UPPER: case BI_LOWER: case BI_TRIM:
        if (!arity(e, 1, 1, name)) return ty_str();
        want(arg(e, 0), arg_type(e, 0), ty_str(), cx_fmt("`%s`", name));
        return ty_str();

    case BI_SPLIT:
        if (!arity(e, 2, 2, name)) return ty_array(ty_str());
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`split`");
        want(arg(e, 1), arg_type(e, 1), ty_str(), "`split`");
        return ty_array(ty_str());

    case BI_JOIN:
        if (!arity(e, 2, 2, name)) return ty_str();
        want(arg(e, 0), arg_type(e, 0), ty_array(ty_str()), "`join`");
        want(arg(e, 1), arg_type(e, 1), ty_str(), "`join`");
        return ty_str();

    case BI_FIND:
        if (!arity(e, 2, 2, name)) return ty_int();
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`find`");
        want(arg(e, 1), arg_type(e, 1), ty_str(), "`find`");
        return ty_int();

    case BI_SLICE: {
        if (!arity(e, 3, 3, name)) return ty_err();
        Type *a = arg_type(e, 0);
        want(arg(e, 1), arg_type(e, 1), ty_int(), "`slice`");
        want(arg(e, 2), arg_type(e, 2), ty_int(), "`slice`");
        if (is_err(a)) return ty_err();
        if (a->kind != TY_STR && a->kind != TY_ARRAY) {
            err_at(e->line, e->col, "`slice` works on text and arrays, not on %s", ty_show(a));
            return ty_err();
        }
        return a;
    }

    case BI_CONTAINS: {
        if (!arity(e, 2, 2, name)) return ty_bool();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_bool();
        if (a->kind == TY_STR) { want(arg(e, 1), arg_type(e, 1), ty_str(), "`contains`"); return ty_bool(); }
        if (a->kind == TY_ARRAY) {
            if (!primitive_elem(a->elem))
                err_at(e->line, e->col, "`contains` cannot compare values of type %s",
                       ty_show(a->elem));
            want(arg(e, 1), arg_type(e, 1), a->elem, "`contains`");
            return ty_bool();
        }
        err_at(e->line, e->col, "`contains` works on text and arrays, not on %s", ty_show(a));
        return ty_bool();
    }

    case BI_STARTS_WITH: case BI_ENDS_WITH:
        if (!arity(e, 2, 2, name)) return ty_bool();
        want(arg(e, 0), arg_type(e, 0), ty_str(), cx_fmt("`%s`", name));
        want(arg(e, 1), arg_type(e, 1), ty_str(), cx_fmt("`%s`", name));
        return ty_bool();

    case BI_REPLACE:
        if (!arity(e, 3, 3, name)) return ty_str();
        for (int i = 0; i < 3 && i < e->args.len; i++)
            want(arg(e, i), arg_type(e, i), ty_str(), "`replace`");
        return ty_str();

    case BI_REPEAT:
        if (!arity(e, 2, 2, name)) return ty_str();
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`repeat`");
        want(arg(e, 1), arg_type(e, 1), ty_int(), "`repeat`");
        return ty_str();

    case BI_CHAR_AT:
        if (!arity(e, 2, 2, name)) return ty_str();
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`char_at`");
        want(arg(e, 1), arg_type(e, 1), ty_int(), "`char_at`");
        return ty_str();

    case BI_CODE_AT:
        if (!arity(e, 2, 2, name)) return ty_int();
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`code_at`");
        want(arg(e, 1), arg_type(e, 1), ty_int(), "`code_at`");
        return ty_int();

    case BI_FROM_CODE:
        if (!arity(e, 1, 1, name)) return ty_str();
        want(arg(e, 0), arg_type(e, 0), ty_int(), "`from_code`");
        return ty_str();

    case BI_SORT: case BI_REVERSE: {
        if (!arity(e, 1, 1, name)) return ty_void();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_void();
        if (a->kind != TY_ARRAY) {
            err_at(e->line, e->col, "`%s` works on arrays, but this is %s", name, ty_show(a));
            return ty_void();
        }
        if (bi == BI_SORT && a->elem->kind != TY_INT && a->elem->kind != TY_FLOAT &&
            a->elem->kind != TY_STR) {
            err_at(e->line, e->col, "`sort` can order [int], [float], and [string], not %s",
                   ty_show(a));
        }
        return ty_void();
    }

    case BI_READ_FILE:
        if (!arity(e, 1, 1, name)) return ty_str();
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`read_file`");
        return ty_str();

    case BI_WRITE_FILE:
        if (!arity(e, 2, 2, name)) return ty_void();
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`write_file`");
        want(arg(e, 1), arg_type(e, 1), ty_str(), "`write_file`");
        return ty_void();

    case BI_PANIC:
        if (!arity(e, 1, 1, name)) return ty_void();
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`panic`");
        return ty_void();

    case BI_ASSERT:
        if (!arity(e, 1, 2, name)) return ty_void();
        want(arg(e, 0), arg_type(e, 0), ty_bool(), "`assert`");
        if (e->args.len == 2) want(arg(e, 1), arg_type(e, 1), ty_str(), "`assert`");
        return ty_void();

    /* ---- maps ---- */
    case BI_HAS: {
        if (!arity(e, 2, 2, name)) return ty_bool();
        Type *m = arg_type(e, 0);
        if (is_err(m)) return ty_bool();
        if (m->kind != TY_MAP) {
            err_at(e->line, e->col, "`has` asks a map about a key, but this is %s",
                   ty_show(m));
            if (m->kind == TY_ARRAY) err_help("for an array, use `contains(items, value)`");
            return ty_bool();
        }
        want(arg(e, 1), arg_type(e, 1), m->key, "this map's key");
        return ty_bool();
    }

    case BI_GET: {
        if (!arity(e, 3, 3, name)) return ty_err();
        Type *m = arg_type(e, 0);
        if (is_err(m)) return ty_err();
        if (m->kind != TY_MAP) {
            err_at(e->line, e->col, "`get` reads from a map, but this is %s", ty_show(m));
            return ty_err();
        }
        want(arg(e, 1), arg_type(e, 1), m->key, "this map's key");
        want(arg(e, 2), arg_type(e, 2), m->elem, "the value to fall back on");
        return m->elem;
    }

    case BI_KEYS: case BI_VALUES: {
        if (!arity(e, 1, 1, name)) return ty_err();
        Type *m = arg_type(e, 0);
        if (is_err(m)) return ty_err();
        if (m->kind != TY_MAP) {
            err_at(e->line, e->col, "`%s` works on maps, but this is %s", name, ty_show(m));
            return ty_err();
        }
        return ty_array(bi == BI_KEYS ? m->key : m->elem);
    }

    case BI_CLEAR: {
        if (!arity(e, 1, 1, name)) return ty_void();
        Type *a = arg_type(e, 0);
        if (!is_err(a) && a->kind != TY_ARRAY && a->kind != TY_MAP)
            err_at(e->line, e->col, "`clear` empties an array or a map, but this is %s",
                   ty_show(a));
        return ty_void();
    }

    /* ---- numbers ---- */
    case BI_SIN: case BI_COS: case BI_TAN: case BI_ASIN: case BI_ACOS:
    case BI_ATAN: case BI_LOG: case BI_LOG10: case BI_EXP:
        if (!arity(e, 1, 1, name)) return ty_float();
        want(arg(e, 0), arg_type(e, 0), ty_float(), cx_fmt("`%s`", name));
        return ty_float();

    case BI_ATAN2:
        if (!arity(e, 2, 2, name)) return ty_float();
        want(arg(e, 0), arg_type(e, 0), ty_float(), "`atan2`");
        want(arg(e, 1), arg_type(e, 1), ty_float(), "`atan2`");
        return ty_float();

    case BI_SIGN: {
        if (!arity(e, 1, 1, name)) return ty_int();
        Type *a = arg_type(e, 0);
        if (!is_err(a) && !ty_is_num(a))
            err_at(e->line, e->col, "`sign` works on numbers, not on %s", ty_show(a));
        return ty_int();
    }

    case BI_CLAMP: {
        if (!arity(e, 3, 3, name)) return ty_int();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_int();
        if (!ty_is_num(a)) {
            err_at(e->line, e->col, "`clamp` works on numbers, not on %s", ty_show(a));
            return ty_int();
        }
        want(arg(e, 1), arg_type(e, 1), a, "`clamp`");
        want(arg(e, 2), arg_type(e, 2), a, "`clamp`");
        return a;
    }

    case BI_IS_NAN: case BI_IS_INF:
        if (!arity(e, 1, 1, name)) return ty_bool();
        want(arg(e, 0), arg_type(e, 0), ty_float(), cx_fmt("`%s`", name));
        return ty_bool();

    case BI_RAND_FLOAT:
        arity(e, 0, 0, name);
        return ty_float();

    /* ---- text ---- */
    case BI_PAD_START: case BI_PAD_END:
        if (!arity(e, 3, 3, name)) return ty_str();
        want(arg(e, 0), arg_type(e, 0), ty_str(), cx_fmt("`%s`", name));
        want(arg(e, 1), arg_type(e, 1), ty_int(), cx_fmt("`%s`", name));
        want(arg(e, 2), arg_type(e, 2), ty_str(), cx_fmt("`%s`", name));
        return ty_str();

    case BI_TRIM_START: case BI_TRIM_END: case BI_CAPITALIZE:
        if (!arity(e, 1, 1, name)) return ty_str();
        want(arg(e, 0), arg_type(e, 0), ty_str(), cx_fmt("`%s`", name));
        return ty_str();

    case BI_LINES: case BI_CHARS:
        if (!arity(e, 1, 1, name)) return ty_array(ty_str());
        want(arg(e, 0), arg_type(e, 0), ty_str(), cx_fmt("`%s`", name));
        return ty_array(ty_str());

    case BI_LAST_INDEX_OF:
        if (!arity(e, 2, 2, name)) return ty_int();
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`last_index_of`");
        want(arg(e, 1), arg_type(e, 1), ty_str(), "`last_index_of`");
        return ty_int();

    case BI_COUNT: case BI_INDEX_OF: {
        if (!arity(e, 2, 2, name)) return ty_int();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_int();
        if (a->kind == TY_STR) {
            want(arg(e, 1), arg_type(e, 1), ty_str(), cx_fmt("`%s`", name));
            return ty_int();
        }
        if (a->kind == TY_ARRAY) {
            if (!primitive_elem(a->elem))
                err_at(e->line, e->col, "`%s` cannot compare values of type %s",
                       name, ty_show(a->elem));
            want(arg(e, 1), arg_type(e, 1), a->elem, cx_fmt("`%s`", name));
            return ty_int();
        }
        err_at(e->line, e->col, "`%s` works on text and arrays, not on %s", name, ty_show(a));
        return ty_int();
    }

    case BI_IS_DIGIT: case BI_IS_ALPHA: case BI_IS_ALNUM:
    case BI_IS_SPACE: case BI_IS_UPPER: case BI_IS_LOWER:
        if (!arity(e, 1, 1, name)) return ty_bool();
        want(arg(e, 0), arg_type(e, 0), ty_str(), cx_fmt("`%s`", name));
        return ty_bool();

    /* ---- arrays ---- */
    case BI_SUM: {
        if (!arity(e, 1, 1, name)) return ty_int();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_int();
        if (a->kind != TY_ARRAY || (a->elem->kind != TY_INT && a->elem->kind != TY_FLOAT)) {
            err_at(e->line, e->col, "`sum` adds up [int] or [float], not %s", ty_show(a));
            return ty_int();
        }
        return a->elem;
    }

    case BI_MIN_OF: case BI_MAX_OF: {
        if (!arity(e, 1, 1, name)) return ty_err();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_err();
        if (a->kind != TY_ARRAY || (a->elem->kind != TY_INT &&
            a->elem->kind != TY_FLOAT && a->elem->kind != TY_STR)) {
            err_at(e->line, e->col, "`%s` works on [int], [float], and [string], not %s",
                   name, ty_show(a));
            return ty_err();
        }
        return a->elem;
    }

    case BI_COPY: {
        if (!arity(e, 1, 1, name)) return ty_err();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_err();
        if (a->kind != TY_ARRAY) {
            err_at(e->line, e->col, "`copy` duplicates an array, but this is %s", ty_show(a));
            return ty_err();
        }
        return a;
    }

    case BI_CONCAT: {
        if (!arity(e, 2, 2, name)) return ty_err();
        Type *a = arg_type(e, 0);
        if (is_err(a)) return ty_err();
        if (a->kind != TY_ARRAY) {
            err_at(e->line, e->col, "`concat` joins two arrays, but this is %s", ty_show(a));
            if (a->kind == TY_STR) err_help("join text with `+`");
            return ty_err();
        }
        want(arg(e, 1), arg_type(e, 1), a, "`concat`");
        return a;
    }

    case BI_SHUFFLE: {
        if (!arity(e, 1, 1, name)) return ty_void();
        Type *a = arg_type(e, 0);
        if (!is_err(a) && a->kind != TY_ARRAY)
            err_at(e->line, e->col, "`shuffle` works on arrays, but this is %s", ty_show(a));
        return ty_void();
    }

    case BI_SWAP: {
        if (!arity(e, 3, 3, name)) return ty_void();
        Type *a = arg_type(e, 0);
        if (!is_err(a) && a->kind != TY_ARRAY)
            err_at(e->line, e->col, "`swap` works on arrays, but this is %s", ty_show(a));
        want(arg(e, 1), arg_type(e, 1), ty_int(), "`swap`");
        want(arg(e, 2), arg_type(e, 2), ty_int(), "`swap`");
        return ty_void();
    }

    /* ---- the world outside ---- */
    case BI_EPRINT:  return ty_void();
    case BI_EXIT:
        if (!arity(e, 1, 1, name)) return ty_void();
        want(arg(e, 0), arg_type(e, 0), ty_int(), "`exit`");
        return ty_void();
    case BI_ARGS:     arity(e, 0, 0, name); return ty_array(ty_str());
    case BI_CLOCK_MS: arity(e, 0, 0, name); return ty_int();
    case BI_ENV:
        if (!arity(e, 1, 1, name)) return ty_str();
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`env`");
        return ty_str();
    case BI_SLEEP_MS:
        if (!arity(e, 1, 1, name)) return ty_void();
        want(arg(e, 0), arg_type(e, 0), ty_int(), "`sleep_ms`");
        return ty_void();

    case BI_FILE_EXISTS:
        if (!arity(e, 1, 1, name)) return ty_bool();
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`file_exists`");
        return ty_bool();
    case BI_DELETE_FILE:
        if (!arity(e, 1, 1, name)) return ty_void();
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`delete_file`");
        return ty_void();
    case BI_APPEND_FILE:
        if (!arity(e, 2, 2, name)) return ty_void();
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`append_file`");
        want(arg(e, 1), arg_type(e, 1), ty_str(), "`append_file`");
        return ty_void();
    case BI_READ_LINES:
        if (!arity(e, 1, 1, name)) return ty_array(ty_str());
        want(arg(e, 0), arg_type(e, 0), ty_str(), "`read_lines`");
        return ty_array(ty_str());
    }
    return ty_err();
}

/* Check the arguments of a call against a declared parameter list. */
static void check_args(Expr *e, FnDecl *f, const char *what) {
    if (e->args.len != f->params.len) {
        err_at(e->line, e->col, "%s takes %d argument%s, but got %d",
               what, f->params.len, f->params.len == 1 ? "" : "s", e->args.len);
        err_help("it is declared on line %d", f->line);
    }
    for (int i = 0; i < e->args.len; i++) {
        VarSym *pv = i < f->params.len ? f->params.items[i] : NULL;
        Type *at_ = check_expr(arg(e, i), pv ? pv->type : NULL);
        if (pv) want(arg(e, i), at_, pv->type, cx_fmt("parameter `%s`", pv->name));
    }
}

/* obj.method(args) and super.method(args) */
static Type *check_method_call(Expr *e) {
    Expr *target = e->a;                 /* the EX_FIELD node */
    const char *name = target->name;
    bool via_super = target->a->kind == EX_SUPER;

    Type *obj = check_expr(target->a, NULL);
    if (is_err(obj)) {
        for (int i = 0; i < e->args.len; i++) check_expr(arg(e, i), NULL);
        return ty_err();
    }
    if (obj->kind != TY_CLASS) {
        for (int i = 0; i < e->args.len; i++) check_expr(arg(e, i), NULL);
        err_at(e->line, e->col, "%s has no methods", ty_show(obj));
        if (obj->kind == TY_STRUCT)
            err_help("a `type ... = struct` holds data only; use a `class` for methods");
        return ty_err();
    }

    FnDecl *m = find_method(obj->cdef, name);
    if (!m) {
        for (int i = 0; i < e->args.len; i++) check_expr(arg(e, i), NULL);
        err_at(target->line, target->col, "`%s` has no method named `%s`",
               obj->cdef->name, name);
        Vec fields = {0}, methods = {0};
        collect_members(obj->cdef, &fields, &methods);
        const char *mm = best_match(name, &methods);
        if (mm) err_help("did you mean `%s`?", mm);
        else if (find_field(obj->cdef, name, NULL))
            err_help("`%s` is a field, not a method; drop the `( )`", name);
        return ty_err();
    }
    if (m->is_init) {
        bool ok_super_init = via_super && cur_method && cur_method->is_init;
        if (!ok_super_init) {
            err_at(e->line, e->col, "`init` runs when the object is made, "
                   "so it cannot be called again");
            err_help("`super.init(...)` may only appear inside another `init`");
            return ty_err();
        }
    }

    e->kind = EX_METHOD;
    e->fn = m;
    e->obj_type = obj;
    e->name = cx_strdup(name);
    e->enum_index = via_super;           /* remember: skip virtual dispatch */
    check_args(e, m, cx_fmt("`%s.%s`", obj->cdef->name, name));
    return m->ret;
}

static Type *check_call(Expr *e) {
    if (e->a && e->a->kind == EX_FIELD) return check_method_call(e);

    if (!e->a || e->a->kind != EX_IDENT) {
        err_at(e->line, e->col, "only named functions can be called");
        err_help("Cub does not have function values yet");
        return ty_err();
    }
    const char *name = e->a->name;

    /* `Dog("Rex")` -- making an object */
    Type *named = find_named(name);
    if (named && named->kind == TY_CLASS) {
        ClassDef *cd = named->cdef;
        e->kind = EX_NEW;
        e->cls = cd;
        if (cd->init) {
            check_args(e, cd->init, cx_fmt("`%s`", cd->name));
        } else {
            for (int i = 0; i < e->args.len; i++) check_expr(arg(e, i), NULL);
            if (e->args.len != 0) {
                err_at(e->line, e->col, "`%s` takes no arguments, but got %d",
                       cd->name, e->args.len);
                err_help("give it an `init` if it should be built from values");
            }
        }
        return named;
    }

    FnDecl *f = find_fn(name);
    if (f) {
        e->fn = f;
        if (e->args.len != f->params.len) {
            err_at(e->line, e->col, "`%s` takes %d argument%s, but got %d",
                   name, f->params.len, f->params.len == 1 ? "" : "s", e->args.len);
            err_help("it is declared on line %d", f->line);
        }
        for (int i = 0; i < e->args.len; i++) {
            Type *at_ = check_expr(arg(e, i), i < f->params.len
                                  ? ((VarSym *)f->params.items[i])->type : NULL);
            if (i < f->params.len) {
                VarSym *pv = f->params.items[i];
                want(arg(e, i), at_, pv->type, cx_fmt("parameter `%s`", pv->name));
            }
        }
        return f->ret;
    }

    int bi = builtin_lookup(name);
    if (bi != BI_NONE) return check_builtin(e, bi);

    for (int i = 0; i < e->args.len; i++) check_expr(arg(e, i), NULL);
    VarSym *v = lookup(name);
    if (v) {
        err_at(e->line, e->col, "`%s` is a variable, not a function", name);
        return ty_err();
    }
    if (cur_class && find_method(cur_class, name)) {
        err_at(e->line, e->col, "there is no function named `%s`", name);
        err_help("`%s` is a method of `%s`; call it with `self.%s(...)`",
                 name, cur_class->name, name);
        return ty_err();
    }
    err_at(e->line, e->col, "there is no function named `%s`", name);
    suggest_name(name);
    return ty_err();
}

static Type *check_binary(Expr *e) {
    Type *a = check_expr(e->a, NULL);
    Type *b = check_expr(e->b, a && a->kind != TY_ERR ? a : NULL);
    if (is_err(a) || is_err(b)) return ty_err();

    switch (e->op) {
    case TK_ANDAND: case TK_OROR:
        if (a->kind != TY_BOOL || b->kind != TY_BOOL) {
            err_at(e->line, e->col, "`%s` combines two conditions, but got %s and %s",
                   e->op == TK_ANDAND ? "and" : "or", ty_show(a), ty_show(b));
            err_help("Cub has no truthiness: compare explicitly, as in `count > 0`");
        }
        return ty_bool();

    case TK_EQ: case TK_NE:
        if (a->kind == TY_CLASS && b->kind == TY_CLASS &&
            (ty_assignable(a, b) || ty_assignable(b, a)))
            return ty_bool();               /* compares identity, not contents */
        if (!ty_same(a, b)) {
            err_at(e->line, e->col, "cannot compare %s with %s", ty_show(a), ty_show(b));
            if (ty_is_num(a) && ty_is_num(b))
                err_help("convert one side first, with `int(x)` or `float(x)`");
            return ty_bool();
        }
        if (a->kind == TY_ARRAY || a->kind == TY_STRUCT || a->kind == TY_MAP) {
            err_at(e->line, e->col, "%s values cannot be compared with `==` yet",
                   ty_show(a));
            err_help("compare their fields or items instead");
        }
        return ty_bool();

    case TK_LT: case TK_LE: case TK_GT: case TK_GE:
        if (!ty_same(a, b)) {
            err_at(e->line, e->col, "cannot compare %s with %s", ty_show(a), ty_show(b));
            if (ty_is_num(a) && ty_is_num(b))
                err_help("convert one side first, with `int(x)` or `float(x)`");
        } else if (!ty_is_num(a) && a->kind != TY_STR) {
            err_at(e->line, e->col, "%s values have no order", ty_show(a));
        }
        return ty_bool();

    case TK_PLUS:
        if (a->kind == TY_STR && b->kind == TY_STR) return ty_str();
        /* fall through */
    case TK_MINUS: case TK_STAR: case TK_SLASH: case TK_PERCENT: {
        const char *verb = e->op == TK_PLUS ? "add" : e->op == TK_MINUS ? "subtract"
                         : e->op == TK_STAR ? "multiply" : e->op == TK_SLASH ? "divide"
                         : "take the remainder of";
        if (!ty_same(a, b)) {
            err_at(e->line, e->col, "cannot %s %s and %s", verb, ty_show(a), ty_show(b));
            if (a->kind == TY_STR || b->kind == TY_STR)
                err_help("turn the other side into text with `str(x)`, "
                         "or write \"...{value}...\"");
            else if (ty_is_num(a) && ty_is_num(b))
                err_help("Cub never mixes int and float silently; "
                         "use `float(x)` or `int(x)`");
            return ty_err();
        }
        if (!ty_is_num(a)) {
            err_at(e->line, e->col, "cannot %s two %s values", verb, ty_show(a));
            if (a->kind == TY_ARRAY && e->op == TK_PLUS)
                err_help("build a new array with `push`, or loop over both");
            return ty_err();
        }
        return a;
    }
    default:
        return ty_err();
    }
}

static Type *check_field(Expr *e) {
    /* `Color.Red` -- an enum value rather than a field access */
    if (e->a->kind == EX_IDENT && !lookup(e->a->name)) {
        Type *t = find_named(e->a->name);
        if (t && t->kind == TY_ENUM) {
            EnumDef *ed = t->edef;
            for (int i = 0; i < ed->vals.len; i++) {
                if (strcmp((char *)ed->vals.items[i], e->name) == 0) {
                    e->kind = EX_ENUMVAL;
                    e->enum_index = i;
                    e->name = cx_strdup(e->name);
                    e->sval = ed->name;
                    return t;
                }
            }
            err_at(e->line, e->col, "`%s` has no value named `%s`", ed->name, e->name);
            const char *m = best_match(e->name, &ed->vals);
            if (m) {
                err_help("did you mean `%s.%s`?", ed->name, m);
            } else {
                Buf list;
                buf_init(&list);
                for (int i = 0; i < ed->vals.len; i++)
                    buf_printf(&list, "%s%s", i ? ", " : "", (char *)ed->vals.items[i]);
                err_help("`%s` holds: %s", ed->name, list.data);
            }
            return ty_err();
        }
        if (t && t->kind == TY_STRUCT) {
            err_at(e->line, e->col, "`%s` is a type, not a value", e->a->name);
            err_help("make one with `%s { ... }` first", e->a->name);
            return ty_err();
        }
    }

    Type *a = check_expr(e->a, NULL);
    if (is_err(a)) return ty_err();

    if (a->kind == TY_STRUCT) {
        StructDef *sd = a->sdef;
        for (int i = 0; i < sd->fnames.len; i++)
            if (strcmp((char *)sd->fnames.items[i], e->name) == 0)
                return sd->ftypes.items[i];
        err_at(e->line, e->col, "`%s` has no field named `%s`", sd->name, e->name);
        const char *m = best_match(e->name, &sd->fnames);
        if (m) err_help("did you mean `%s`?", m);
        return ty_err();
    }

    if (a->kind == TY_CLASS) {
        Type *ft = NULL;
        ClassDef *owner = find_field(a->cdef, e->name, &ft);
        if (owner) { e->obj_type = a; return ft; }
        err_at(e->line, e->col, "`%s` has no field named `%s`", a->cdef->name, e->name);
        Vec fields = {0}, methods = {0};
        collect_members(a->cdef, &fields, &methods);
        const char *m = best_match(e->name, &fields);
        if (m) err_help("did you mean `%s`?", m);
        else if (find_method(a->cdef, e->name))
            err_help("`%s` is a method; call it with `%s()`", e->name, e->name);
        return ty_err();
    }

    if (a->kind == TY_ARRAY || a->kind == TY_STR || a->kind == TY_MAP) {
        err_at(e->line, e->col, "%s has no field `%s`", ty_show(a), e->name);
        if (strcmp(e->name, "len") == 0 || strcmp(e->name, "length") == 0 ||
            strcmp(e->name, "size") == 0 || strcmp(e->name, "count") == 0)
            err_help("ask for the length with `len(x)`");
        return ty_err();
    }

    err_at(e->line, e->col, "%s has no fields", ty_show(a));
    return ty_err();
}

static Type *check_structlit(Expr *e) {
    Type *t = find_named(e->name);
    if (!t) {
        err_at(e->line, e->col, "there is no type named `%s`", e->name);
        for (int i = 0; i < e->args.len; i++) check_expr(arg(e, i), NULL);
        return ty_err();
    }
    if (t->kind != TY_STRUCT) {
        err_at(e->line, e->col, "`%s` is an enum, so it has no fields", e->name);
        err_help("write `%s.%s` to pick a value", e->name,
                 t->edef->vals.len ? (char *)t->edef->vals.items[0] : "Value");
        return ty_err();
    }
    StructDef *sd = t->sdef;
    bool *seen = cx_alloc(sizeof(bool) * (size_t)(sd->fnames.len + 1));

    for (int i = 0; i < e->args.len; i++) {
        const char *fname = e->fnames.items[i];
        int idx = -1;
        for (int j = 0; j < sd->fnames.len; j++)
            if (strcmp((char *)sd->fnames.items[j], fname) == 0) { idx = j; break; }
        if (idx < 0) {
            check_expr(arg(e, i), NULL);
            err_at(arg(e, i)->line, arg(e, i)->col, "`%s` has no field named `%s`",
                   sd->name, fname);
            const char *m = best_match(fname, &sd->fnames);
            if (m) err_help("did you mean `%s`?", m);
            continue;
        }
        if (seen[idx])
            err_at(arg(e, i)->line, arg(e, i)->col, "field `%s` is given twice", fname);
        seen[idx] = true;
        Type *ft = sd->ftypes.items[idx];
        Type *at_ = check_expr(arg(e, i), ft);
        want(arg(e, i), at_, ft, cx_fmt("field `%s`", fname));
    }

    for (int j = 0; j < sd->fnames.len; j++) {
        if (!seen[j]) {
            err_at(e->line, e->col, "`%s` is missing field `%s`", sd->name,
                   (char *)sd->fnames.items[j]);
            err_help("every field must be given a value: `%s: <%s>`",
                     (char *)sd->fnames.items[j], ty_show(sd->ftypes.items[j]));
        }
    }
    return t;
}

static Type *check_expr(Expr *e, Type *hint) {
    if (!e) return ty_err();
    Type *t = ty_err();

    switch (e->kind) {
    case EX_INT:   t = ty_int();   break;
    case EX_FLOAT: t = ty_float(); break;
    case EX_BOOL:  t = ty_bool();  break;
    case EX_STR:   t = ty_str();   break;

    case EX_INTERP:
        for (int i = 0; i < e->args.len; i++) {
            Type *pt = check_expr(arg(e, i), NULL);
            if (pt && pt->kind == TY_VOID)
                err_at(arg(e, i)->line, arg(e, i)->col,
                       "this produces no value, so there is nothing to insert");
        }
        t = ty_str();
        break;

    case EX_IDENT: {
        VarSym *v = lookup(e->name);
        if (v) { e->var = v; t = v->type; break; }

        /* constants everyone expects to be there */
        static const struct { const char *name; double f; bool is_int; int64_t i; } consts[] = {
            {"PI",      3.14159265358979323846, false, 0},
            {"TAU",     6.28318530717958647692, false, 0},
            {"E",       2.71828182845904523536, false, 0},
            {"INF",     HUGE_VAL,               false, 0},
            {"NAN",     0.0,                    false, 0},
            {"INT_MAX", 0, true,  9223372036854775807LL},
            {"INT_MIN", 0, true, -9223372036854775807LL - 1},
            {NULL, 0, false, 0}
        };
        bool matched = false;
        for (int ci = 0; consts[ci].name; ci++) {
            if (strcmp(consts[ci].name, e->name) != 0) continue;
            if (consts[ci].is_int) { e->kind = EX_INT; e->ival = consts[ci].i; t = ty_int(); }
            else {
                e->kind = EX_FLOAT;
                e->fval = strcmp(e->name, "NAN") == 0 ? (0.0 / 0.0) : consts[ci].f;
                t = ty_float();
            }
            matched = true;
            break;
        }
        if (matched) break;
        Type *nt = find_named(e->name);
        if (nt && nt->kind == TY_ENUM) {
            err_at(e->line, e->col, "`%s` is a type; pick one of its values", e->name);
            err_help("write `%s.%s`", e->name,
                     nt->edef->vals.len ? (char *)nt->edef->vals.items[0] : "Value");
            break;
        }
        if (nt) {
            err_at(e->line, e->col, "`%s` is a type, not a value", e->name);
            err_help("make one with `%s { ... }`", e->name);
            break;
        }
        if (find_fn(e->name) || builtin_lookup(e->name) != BI_NONE) {
            err_at(e->line, e->col, "`%s` is a function; call it with `%s(...)`",
                   e->name, e->name);
            break;
        }
        if (cur_class && find_field(cur_class, e->name, NULL)) {
            err_at(e->line, e->col, "`%s` is not defined here", e->name);
            err_help("`%s` is a field of `%s`; write `self.%s`",
                     e->name, cur_class->name, e->name);
            break;
        }
        if (cur_class && find_method(cur_class, e->name)) {
            err_at(e->line, e->col, "`%s` is not defined here", e->name);
            err_help("`%s` is a method of `%s`; call it with `self.%s(...)`",
                     e->name, cur_class->name, e->name);
            break;
        }
        err_at(e->line, e->col, "`%s` is not defined here", e->name);
        suggest_name(e->name);
        break;
    }

    case EX_UNARY: {
        Type *a = check_expr(e->a, hint);
        if (is_err(a)) break;
        if (e->op == TK_MINUS) {
            if (!ty_is_num(a)) {
                err_at(e->line, e->col, "cannot negate %s", ty_show(a));
                break;
            }
            t = a;
        } else {
            if (a->kind != TY_BOOL) {
                err_at(e->line, e->col, "`not` needs a condition, but this is %s", ty_show(a));
                err_help("Cub has no truthiness: compare explicitly, as in `count == 0`");
            }
            t = ty_bool();
        }
        break;
    }

    case EX_SELF:
        if (!cur_class) {
            err_at(e->line, e->col, "`self` only means something inside a method");
            break;
        }
        t = find_named(cur_class->name);
        break;

    case EX_SUPER:
        if (!cur_class) {
            err_at(e->line, e->col, "`super` only means something inside a method");
            break;
        }
        if (!cur_class->base) {
            err_at(e->line, e->col, "`%s` is not built on another class, "
                   "so it has no `super`", cur_class->name);
            err_help("write `class %s : Parent { ... }` to build on one", cur_class->name);
            break;
        }
        t = find_named(cur_class->base->name);
        break;

    case EX_BINARY: t = check_binary(e); break;
    case EX_CALL:   t = check_call(e);   break;

    case EX_INDEX: {
        Type *a = check_expr(e->a, NULL);
        Type *i = check_expr(e->b, (a && a->kind == TY_MAP) ? a->key : ty_int());
        if (!is_err(i) && a && a->kind != TY_MAP) want(e->b, i, ty_int(), "an index");
        if (is_err(a)) break;
        if (a->kind == TY_ARRAY) { t = a->elem; break; }
        if (a->kind == TY_MAP) {
            want(e->b, i, a->key, "this map's key");
            t = a->elem;
            break;
        }
        if (a->kind == TY_STR) {
            err_at(e->line, e->col, "text cannot be indexed with `[ ]`");
            err_help("use `char_at(s, i)` for a one-character string, "
                     "or `code_at(s, i)` for its number");
            break;
        }
        err_at(e->line, e->col, "%s cannot be indexed", ty_show(a));
        break;
    }

    case EX_FIELD: t = check_field(e); break;

    case EX_ARRAYLIT: {
        Type *elem = (hint && hint->kind == TY_ARRAY) ? hint->elem : NULL;
        if (e->args.len == 0) {
            if (!elem) {
                err_at(e->line, e->col, "the type of this empty array is unclear");
                err_help("add a type, as in `var items: [int] = []`");
                break;
            }
            t = ty_array(elem);
            break;
        }
        Type *first = check_expr(arg(e, 0), elem);
        if (!elem) elem = first;
        else want(arg(e, 0), first, elem, "this array");
        for (int i = 1; i < e->args.len; i++) {
            Type *it = check_expr(arg(e, i), elem);
            if (!is_err(it) && !is_err(elem) && !ty_assignable(it, elem)) {
                err_at(arg(e, i)->line, arg(e, i)->col,
                       "this array holds %s, but item %d is %s", ty_show(elem), i + 1, ty_show(it));
                err_help("every item in an array has the same type");
            }
        }
        if (is_err(elem)) break;
        if (elem->kind == TY_VOID) {
            err_at(e->line, e->col, "an array cannot hold values that do not exist");
            break;
        }
        t = ty_array(elem);
        break;
    }

    case EX_IFEXPR: {
        Type *c = check_expr(e->a, ty_bool());
        if (!is_err(c) && c->kind != TY_BOOL) {
            err_at(e->a->line, e->a->col,
                   "`if` needs a condition, but this is %s", ty_show(c));
            err_help("Cub has no truthiness: write a comparison, as in `count > 0`");
        }
        Type *then_t = check_expr(e->b, hint);
        Type *else_t = check_expr(arg(e, 0), !is_err(then_t) ? then_t : hint);
        if (is_err(then_t) || is_err(else_t)) break;
        if (then_t->kind == TY_VOID || else_t->kind == TY_VOID) {
            err_at(e->line, e->col, "an `if` used as a value must produce something "
                   "in both branches");
            break;
        }
        if (!ty_same(then_t, else_t)) {
            err_at(arg(e, 0)->line, arg(e, 0)->col,
                   "both branches must give the same type, but this one is %s "
                   "and the other is %s", ty_show(else_t), ty_show(then_t));
            err_help("convert one of them, or use an `if` statement instead");
            break;
        }
        t = then_t;
        break;
    }

    case EX_MAPLIT: {
        Type *kt = (hint && hint->kind == TY_MAP) ? hint->key : NULL;
        Type *vt = (hint && hint->kind == TY_MAP) ? hint->elem : NULL;
        if (e->args.len == 0) {
            if (!kt) {
                err_at(e->line, e->col, "the type of this empty map is unclear");
                err_help("add a type, as in `var ages: [string: int] = [:]`");
                break;
            }
            t = ty_map(kt, vt);
            break;
        }
        for (int i = 0; i + 1 < e->args.len; i += 2) {
            Type *k = check_expr(arg(e, i), kt);
            Type *v = check_expr(arg(e, i + 1), vt);
            if (!kt && !is_err(k)) kt = k;
            if (!vt && !is_err(v)) vt = v;
            if (kt) want(arg(e, i), k, kt, "this map's keys");
            if (vt) want(arg(e, i + 1), v, vt, "this map's values");
        }
        if (!kt || !vt || is_err(kt) || is_err(vt)) break;
        if (kt->kind != TY_STR && kt->kind != TY_INT) {
            err_at(e->line, e->col, "a map key must be a string or an int, not %s",
                   ty_show(kt));
            break;
        }
        t = ty_map(kt, vt);
        break;
    }

    case EX_STRUCTLIT: t = check_structlit(e); break;
    case EX_ENUMVAL:   t = find_named(e->sval); break;

    case EX_NEW:    /* rewritten from EX_CALL while checking; already typed */
    case EX_METHOD: t = e->type ? e->type : ty_err(); break;
    }

    e->type = t ? t : ty_err();
    return e->type;
}

/* ------------------------------------------------------------------ */
/* assignability                                                       */
/* ------------------------------------------------------------------ */

/* Walk to the root variable of an assignment target.  Arrays are shared
 * references, so reaching through `[ ]` escapes the root's mutability;
 * struct fields are part of the value, so they do not. */
static void check_assignable(Expr *lhs) {
    Expr *e = lhs;
    bool through_ref = false;
    for (;;) {
        if (e->kind == EX_IDENT) break;
        if (e->kind == EX_SELF || e->kind == EX_SUPER) return;
        if (e->kind == EX_FIELD) {
            /* reaching through an object is reaching through a reference */
            if (e->a->type && e->a->type->kind == TY_CLASS) through_ref = true;
            e = e->a;
            continue;
        }
        if (e->kind == EX_INDEX) { through_ref = true; e = e->a; continue; }
        err_at(lhs->line, lhs->col, "this is not something you can assign to");
        err_help("assign to a variable, an array item, or a struct field");
        return;
    }
    VarSym *v = e->var;
    if (!v) return;
    if (through_ref) return;
    if (!v->is_mut) {
        if (lhs->kind == EX_IDENT)
            err_at(lhs->line, lhs->col, "`%s` was declared with `let`, so it never changes",
                   v->name);
        else
            err_at(lhs->line, lhs->col, "`%s` was declared with `let`, so its fields never change",
                   v->name);
        err_help("declare it with `var %s = ...` if it needs to change", v->name);
    }
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static void check_stmt(Stmt *s) {
    switch (s->kind) {
    case ST_LET: {
        Type *want_ = s->decl_type ? resolve_type(s->decl_type, s->line, s->col) : NULL;
        Type *got = check_expr(s->rhs, want_);
        if (got && got->kind == TY_VOID) {
            err_at(s->rhs->line, s->rhs->col, "`%s` produces no value, so `%s` has nothing to hold",
                   s->rhs->kind == EX_CALL ? s->rhs->name : "this", s->name);
            got = ty_err();
        }
        if (want_) want(s->rhs, got, want_, cx_fmt("`%s`", s->name));
        s->decl_type = want_ ? want_ : got;
        s->var = declare(s->name, s->decl_type, s->is_mut, false, s->line, s->col);
        break;
    }

    case ST_ASSIGN: {
        Type *l = check_expr(s->lhs, NULL);
        Type *r = check_expr(s->rhs, l);
        check_assignable(s->lhs);
        if (is_err(l) || is_err(r)) break;
        if (s->op == TK_ASSIGN) {
            want(s->rhs, r, l, "this variable");
            break;
        }
        const char *opname = s->op == TK_PLUSEQ ? "+=" : s->op == TK_MINUSEQ ? "-="
                           : s->op == TK_STAREQ ? "*=" : s->op == TK_SLASHEQ ? "/=" : "%=";
        if (s->op == TK_PLUSEQ && l->kind == TY_STR) {
            want(s->rhs, r, ty_str(), "`+=` on text");
            break;
        }
        if (!ty_is_num(l)) {
            err_at(s->line, s->col, "`%s` works on numbers, but this is %s", opname, ty_show(l));
            break;
        }
        want(s->rhs, r, l, cx_fmt("`%s`", opname));
        break;
    }

    case ST_EXPR: {
        Type *t = check_expr(s->rhs, NULL);
        if (s->rhs->kind != EX_CALL && s->rhs->kind != EX_METHOD &&
            s->rhs->kind != EX_NEW) {
            err_at(s->line, s->col, "this value is computed and then thrown away");
            err_help("did you mean to assign it, or to call a function?");
        } else if (t && t->kind != TY_VOID && s->rhs->fn) {
            /* calling a value-returning function and ignoring it is fine */
        }
        break;
    }

    case ST_IF: {
        Type *c = check_expr(s->cond, ty_bool());
        if (!is_err(c) && c->kind != TY_BOOL) {
            err_at(s->cond->line, s->cond->col,
                   "`if` needs a condition, but this is %s", ty_show(c));
            err_help("Cub has no truthiness: write a comparison, as in `count > 0`");
        }
        push_scope(); check_stmts(&s->body); pop_scope();
        push_scope(); check_stmts(&s->els);  pop_scope();
        break;
    }

    case ST_WHILE: {
        Type *c = check_expr(s->cond, ty_bool());
        if (!is_err(c) && c->kind != TY_BOOL) {
            err_at(s->cond->line, s->cond->col,
                   "`while` needs a condition, but this is %s", ty_show(c));
            err_help("Cub has no truthiness: write a comparison, as in `i < 10`");
        }
        loop_depth++;
        push_scope(); check_stmts(&s->body); pop_scope();
        loop_depth--;
        break;
    }

    case ST_FORRANGE: {
        Type *a = check_expr(s->from, ty_int());
        Type *b = check_expr(s->to, ty_int());
        if (!is_err(a)) want(s->from, a, ty_int(), "a range");
        if (!is_err(b)) want(s->to, b, ty_int(), "a range");
        push_scope();
        s->var = declare(s->name, ty_int(), false, false, s->line, s->col);
        loop_depth++;
        check_stmts(&s->body);
        loop_depth--;
        pop_scope();
        break;
    }

    case ST_FORIN: {
        Type *a = check_expr(s->rhs, NULL);
        Type *elem = ty_err();
        if (!is_err(a)) {
            if (a->kind == TY_ARRAY) elem = a->elem;
            else if (a->kind == TY_STR) {
                err_at(s->rhs->line, s->rhs->col, "text cannot be looped over directly");
                err_help("loop over positions instead: `for i in 0..len(s) { ... char_at(s, i) }`");
            } else {
                err_at(s->rhs->line, s->rhs->col, "`for ... in` needs an array or a range, "
                       "but this is %s", ty_show(a));
                err_help("count with `for %s in 0..10 { }`", s->name);
            }
        }
        push_scope();
        s->var = declare(s->name, elem, false, false, s->line, s->col);
        loop_depth++;
        check_stmts(&s->body);
        loop_depth--;
        pop_scope();
        break;
    }

    case ST_RETURN: {
        Type *want_ = cur_fn ? cur_fn->ret : ty_void();
        if (s->rhs) {
            Type *got = check_expr(s->rhs, want_);
            if (want_->kind == TY_VOID) {
                if (!is_err(got)) {
                    err_at(s->line, s->col, "`%s` returns nothing, so it cannot return a value",
                           cur_fn->name);
                    err_help("declare what it gives back: `fn %s(...) -> %s`",
                             cur_fn->name, ty_show(got));
                }
            } else {
                want(s->rhs, got, want_, cx_fmt("`%s`", cur_fn->name));
            }
        } else if (want_->kind != TY_VOID) {
            err_at(s->line, s->col, "`%s` must return %s", cur_fn->name, ty_show(want_));
            err_help("write `return <value>`");
        }
        break;
    }

    case ST_BREAK:
    case ST_CONTINUE:
        if (loop_depth == 0) {
            err_at(s->line, s->col, "`%s` only makes sense inside a loop",
                   s->kind == ST_BREAK ? "break" : "continue");
        }
        break;

    case ST_BLOCK:
        push_scope();
        check_stmts(&s->body);
        pop_scope();
        break;
    }
}

static void check_stmts(Vec *body) {
    for (int i = 0; i < body->len; i++) check_stmt(body->items[i]);
}

/* Does this block call `super.init(...)` anywhere? */
static bool calls_super_init(Vec *body) {
    for (int i = 0; i < body->len; i++) {
        Stmt *st = body->items[i];
        if (st->kind == ST_EXPR && st->rhs && st->rhs->kind == EX_METHOD &&
            st->rhs->fn && st->rhs->fn->is_init && st->rhs->enum_index)
            return true;
        if (calls_super_init(&st->body) || calls_super_init(&st->els)) return true;
    }
    return false;
}

/* Does this block contain a `break` for the loop we are looking at?
 * Nested loops own their own breaks, so we do not descend into them. */
static bool has_break(Vec *body) {
    for (int i = 0; i < body->len; i++) {
        Stmt *s = body->items[i];
        if (s->kind == ST_BREAK) return true;
        if (s->kind == ST_IF && (has_break(&s->body) || has_break(&s->els))) return true;
        if (s->kind == ST_BLOCK && has_break(&s->body)) return true;
    }
    return false;
}

/* Does this block return on every path? */
static bool always_returns(Vec *body) {
    for (int i = 0; i < body->len; i++) {
        Stmt *s = body->items[i];
        if (s->kind == ST_RETURN) return true;
        if (s->kind == ST_BLOCK && always_returns(&s->body)) return true;
        if (s->kind == ST_IF && s->els.len > 0 &&
            always_returns(&s->body) && always_returns(&s->els)) return true;
        if (s->kind == ST_EXPR && s->rhs && s->rhs->kind == EX_CALL &&
            s->rhs->builtin == BI_PANIC) return true;
        /* `while true { ... }` only ends by returning, unless it breaks out */
        if (s->kind == ST_WHILE && s->cond->kind == EX_BOOL && s->cond->bval &&
            !has_break(&s->body)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* struct layout sanity                                                */
/* ------------------------------------------------------------------ */

static bool contains_struct(Type *t, StructDef *target, Vec *seen) {
    if (!t || t->kind != TY_STRUCT) return false;      /* arrays break the cycle */
    if (t->sdef == target) return true;
    for (int i = 0; i < seen->len; i++)
        if (seen->items[i] == t->sdef) return false;
    vec_push(seen, t->sdef);
    for (int i = 0; i < t->sdef->ftypes.len; i++)
        if (contains_struct(t->sdef->ftypes.items[i], target, seen)) return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* program                                                             */
/* ------------------------------------------------------------------ */

void check_program(Program *p) {
    prog = p;
    push_scope();

    /* 1. register named types */
    for (int i = 0; i < p->structs.len; i++) {
        StructDef *sd = p->structs.items[i];
        if (find_named(sd->name)) {
            err_at(sd->line, sd->col, "a type named `%s` already exists", sd->name);
            continue;
        }
        Type *t = ty_named(TY_STRUCT, sd->name);
        t->sdef = sd;
        vec_push(&named_types, t);
    }
    for (int i = 0; i < p->enums.len; i++) {
        EnumDef *ed = p->enums.items[i];
        if (find_named(ed->name)) {
            err_at(ed->line, ed->col, "a type named `%s` already exists", ed->name);
            continue;
        }
        Type *t = ty_named(TY_ENUM, ed->name);
        t->edef = ed;
        vec_push(&named_types, t);
    }
    for (int i = 0; i < p->classes.len; i++) {
        ClassDef *cd = p->classes.items[i];
        if (find_named(cd->name)) {
            err_at(cd->line, cd->col, "a type named `%s` already exists", cd->name);
            continue;
        }
        Type *t = ty_named(TY_CLASS, cd->name);
        t->cdef = cd;
        vec_push(&named_types, t);
    }

    /* 2. resolve field types, then reject impossible layouts */
    for (int i = 0; i < p->structs.len; i++) {
        StructDef *sd = p->structs.items[i];
        for (int j = 0; j < sd->ftypes.len; j++) {
            for (int k = 0; k < j; k++)
                if (strcmp((char *)sd->fnames.items[k], (char *)sd->fnames.items[j]) == 0)
                    err_at(sd->line, sd->col, "`%s` declares field `%s` twice",
                           sd->name, (char *)sd->fnames.items[j]);
            sd->ftypes.items[j] = resolve_type(sd->ftypes.items[j], sd->line, sd->col);
            Type *ft = sd->ftypes.items[j];
            if (ft->kind == TY_VOID)
                err_at(sd->line, sd->col, "field `%s` has no type",
                       (char *)sd->fnames.items[j]);
        }
    }
    for (int i = 0; i < p->structs.len; i++) {
        StructDef *sd = p->structs.items[i];
        for (int j = 0; j < sd->ftypes.len; j++) {
            Vec seen = {0};
            if (contains_struct(sd->ftypes.items[j], sd, &seen)) {
                err_at(sd->line, sd->col,
                       "`%s` contains itself through field `%s`, so it has no size",
                       sd->name, (char *)sd->fnames.items[j]);
                err_help("hold it in an array instead: `%s: [%s]`",
                         (char *)sd->fnames.items[j], sd->name);
            }
        }
    }

    /* 2b. classes: parents, fields, and methods */
    for (int i = 0; i < p->classes.len; i++) {
        ClassDef *cd = p->classes.items[i];
        if (!cd->base_name) continue;
        Type *bt = find_named(cd->base_name);
        if (!bt || bt->kind != TY_CLASS) {
            err_at(cd->line, cd->col, "`%s` is built on `%s`, which is not a class",
                   cd->name, cd->base_name);
            if (bt) err_help("only a `class` can be used as a parent");
            else    err_help("declare it first with `class %s { ... }`", cd->base_name);
            continue;
        }
        cd->base = bt->cdef;
    }
    /* inheritance must not loop, and depth drives the generated layout */
    for (int i = 0; i < p->classes.len; i++) {
        ClassDef *cd = p->classes.items[i];
        int steps = 0;
        ClassDef *c = cd;
        while (c->base) {
            if (++steps > p->classes.len) {
                err_at(cd->line, cd->col, "`%s` ends up built on itself", cd->name);
                err_help("a class cannot inherit from one of its own children");
                cd->base = NULL;
                steps = 0;
                break;
            }
            c = c->base;
        }
        cd->depth = steps;
    }
    for (int i = 0; i < p->classes.len; i++) {
        ClassDef *cd = p->classes.items[i];
        for (int j = 0; j < cd->ftypes.len; j++) {
            for (int k = 0; k < j; k++)
                if (strcmp((char *)cd->fnames.items[k], (char *)cd->fnames.items[j]) == 0)
                    err_at(cd->line, cd->col, "`%s` declares field `%s` twice",
                           cd->name, (char *)cd->fnames.items[j]);
            cd->ftypes.items[j] = resolve_type(cd->ftypes.items[j], cd->line, cd->col);
            if (cd->base && find_field(cd->base, cd->fnames.items[j], NULL)) {
                err_at(cd->line, cd->col, "`%s` already has a field named `%s`, "
                       "inherited from `%s`", cd->name,
                       (char *)cd->fnames.items[j], cd->base->name);
                err_help("give this one a different name");
            }
        }
    }
    /* method signatures, overrides, and dispatch slots */
    for (int i = 0; i < p->classes.len; i++) {
        ClassDef *cd = p->classes.items[i];
        for (int j = 0; j < cd->methods.len; j++) {
            FnDecl *m = cd->methods.items[j];
            m->owner = cd;
            m->cname = cx_fmt("cubm_%s_%s", cd->name, m->name);
            m->ret = resolve_type(m->ret, m->line, m->col);
            for (int k = 0; k < m->params.len; k++) {
                VarSym *v = m->params.items[k];
                v->type = resolve_type(v->type, m->line, m->col);
                v->cname = cx_fmt("cubp_%s_%d", v->name, ++uid);
            }
            for (int k = 0; k < j; k++)
                if (strcmp(((FnDecl *)cd->methods.items[k])->name, m->name) == 0)
                    err_at(m->line, m->col, "`%s` already has a method named `%s`",
                           cd->name, m->name);
            if (find_field(cd, m->name, NULL))
                err_at(m->line, m->col,
                       "`%s` already has a field named `%s`", cd->name, m->name);

            if (m->is_init) { m->slot_owner = NULL; continue; }

            FnDecl *inherited = cd->base ? find_method(cd->base, m->name) : NULL;
            if (inherited) {
                m->is_override = true;
                m->slot_owner = inherited->slot_owner;
                if (m->params.len != inherited->params.len) {
                    if (inherited->params.len == 0)
                        err_at(m->line, m->col,
                               "`%s.%s` replaces `%s.%s`, so it must take no arguments",
                               cd->name, m->name, inherited->owner->name, m->name);
                    else
                        err_at(m->line, m->col,
                               "`%s.%s` replaces `%s.%s`, so it must take the same %d "
                               "argument%s", cd->name, m->name, inherited->owner->name,
                               m->name, inherited->params.len,
                               inherited->params.len == 1 ? "" : "s");
                } else {
                    for (int k = 0; k < m->params.len; k++) {
                        Type *mine = ((VarSym *)m->params.items[k])->type;
                        Type *theirs = ((VarSym *)inherited->params.items[k])->type;
                        if (!ty_same(mine, theirs))
                            err_at(m->line, m->col,
                                   "`%s.%s` replaces `%s.%s`, so argument %d must be %s, "
                                   "not %s", cd->name, m->name, inherited->owner->name,
                                   m->name, k + 1, ty_show(theirs), ty_show(mine));
                    }
                }
                if (!ty_same(m->ret, inherited->ret))
                    err_at(m->line, m->col,
                           "`%s.%s` replaces `%s.%s`, so it must return %s, not %s",
                           cd->name, m->name, inherited->owner->name, m->name,
                           ty_show(inherited->ret), ty_show(m->ret));
            } else {
                m->slot_owner = cd;
                vec_push(&cd->own_slots, m);
            }
        }
    }

    /* 3. function signatures */
    for (int i = 0; i < p->fns.len; i++) {
        FnDecl *f = p->fns.items[i];
        for (int j = 0; j < i; j++)
            if (strcmp(((FnDecl *)p->fns.items[j])->name, f->name) == 0) {
                err_at(f->line, f->col, "a function named `%s` already exists", f->name);
                err_help("it was declared on line %d", ((FnDecl *)p->fns.items[j])->line);
            }
        if (builtin_lookup(f->name) != BI_NONE) {
            err_at(f->line, f->col, "`%s` is a built-in function", f->name);
            err_help("choose another name; built-ins cannot be replaced");
        }
        if (find_named(f->name))
            err_at(f->line, f->col, "`%s` is already the name of a type", f->name);
        f->ret = resolve_type(f->ret, f->line, f->col);
        for (int j = 0; j < f->params.len; j++) {
            VarSym *v = f->params.items[j];
            v->type = resolve_type(v->type, f->line, f->col);
            v->cname = cx_fmt("cubp_%s_%d", v->name, ++uid);
        }
        f->cname = cx_fmt("cubf_%s", f->name);
    }

    /* 4. globals live in the outermost scope */
    for (int i = 0; i < p->globals.len; i++) {
        Stmt *s = p->globals.items[i];
        Type *want_ = s->decl_type ? resolve_type(s->decl_type, s->line, s->col) : NULL;
        Type *got = check_expr(s->rhs, want_);
        if (want_) want(s->rhs, got, want_, cx_fmt("`%s`", s->name));
        s->decl_type = want_ ? want_ : got;
        s->var = declare(s->name, s->decl_type, s->is_mut, true, s->line, s->col);
    }

    /* 5. function bodies */
    for (int i = 0; i < p->fns.len; i++) {
        FnDecl *f = p->fns.items[i];
        cur_fn = f;
        push_scope();
        for (int j = 0; j < f->params.len; j++) {
            VarSym *v = f->params.items[j];
            if (lookup_local(v->name))
                err_at(f->line, f->col, "`%s` appears twice in the parameter list", v->name);
            vec_push(&scope->syms, v);
        }
        check_stmts(&f->body);
        pop_scope();
        if (f->ret->kind != TY_VOID && !always_returns(&f->body)) {
            err_at(f->line, f->col, "`%s` must return %s on every path",
                   f->name, ty_show(f->ret));
            err_help("add a `return` at the end, or an `else` branch that returns");
        }
        cur_fn = NULL;
    }

    /* 5b. method bodies */
    for (int i = 0; i < p->classes.len; i++) {
        ClassDef *cd = p->classes.items[i];
        Type *self_type = find_named(cd->name);

        for (int j = 0; j < cd->methods.len; j++) {
            FnDecl *m = cd->methods.items[j];
            cur_class = cd;
            cur_method = m;
            cur_fn = m;

            VarSym *self = cx_alloc(sizeof(VarSym));
            self->name = cx_strdup("self");
            self->type = self_type;
            self->cname = cx_strdup("self");
            m->self_sym = self;

            push_scope();
            for (int k = 0; k < m->params.len; k++) {
                VarSym *v = m->params.items[k];
                if (lookup_local(v->name))
                    err_at(m->line, m->col, "`%s` appears twice in the parameter list",
                           v->name);
                if (strcmp(v->name, "self") == 0)
                    err_at(m->line, m->col, "`self` is the object itself, "
                           "so it cannot also be a parameter name");
                vec_push(&scope->syms, v);
            }
            check_stmts(&m->body);
            pop_scope();

            if (m->ret->kind != TY_VOID && !always_returns(&m->body)) {
                err_at(m->line, m->col, "`%s.%s` must return %s on every path",
                       cd->name, m->name, ty_show(m->ret));
                err_help("add a `return` at the end, or an `else` branch that returns");
            }
            if (m->is_init && m->ret->kind != TY_VOID) {
                err_at(m->line, m->col, "`init` sets an object up, so it returns nothing");
                err_help("drop the `-> %s`", ty_show(m->ret));
            }
            cur_class = NULL;
            cur_method = NULL;
            cur_fn = NULL;
        }

        /* A parent that needs values must be given them explicitly. */
        if (cd->base && find_method(cd->base, "init")) {
            FnDecl *base_init = find_method(cd->base, "init");
            if (!cd->init) {
                if (base_init->params.len > 0) {
                    err_at(cd->line, cd->col,
                           "`%s` needs an `init`, because `%s` is built from values",
                           cd->name, cd->base->name);
                    err_help("add `fn init(...) { super.init(...) }`");
                }
            } else if (!calls_super_init(&cd->init->body)) {
                err_at(cd->init->line, cd->init->col,
                       "`%s.init` never sets up the `%s` it is built on",
                       cd->name, cd->base->name);
                err_help("call `super.init(%s)` first",
                         base_init->params.len ? "..." : "");
            }
        }
    }

    /* 6. the entry point */
    FnDecl *m = find_fn("main");
    if (!m) {
        err_at(1, 1, "this program has no `main` function");
        err_help("every Cub program starts at `fn main() { ... }`");
    } else {
        if (m->params.len != 0) {
            err_at(m->line, m->col, "`main` does not take any arguments");
            err_help("read the command line with `args()` in a future release; "
                     "for now use `input()`");
        }
        if (m->ret->kind != TY_VOID && m->ret->kind != TY_INT) {
            err_at(m->line, m->col, "`main` returns nothing or an exit code, not %s",
                   ty_show(m->ret));
        }
    }

    pop_scope();
    stop_if_errors();
}
