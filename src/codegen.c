/* codegen.c -- emits standalone C99.
 *
 * The output is one self-contained file: the runtime, then the program.
 * Nothing in it depends on Cub being installed, so `cubc --emit-c` gives
 * you portable C you can drop into any project.
 *
 * Layout of the generated file:
 *
 *     runtime          (from runtime/cub_rt.h)
 *     type decls       enums, then structs in dependency order
 *     prototypes       stringify helpers and user functions
 *     helper bodies    one `str` helper per composite type actually used
 *     function bodies
 *     main
 */
#include "cub.h"

static Buf     lambdas;    /* functions written inline, lifted to the top */
static Buf     out;        /* everything before the function bodies */
static Buf     bodies;     /* user function bodies                  */
static Buf     helpers;    /* generated stringify helpers           */
static Program *prog;
static const char *unit;   /* source file name, for runtime messages */
static int      tmp_id;
static Vec      helper_types;

/* ------------------------------------------------------------------ */
/* small emitters                                                      */
/* ------------------------------------------------------------------ */

static Buf *dst;                            /* current destination */
#define E(...) buf_printf(dst, __VA_ARGS__)

static void indent(int n) { for (int i = 0; i < n; i++) buf_puts(dst, "    "); }

/* ------------------------------------------------------------------ */
/* statements hoisted out of an expression                             */
/* ------------------------------------------------------------------ */

/* `or` and `try` cannot be written as C expressions: one must not
 * evaluate its fallback unless it is needed, and the other returns from
 * the function it sits in.  Both put their working out here, and the
 * expression they leave behind is just the name of a temporary. */
static Buf  *prelude;        /* the statements to emit before this one  */
static int   prelude_lvl;
static Type *cur_ret;        /* the enclosing function's return type    */

static void hoist(const char *fmt, ...) {
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    Buf *saved = dst;
    dst = prelude;
    indent(prelude_lvl);
    buf_puts(dst, line);
    dst = saved;
}


/* Render text as a C string literal.  Non-printable bytes become octal
 * escapes so embedded zeros and UTF-8 both survive the trip. */
static char *c_string(const char *s, size_t n) {
    Buf b;
    buf_init(&b);
    buf_putc(&b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  buf_puts(&b, "\\\""); break;
        case '\\': buf_puts(&b, "\\\\"); break;
        case '\n': buf_puts(&b, "\\n");  break;
        case '\t': buf_puts(&b, "\\t");  break;
        case '\r': buf_puts(&b, "\\r");  break;
        default:
            if (c < 32 || c >= 127) buf_printf(&b, "\\%03o", c);
            else buf_putc(&b, (char)c);
        }
    }
    buf_putc(&b, '"');
    return b.data;
}

static char *lit(const char *s) { return c_string(s, strlen(s)); }

/* Location arguments passed to runtime checks. */
static char *loc(int line) { return cx_fmt("%s, %d", lit(unit), line); }

/* Follow the declaration being emitted, so a runtime message names the
 * file the code was actually written in. */
static void in_unit(Source *src) { if (src) unit = src->path; }

/* ------------------------------------------------------------------ */
/* C type names                                                        */
/* ------------------------------------------------------------------ */

/* `T?` and `T!` share one C shape: a flag, the value, and the reason it is
 * missing (empty for a `T?`, which has no reason to give).  One struct is
 * generated per inner type and both spellings use it. */
static Vec maybe_types;

static const char *maybe_ctype(Type *t) {
    Type *inner = t->elem;
    for (int i = 0; i < maybe_types.len; i++)
        if (ty_same(((Type *)maybe_types.items[i])->elem, inner))
            return cx_fmt("CubMaybe_%s", ty_mangle(inner));
    vec_push(&maybe_types, t);
    return cx_fmt("CubMaybe_%s", ty_mangle(inner));
}

static const char *ctype(Type *t) {
    switch (t->kind) {
    case TY_OPT:
    case TY_FAIL:   return maybe_ctype(t);
    case TY_FN:     return "CubFn";
    case TY_INT:    return "int64_t";
    case TY_FLOAT:  return "double";
    case TY_BOOL:   return "bool";
    case TY_STR:    return "CubStr";
    case TY_ARRAY:  return "CubArr";
    case TY_MAP:    return "CubMap";
    case TY_STRUCT: return cx_fmt("CubS_%s", t->name);
    case TY_ENUM:   return cx_fmt("CubE_%s", t->name);
    case TY_CLASS:  return "void *";     /* every object is a tracked pointer */
    case TY_VOID:   return "void";
    default:        return "void";
    }
}

/* The C signature a function value is called through: every one takes the
 * values it captured first, as a `void *`. */
static char *fn_ptr_cast(Type *ft) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "%s (*)(void *", ctype(ft->elem));
    for (int i = 0; i < ft->fparams.len; i++)
        buf_printf(&b, ", %s", ctype(ft->fparams.items[i]));
    buf_puts(&b, ")");
    return b.data;
}

/* `map`, `filter` and friends need a loop written for the types in play,
 * so one is generated per shape and remembered here. */
typedef struct {
    int   bi;
    Type *elem;
    Type *out;
    char *name;
} WalkHelper;

static Vec walk_helpers;

static const char *need_walk(int bi, Type *elem, Type *out) {
    for (int i = 0; i < walk_helpers.len; i++) {
        WalkHelper *w = walk_helpers.items[i];
        if (w->bi == bi && ty_same(w->elem, elem) &&
            (out == NULL) == (w->out == NULL) &&
            (out == NULL || ty_same(w->out, out)))
            return w->name;
    }
    WalkHelper *w = cx_alloc(sizeof(WalkHelper));
    w->bi = bi;
    w->elem = elem;
    w->out = out;
    static const char *verb[] = { "map", "filter", "any", "all", "find_by", "sort_by" };
    w->name = cx_fmt("cub_%s_%s%s%s", verb[bi - BI_MAP], ty_mangle(elem),
                     out ? "_to_" : "", out ? ty_mangle(out) : "");
    vec_push(&walk_helpers, w);
    return w->name;
}

/* A named function used as a value gets a wrapper, so that everything
 * called through a `CubFn` has the same shape. */
static Vec fn_wrappers;

static void gen_stmts(Vec *body, int lvl);

static void need_fn_wrapper(FnDecl *f) {
    for (int i = 0; i < fn_wrappers.len; i++)
        if (fn_wrappers.items[i] == f) return;
    vec_push(&fn_wrappers, f);
}

/* ------------------------------------------------------------------ */
/* classes                                                             */
/* ------------------------------------------------------------------ */

/* A subclass embeds its parent as its first member, so reaching an
 * inherited field means stepping through `base` once per level.  That
 * keeps every access plain, legal C -- no casts between struct types. */
static char *base_steps(int steps) {
    Buf b;
    buf_init(&b);
    for (int i = 0; i < steps; i++) buf_puts(&b, "base.");
    return b.data;
}

static ClassDef *field_owner(ClassDef *c, const char *name) {
    for (; c; c = c->base)
        for (int i = 0; i < c->fnames.len; i++)
            if (strcmp((char *)c->fnames.items[i], name) == 0) return c;
    return NULL;
}

static FnDecl *method_of(ClassDef *c, const char *name) {
    for (; c; c = c->base)
        for (int i = 0; i < c->methods.len; i++) {
            FnDecl *m = c->methods.items[i];
            if (strcmp(m->name, name) == 0) return m;
        }
    return NULL;
}

/* The object pointer, checked for emptiness, typed as its static class. */
static char *obj_ptr(const char *expr, ClassDef *cd, int line) {
    return cx_fmt("((CubC_%s *)cub_obj_ck((void *)(%s), %s, %s))",
                  cd->name, expr, lit(cd->name), loc(line));
}

/* Read the dispatch table, which lives in the root of the family. */
static char *obj_vt(const char *expr, ClassDef *cd, int line) {
    return cx_fmt("%s->%svt", obj_ptr(expr, cd, line), base_steps(cd->depth));
}

/* ------------------------------------------------------------------ */
/* stringify helpers                                                   */
/* ------------------------------------------------------------------ */

static const char *helper_name(Type *t) { return cx_fmt("cubstr_%s", ty_mangle(t)); }

static const char *need_helper(Type *t) {
    for (int i = 0; i < helper_types.len; i++)
        if (ty_same(helper_types.items[i], t)) return helper_name(t);
    vec_push(&helper_types, t);
    return helper_name(t);
}

/* A C expression of type CubStr for `src`, which has type t.
 * `quoted` adds quotes around text, which is what you want inside a
 * container so that ["a", "b"] is distinguishable from [a, b]. */
static char *str_of(Type *t, const char *src, bool quoted) {
    switch (t->kind) {
    case TY_INT:   return cx_fmt("cub_str_from_int(%s)", src);
    case TY_FLOAT: return cx_fmt("cub_str_from_float(%s)", src);
    case TY_BOOL:  return cx_fmt("cub_str_from_bool(%s)", src);
    case TY_STR:   return quoted ? cx_fmt("cubstr_quoted(%s)", src) : cx_fmt("(%s)", src);
    case TY_ARRAY:
    case TY_MAP:
    case TY_STRUCT:
    case TY_ENUM:
    case TY_CLASS: return cx_fmt("%s(%s)", need_helper(t), src);
    default:       return cx_fmt("cub_str_lit(\"?\", 1)");
    }
}

static void emit_helper(Type *t) {
    Buf *save = dst;
    dst = &helpers;

    if (t->kind == TY_ENUM) {
        EnumDef *ed = t->edef;
        E("static CubStr %s(CubE_%s v) {\n", helper_name(t), t->name);
        E("    switch (v%s) {\n", ed->tagged ? ".tag" : "");
        for (int i = 0; i < ed->vals.len; i++) {
            char *vn = ed->vals.items[i];
            Vec *carries = ed->tagged ? ed->vfields.items[i] : NULL;
            if (!carries || carries->len == 0) {
                E("    case CubE_%s_%s: return cub_str_lit(%s, %d);\n",
                  t->name, vn, lit(vn), (int)strlen(vn));
                continue;
            }
            /* a value that carries something prints what it carries */
            E("    case CubE_%s_%s: {\n", t->name, vn);
            E("        CubStr r = cub_str_lit(%s, %d);\n",
              lit(cx_fmt("%s(", vn)), (int)strlen(vn) + 1);
            for (int k = 0; k < carries->len; k++) {
                VarSym *f = carries->items[k];
                if (k) E("        r = cub_str_concat(r, cub_str_lit(\", \", 2));\n");
                E("        r = cub_str_concat(r, %s);\n",
                  str_of(f->type, cx_fmt("v.as.v_%s.f_%s", vn, f->name), true));
            }
            E("        return cub_str_concat(r, cub_str_lit(\")\", 1));\n");
            E("    }\n");
        }
        E("    }\n    return cub_str_lit(\"?\", 1);\n}\n\n");
    } else if (t->kind == TY_STRUCT) {
        StructDef *sd = t->sdef;
        E("static CubStr %s(CubS_%s v) {\n", helper_name(t), t->name);
        E("    CubStr r = cub_str_lit(%s, %d);\n",
          lit(cx_fmt("%s{", sd->name)), (int)strlen(sd->name) + 1);
        for (int i = 0; i < sd->fnames.len; i++) {
            char *fn = sd->fnames.items[i];
            Type *ft = sd->ftypes.items[i];
            const char *sep = i ? ", " : "";
            char *label = cx_fmt("%s%s: ", sep, fn);
            E("    r = cub_str_concat(r, cub_str_lit(%s, %d));\n",
              lit(label), (int)strlen(label));
            E("    r = cub_str_concat(r, %s);\n",
              str_of(ft, cx_fmt("v.f_%s", fn), true));
        }
        E("    return cub_str_concat(r, cub_str_lit(\"}\", 1));\n}\n\n");
    } else if (t->kind == TY_CLASS) {
        ClassDef *cd = t->cdef;
        FnDecl *ts = method_of(cd, "to_string");
        bool usable = ts && ts->params.len == 0 && ts->ret->kind == TY_STR;

        E("static CubStr %s(void *p) {\n", helper_name(t));
        E("    if (!p) return cub_str_lit(\"nothing\", 7);\n");
        if (usable) {
            E("    return ((const struct CubC_%s_vt *)%s)->m_%s(p);\n",
              ts->slot_owner->name, obj_vt("p", cd, cd->line), ts->name);
        } else {
            E("    CubC_%s *o = (CubC_%s *)p;\n", cd->name, cd->name);
            E("    CubStr r = cub_str_concat(cub_str_lit(%s, %d), cub_str_lit(\"{\", 1));\n",
              lit(cd->name), (int)strlen(cd->name));
            /* fields, oldest ancestor first */
            Vec chain = {0};
            for (ClassDef *c = cd; c; c = c->base) vec_push(&chain, c);
            int printed = 0;
            for (int ci = chain.len - 1; ci >= 0; ci--) {
                ClassDef *c = chain.items[ci];
                for (int fi = 0; fi < c->fnames.len; fi++) {
                    char *fn = c->fnames.items[fi];
                    Type *ft = c->ftypes.items[fi];
                    char *label = cx_fmt("%s%s: ", printed ? ", " : "", fn);
                    E("    r = cub_str_concat(r, cub_str_lit(%s, %d));\n",
                      lit(label), (int)strlen(label));
                    E("    r = cub_str_concat(r, %s);\n",
                      str_of(ft, cx_fmt("o->%sf_%s", base_steps(cd->depth - c->depth), fn), true));
                    printed++;
                }
            }
            E("    return cub_str_concat(r, cub_str_lit(\"}\", 1));\n");
        }
        E("}\n\n");
    } else if (t->kind == TY_MAP) {
        E("static CubStr %s(CubMap m) {\n", helper_name(t));
        E("    CubStr r = cub_str_lit(\"[\", 1);\n");
        E("    int64_t shown = 0;\n");
        E("    for (int64_t i = 0; i < m->cap; i++) {\n");
        E("        if (m->slots[i].state != 1) continue;\n");
        E("        if (shown++) r = cub_str_concat(r, cub_str_lit(\", \", 2));\n");
        if (t->key->kind == TY_STR)
            E("        r = cub_str_concat(r, cubstr_quoted(m->slots[i].skey));\n");
        else
            E("        r = cub_str_concat(r, cub_str_from_int(m->slots[i].ikey));\n");
        E("        r = cub_str_concat(r, cub_str_lit(\": \", 2));\n");
        E("        r = cub_str_concat(r, %s);\n",
          str_of(t->elem, cx_fmt("(*(%s *)(m->slots[i].value))", ctype(t->elem)), true));
        E("    }\n");
        E("    if (shown == 0) return cub_str_lit(\"[:]\", 3);\n");
        E("    return cub_str_concat(r, cub_str_lit(\"]\", 1));\n}\n\n");
    } else if (t->kind == TY_ARRAY) {
        E("static CubStr %s(CubArr a) {\n", helper_name(t));
        E("    CubStr r = cub_str_lit(\"[\", 1);\n");
        E("    for (int64_t i = 0; i < a->len; i++) {\n");
        E("        if (i) r = cub_str_concat(r, cub_str_lit(\", \", 2));\n");
        E("        r = cub_str_concat(r, %s);\n",
          str_of(t->elem, cx_fmt("(*(%s *)(a->data + i * a->esz))", ctype(t->elem)), true));
        E("    }\n");
        E("    return cub_str_concat(r, cub_str_lit(\"]\", 1));\n}\n\n");
    }

    dst = save;
}

/* ------------------------------------------------------------------ */
/* expressions                                                         */
/* ------------------------------------------------------------------ */

static void gen_expr(Expr *e);

/* What a field holds before `init` runs.  Objects start empty, and using
 * an empty one is caught at runtime with a message naming the class. */
static char *default_value(Type *t) {
    switch (t->kind) {
    case TY_INT:   return cx_fmt("0");
    case TY_FLOAT: return cx_fmt("0.0");
    case TY_BOOL:  return cx_fmt("false");
    case TY_STR:   return cx_fmt("cub_str_lit(\"\", 0)");
    case TY_ARRAY: return cx_fmt("cub_arr_new(sizeof(%s), 0)", ctype(t->elem));
    case TY_MAP:   return cx_fmt("cub_map_new(sizeof(%s), %d)",
                                 ctype(t->elem), t->key->kind == TY_STR ? 1 : 0);
    case TY_CLASS: return cx_fmt("NULL");
    case TY_FN:    return cx_fmt("((CubFn){ 0, 0 })");
    case TY_ENUM:  return cx_fmt("(CubE_%s)0", t->name);
    /* a field that may be missing starts out missing */
    case TY_OPT:
    case TY_FAIL:  return cx_fmt("(%s){ .ok = false }", ctype(t));
    case TY_STRUCT: {
        StructDef *sd = t->sdef;
        Buf b;
        buf_init(&b);
        buf_printf(&b, "((CubS_%s){", sd->name);
        for (int i = 0; i < sd->fnames.len; i++)
            buf_printf(&b, "%s.f_%s = %s", i ? ", " : "",
                       (char *)sd->fnames.items[i], default_value(sd->ftypes.items[i]));
        if (sd->fnames.len == 0) buf_puts(&b, "0");
        buf_puts(&b, "})");
        return b.data;
    }
    default: return cx_fmt("0");
    }
}

/* Capture an expression into a string instead of writing it out. */
static char *expr_str(Expr *e) {
    Buf tmp;
    buf_init(&tmp);
    Buf *save = dst;
    dst = &tmp;
    gen_expr(e);
    dst = save;
    return tmp.data;
}

static char *str_of_expr(Expr *e, bool quoted) {
    return str_of(e->type, expr_str(e), quoted);
}

/* Concatenate the string forms of a list of expressions. */
static char *concat_args(Vec *args, bool quoted) {
    if (args->len == 0) return cx_fmt("cub_str_lit(\"\", 0)");
    char *acc = str_of_expr(args->items[0], quoted);
    for (int i = 1; i < args->len; i++)
        acc = cx_fmt("cub_str_concat(%s, %s)", acc, str_of_expr(args->items[i], quoted));
    return acc;
}

static const char *op_c(TokKind k) {
    switch (k) {
    case TK_PLUS: return "+"; case TK_MINUS: return "-";
    case TK_STAR: return "*"; case TK_SLASH: return "/"; case TK_PERCENT: return "%";
    case TK_EQ: return "=="; case TK_NE: return "!=";
    case TK_LT: return "<";  case TK_LE: return "<=";
    case TK_GT: return ">";  case TK_GE: return ">=";
    case TK_ANDAND: return "&&"; case TK_OROR: return "||";
    default: return "?";
    }
}

/* Enums that carry things need an equality of their own: same value, and
 * the same things carried. */
static Vec eq_helpers;

static const char *need_eq_helper(Type *t) {
    for (int i = 0; i < eq_helpers.len; i++)
        if (ty_same(eq_helpers.items[i], t)) return "";
    vec_push(&eq_helpers, t);
    return "";
}

static void emit_eq_helper(Type *t) {
    EnumDef *ed = t->edef;
    E("static bool cub_eq_%s(CubE_%s a, CubE_%s b) {\n",
      ty_mangle(t), ed->name, ed->name);
    E("    if (a.tag != b.tag) return false;\n");
    bool any = false;
    for (int i = 0; i < ed->vals.len; i++)
        if (((Vec *)ed->vfields.items[i])->len) any = true;
    if (any) {
        E("    switch (a.tag) {\n");
        for (int i = 0; i < ed->vals.len; i++) {
            Vec *fields = ed->vfields.items[i];
            if (!fields->len) continue;
            char *vn = ed->vals.items[i];
            E("    case CubE_%s_%s:\n        return", ed->name, vn);
            for (int k = 0; k < fields->len; k++) {
                VarSym *f = fields->items[k];
                const char *av = cx_fmt("a.as.v_%s.f_%s", vn, f->name);
                const char *bv = cx_fmt("b.as.v_%s.f_%s", vn, f->name);
                if (k) E(" &&");
                if (f->type->kind == TY_STR)
                    E(" cub_str_eq(%s, %s)", av, bv);
                else if (f->type->kind == TY_ENUM && f->type->edef->tagged)
                    E(" cub_eq_%s(%s, %s)", ty_mangle(f->type), av, bv);
                else
                    E(" %s == %s", av, bv);
            }
            E(";\n");
        }
        E("    }\n");
    }
    E("    return true;\n}\n\n");
}

static void gen_binary(Expr *e) {
    Type *t = e->a->type;

    if (t->kind == TY_ENUM && t->edef->tagged &&
        (e->op == TK_EQ || e->op == TK_NE)) {
        need_eq_helper(t);
        E("%scub_eq_%s(%s, %s)", e->op == TK_NE ? "!" : "", ty_mangle(t),
          expr_str(e->a), expr_str(e->b));
        return;
    }

    if (t->kind == TY_STR) {
        char *a = expr_str(e->a), *b = expr_str(e->b);
        switch (e->op) {
        case TK_PLUS: E("cub_str_concat(%s, %s)", a, b); return;
        case TK_EQ:   E("cub_str_eq(%s, %s)", a, b); return;
        case TK_NE:   E("(!cub_str_eq(%s, %s))", a, b); return;
        default:      E("(cub_str_cmp(%s, %s) %s 0)", a, b, op_c(e->op)); return;
        }
    }

    /* Every int arithmetic operator is checked: `/` and `%` for a zero on
     * the right, the rest for a result too large to hold. */
    if (t->kind == TY_INT) {
        const char *fn = NULL;
        switch (e->op) {
        case TK_SLASH:   fn = "cub_div_int"; break;
        case TK_PERCENT: fn = "cub_mod_int"; break;
        case TK_PLUS:    fn = "cub_add_int"; break;
        case TK_MINUS:   fn = "cub_sub_int"; break;
        case TK_STAR:    fn = "cub_mul_int"; break;
        default: break;
        }
        if (fn) {
            E("%s(%s, %s, %s)", fn, expr_str(e->a), expr_str(e->b), loc(e->line));
            return;
        }
    }

    E("(");
    gen_expr(e->a);
    E(" %s ", op_c(e->op));
    gen_expr(e->b);
    E(")");
}

/* Maps are keyed by text or by number; the runtime takes both and uses the
 * one that matches. */
static char *map_key_args(Type *mt, Expr *key) {
    return mt->key->kind == TY_STR
        ? cx_fmt("%s, 0", expr_str(key))
        : cx_fmt("cub_str_lit(\"\", 0), %s", expr_str(key));
}

/* Comparing array items needs a routine that knows how wide they are. */
static const char *elem_fn(Type *elem, const char *base) {
    if (elem->kind == TY_STR)   return cx_fmt("%s_str", base);
    if (elem->kind == TY_FLOAT) return cx_fmt("%s_float", base);
    return cx_fmt("%s_mem", base);
}

static char *elem_arg(Type *elem, Expr *v) {
    if (elem->kind == TY_STR || elem->kind == TY_FLOAT) return expr_str(v);
    return cx_fmt("(%s[]){%s}", ctype(elem), expr_str(v));
}

static void gen_builtin(Expr *e) {
    Vec *a = &e->args;
    Expr *a0 = a->len > 0 ? a->items[0] : NULL;
    Expr *a1 = a->len > 1 ? a->items[1] : NULL;
    Expr *a2 = a->len > 2 ? a->items[2] : NULL;

    switch (e->builtin) {
    case BI_PRINT: E("cub_println(%s)", concat_args(a, false)); return;
    case BI_WRITE: E("cub_write(%s)", concat_args(a, false));   return;

    case BI_LEN:
        if (a0->type->kind == TY_STR) E("cub_str_len(%s)", expr_str(a0));
        else                          E("((%s)->len)", expr_str(a0));
        return;

    case BI_PUSH:
        E("cub_arr_push(%s, (%s[]){%s})", expr_str(a0),
          ctype(a0->type->elem), expr_str(a1));
        return;

    case BI_POP:
        E("(*(%s *)cub_arr_pop(%s, %s))", ctype(e->type), expr_str(a0), loc(e->line));
        return;

    case BI_REMOVE:
        if (a0->type->kind == TY_MAP)
            E("cub_map_del(%s, %s)", expr_str(a0), map_key_args(a0->type, a1));
        else
            E("cub_arr_remove(%s, %s, %s)", expr_str(a0), expr_str(a1), loc(e->line));
        return;

    case BI_INSERT:
        E("cub_arr_insert(%s, %s, (%s[]){%s}, %s)", expr_str(a0), expr_str(a1),
          ctype(a0->type->elem), expr_str(a2), loc(e->line));
        return;

    case BI_MAP: case BI_FILTER: case BI_ANY: case BI_ALL:
    case BI_FIND_BY: case BI_SORT_BY: {
        Type *elem = a0->type->elem;
        Type *out = e->builtin == BI_MAP ? e->type->elem
                  : e->builtin == BI_FIND_BY ? NULL : NULL;
        E("%s(%s, %s)", need_walk(e->builtin, elem, out),
          expr_str(a0), expr_str(a1));
        return;
    }

    case BI_FAIL:
        E("(%s){ .ok = false, .err = %s }", ctype(e->type), expr_str(a0));
        return;

    case BI_STR: E("%s", str_of_expr(a0, false)); return;

    case BI_INT:
        switch (a0->type->kind) {
        case TY_FLOAT: E("cub_int_of_float(%s, %s)", expr_str(a0), loc(e->line)); break;
        case TY_STR:   E("cub_int_of_str(%s)", expr_str(a0)); break;
        case TY_BOOL:  E("((int64_t)(%s))", expr_str(a0)); break;
        default:       E("(%s)", expr_str(a0)); break;
        }
        return;

    case BI_FLOAT:
        switch (a0->type->kind) {
        case TY_INT: E("((double)(%s))", expr_str(a0)); break;
        case TY_STR: E("cub_float_of_str(%s)", expr_str(a0)); break;
        default:     E("(%s)", expr_str(a0)); break;
        }
        return;

    case BI_ABS:
        E("%s(%s)", e->type->kind == TY_INT ? "cub_abs_int" : "cub_abs_float", expr_str(a0));
        return;

    case BI_MIN: case BI_MAX: {
        const char *nm = e->builtin == BI_MIN
            ? (e->type->kind == TY_INT ? "cub_min_int" : "cub_min_float")
            : (e->type->kind == TY_INT ? "cub_max_int" : "cub_max_float");
        E("%s(%s, %s)", nm, expr_str(a0), expr_str(a1));
        return;
    }

    case BI_SQRT:  E("sqrt(%s)", expr_str(a0));  return;
    case BI_FLOOR: E("floor(%s)", expr_str(a0)); return;
    case BI_CEIL:  E("ceil(%s)", expr_str(a0));  return;
    case BI_ROUND: E("cub_round(%s)", expr_str(a0)); return;
    case BI_POW:   E("pow(%s, %s)", expr_str(a0), expr_str(a1)); return;

    case BI_RAND_INT:  E("cub_rand_int(%s, %s, %s)", expr_str(a0), expr_str(a1), loc(e->line)); return;
    case BI_RAND_SEED: E("cub_rand_seed(%s)", expr_str(a0)); return;
    case BI_INPUT:     E("cub_input()");    return;
    case BI_TIME_MS:   E("cub_time_ms()");  return;

    case BI_UPPER: E("cub_str_upper(%s)", expr_str(a0)); return;
    case BI_LOWER: E("cub_str_lower(%s)", expr_str(a0)); return;
    case BI_TRIM:  E("cub_str_trim(%s)", expr_str(a0));  return;
    case BI_SPLIT: E("cub_str_split(%s, %s)", expr_str(a0), expr_str(a1)); return;
    case BI_JOIN:  E("cub_str_join(%s, %s)", expr_str(a0), expr_str(a1));  return;
    case BI_FIND:  E("cub_str_find(%s, %s)", expr_str(a0), expr_str(a1));  return;

    case BI_SLICE:
        if (a0->type->kind == TY_STR)
            E("cub_str_slice(%s, %s, %s)", expr_str(a0), expr_str(a1), expr_str(a2));
        else
            E("cub_arr_slice(%s, %s, %s)", expr_str(a0), expr_str(a1), expr_str(a2));
        return;

    case BI_CONTAINS:
        if (a0->type->kind == TY_STR) {
            E("cub_str_contains(%s, %s)", expr_str(a0), expr_str(a1));
        } else if (a0->type->elem->kind == TY_STR) {
            E("cub_arr_contains_str(%s, %s)", expr_str(a0), expr_str(a1));
        } else if (a0->type->elem->kind == TY_FLOAT) {
            E("cub_arr_contains_float(%s, %s)", expr_str(a0), expr_str(a1));
        } else {
            E("cub_arr_contains_mem(%s, (%s[]){%s})", expr_str(a0),
              ctype(a0->type->elem), expr_str(a1));
        }
        return;

    case BI_STARTS_WITH: E("cub_str_starts_with(%s, %s)", expr_str(a0), expr_str(a1)); return;
    case BI_ENDS_WITH:   E("cub_str_ends_with(%s, %s)", expr_str(a0), expr_str(a1));   return;
    case BI_REPLACE:     E("cub_str_replace(%s, %s, %s)", expr_str(a0), expr_str(a1), expr_str(a2)); return;
    case BI_REPEAT:      E("cub_str_repeat(%s, %s)", expr_str(a0), expr_str(a1)); return;
    case BI_CHAR_AT:     E("cub_str_char_at(%s, %s, %s)", expr_str(a0), expr_str(a1), loc(e->line)); return;
    case BI_CODE_AT:     E("cub_str_code_at(%s, %s, %s)", expr_str(a0), expr_str(a1), loc(e->line)); return;
    case BI_FROM_CODE:   E("cub_str_from_code(%s, %s)", expr_str(a0), loc(e->line)); return;

    case BI_SORT: {
        const char *nm = a0->type->elem->kind == TY_INT ? "cub_sort_int"
                       : a0->type->elem->kind == TY_FLOAT ? "cub_sort_float" : "cub_sort_str";
        E("%s(%s)", nm, expr_str(a0));
        return;
    }
    case BI_REVERSE: E("cub_arr_reverse(%s)", expr_str(a0)); return;

    case BI_READ_FILE:  E("cub_read_file(%s)", expr_str(a0)); return;
    case BI_WRITE_FILE: E("cub_write_file(%s, %s)", expr_str(a0), expr_str(a1)); return;

    case BI_PANIC: E("cub_panic_at(%s, \"%%s\", (%s).data)", loc(e->line), expr_str(a0)); return;

    case BI_ASSERT:
        E("cub_assert(%s, %s, %s)", expr_str(a0),
          a1 ? expr_str(a1) : "cub_str_lit(\"\", 0)", loc(e->line));
        return;

    /* ---- maps ---- */
    case BI_HAS:
        E("cub_map_has(%s, %s)", expr_str(a0), map_key_args(a0->type, a1));
        return;
    case BI_GET:
        E("(*(%s *)cub_map_get(%s, %s, (%s[]){%s}))", ctype(e->type), expr_str(a0),
          map_key_args(a0->type, a1), ctype(e->type), expr_str(a2));
        return;
    case BI_KEYS:   E("cub_map_keys(%s)", expr_str(a0)); return;
    case BI_VALUES: E("cub_map_values(%s)", expr_str(a0)); return;
    case BI_CLEAR:
        if (a0->type->kind == TY_MAP) E("cub_map_clear(%s)", expr_str(a0));
        else                          E("cub_arr_clear(%s)", expr_str(a0));
        return;

    /* ---- numbers ---- */
    case BI_SIN:   E("sin(%s)", expr_str(a0));   return;
    case BI_COS:   E("cos(%s)", expr_str(a0));   return;
    case BI_TAN:   E("tan(%s)", expr_str(a0));   return;
    case BI_ASIN:  E("asin(%s)", expr_str(a0));  return;
    case BI_ACOS:  E("acos(%s)", expr_str(a0));  return;
    case BI_ATAN:  E("atan(%s)", expr_str(a0));  return;
    case BI_LOG:   E("log(%s)", expr_str(a0));   return;
    case BI_LOG10: E("log10(%s)", expr_str(a0)); return;
    case BI_EXP:   E("exp(%s)", expr_str(a0));   return;
    case BI_ATAN2: E("atan2(%s, %s)", expr_str(a0), expr_str(a1)); return;
    case BI_SIGN:
        E("%s(%s)", a0->type->kind == TY_INT ? "cub_sign_int" : "cub_sign_float",
          expr_str(a0));
        return;
    case BI_CLAMP:
        E("%s(%s, %s, %s, %s)",
          e->type->kind == TY_INT ? "cub_clamp_int" : "cub_clamp_float",
          expr_str(a0), expr_str(a1), expr_str(a2), loc(e->line));
        return;
    case BI_IS_NAN: E("cub_is_nan(%s)", expr_str(a0)); return;
    case BI_IS_INF: E("cub_is_inf(%s)", expr_str(a0)); return;
    case BI_RAND_FLOAT: E("cub_rand_float()"); return;

    /* ---- text ---- */
    case BI_PAD_START:
        E("cub_str_pad(%s, %s, %s, 1)", expr_str(a0), expr_str(a1), expr_str(a2)); return;
    case BI_PAD_END:
        E("cub_str_pad(%s, %s, %s, 0)", expr_str(a0), expr_str(a1), expr_str(a2)); return;
    case BI_TRIM_START:  E("cub_str_trim_start(%s)", expr_str(a0)); return;
    case BI_TRIM_END:    E("cub_str_trim_end(%s)", expr_str(a0));   return;
    case BI_CAPITALIZE:  E("cub_str_capitalize(%s)", expr_str(a0)); return;
    case BI_LINES:       E("cub_str_lines(%s)", expr_str(a0));      return;
    case BI_CHARS:       E("cub_str_split(%s, cub_str_lit(\"\", 0))", expr_str(a0)); return;
    case BI_LAST_INDEX_OF: E("cub_str_last_index_of(%s, %s)", expr_str(a0), expr_str(a1)); return;

    case BI_COUNT:
        if (a0->type->kind == TY_STR)
            E("cub_str_count(%s, %s)", expr_str(a0), expr_str(a1));
        else
            E("%s(%s, %s)", elem_fn(a0->type->elem, "cub_arr_count"),
              expr_str(a0), elem_arg(a0->type->elem, a1));
        return;

    case BI_INDEX_OF:
        if (a0->type->kind == TY_STR)
            E("cub_str_find(%s, %s)", expr_str(a0), expr_str(a1));
        else
            E("%s(%s, %s)", elem_fn(a0->type->elem, "cub_arr_index"),
              expr_str(a0), elem_arg(a0->type->elem, a1));
        return;

    case BI_IS_DIGIT: E("cub_is_digit(%s)", expr_str(a0)); return;
    case BI_IS_ALPHA: E("cub_is_alpha(%s)", expr_str(a0)); return;
    case BI_IS_ALNUM: E("cub_is_alnum(%s)", expr_str(a0)); return;
    case BI_IS_SPACE: E("cub_is_space(%s)", expr_str(a0)); return;
    case BI_IS_UPPER: E("cub_is_upper(%s)", expr_str(a0)); return;
    case BI_IS_LOWER: E("cub_is_lower(%s)", expr_str(a0)); return;

    /* ---- arrays ---- */
    case BI_SUM:
        E("%s(%s)", e->type->kind == TY_INT ? "cub_sum_int" : "cub_sum_float",
          expr_str(a0));
        return;
    case BI_MIN_OF: case BI_MAX_OF: {
        const char *base = e->builtin == BI_MIN_OF ? "cub_min_of" : "cub_max_of";
        const char *suffix = e->type->kind == TY_INT ? "int"
                           : e->type->kind == TY_FLOAT ? "float" : "str";
        E("%s_%s(%s, %s)", base, suffix, expr_str(a0), loc(e->line));
        return;
    }
    case BI_COPY:    E("cub_arr_copy(%s)", expr_str(a0)); return;
    case BI_CONCAT:  E("cub_arr_concat(%s, %s)", expr_str(a0), expr_str(a1)); return;
    case BI_SHUFFLE: E("cub_arr_shuffle(%s)", expr_str(a0)); return;
    case BI_SWAP:
        E("cub_arr_swap(%s, %s, %s, %s)", expr_str(a0), expr_str(a1), expr_str(a2),
          loc(e->line));
        return;

    /* ---- the world outside ---- */
    case BI_EPRINT:   E("cub_eprint(%s)", concat_args(a, false)); return;
    case BI_EXIT:     E("cub_exit(%s)", expr_str(a0)); return;
    case BI_ARGS:     E("cub_args()");     return;
    case BI_PLATFORM: E("cub_platform()"); return;
    case BI_CLOCK_MS: E("cub_clock_ms()"); return;
    case BI_ENV:      E("cub_env(%s)", expr_str(a0)); return;
    case BI_SLEEP_MS: E("cub_sleep_ms(%s)", expr_str(a0)); return;

    case BI_FILE_EXISTS: E("cub_file_exists(%s)", expr_str(a0)); return;
    case BI_DELETE_FILE: E("cub_delete_file(%s)", expr_str(a0)); return;
    case BI_APPEND_FILE:
        E("cub_append_file(%s, %s)", expr_str(a0), expr_str(a1)); return;
    case BI_READ_LINES:
        E("cub_read_lines(%s)", expr_str(a0)); return;
    }
    E("/* unhandled builtin */ 0");
}

/* `match` becomes a switch on the tag, with what each value carries bound
 * to names inside its case.  `into` is where an arm's value goes when the
 * match is being used as one. */
static void gen_match(Expr *subject, Vec *arms, const char *into, int lvl);

static void gen_expr(Expr *e) {
    switch (e->kind) {
    case EX_NOTHING:
        E("(%s){ .ok = false }", ctype(e->type));
        break;

    case EX_WRAP:
        if (e->type->elem->kind == TY_VOID) E("(%s){ .ok = true }", ctype(e->type));
        else E("(%s){ .ok = true, .value = %s }", ctype(e->type), expr_str(e->a));
        break;

    case EX_INSIST:
        /* the type being looked into may appear nowhere else, so make sure
         * its definition and helper get written out */
        (void)ctype(e->a->type);
        E("cub_insist_%s(%s, %s)", ty_mangle(e->type),
          expr_str(e->a), loc(e->line));
        break;

    case EX_ORELSE: {
        int id = ++tmp_id;
        Type *mt = e->a->type;
        char *m = expr_str(e->a);
        hoist("%s cub_m%d = %s;\n", ctype(mt), id, m);
        hoist("%s cub_v%d;\n", ctype(mt->elem), id);
        hoist("if (cub_m%d.ok) cub_v%d = cub_m%d.value;\n", id, id, id);
        char *fb = expr_str(e->b);
        hoist("else cub_v%d = %s;\n", id, fb);
        E("cub_v%d", id);
        break;
    }

    case EX_TRY: {
        int id = ++tmp_id;
        Type *mt = e->a->type;
        char *m = expr_str(e->a);
        hoist("%s cub_m%d = %s;\n", ctype(mt), id, m);
        hoist("if (!cub_m%d.ok) return (%s){ .ok = false, .err = cub_m%d.err };\n",
              id, ctype(cur_ret), id);
        E("cub_m%d.value", id);
        break;
    }

    case EX_INT:   E("INT64_C(%lld)", (long long)e->ival); break;
    case EX_FLOAT:
        if (e->fval != e->fval)                E("(0.0 / 0.0)");
        else if (e->fval > 1.7976931348623157e308)  E("(1.0 / 0.0)");
        else if (e->fval < -1.7976931348623157e308) E("(-1.0 / 0.0)");
        else E("((double)%.17g)", e->fval);
        break;
    case EX_BOOL:  E("%s", e->bval ? "true" : "false"); break;
    case EX_STR:   E("cub_str_lit(%s, %d)", lit(e->sval), (int)strlen(e->sval)); break;

    case EX_INTERP: E("%s", concat_args(&e->args, false)); break;

    case EX_IDENT: E("%s", e->var ? e->var->cname : e->name); break;

    case EX_UNARY:
        if (e->op == TK_MINUS && e->type && e->type->kind == TY_INT) {
            E("cub_neg_int(%s, %s)", expr_str(e->a), loc(e->line));
            return;
        }
        E("(%s", e->op == TK_MINUS ? "-" : "!");
        gen_expr(e->a);
        E(")");
        break;

    case EX_BINARY: gen_binary(e); break;

    case EX_CALL:
        if (e->fn && e->fn->is_extern) {
            /* Text is a pointer and a length here and a bare pointer there,
             * so it is unwrapped going out and measured coming back. */
            bool gives_text = e->fn->ret->kind == TY_STR;
            if (gives_text) E("cub_str_of_c(");
            E("%s(", e->fn->cname);
            for (int i = 0; i < e->args.len; i++) {
                Expr *a = e->args.items[i];
                if (i) E(", ");
                if (a->type && a->type->kind == TY_STR) E("(%s).data", expr_str(a));
                else gen_expr(a);
            }
            E(")");
            if (gives_text) E(")");
        } else if (e->fn) {
            E("%s(", e->fn->cname);
            for (int i = 0; i < e->args.len; i++) {
                if (i) E(", ");
                gen_expr(e->args.items[i]);
            }
            E(")");
        } else {
            gen_builtin(e);
        }
        break;

    case EX_INDEX:
        if (e->a->type->kind == TY_MAP) {
            E("(*(%s *)cub_map_at(%s, %s, %s))", ctype(e->type),
              expr_str(e->a),
              e->a->type->key->kind == TY_STR
                  ? cx_fmt("%s, 0", expr_str(e->b))
                  : cx_fmt("cub_str_lit(\"\", 0), %s", expr_str(e->b)),
              loc(e->line));
            break;
        }
        E("(*(%s *)cub_arr_at(%s, %s, %s))", ctype(e->type),
          expr_str(e->a), expr_str(e->b), loc(e->line));
        break;

    case EX_FIELD: {
        Type *ot = e->a->type;
        if (ot && ot->kind == TY_CLASS) {
            ClassDef *cd = ot->cdef;
            ClassDef *owner = field_owner(cd, e->name);
            int steps = owner ? cd->depth - owner->depth : 0;
            E("%s->%sf_%s", obj_ptr(expr_str(e->a), cd, e->line),
              base_steps(steps), e->name);
            break;
        }
        E("(");
        gen_expr(e->a);
        E(").f_%s", e->name);
        break;
    }

    case EX_SELF:
    case EX_SUPER:
        E("((void *)self)");
        break;

    case EX_NEW: {
        E("cubnew_%s(", e->cls->name);
        for (int i = 0; i < e->args.len; i++) {
            if (i) E(", ");
            gen_expr(e->args.items[i]);
        }
        E(")");
        break;
    }

    case EX_METHOD: {
        FnDecl *m = e->fn;
        Expr *target = e->a->a;           /* the object the method belongs to */

        if (m->is_static) {               /* no object involved at all */
            E("%s(", m->cname);
            for (int i = 0; i < e->args.len; i++) {
                if (i) E(", ");
                gen_expr(e->args.items[i]);
            }
            E(")");
            break;
        }

        char *obj = expr_str(target);
        if (e->enum_index) {
            /* super.method(...) -- the parent's own version, chosen here */
            E("%s((void *)self", m->cname);
        } else {
            ClassDef *cd = e->obj_type->cdef;
            E("((const struct CubC_%s_vt *)%s)->m_%s(%s",
              m->slot_owner->name, obj_vt(obj, cd, e->line), m->name,
              cx_fmt("(void *)%s", obj_ptr(obj, cd, e->line)));
        }
        for (int i = 0; i < e->args.len; i++) {
            E(", ");
            gen_expr(e->args.items[i]);
        }
        E(")");
        break;
    }

    case EX_ARRAYLIT: {
        const char *ct = ctype(e->type->elem);
        if (e->args.len == 0) { E("cub_arr_new(sizeof(%s), 0)", ct); break; }
        E("cub_arr_lit(sizeof(%s), %d, (%s[]){", ct, e->args.len, ct);
        for (int i = 0; i < e->args.len; i++) {
            if (i) E(", ");
            gen_expr(e->args.items[i]);
        }
        E("})");
        break;
    }

    case EX_MAPLIT: {
        Type *mt = e->type;
        bool str_key = mt->key->kind == TY_STR;
        E("cub_map_lit(sizeof(%s), %d, %d", ctype(mt->elem), str_key, e->args.len / 2);
        for (int i = 0; i + 1 < e->args.len; i += 2) {
            E(", ");
            gen_expr(e->args.items[i]);
            E(", (%s[]){", ctype(mt->elem));
            gen_expr(e->args.items[i + 1]);
            E("}");
        }
        E(")");
        break;
    }

    case EX_STRUCTLIT: {
        StructDef *sd = e->type->sdef;
        E("((CubS_%s){", sd->name);
        for (int i = 0; i < sd->fnames.len; i++) {
            char *fname = sd->fnames.items[i];
            if (i) E(", ");
            E(".f_%s = ", fname);
            bool found = false;
            for (int j = 0; j < e->args.len; j++)
                if (strcmp((char *)e->fnames.items[j], fname) == 0) {
                    gen_expr(e->args.items[j]);
                    found = true;
                    break;
                }
            if (!found) E("0");
            }
        E("})");
        break;
    }

    case EX_IFEXPR:
        E("(");
        gen_expr(e->a);
        E(" ? ");
        gen_expr(e->b);
        E(" : ");
        gen_expr(e->args.items[0]);
        E(")");
        break;

    case EX_ENUMVAL:
        if (e->type && e->type->kind == TY_ENUM && e->type->edef->tagged)
            E("((CubE_%s){ .tag = CubE_%s_%s })", e->sval, e->sval, e->name);
        else
            E("CubE_%s_%s", e->sval, e->name);
        break;

    case EX_ENUMMAKE: {
        EnumDef *ed = e->type->edef;
        Vec *carries = ed->vfields.items[e->enum_index];
        E("((CubE_%s){ .tag = CubE_%s_%s, .as.v_%s = {",
          ed->name, ed->name, e->name, e->name);
        for (int i = 0; i < e->args.len; i++) {
            VarSym *f = carries->items[i];
            E("%s .f_%s = %s", i ? "," : "", f->name, expr_str(e->args.items[i]));
        }
        E(" } })");
        break;
    }

    case EX_FNLIT: {
        FnDecl *f = e->lambda;
        Buf body;
        buf_init(&body);
        Buf *saved_dst = dst, *saved_pre = prelude;
        int saved_lvl = prelude_lvl;
        Type *saved_ret = cur_ret;
        dst = &body;
        prelude = NULL;
        cur_ret = f->ret;

        if (f->captures.len) {
            E("struct CubEnv_%d {\n", f->lambda_id);
            for (int i = 0; i < f->captures.len; i++) {
                VarSym *c = f->captures.items[i];
                E("    %s %s;\n", ctype(c->type), strchr(c->cname, '>') + 1);
            }
            E("};\n\n");
        }

        E("static %s %s(void *cub_env", ctype(f->ret), f->cname);
        for (int i = 0; i < f->params.len; i++) {
            VarSym *v = f->params.items[i];
            E(", %s %s", ctype(v->type), v->cname);
        }
        E(") {\n");
        if (f->captures.len)
            E("    struct CubEnv_%d *env = (struct CubEnv_%d *)cub_env;\n",
              f->lambda_id, f->lambda_id);
        else
            E("    (void)cub_env;\n");
        E("    cub_stack_check(%s, %s);\n", lit("this function"), loc(f->line));
        gen_stmts(&f->body, 1);
        if (f->ret->kind == TY_VOID) { indent(1); E("return;\n"); }
        else if (f->ret->kind == TY_STR) { indent(1); E("return cub_str_lit(\"\", 0);\n"); }
        else if (f->ret->kind == TY_ARRAY) { indent(1); E("return cub_arr_new(1, 0);\n"); }
        else { indent(1); E("{ %s cub_unreachable = {0}; return cub_unreachable; }\n",
                            ctype(f->ret)); }
        E("}\n\n");

        dst = saved_dst;
        prelude = saved_pre;
        prelude_lvl = saved_lvl;
        cur_ret = saved_ret;
        buf_puts(&lambdas, body.data);

        if (!f->captures.len) {
            E("((CubFn){ (void *)%s, 0 })", f->cname);
        } else {
            int id = ++tmp_id;
            hoist("struct CubEnv_%d *cub_e%d = "
                  "(struct CubEnv_%d *)cub_alloc(sizeof *cub_e%d);\n",
                  f->lambda_id, id, f->lambda_id, id);
            for (int i = 0; i < f->captures.len; i++) {
                VarSym *c = f->captures.items[i];
                VarSym *o = f->cap_outer.items[i];
                hoist("cub_e%d->%s = %s;\n", id, strchr(c->cname, '>') + 1, o->cname);
            }
            E("((CubFn){ (void *)%s, (void *)cub_e%d })", f->cname, id);
        }
        break;
    }

    case EX_FNREF:
        need_fn_wrapper(e->fn);
        E("((CubFn){ (void *)cubw_%s, 0 })", e->fn->name);
        break;

    case EX_CALLVAL: {
        Type *ft = e->a->type;
        char *fv = expr_str(e->a);
        E("((%s)(%s).fn)((%s).env", fn_ptr_cast(ft), fv, fv);
        for (int i = 0; i < e->args.len; i++)
            E(", %s", expr_str(e->args.items[i]));
        E(")");
        break;
    }

    case EX_MATCH: {
        int id = ++tmp_id;
        char *tmp = cx_fmt("cub_r%d", id);
        hoist("%s %s = %s;\n", ctype(e->type), tmp, default_value(e->type));
        /* the switch itself is a statement, so it goes in front of the one
         * being emitted, and what is left behind is the temporary */
        Buf *saved = dst;
        dst = prelude;
        gen_match(e->a, &e->arms, tmp, prelude_lvl);
        dst = saved;
        E("%s", tmp);
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static void gen_stmts(Vec *body, int lvl);

/* ------------------------------------------------------------------ */

static void gen_assign_target(Expr *lhs) {
    /* `m[key] = value` puts the key there if it is new, so it cannot go
     * through the read path, which insists the key already exists. */
    if (lhs->kind == EX_INDEX && lhs->a->type->kind == TY_MAP) {
        Type *mt = lhs->a->type;
        buf_printf(dst, "(*(%s *)cub_map_put(%s, %s))", ctype(lhs->type),
                   expr_str(lhs->a),
                   mt->key->kind == TY_STR
                       ? cx_fmt("%s, 0", expr_str(lhs->b))
                       : cx_fmt("cub_str_lit(\"\", 0), %s", expr_str(lhs->b)));
        return;
    }
    gen_expr(lhs);
}

static void gen_stmt_inner(Stmt *s, int lvl);

/* Emit one statement, with whatever `or` and `try` needed in front of it. */
static void gen_stmt(Stmt *s, int lvl) {
    Buf pre, body;
    buf_init(&pre);
    buf_init(&body);

    Buf *saved_dst = dst, *saved_pre = prelude;
    int saved_lvl = prelude_lvl;
    prelude = &pre;
    prelude_lvl = lvl;
    dst = &body;

    /* A `while` tests its condition again on every pass, so work hoisted
     * out of it belongs inside the loop rather than before it. */
    if (s->kind == ST_WHILE) {
        Buf cond;
        buf_init(&cond);
        Buf *d = dst;
        dst = &cond;
        gen_expr(s->cond);
        dst = d;
        if (pre.len > 0) {
            indent(lvl);
            E("for (;;) {\n");
            buf_puts(dst, pre.data);
            pre.len = 0;
            if (pre.data) pre.data[0] = 0;
            indent(lvl + 1);
            E("if (!(%s)) break;\n", cond.data);
            gen_stmts(&s->body, lvl + 1);
            indent(lvl);
            E("}\n");
            goto done;
        }
        indent(lvl);
        E("while (%s) {\n", cond.data);
        gen_stmts(&s->body, lvl + 1);
        indent(lvl);
        E("}\n");
        goto done;
    }

    gen_stmt_inner(s, lvl);

done:
    dst = saved_dst;
    prelude = saved_pre;
    prelude_lvl = saved_lvl;
    buf_puts(dst, pre.data);
    buf_puts(dst, body.data);
}

static void gen_stmt_inner(Stmt *s, int lvl) {
    switch (s->kind) {
    case ST_LET:
        indent(lvl);
        E("%s %s = ", ctype(s->var->type), s->var->cname);
        gen_expr(s->rhs);
        E(";\n");
        break;

    case ST_ASSIGN: {
        indent(lvl);
        if (s->op == TK_ASSIGN) {
            gen_assign_target(s->lhs);
            E(" = ");
            gen_expr(s->rhs);
        } else if (s->lhs->type->kind == TY_STR) {
            char *t = expr_str(s->lhs);
            E("%s = cub_str_concat(%s, %s)", t, t, expr_str(s->rhs));
        } else if (s->lhs->type->kind == TY_INT) {
            const char *fn = s->op == TK_SLASHEQ   ? "cub_div_int"
                           : s->op == TK_PERCENTEQ ? "cub_mod_int"
                           : s->op == TK_PLUSEQ    ? "cub_add_int"
                           : s->op == TK_MINUSEQ   ? "cub_sub_int"
                                                   : "cub_mul_int";
            char *t = expr_str(s->lhs);
            E("%s = %s(%s, %s, %s)", t, fn, t, expr_str(s->rhs), loc(s->line));
        } else {
            const char *o = s->op == TK_PLUSEQ ? "+=" : s->op == TK_MINUSEQ ? "-="
                          : s->op == TK_STAREQ ? "*=" : s->op == TK_SLASHEQ ? "/=" : "%=";
            gen_assign_target(s->lhs);
            E(" %s ", o);
            gen_expr(s->rhs);
        }
        E(";\n");
        break;
    }

    case ST_EXPR:
        indent(lvl);
        gen_expr(s->rhs);
        E(";\n");
        break;

    case ST_IF:
        indent(lvl);
        E("if (");
        gen_expr(s->cond);
        E(") {\n");
        gen_stmts(&s->body, lvl + 1);
        indent(lvl);
        E("}");
        if (s->els.len > 0) {
            Stmt *first = s->els.items[0];
            if (s->els.len == 1 && first->kind == ST_IF) {
                E(" else ");
                Buf tmp;
                buf_init(&tmp);
                Buf *save = dst;
                dst = &tmp;
                gen_stmt(first, lvl);
                dst = save;
                /* the nested if was written with a leading indent; trim it */
                const char *p = tmp.data;
                while (*p == ' ') p++;
                E("%s", p);
                break;
            }
            E(" else {\n");
            gen_stmts(&s->els, lvl + 1);
            indent(lvl);
            E("}\n");
            break;
        }
        E("\n");
        break;

    case ST_MATCH:
        gen_match(s->rhs, &s->arms, NULL, lvl);
        break;

    case ST_IFLET: {
        int id = ++tmp_id;
        Type *mt = s->rhs->type;
        indent(lvl);
        E("{\n");
        indent(lvl + 1);
        E("%s cub_m%d = ", ctype(mt), id);
        gen_expr(s->rhs);
        E(";\n");
        indent(lvl + 1);
        E("if (cub_m%d.ok) {\n", id);
        if (s->var && mt->elem->kind != TY_VOID) {
            indent(lvl + 2);
            E("%s %s = cub_m%d.value;\n", ctype(mt->elem), s->var->cname, id);
            indent(lvl + 2);
            E("(void)%s;\n", s->var->cname);
        }
        gen_stmts(&s->body, lvl + 2);
        indent(lvl + 1);
        E("} else {\n");
        if (s->err_var) {
            indent(lvl + 2);
            E("CubStr %s = cub_m%d.err;\n", s->err_var->cname, id);
            indent(lvl + 2);
            E("(void)%s;\n", s->err_var->cname);
        }
        gen_stmts(&s->els, lvl + 2);
        indent(lvl + 1);
        E("}\n");
        indent(lvl);
        E("}\n");
        break;
    }

    case ST_WHILE:      /* handled in gen_stmt, which can rewrite the loop */
        break;

    case ST_FORRANGE: {
        int id = ++tmp_id;
        indent(lvl);
        E("for (int64_t %s = ", s->var->cname);
        gen_expr(s->from);
        E(", cub_end_%d = ", id);
        gen_expr(s->to);
        E("; %s %s cub_end_%d; %s++) {\n",
          s->var->cname, s->inclusive ? "<=" : "<", id, s->var->cname);
        gen_stmts(&s->body, lvl + 1);
        indent(lvl);
        E("}\n");
        break;
    }

    case ST_FORIN: {
        int id = ++tmp_id;
        indent(lvl);
        E("{\n");
        indent(lvl + 1);
        E("CubArr cub_seq_%d = ", id);
        gen_expr(s->rhs);
        E(";\n");
        indent(lvl + 1);
        E("for (int64_t cub_i_%d = 0; cub_i_%d < cub_seq_%d->len; cub_i_%d++) {\n",
          id, id, id, id);
        indent(lvl + 2);
        E("%s %s = *(%s *)(cub_seq_%d->data + cub_i_%d * cub_seq_%d->esz);\n",
          ctype(s->var->type), s->var->cname, ctype(s->var->type), id, id, id);
        gen_stmts(&s->body, lvl + 2);
        indent(lvl + 1);
        E("}\n");
        indent(lvl);
        E("}\n");
        break;
    }

    case ST_RETURN:
        if (!s->rhs && cur_ret && cur_ret->kind == TY_FAIL) {
            indent(lvl);
            E("return (%s){ .ok = true };\n", ctype(cur_ret));
            break;
        }
        indent(lvl);
        if (s->rhs) { E("return "); gen_expr(s->rhs); E(";\n"); }
        else E("return;\n");
        break;

    case ST_BREAK:    indent(lvl); E("break;\n");    break;
    case ST_CONTINUE: indent(lvl); E("continue;\n"); break;

    case ST_BLOCK:
        indent(lvl);
        E("{\n");
        gen_stmts(&s->body, lvl + 1);
        indent(lvl);
        E("}\n");
        break;
    }
}

/* One switch, one case per arm.  The subject is worked out once into a
 * temporary, because it may be a call. */
static void gen_match(Expr *subject, Vec *arms, const char *into, int lvl) {
    EnumDef *ed = subject->type->edef;
    int id = ++tmp_id;
    char *subj = cx_fmt("cub_s%d", id);

    indent(lvl);
    E("{\n");
    indent(lvl + 1);
    E("%s %s = %s;\n", ctype(subject->type), subj, expr_str(subject));
    indent(lvl + 1);
    E("switch (%s%s) {\n", subj, ed->tagged ? ".tag" : "");

    for (int i = 0; i < arms->len; i++) {
        MatchArm *a = arms->items[i];
        indent(lvl + 1);
        if (a->is_default) E("default: {\n");
        else               E("case CubE_%s_%s: {\n", ed->name, a->variant);

        Vec *carries = a->index >= 0 ? ed->vfields.items[a->index] : NULL;
        for (int k = 0; carries && k < a->binds.len; k++) {
            VarSym *b = a->binds.items[k];
            VarSym *f = carries->items[k];
            indent(lvl + 2);
            E("%s %s = %s.as.v_%s.f_%s;\n", ctype(b->type), b->cname,
              subj, a->variant, f->name);
            indent(lvl + 2);
            E("(void)%s;\n", b->cname);
        }
        if (into) {
            Buf pre, val;
            buf_init(&pre);
            buf_init(&val);
            Buf *sd = dst, *sp = prelude;
            int sl = prelude_lvl;
            prelude = &pre;
            prelude_lvl = lvl + 2;
            dst = &val;
            gen_expr(a->value);
            dst = sd;
            prelude = sp;
            prelude_lvl = sl;
            buf_puts(dst, pre.data);
            indent(lvl + 2);
            E("%s = %s;\n", into, val.data);
        } else {
            gen_stmts(&a->body, lvl + 2);
        }
        indent(lvl + 2);
        E("break;\n");
        indent(lvl + 1);
        E("}\n");
    }

    indent(lvl + 1);
    E("}\n");
    indent(lvl);
    E("}\n");
}

static void gen_stmts(Vec *body, int lvl) {
    for (int i = 0; i < body->len; i++) gen_stmt(body->items[i], lvl);
}

/* ------------------------------------------------------------------ */
/* declarations                                                        */
/* ------------------------------------------------------------------ */

/* One definition per inner type, emitted the first time something needs
 * it -- a field may hold a `Point?`, and `Point` has to exist first. */
static Vec maybe_emitted;

/* The runtime declares the shapes it hands back itself. */
static bool maybe_predefined(Type *m) {
    const char *n = ty_mangle(m->elem);
    return strcmp(n, "int") == 0 || strcmp(n, "float") == 0 ||
           strcmp(n, "string") == 0 || strcmp(n, "arr_string") == 0 ||
           strcmp(n, "void") == 0;
}

static void emit_maybe_type(Type *m) {
    for (int i = 0; i < maybe_emitted.len; i++)
        if (ty_same(((Type *)maybe_emitted.items[i])->elem, m->elem)) return;
    vec_push(&maybe_emitted, m);

    const char *nm = ctype(m);
    if (maybe_predefined(m)) {                 /* only the helper is ours */
        E("static %s cub_insist_%s(%s m, const char *f, int l) {\n",
          m->elem->kind == TY_VOID ? "void" : ctype(m->elem), ty_mangle(m->elem), nm);
        E("    if (!m.ok) cub_panic_at(f, l, \"%%s\", m.err.len\n"
          "        ? (const char *)m.err.data\n"
          "        : \"there is nothing here\");\n");
        if (m->elem->kind != TY_VOID) E("    return m.value;\n");
        E("}\n\n");
        return;
    }
    E("typedef struct {\n    bool ok;\n");
    if (m->elem->kind != TY_VOID) E("    %s value;\n", ctype(m->elem));
    E("    CubStr err;\n} %s;\n\n", nm);

    E("static %s cub_insist_%s(%s m, const char *f, int l) {\n",
      m->elem->kind == TY_VOID ? "void" : ctype(m->elem), ty_mangle(m->elem), nm);
    E("    if (!m.ok) cub_panic_at(f, l, \"%%s\", m.err.len\n"
      "        ? (const char *)m.err.data\n"
      "        : \"there is nothing here\");\n");
    if (m->elem->kind != TY_VOID) E("    return m.value;\n");
    E("}\n\n");
}

/* Whatever this type is built out of has to be defined before it is. */
static void emit_struct(StructDef *sd, Vec *done);

static void ensure_type(Type *t, Vec *done) {
    if (!t) return;
    if (t->kind == TY_STRUCT && t->sdef) emit_struct(t->sdef, done);
    else if (t->kind == TY_OPT || t->kind == TY_FAIL) {
        ensure_type(t->elem, done);
        emit_maybe_type(t);
    }
}

static void emit_struct(StructDef *sd, Vec *done) {
    for (int i = 0; i < done->len; i++)
        if (done->items[i] == sd) return;
    /* whatever a field is built out of must be defined first */
    for (int i = 0; i < sd->ftypes.len; i++)
        ensure_type(sd->ftypes.items[i], done);
    for (int i = 0; i < done->len; i++)
        if (done->items[i] == sd) return;
    vec_push(done, sd);

    E("typedef struct CubS_%s {\n", sd->name);
    for (int i = 0; i < sd->fnames.len; i++)
        E("    %s f_%s;\n", ctype(sd->ftypes.items[i]), (char *)sd->fnames.items[i]);
    E("} CubS_%s;\n\n", sd->name);
}

/* Classes are written out oldest ancestor first, so that a subclass can
 * embed its parent by value. */
static int class_order(const void *a, const void *b) {
    const ClassDef *x = *(const ClassDef *const *)a;
    const ClassDef *y = *(const ClassDef *const *)b;
    return x->depth - y->depth;
}

static void emit_class_struct(ClassDef *cd, Vec *done) {
    for (int i = 0; i < cd->ftypes.len; i++)
        ensure_type(cd->ftypes.items[i], done);
    E("struct CubC_%s {\n", cd->name);
    if (cd->base) E("    CubC_%s base;\n", cd->base->name);
    else          E("    const void *vt;\n");
    for (int i = 0; i < cd->fnames.len; i++)
        E("    %s f_%s;\n", ctype(cd->ftypes.items[i]), (char *)cd->fnames.items[i]);
    E("};\n\n");
}

static void method_signature(FnDecl *m) {
    if (m->is_static) {
        E("static %s %s(", ctype(m->ret), m->cname);
        if (m->params.len == 0) E("void");
        for (int i = 0; i < m->params.len; i++) {
            VarSym *v = m->params.items[i];
            E("%s%s %s", i ? ", " : "", ctype(v->type), v->cname);
        }
        E(")");
        return;
    }
    E("static %s %s(void *cub_self", ctype(m->ret), m->cname);
    for (int i = 0; i < m->params.len; i++) {
        VarSym *v = m->params.items[i];
        E(", %s %s", ctype(v->type), v->cname);
    }
    E(")");
}

static void emit_vt_struct(ClassDef *cd) {
    E("struct CubC_%s_vt {\n", cd->name);
    if (cd->base) E("    struct CubC_%s_vt base;\n", cd->base->name);
    else          E("    const char *cub_class;\n");
    for (int i = 0; i < cd->own_slots.len; i++) {
        FnDecl *m = cd->own_slots.items[i];
        E("    %s (*m_%s)(void *", ctype(m->ret), m->name);
        for (int j = 0; j < m->params.len; j++)
            E(", %s", ctype(((VarSym *)m->params.items[j])->type));
        E(");\n");
    }
    E("};\n\n");
}

/* Fill in one dispatch table: every slot in `layout` gets whichever
 * version `actual` ends up with. */
static void emit_vt_value(ClassDef *layout, ClassDef *actual) {
    E("{ ");
    if (layout->base) { emit_vt_value(layout->base, actual); }
    else              { E("%s", lit(actual->name)); }
    for (int i = 0; i < layout->own_slots.len; i++) {
        FnDecl *slot = layout->own_slots.items[i];
        FnDecl *impl = method_of(actual, slot->name);
        E(", %s", impl ? impl->cname : "0");
    }
    E(" }");
}

static void emit_constructor(ClassDef *cd) {
    FnDecl *ini = method_of(cd, "init");

    E("static void *cubnew_%s(", cd->name);
    if (!ini || ini->params.len == 0) E("void");
    for (int i = 0; ini && i < ini->params.len; i++) {
        VarSym *v = ini->params.items[i];
        if (i) E(", ");
        E("%s %s", ctype(v->type), v->cname);
    }
    E(") {\n");
    E("    CubC_%s *o = (CubC_%s *)cub_obj_new(sizeof(CubC_%s));\n",
      cd->name, cd->name, cd->name);
    E("    o->%svt = &CubC_%s_vtable;\n", base_steps(cd->depth), cd->name);

    /* every field starts from a known value, oldest ancestor first */
    Vec chain = {0};
    for (ClassDef *c = cd; c; c = c->base) vec_push(&chain, c);
    for (int ci = chain.len - 1; ci >= 0; ci--) {
        ClassDef *c = chain.items[ci];
        for (int fi = 0; fi < c->fnames.len; fi++)
            E("    o->%sf_%s = %s;\n", base_steps(cd->depth - c->depth),
              (char *)c->fnames.items[fi], default_value(c->ftypes.items[fi]));
    }
    if (ini) {
        E("    %s((void *)o", ini->cname);
        for (int i = 0; i < ini->params.len; i++)
            E(", %s", ((VarSym *)ini->params.items[i])->cname);
        E(");\n");
    }
    E("    return (void *)o;\n}\n\n");
}

static void fn_signature(FnDecl *f) {
    E("static %s %s(", ctype(f->ret), f->cname);
    if (f->params.len == 0) E("void");
    for (int i = 0; i < f->params.len; i++) {
        VarSym *v = f->params.items[i];
        if (i) E(", ");
        E("%s %s", ctype(v->type), v->cname);
    }
    E(")");
}

char *codegen_program(Program *p, const char *unit_name) {
    prog = p;
    unit = unit_name;
    buf_init(&out);
    buf_init(&bodies);
    buf_init(&lambdas);
    buf_init(&helpers);
    dst = &out;

    E("/* Generated by cubc %s from %s.\n"
      " * This file is standalone C99: compile it anywhere, no Cub required.\n"
      " */\n", CUB_VERSION, unit_name);
    E("%s\n", CUB_RUNTIME_SRC);

    if (p->headers.len || p->externs.len) {
        E("/* ---- C the program reaches into ---- */\n\n");
        for (int i = 0; i < p->headers.len; i++)
            E("#include <%s>\n", (char *)p->headers.items[i]);
        if (p->headers.len) E("\n");
    }

    E("/* ---- types ---- */\n\n");
    Vec done = {0};
    for (int i = 0; i < p->enums.len; i++) {
        EnumDef *ed = p->enums.items[i];
        if (ed->tagged) continue;              /* written out below */
        E("typedef enum CubE_%s {", ed->name);
        for (int j = 0; j < ed->vals.len; j++)
            E("%s CubE_%s_%s%s", j ? "" : "", ed->name, (char *)ed->vals.items[j],
              j + 1 < ed->vals.len ? "," : "");
        E(" } CubE_%s;\n", ed->name);
    }
    if (p->enums.len) E("\n");

    for (int i = 0; i < p->structs.len; i++) emit_struct(p->structs.items[i], &done);

    /* An enum whose values carry things becomes a tag and a union, and
     * anything it carries has to be defined first. */
    for (int i = 0; i < p->enums.len; i++) {
        EnumDef *ed = p->enums.items[i];
        if (!ed->tagged) continue;
        for (int j = 0; j < ed->vfields.len; j++) {
            Vec *fields = ed->vfields.items[j];
            for (int k = 0; k < fields->len; k++)
                ensure_type(((VarSym *)fields->items[k])->type, &done);
        }
        E("enum {");
        for (int j = 0; j < ed->vals.len; j++)
            E("%s CubE_%s_%s%s", j ? "" : "", ed->name, (char *)ed->vals.items[j],
              j + 1 < ed->vals.len ? "," : "");
        E(" };\n");
        E("typedef struct CubE_%s {\n    int tag;\n", ed->name);
        bool any = false;
        for (int j = 0; j < ed->vals.len; j++)
            if (((Vec *)ed->vfields.items[j])->len) any = true;
        if (any) {
            E("    union {\n");
            for (int j = 0; j < ed->vals.len; j++) {
                Vec *fields = ed->vfields.items[j];
                if (!fields->len) continue;
                E("        struct {");
                for (int k = 0; k < fields->len; k++) {
                    VarSym *v = fields->items[k];
                    E(" %s f_%s;", ctype(v->type), v->name);
                }
                E(" } v_%s;\n", (char *)ed->vals.items[j]);
            }
            E("    } as;\n");
        }
        E("} CubE_%s;\n\n", ed->name);
    }

    /* ---- classes ---- */
    Vec ordered = {0};
    for (int i = 0; i < p->classes.len; i++) vec_push(&ordered, p->classes.items[i]);
    if (ordered.len > 1)
        qsort(ordered.items, (size_t)ordered.len, sizeof(void *), class_order);

    for (int i = 0; i < ordered.len; i++)
        E("typedef struct CubC_%s CubC_%s;\n", ((ClassDef *)ordered.items[i])->name,
          ((ClassDef *)ordered.items[i])->name);
    if (ordered.len) E("\n");
    for (int i = 0; i < ordered.len; i++) emit_class_struct(ordered.items[i], &done);
    for (int i = 0; i < ordered.len; i++) emit_vt_struct(ordered.items[i]);

    /* quoted text helper, used when printing containers */
    E("static CubStr cubstr_quoted(CubStr s) {\n"
      "    return cub_str_concat(cub_str_concat(cub_str_lit(\"\\\"\", 1), s),\n"
      "                          cub_str_lit(\"\\\"\", 1));\n}\n\n");

    /* ---- method and constructor bodies ---- */
    dst = &bodies;
    for (int i = 0; i < ordered.len; i++) {
        ClassDef *cd = ordered.items[i];
        in_unit(cd->src);
        for (int j = 0; j < cd->methods.len + cd->statics.len; j++) {
            bool is_static = j >= cd->methods.len;
            FnDecl *m = is_static ? cd->statics.items[j - cd->methods.len]
                                  : cd->methods.items[j];
            method_signature(m);
            cur_ret = m->ret;
            E(" {\n");
            E("    cub_stack_check(%s, %s);\n",
              lit(cx_fmt("%s.%s", cd->name, m->name)), loc(m->line));
            if (!is_static) {
                E("    CubC_%s *self = (CubC_%s *)cub_self;\n", cd->name, cd->name);
                E("    (void)self;\n");
            }
            gen_stmts(&m->body, 1);
            if (m->ret->kind == TY_VOID) { indent(1); E("return;\n"); }
            else if (m->ret->kind == TY_FAIL && m->ret->elem->kind == TY_VOID) {
                indent(1); E("return (%s){ .ok = true };\n", ctype(m->ret));
            }
        else if (m->ret->kind == TY_FAIL && m->ret->elem->kind == TY_VOID) {
            indent(1); E("return (%s){ .ok = true };\n", ctype(m->ret));
        }
            else if (m->ret->kind == TY_STR) { indent(1); E("return cub_str_lit(\"\", 0);\n"); }
            else if (m->ret->kind == TY_ARRAY) { indent(1); E("return cub_arr_new(1, 0);\n"); }
            else if (m->ret->kind == TY_CLASS) { indent(1); E("return NULL;\n"); }
            else { indent(1); E("{ %s cub_unreachable = {0}; return cub_unreachable; }\n",
                                ctype(m->ret)); }
            E("}\n\n");
        }
        emit_constructor(cd);
    }

    /* ---- function bodies (collects the helpers they need) ---- */
    for (int i = 0; i < p->fns.len; i++) {
        FnDecl *f = p->fns.items[i];
        if (f->is_extern) continue;          /* C already has the body */
        in_unit(f->src);
        fn_signature(f);
        cur_ret = f->ret;
        E(" {\n");
        E("    cub_stack_check(%s, %s);\n", lit(f->name), loc(f->line));
        gen_stmts(&f->body, 1);
        if (f->ret->kind == TY_VOID) { indent(1); E("return;\n"); }
        else if (f->ret->kind == TY_FAIL && f->ret->elem->kind == TY_VOID) {
            indent(1); E("return (%s){ .ok = true };\n", ctype(f->ret));
        }
        else if (f->ret->kind == TY_STR) { indent(1); E("return cub_str_lit(\"\", 0);\n"); }
        else if (f->ret->kind == TY_ARRAY) { indent(1); E("return cub_arr_new(1, 0);\n"); }
        else { indent(1); E("{ %s cub_unreachable = {0}; return cub_unreachable; }\n", ctype(f->ret)); }
        E("}\n\n");
    }

    /* globals: declared at file scope, initialised at startup */
    Buf ginit;
    buf_init(&ginit);
    dst = &ginit;
    E("static void cub_init_globals(void) {\n");
    for (int i = 0; i < p->globals.len; i++) {
        Stmt *s = p->globals.items[i];
        indent(1);
        E("%s = ", s->var->cname);
        gen_expr(s->rhs);
        E(";\n");
    }
    E("}\n\n");

    /* helpers may pull in further helpers, so drain the worklist */
    for (int i = 0; i < helper_types.len; i++) emit_helper(helper_types.items[i]);

    /* ---- assemble ---- */
    dst = &out;
    for (int i = 0; i < eq_helpers.len; i++) emit_eq_helper(eq_helpers.items[i]);

    /* Anything that turned up while the bodies were written: a local
     * `int?`, or the `int!` behind an `int(text)!`, need not appear in any
     * field.  This runs after the bodies for that reason. */
    for (int i = 0; i < maybe_types.len; i++)
        emit_maybe_type(maybe_types.items[i]);

    E("/* ---- globals ---- */\n\n");
    for (int i = 0; i < p->globals.len; i++) {
        Stmt *s = p->globals.items[i];
        E("static %s %s;\n", ctype(s->var->type), s->var->cname);
    }
    if (p->globals.len) E("\n");

    E("/* ---- prototypes ---- */\n\n");
    for (int i = 0; i < helper_types.len; i++) {
        Type *t = helper_types.items[i];
        E("static CubStr %s(%s);\n", helper_name(t),
          t->kind == TY_ARRAY ? "CubArr" : ctype(t));
    }
    for (int i = 0; i < p->fns.len; i++) {
        FnDecl *f = p->fns.items[i];
        if (f->is_extern) continue;
        fn_signature(f);
        E(";\n");
    }

    for (int i = 0; i < ordered.len; i++) {
        ClassDef *cd = ordered.items[i];
        for (int j = 0; j < cd->methods.len; j++) {
            method_signature(cd->methods.items[j]);
            E(";\n");
        }
        for (int j = 0; j < cd->statics.len; j++) {
            method_signature(cd->statics.items[j]);
            E(";\n");
        }
        FnDecl *ini = method_of(cd, "init");
        E("static void *cubnew_%s(", cd->name);
        if (!ini || ini->params.len == 0) E("void");
        for (int j = 0; ini && j < ini->params.len; j++)
            E("%s%s", j ? ", " : "", ctype(((VarSym *)ini->params.items[j])->type));
        E(");\n");
    }
    if (ordered.len) {
        E("\n/* ---- dispatch tables ---- */\n\n");
        for (int i = 0; i < ordered.len; i++) {
            ClassDef *cd = ordered.items[i];
            E("static const struct CubC_%s_vt CubC_%s_vtable = ", cd->name, cd->name);
            emit_vt_value(cd, cd);
            E(";\n");
        }
    }
    E("\n/* ---- printing helpers ---- */\n\n");
    buf_puts(&out, helpers.data);
    /* Functions written inline, and wrappers for named ones used as
     * values: both are ordinary C functions, lifted out to file scope. */
    for (int i = 0; i < fn_wrappers.len; i++) {
        FnDecl *f = fn_wrappers.items[i];
        E("static %s cubw_%s(void *cub_env", ctype(f->ret), f->name);
        for (int j = 0; j < f->params.len; j++) {
            VarSym *v = f->params.items[j];
            E(", %s %s", ctype(v->type), v->cname);
        }
        E(") {\n    (void)cub_env;\n    ");
        if (f->ret->kind != TY_VOID) E("return ");
        E("%s(", f->cname);
        for (int j = 0; j < f->params.len; j++)
            E("%s%s", j ? ", " : "", ((VarSym *)f->params.items[j])->cname);
        E(");\n}\n\n");
    }
    /* the loops that `map` and friends turned out to need */
    for (int i = 0; i < walk_helpers.len; i++) {
        WalkHelper *w = walk_helpers.items[i];
        const char *et = ctype(w->elem);
        const char *fcall_ret = w->bi == BI_MAP ? ctype(w->out)
                              : w->bi == BI_SORT_BY ? "int64_t" : "bool";

        Buf sig;
        buf_init(&sig);
        buf_printf(&sig, "%s (*)(void *, %s", fcall_ret, et);
        if (w->bi == BI_SORT_BY) buf_printf(&sig, ", %s", et);
        buf_puts(&sig, ")");

        switch (w->bi) {
        case BI_MAP:
            E("static CubArr %s(CubArr a, CubFn f) {\n", w->name);
            E("    CubArr out = cub_arr_new((int64_t)sizeof(%s), a->len);\n", ctype(w->out));
            E("    for (int64_t i = 0; i < a->len; i++) {\n");
            E("        %s v = ((%s *)a->data)[i];\n", et, et);
            E("        %s r = ((%s)f.fn)(f.env, v);\n", ctype(w->out), sig.data);
            E("        cub_arr_push(out, &r);\n    }\n    return out;\n}\n\n");
            break;
        case BI_FILTER:
            E("static CubArr %s(CubArr a, CubFn f) {\n", w->name);
            E("    CubArr out = cub_arr_new(a->esz, 4);\n");
            E("    for (int64_t i = 0; i < a->len; i++) {\n");
            E("        %s v = ((%s *)a->data)[i];\n", et, et);
            E("        if (((%s)f.fn)(f.env, v)) cub_arr_push(out, &v);\n", sig.data);
            E("    }\n    return out;\n}\n\n");
            break;
        case BI_ANY:
        case BI_ALL:
            E("static bool %s(CubArr a, CubFn f) {\n", w->name);
            E("    for (int64_t i = 0; i < a->len; i++) {\n");
            E("        %s v = ((%s *)a->data)[i];\n", et, et);
            E("        if (%s((%s)f.fn)(f.env, v)) return %s;\n",
              w->bi == BI_ALL ? "!" : "", sig.data, w->bi == BI_ALL ? "false" : "true");
            E("    }\n    return %s;\n}\n\n", w->bi == BI_ALL ? "true" : "false");
            break;
        case BI_FIND_BY: {
            Type *opt = ty_opt(w->elem);
            E("static %s %s(CubArr a, CubFn f) {\n", ctype(opt), w->name);
            E("    for (int64_t i = 0; i < a->len; i++) {\n");
            E("        %s v = ((%s *)a->data)[i];\n", et, et);
            E("        if (((%s)f.fn)(f.env, v))\n", sig.data);
            E("            return (%s){ .ok = true, .value = v };\n", ctype(opt));
            E("    }\n    return (%s){ .ok = false };\n}\n\n", ctype(opt));
            break;
        }
        case BI_SORT_BY:
            /* an insertion sort: it is stable, and the comparison is a
             * call through a pointer, so a clever sort would not pay */
            E("static void %s(CubArr a, CubFn f) {\n", w->name);
            E("    %s *d = (%s *)a->data;\n", et, et);
            E("    for (int64_t i = 1; i < a->len; i++) {\n");
            E("        %s v = d[i];\n", et);
            E("        int64_t j = i - 1;\n");
            E("        while (j >= 0 && ((%s)f.fn)(f.env, d[j], v) > 0) {\n", sig.data);
            E("            d[j + 1] = d[j];\n            j--;\n        }\n");
            E("        d[j + 1] = v;\n    }\n}\n\n");
            break;
        }
    }

    if (lambdas.data) buf_puts(&out, lambdas.data);

    E("/* ---- program ---- */\n\n");
    buf_puts(&out, ginit.data);
    buf_puts(&out, bodies.data);

    FnDecl *m = p->entry;

    E("int main(int argc, char **argv) {\n");
    E("    cub_rt_init(argc, argv);\n");
    E("    cub_stack_init();\n");
    E("    cub_init_globals();\n");

    /* The starting point is a plain function, a static method, or an
     * ordinary method -- in which case an object is made to run it on. */
    char *call;
    if (!m)                          call = cx_fmt("(void)0");
    else if (!p->entry_class)        call = cx_fmt("%s()", m->cname);
    else if (m->is_static)           call = cx_fmt("%s()", m->cname);
    else {
        E("    void *cub_app = cubnew_%s();\n", p->entry_class->name);
        call = cx_fmt("%s(cub_app)", m->cname);
    }

    if (m && m->ret->kind == TY_INT) {
        E("    int rc = (int)%s;\n", call);
        E("    fflush(stdout);\n    cub_rt_shutdown();\n    return rc;\n}\n");
    } else {
        E("    %s;\n", call);
        E("    fflush(stdout);\n    cub_rt_shutdown();\n    return 0;\n}\n");
    }

    return out.data;
}
