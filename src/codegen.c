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

static const char *ctype(Type *t) {
    switch (t->kind) {
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
        E("    switch (v) {\n");
        for (int i = 0; i < ed->vals.len; i++) {
            char *vn = ed->vals.items[i];
            E("    case CubE_%s_%s: return cub_str_lit(%s, %d);\n",
              t->name, vn, lit(vn), (int)strlen(vn));
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
    case TY_ENUM:  return cx_fmt("(CubE_%s)0", t->name);
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

static void gen_binary(Expr *e) {
    Type *t = e->a->type;

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

    case BI_STR: E("%s", str_of_expr(a0, false)); return;

    case BI_INT:
        switch (a0->type->kind) {
        case TY_FLOAT: E("cub_int_of_float(%s, %s)", expr_str(a0), loc(e->line)); break;
        case TY_STR:   E("cub_int_of_str(%s, %s)", expr_str(a0), loc(e->line)); break;
        case TY_BOOL:  E("((int64_t)(%s))", expr_str(a0)); break;
        default:       E("(%s)", expr_str(a0)); break;
        }
        return;

    case BI_FLOAT:
        switch (a0->type->kind) {
        case TY_INT: E("((double)(%s))", expr_str(a0)); break;
        case TY_STR: E("cub_float_of_str(%s, %s)", expr_str(a0), loc(e->line)); break;
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

    case BI_READ_FILE:  E("cub_read_file(%s, %s)", expr_str(a0), loc(e->line)); return;
    case BI_WRITE_FILE: E("cub_write_file(%s, %s, %s)", expr_str(a0), expr_str(a1), loc(e->line)); return;

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
    case BI_DELETE_FILE: E("cub_delete_file(%s, %s)", expr_str(a0), loc(e->line)); return;
    case BI_APPEND_FILE:
        E("cub_append_file(%s, %s, %s)", expr_str(a0), expr_str(a1), loc(e->line)); return;
    case BI_READ_LINES:
        E("cub_str_lines(cub_read_file(%s, %s))", expr_str(a0), loc(e->line)); return;
    }
    E("/* unhandled builtin */ 0");
}

static void gen_expr(Expr *e) {
    switch (e->kind) {
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
        if (e->fn) {
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

    case EX_ENUMVAL: E("CubE_%s_%s", e->sval, e->name); break;
    }
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static void gen_stmts(Vec *body, int lvl);

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

static void gen_stmt(Stmt *s, int lvl) {
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

    case ST_WHILE:
        indent(lvl);
        E("while (");
        gen_expr(s->cond);
        E(") {\n");
        gen_stmts(&s->body, lvl + 1);
        indent(lvl);
        E("}\n");
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

static void gen_stmts(Vec *body, int lvl) {
    for (int i = 0; i < body->len; i++) gen_stmt(body->items[i], lvl);
}

/* ------------------------------------------------------------------ */
/* declarations                                                        */
/* ------------------------------------------------------------------ */

static void emit_struct(StructDef *sd, Vec *done) {
    for (int i = 0; i < done->len; i++)
        if (done->items[i] == sd) return;
    /* a struct held by value must be defined first */
    for (int i = 0; i < sd->ftypes.len; i++) {
        Type *ft = sd->ftypes.items[i];
        if (ft->kind == TY_STRUCT) emit_struct(ft->sdef, done);
    }
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

static void emit_class_struct(ClassDef *cd) {
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
    buf_init(&helpers);
    dst = &out;

    E("/* Generated by cubc %s from %s.\n"
      " * This file is standalone C99: compile it anywhere, no Cub required.\n"
      " */\n", CUB_VERSION, unit_name);
    E("%s\n", CUB_RUNTIME_SRC);

    E("/* ---- types ---- */\n\n");
    for (int i = 0; i < p->enums.len; i++) {
        EnumDef *ed = p->enums.items[i];
        E("typedef enum CubE_%s {", ed->name);
        for (int j = 0; j < ed->vals.len; j++)
            E("%s CubE_%s_%s%s", j ? "" : "", ed->name, (char *)ed->vals.items[j],
              j + 1 < ed->vals.len ? "," : "");
        E(" } CubE_%s;\n", ed->name);
    }
    if (p->enums.len) E("\n");

    Vec done = {0};
    for (int i = 0; i < p->structs.len; i++) emit_struct(p->structs.items[i], &done);

    /* ---- classes ---- */
    Vec ordered = {0};
    for (int i = 0; i < p->classes.len; i++) vec_push(&ordered, p->classes.items[i]);
    if (ordered.len > 1)
        qsort(ordered.items, (size_t)ordered.len, sizeof(void *), class_order);

    for (int i = 0; i < ordered.len; i++)
        E("typedef struct CubC_%s CubC_%s;\n", ((ClassDef *)ordered.items[i])->name,
          ((ClassDef *)ordered.items[i])->name);
    if (ordered.len) E("\n");
    for (int i = 0; i < ordered.len; i++) emit_class_struct(ordered.items[i]);
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
            E(" {\n");
            E("    cub_stack_check(%s, %s);\n",
              lit(cx_fmt("%s.%s", cd->name, m->name)), loc(m->line));
            if (!is_static) {
                E("    CubC_%s *self = (CubC_%s *)cub_self;\n", cd->name, cd->name);
                E("    (void)self;\n");
            }
            gen_stmts(&m->body, 1);
            if (m->ret->kind == TY_VOID) { indent(1); E("return;\n"); }
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
        in_unit(f->src);
        fn_signature(f);
        E(" {\n");
        E("    cub_stack_check(%s, %s);\n", lit(f->name), loc(f->line));
        gen_stmts(&f->body, 1);
        if (f->ret->kind == TY_VOID) { indent(1); E("return;\n"); }
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
        fn_signature(p->fns.items[i]);
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
