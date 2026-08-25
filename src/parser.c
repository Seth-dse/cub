/* parser.c -- recursive descent with precedence climbing.
 *
 * Every statement ends with a semicolon, so line breaks mean nothing to the
 * grammar and an expression can be spread over as many lines as it needs:
 *
 *     let a = 1 + 2;
 *     let b = first_part
 *           + second_part;
 *
 * A function leads with what it gives back, as C does; everything else
 * names the thing first:
 *
 *     let count: int = 0;
 *     int add(a: int, b: int) { ... }
 *     void run() { ... }
 *     struct Point { x: int; y: int; }
 *     class Dog extends Animal { ... }
 *
 * Where an older shape is recognisable, the parser says so by name rather
 * than complaining about an unexpected token.
 */
#include "cub.h"

typedef struct {
    Token  *t;
    int     n, i;
    bool    no_struct_lit;  /* while parsing if/while/for headers */
    bool    in_arm;         /* a one-statement match arm ends at `,` */
    Source *src;            /* the file these tokens came from    */
} P;

/* ---------------- token helpers ---------------- */

static Token *cur(P *p)  { return &p->t[p->i]; }
static Token *prev(P *p) { return &p->t[p->i > 0 ? p->i - 1 : 0]; }

static bool at(P *p, TokKind k) { return cur(p)->kind == k; }
static bool at_next(P *p, TokKind k) {
    int j = p->i + 1 < p->n ? p->i + 1 : p->n - 1;
    return p->t[j].kind == k;
}

static bool eat(P *p, TokKind k) {
    if (!at(p, k)) return false;
    p->i++;
    return true;
}

static void expect(P *p, TokKind k, const char *what) {
    if (eat(p, k)) return;
    Token *c = cur(p);
    err_at(c->line, c->col, "expected %s here, but found `%s`", what, tok_name(c->kind));
    stop_if_errors();
}

/* Statements end at `;`.  The complaint points just past the last token of
 * the statement, which is where the semicolon should have gone. */
static void expect_semi(P *p, const char *what) {
    if (eat(p, TK_SEMI)) return;
    /* `Circle(r) => print(r),` -- the comma ends the arm, and so does the
     * brace that closes the match. */
    if (p->in_arm && (at(p, TK_COMMA) || at(p, TK_RBRACE))) return;
    Token *pv = prev(p);
    err_at(pv->line, pv->col + pv->raw_len, "expected `;` to end this %s", what);
    if (strcmp(what, "field") == 0)
        err_help("every field in a struct or a class ends with a semicolon");
    else
        err_help("every statement in Cub ends with a semicolon");
    stop_if_errors();
}

/* A stray `;` on its own is harmless; skip any run of them. */
static void skip_semis(P *p) { while (eat(p, TK_SEMI)) {} }

/* ---------------- node constructors ---------------- */

static Expr *mkexpr(P *p, ExprKind k) {
    Expr *e = cx_alloc(sizeof(Expr));
    e->kind = k;
    e->builtin = BI_NONE;
    e->line = prev(p)->line;
    e->col = prev(p)->col;
    return e;
}

static Stmt *mkstmt(P *p, StmtKind k) {
    Stmt *s = cx_alloc(sizeof(Stmt));
    s->kind = k;
    s->line = cur(p)->line;
    s->col = cur(p)->col;
    return s;
}

/* ---------------- types ---------------- */

/* `int`, `int?`, `int!` -- the suffix says the value may be missing, and
 * with `!` a failure brings a reason along.  They do not nest: a value is
 * either there, or missing once. */
static Type *parse_suffix(P *p, Type *base) {
    if (!at(p, TK_QUESTION) && !at(p, TK_BANG)) return base;
    Token *c = cur(p);
    bool optional = at(p, TK_QUESTION);
    p->i++;
    if (at(p, TK_QUESTION) || at(p, TK_BANG)) {
        err_at(c->line, c->col, "`%s%s%s` is not a type Cub has",
               ty_show(base), optional ? "?" : "!",
               at(p, TK_QUESTION) ? "?" : "!");
        err_help("a value is either missing or it is not; write `%s?` for one "
                 "that may be absent, or `%s!` for one that may fail",
                 ty_show(base), ty_show(base));
        stop_if_errors();
    }
    if (base->kind == TY_VOID && optional) {
        err_at(c->line, c->col, "`void?` says nothing that `void` does not");
        err_help("write `void` for a function that gives nothing back, or "
                 "`void!` for one that can fail");
        stop_if_errors();
    }
    return optional ? ty_opt(base) : ty_fail(base);
}

static Type *parse_type(P *p) {
    /* `(int, int) -> int` -- what a function held as a value looks like */
    if (at(p, TK_LPAREN)) {
        Token *open = cur(p);
        p->i++;
        Vec params = {0};
        while (!at(p, TK_RPAREN)) {
            vec_push(&params, parse_type(p));
            if (!eat(p, TK_COMMA)) break;
        }
        expect(p, TK_RPAREN, "`)` to close the parameter types");
        if (!eat(p, TK_ARROW)) {
            err_at(open->line, open->col,
                   "a function type says what it gives back with `->`");
            err_help("write `(int, int) -> int`, or `(int) -> void` for one "
                     "that gives nothing back");
            stop_if_errors();
        }
        Type *ret = parse_type(p);
        return parse_suffix(p, ty_fn(&params, ret));
    }
    if (eat(p, TK_VOID)) return parse_suffix(p, ty_void());
    if (eat(p, TK_LBRACK)) {
        Type *el = parse_type(p);
        if (eat(p, TK_COLON)) {                 /* [key: value] is a map */
            Type *val = parse_type(p);
            expect(p, TK_RBRACK, "`]` to close the map type");
            return ty_map(el, val);
        }
        expect(p, TK_RBRACK, "`]` to close the array type");
        return parse_suffix(p, ty_array(el));
    }
    Token *c = cur(p);
    if (!at(p, TK_IDENT)) {
        err_at(c->line, c->col, "expected a type name, but found `%s`", tok_name(c->kind));
        err_help("types are int, float, bool, string, [T], or a type you declared");
        stop_if_errors();
    }
    p->i++;
    const char *n = c->lex;
    if (strcmp(n, "int") == 0)    return parse_suffix(p, ty_int());
    if (strcmp(n, "float") == 0)  return parse_suffix(p, ty_float());
    if (strcmp(n, "bool") == 0)   return parse_suffix(p, ty_bool());
    if (strcmp(n, "string") == 0) return parse_suffix(p, ty_str());
    /* A user type; the checker decides later whether it is a struct or
     * an enum and swaps in the canonical Type.  `Pair<int, string>` names
     * one made from a generic struct. */
    Type *named = ty_named(TY_STRUCT, c->lex);
    if (at(p, TK_LT)) {
        p->i++;
        while (!at(p, TK_GT) && !at(p, TK_EOF)) {
            vec_push(&named->targs, parse_type(p));
            if (!eat(p, TK_COMMA)) break;
        }
        expect(p, TK_GT, "`>` after the types");
        if (named->targs.len == 0) {
            err_at(c->line, c->col, "`%s<>` names no types", c->lex);
            stop_if_errors();
        }
    }
    return parse_suffix(p, named);
}

/* A function declaration leads with its type, so this is what starts one. */
static bool starts_type(P *p) {
    return at(p, TK_VOID) || at(p, TK_LBRACK) || at(p, TK_IDENT) ||
           at(p, TK_LPAREN);          /* `(int) -> int` gives back a function */
}

/* ---------------- expressions ---------------- */

static Expr *parse_expr(P *p);
static void  parse_block(P *p, Vec *out);
static FnDecl *parse_fn(P *p, bool is_static);
static Stmt *parse_stmt(P *p);
static void  parse_arms(P *p, Vec *out, bool as_value);

static Expr *parse_string(P *p, Token *t) {
    /* A literal with no interpolation is just text. */
    if (t->parts.len == 1) {
        StrPart *sp = t->parts.items[0];
        if (sp->text) {
            Expr *e = mkexpr(p, EX_STR);
            e->sval = sp->text;
            e->line = t->line; e->col = t->col;
            return e;
        }
    }
    Expr *e = mkexpr(p, EX_INTERP);
    e->line = t->line; e->col = t->col;
    for (int i = 0; i < t->parts.len; i++) {
        StrPart *sp = t->parts.items[i];
        if (sp->text) {
            Expr *lit = cx_alloc(sizeof(Expr));
            lit->kind = EX_STR;
            lit->builtin = BI_NONE;
            lit->sval = sp->text;
            lit->line = sp->line; lit->col = sp->col;
            vec_push(&e->args, lit);
        } else {
            vec_push(&e->args, parse_expr_source(sp->expr, sp->line, sp->col));
        }
    }
    return e;
}

/* `if c { a } else { b }` used where a value is expected.  Each branch holds
 * exactly one expression, and the `else` is required -- an expression must
 * always produce something. */
static Expr *parse_if_expr(P *p) {
    Token *t = cur(p);
    p->i++;                                     /* if */

    Expr *e = cx_alloc(sizeof(Expr));
    e->kind = EX_IFEXPR;
    e->builtin = BI_NONE;
    e->line = t->line;
    e->col = t->col;

    bool saved = p->no_struct_lit;
    p->no_struct_lit = true;
    e->a = parse_expr(p);
    p->no_struct_lit = saved;

    expect(p, TK_LBRACE, "`{` and one value for the `if` branch");
    e->b = parse_expr(p);
    expect(p, TK_RBRACE, "`}` after the value");

    if (!eat(p, TK_ELSE)) {
        Token *c = cur(p);
        err_at(c->line, c->col, "an `if` used as a value needs an `else`");
        err_help("every branch must produce something: `if c { a } else { b }`");
        stop_if_errors();
    }

    if (at(p, TK_IF)) {
        vec_push(&e->args, parse_if_expr(p));
        return e;
    }
    expect(p, TK_LBRACE, "`{` and one value for the `else` branch");
    vec_push(&e->args, parse_expr(p));
    expect(p, TK_RBRACE, "`}` after the value");
    return e;
}

/* An anonymous function is a declaration without a name:
 *
 *     let double = int(x: int) { return x * 2; };
 *
 * The only thing it could be confused with is a conversion like
 * `int(count)`, and a parameter list always has `name:` in it. */
static bool starts_fn_literal(P *p) {
    int j = p->i;
    if (j < p->n && p->t[j].kind == TK_LBRACK) {         /* [int](x: int) { } */
        int nest = 0;
        while (j < p->n) {
            if (p->t[j].kind == TK_LBRACK) nest++;
            else if (p->t[j].kind == TK_RBRACK && --nest == 0) { j++; break; }
            j++;
        }
    } else if (j < p->n && (p->t[j].kind == TK_IDENT || p->t[j].kind == TK_VOID)) {
        j++;
    } else {
        return false;
    }
    while (j < p->n && (p->t[j].kind == TK_QUESTION || p->t[j].kind == TK_BANG)) j++;
    if (j >= p->n || p->t[j].kind != TK_LPAREN) return false;

    /* `(name:` can only be a parameter list */
    if (j + 2 < p->n && p->t[j + 1].kind == TK_IDENT &&
        p->t[j + 2].kind == TK_COLON)
        return true;

    /* `f()` is a call and `void() { ... }` is a function of no arguments,
     * told apart by the block that follows -- except in the header of an
     * `if` or a loop, where the block belongs to the header. */
    if (j + 2 < p->n && p->t[j + 1].kind == TK_RPAREN &&
        p->t[j + 2].kind == TK_LBRACE)
        return !p->no_struct_lit;
    return false;
}

static Expr *parse_fn_literal(P *p) {
    Token *t = cur(p);
    FnDecl *f = cx_alloc(sizeof(FnDecl));
    f->src = p->src;
    f->line = t->line;
    f->col = t->col;
    f->name = "this function";
    f->ret = parse_type(p);

    expect(p, TK_LPAREN, "`(` to start the parameter list");
    while (!at(p, TK_RPAREN)) {
        Token *pn = cur(p);
        expect(p, TK_IDENT, "a parameter name");
        if (!eat(p, TK_COLON)) {
            Token *c = cur(p);
            err_at(c->line, c->col, "parameter `%s` needs a type", pn->lex);
            stop_if_errors();
        }
        VarSym *v = cx_alloc(sizeof(VarSym));
        v->name = pn->lex;
        v->type = parse_type(p);
        vec_push(&f->params, v);
        if (!eat(p, TK_COMMA)) break;
    }
    expect(p, TK_RPAREN, "`)` to close the parameter list");
    parse_block(p, &f->body);

    Expr *e = cx_alloc(sizeof(Expr));
    e->kind = EX_FNLIT;
    e->builtin = BI_NONE;
    e->line = t->line;
    e->col = t->col;
    e->lambda = f;
    return e;
}

static Expr *parse_primary(P *p) {
    Token *t = cur(p);

    if (starts_fn_literal(p)) return parse_fn_literal(p);

    if (at(p, TK_IF)) return parse_if_expr(p);

    if (at(p, TK_MATCH)) {
        Token *mt = cur(p);
        p->i++;
        Expr *e = cx_alloc(sizeof(Expr));
        e->kind = EX_MATCH;
        e->builtin = BI_NONE;
        e->line = mt->line; e->col = mt->col;
        bool saved = p->no_struct_lit;
        p->no_struct_lit = true;
        e->a = parse_expr(p);
        p->no_struct_lit = saved;
        parse_arms(p, &e->arms, true);
        return e;
    }

    if (eat(p, TK_NOTHING)) { Expr *e = mkexpr(p, EX_NOTHING); return e; }
    if (eat(p, TK_SELF))  { Expr *e = mkexpr(p, EX_SELF);  return e; }
    if (eat(p, TK_SUPER)) { Expr *e = mkexpr(p, EX_SUPER); return e; }

    if (eat(p, TK_INTLIT))   { Expr *e = mkexpr(p, EX_INT);   e->ival = t->ival; return e; }
    if (eat(p, TK_FLOATLIT)) { Expr *e = mkexpr(p, EX_FLOAT); e->fval = t->fval; return e; }
    if (eat(p, TK_TRUE))     { Expr *e = mkexpr(p, EX_BOOL);  e->bval = true;    return e; }
    if (eat(p, TK_FALSE))    { Expr *e = mkexpr(p, EX_BOOL);  e->bval = false;   return e; }
    if (eat(p, TK_STRLIT))   { return parse_string(p, t); }

    if (eat(p, TK_LPAREN)) {
        bool saved = p->no_struct_lit;
        p->no_struct_lit = false;
        Expr *e = parse_expr(p);
        p->no_struct_lit = saved;
        expect(p, TK_RPAREN, "`)` to close the group");
        return e;
    }

    if (eat(p, TK_LBRACK)) {                  /* array or map literal */
        bool saved = p->no_struct_lit;
        p->no_struct_lit = false;
        Expr *e = mkexpr(p, EX_ARRAYLIT);
        e->line = t->line; e->col = t->col;

        if (eat(p, TK_COLON)) {                /* [:] -- an empty map */
            e->kind = EX_MAPLIT;
            p->no_struct_lit = saved;
            expect(p, TK_RBRACK, "`]` to close the map");
            return e;
        }
        while (!at(p, TK_RBRACK)) {
            Expr *first = parse_expr(p);
            if (e->kind == EX_ARRAYLIT && e->args.len == 0 && at(p, TK_COLON)) {
                e->kind = EX_MAPLIT;           /* [key: value, ...] */
            }
            vec_push(&e->args, first);
            if (e->kind == EX_MAPLIT) {
                expect(p, TK_COLON, "`:` between a key and its value");
                vec_push(&e->args, parse_expr(p));
            }
            if (!eat(p, TK_COMMA)) break;
        }
        p->no_struct_lit = saved;
        expect(p, TK_RBRACK, e->kind == EX_MAPLIT ? "`]` to close the map"
                                                  : "`]` to close the array");
        return e;
    }

    if (at(p, TK_IDENT)) {
        p->i++;
        /* `Name { field: value }` -- but not where a block could start */
        if (at(p, TK_LBRACE) && !p->no_struct_lit) {
            p->i++;
            Expr *e = mkexpr(p, EX_STRUCTLIT);
            e->name = t->lex;
            e->line = t->line; e->col = t->col;
            bool saved = p->no_struct_lit;
            p->no_struct_lit = false;
            while (!at(p, TK_RBRACE)) {
                Token *f = cur(p);
                expect(p, TK_IDENT, "a field name");
                expect(p, TK_COLON, "`:` after the field name");
                vec_push(&e->fnames, f->lex);
                vec_push(&e->args, parse_expr(p));
                if (!eat(p, TK_COMMA)) break;
            }
            p->no_struct_lit = saved;
            expect(p, TK_RBRACE, "`}` to close the value");
            return e;
        }
        Expr *e = mkexpr(p, EX_IDENT);
        e->name = t->lex;
        e->line = t->line; e->col = t->col;
        return e;
    }

    err_at(t->line, t->col, "expected a value here, but found `%s`", tok_name(t->kind));
    stop_if_errors();
    return NULL;
}

static Expr *parse_postfix(P *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        if (at(p, TK_LPAREN)) {
            p->i++;
            Expr *call = cx_alloc(sizeof(Expr));
            call->kind = EX_CALL;
            call->builtin = BI_NONE;
            call->line = e->line; call->col = e->col;
            call->a = e;
            if (e->kind == EX_IDENT) call->name = e->name;
            bool saved = p->no_struct_lit;
            p->no_struct_lit = false;
            while (!at(p, TK_RPAREN)) {
                vec_push(&call->args, parse_expr(p));
                if (!eat(p, TK_COMMA)) break;
            }
            p->no_struct_lit = saved;
            expect(p, TK_RPAREN, "`)` to close the call");
            e = call;
            continue;
        }

        if (at(p, TK_LBRACK)) {
            p->i++;
            bool saved = p->no_struct_lit;
            p->no_struct_lit = false;
            Expr *ix = cx_alloc(sizeof(Expr));
            ix->kind = EX_INDEX;
            ix->builtin = BI_NONE;
            ix->line = cur(p)->line; ix->col = cur(p)->col;
            ix->a = e;
            ix->b = parse_expr(p);
            p->no_struct_lit = saved;
            expect(p, TK_RBRACK, "`]` to close the index");
            e = ix;
            continue;
        }

        if (at(p, TK_BANG)) {
            Token *b = cur(p);
            p->i++;
            Expr *ins = cx_alloc(sizeof(Expr));
            ins->kind = EX_INSIST;
            ins->builtin = BI_NONE;
            ins->line = b->line; ins->col = b->col;
            ins->a = e;
            e = ins;
            continue;
        }

        if (at(p, TK_DOT)) {
            p->i++;
            Token *f = cur(p);
            expect(p, TK_IDENT, "a field name after `.`");
            Expr *fe = cx_alloc(sizeof(Expr));
            fe->kind = EX_FIELD;
            fe->builtin = BI_NONE;
            fe->line = f->line; fe->col = f->col;
            fe->a = e;
            fe->name = f->lex;
            e = fe;
            continue;
        }
        break;
    }
    return e;
}

static Expr *parse_unary(P *p) {
    if (at(p, TK_TRY)) {
        Token *t = cur(p);
        p->i++;
        Expr *e = cx_alloc(sizeof(Expr));
        e->kind = EX_TRY;
        e->builtin = BI_NONE;
        e->line = t->line; e->col = t->col;
        e->a = parse_unary(p);
        return e;
    }
    if (at(p, TK_MINUS) || at(p, TK_BANG) || at(p, TK_NOT)) {
        Token *t = cur(p);
        p->i++;
        Expr *e = cx_alloc(sizeof(Expr));
        e->kind = EX_UNARY;
        e->builtin = BI_NONE;
        e->op = (t->kind == TK_NOT) ? TK_BANG : t->kind;
        e->line = t->line; e->col = t->col;
        e->a = parse_unary(p);
        return e;
    }
    return parse_postfix(p);
}

static int precedence(TokKind k) {
    switch (k) {
    case TK_STAR: case TK_SLASH: case TK_PERCENT:      return 6;
    case TK_PLUS: case TK_MINUS:                       return 5;
    case TK_LT: case TK_LE: case TK_GT: case TK_GE:    return 4;
    case TK_EQ: case TK_NE:                            return 3;
    case TK_ANDAND: case TK_AND:                       return 2;
    case TK_OROR:   case TK_OR:                        return 1;
    default:                                           return 0;
    }
}

static Expr *parse_binary(P *p, int min_prec) {
    Expr *lhs = parse_unary(p);
    for (;;) {
        TokKind k = cur(p)->kind;
        int prec = precedence(k);
        if (prec == 0 || prec < min_prec) break;
        Token *t = cur(p);
        p->i++;
        Expr *rhs = parse_binary(p, prec + 1);
        Expr *b = cx_alloc(sizeof(Expr));
        b->kind = EX_BINARY;
        b->builtin = BI_NONE;
        b->op = (k == TK_AND) ? TK_ANDAND : (k == TK_OR) ? TK_OROR : k;
        b->line = t->line; b->col = t->col;
        b->a = lhs;
        b->b = rhs;
        lhs = b;
    }
    return lhs;
}

static Expr *parse_expr(P *p) { return parse_binary(p, 1); }

/* Parse a standalone expression, used for string interpolation.  The source
 * is padded so line and column numbers still point into the real file. */
Expr *parse_expr_source(const char *src, int line, int col) {
    Buf pad;
    buf_init(&pad);
    for (int i = 1; i < line; i++) buf_putc(&pad, '\n');
    for (int i = 1; i < col; i++)  buf_putc(&pad, ' ');
    buf_puts(&pad, src);

    Source s = { g_source ? g_source->path : "<input>", pad.data };
    int n = 0;
    Token *toks = lex_all(&s, 1, &n);

    P sub = { toks, n, 0, false, false, g_source };
    if (sub.t[0].kind == TK_EOF) {
        err_at(line, col, "there is no expression inside these braces");
        err_help("write `{name}` to insert a value, or `\\{` for a real brace");
        stop_if_errors();
    }
    Expr *e = parse_expr(&sub);
    if (!at(&sub, TK_EOF)) {
        Token *c = cur(&sub);
        err_at(c->line, c->col, "unexpected `%s` inside a text insertion", tok_name(c->kind));
        err_help("each `{ }` holds exactly one expression");
        stop_if_errors();
    }
    return e;
}

/* ---------------- match ---------------- */

/* One arm: `Circle(r) => <statement>` or `_ => { ... }`.  A `,` after an
 * arm is optional, the way it is after the last item of a list. */
static MatchArm *parse_arm(P *p, bool as_value);

static void parse_arms(P *p, Vec *out, bool as_value) {
    expect(p, TK_LBRACE, "`{` to start the arms of the match");
    skip_semis(p);
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        vec_push(out, parse_arm(p, as_value));
        eat(p, TK_COMMA);
        skip_semis(p);
    }
    expect(p, TK_RBRACE, "`}` to close the match");
}

static MatchArm *parse_arm(P *p, bool as_value) {
    MatchArm *a = cx_alloc(sizeof(MatchArm));
    a->line = cur(p)->line;
    a->col = cur(p)->col;

    Token *nm = cur(p);
    expect(p, TK_IDENT, "the name of a value, or `_` for everything else");
    if (strcmp(nm->lex, "_") == 0) a->is_default = true;
    else                           a->variant = nm->lex;

    if (eat(p, TK_LPAREN)) {
        if (a->is_default) {
            err_at(nm->line, nm->col, "`_` stands for every value, so it "
                   "cannot name what one carries");
            stop_if_errors();
        }
        while (!at(p, TK_RPAREN)) {
            Token *bn = cur(p);
            expect(p, TK_IDENT, "a name for what this value carries");
            VarSym *v = cx_alloc(sizeof(VarSym));
            v->name = bn->lex;
            vec_push(&a->binds, v);
            if (!eat(p, TK_COMMA)) break;
        }
        expect(p, TK_RPAREN, "`)` after the names");
    }

    if (!eat(p, TK_FATARROW)) {
        Token *c = cur(p);
        err_at(c->line, c->col, "expected `=>` after the value being matched");
        err_help("an arm reads `%s => <what to do>`", nm->lex);
        stop_if_errors();
    }

    if (as_value) {
        a->value = parse_expr(p);
        return a;
    }
    if (at(p, TK_LBRACE)) {
        parse_block(p, &a->body);
    } else {
        bool saved = p->in_arm;
        p->in_arm = true;
        vec_push(&a->body, parse_stmt(p));
        p->in_arm = saved;
    }
    return a;
}

/* ---------------- statements ---------------- */

static Stmt *parse_let(P *p, bool is_mut) {
    Stmt *s = mkstmt(p, ST_LET);
    s->src = p->src;
    p->i++;                                     /* let / var */
    Token *nm = cur(p);
    expect(p, TK_IDENT, "a name for the variable");
    s->name = nm->lex;
    s->is_mut = is_mut;
    if (eat(p, TK_COLON)) s->decl_type = parse_type(p);
    if (!eat(p, TK_ASSIGN)) {
        Token *c = cur(p);
        err_at(c->line, c->col, "`%s` needs a starting value", nm->lex);
        err_help("write `%s %s = <value>`", is_mut ? "var" : "let", nm->lex);
        stop_if_errors();
    }
    s->rhs = parse_expr(p);
    return s;
}

static bool is_assign_op(TokKind k) {
    return k == TK_ASSIGN || k == TK_PLUSEQ || k == TK_MINUSEQ ||
           k == TK_STAREQ || k == TK_SLASHEQ || k == TK_PERCENTEQ;
}

static Stmt *parse_stmt(P *p) {
    skip_semis(p);
    Token *t = cur(p);

    if (at(p, TK_LET) || at(p, TK_VAR)) {
        Stmt *s = parse_let(p, at(p, TK_VAR));
        expect_semi(p, "declaration");
        return s;
    }

    /* `if let name = maybe { } else why { }` -- the only way into a value
     * that might not be there, so the empty case is never forgotten. */
    if (at(p, TK_MATCH)) {
        Stmt *s = mkstmt(p, ST_MATCH);
        p->i++;
        p->no_struct_lit = true;
        s->rhs = parse_expr(p);
        p->no_struct_lit = false;
        parse_arms(p, &s->arms, false);
        return s;
    }

    if (at(p, TK_IF) && at_next(p, TK_LET)) {
        Stmt *s = mkstmt(p, ST_IFLET);
        p->i += 2;
        Token *nm = cur(p);
        expect(p, TK_IDENT, "a name to give the value");
        s->name = nm->lex;
        expect(p, TK_ASSIGN, "`=` and the value to look inside");
        p->no_struct_lit = true;
        s->rhs = parse_expr(p);
        p->no_struct_lit = false;
        parse_block(p, &s->body);
        if (at(p, TK_ELSE)) {
            p->i++;
            if (at(p, TK_IDENT)) {              /* `else why { }` */
                s->err_name = cur(p)->lex;
                p->i++;
            }
            if (at(p, TK_IF) && !s->err_name) vec_push(&s->els, parse_stmt(p));
            else                              parse_block(p, &s->els);
        }
        return s;
    }

    if (at(p, TK_IF)) {
        Stmt *s = mkstmt(p, ST_IF);
        p->i++;
        p->no_struct_lit = true;
        s->cond = parse_expr(p);
        p->no_struct_lit = false;
        parse_block(p, &s->body);
        if (at(p, TK_ELSE)) {
            p->i++;
            if (at(p, TK_IF)) vec_push(&s->els, parse_stmt(p));
            else              parse_block(p, &s->els);
        }
        return s;
    }

    if (at(p, TK_WHILE)) {
        Stmt *s = mkstmt(p, ST_WHILE);
        p->i++;
        p->no_struct_lit = true;
        s->cond = parse_expr(p);
        p->no_struct_lit = false;
        parse_block(p, &s->body);
        return s;
    }

    if (at(p, TK_FOR)) {
        Stmt *s = mkstmt(p, ST_FORIN);
        p->i++;
        Token *nm = cur(p);
        expect(p, TK_IDENT, "a name for the loop variable");
        s->name = nm->lex;
        if (!eat(p, TK_IN)) {
            Token *c = cur(p);
            err_at(c->line, c->col, "expected `in` after the loop variable");
            err_help("loops read `for %s in 0..10 { }` or `for %s in items { }`",
                     nm->lex, nm->lex);
            stop_if_errors();
        }
        p->no_struct_lit = true;
        Expr *first = parse_expr(p);
        if (at(p, TK_RANGE) || at(p, TK_RANGEEQ)) {
            s->kind = ST_FORRANGE;
            s->inclusive = at(p, TK_RANGEEQ);
            p->i++;
            s->from = first;
            s->to = parse_expr(p);
        } else {
            s->rhs = first;
        }
        p->no_struct_lit = false;
        parse_block(p, &s->body);
        return s;
    }

    if (at(p, TK_RETURN)) {
        Stmt *s = mkstmt(p, ST_RETURN);
        p->i++;
        /* `End => return,` gives nothing back, the same as `return;` */
        bool ends_arm = p->in_arm && (at(p, TK_COMMA) || at(p, TK_RBRACE));
        if (!at(p, TK_SEMI) && !ends_arm) s->rhs = parse_expr(p);
        expect_semi(p, "`return`");
        return s;
    }

    if (at(p, TK_BREAK)) {
        Stmt *s = mkstmt(p, ST_BREAK);
        p->i++;
        expect_semi(p, "`break`");
        return s;
    }
    if (at(p, TK_CONTINUE)) {
        Stmt *s = mkstmt(p, ST_CONTINUE);
        p->i++;
        expect_semi(p, "`continue`");
        return s;
    }

    if (at(p, TK_LBRACE)) {
        Stmt *s = mkstmt(p, ST_BLOCK);
        parse_block(p, &s->body);
        return s;
    }

    if (at(p, TK_FN)) {
        err_at(t->line, t->col, "Cub does not use `fn`");
        err_help("write what the function gives back first, as in "
                 "`int add(a: int, b: int)`, and declare it at the top level");
        stop_if_errors();
    }

    /* expression, possibly the target of an assignment */
    Expr *e = parse_expr(p);
    if (is_assign_op(cur(p)->kind)) {
        Stmt *s = cx_alloc(sizeof(Stmt));
        s->kind = ST_ASSIGN;
        s->line = cur(p)->line;
        s->col = cur(p)->col;
        s->op = cur(p)->kind;
        p->i++;
        s->lhs = e;
        s->rhs = parse_expr(p);
        expect_semi(p, "assignment");
        return s;
    }
    Stmt *s = cx_alloc(sizeof(Stmt));
    s->kind = ST_EXPR;
    s->line = e->line;
    s->col = e->col;
    s->rhs = e;
    expect_semi(p, "statement");
    return s;
}

static void parse_block(P *p, Vec *out) {
    bool saved_arm = p->in_arm;
    p->in_arm = false;
    if (!at(p, TK_LBRACE)) {
        Token *c = cur(p);
        err_at(c->line, c->col, "expected `{` to start a block, but found `%s`", tok_name(c->kind));
        err_help("Cub always wraps the body of an if, loop, or function in braces");
        stop_if_errors();
    }
    p->i++;
    skip_semis(p);
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        vec_push(out, parse_stmt(p));
        skip_semis(p);
    }
    expect(p, TK_RBRACE, "`}` to close the block");
    p->in_arm = saved_arm;
}

/* ---------------- declarations ---------------- */

/* `int add(a: int, b: int) { ... }` -- the return type comes first, and
 * `void` means it gives nothing back, exactly as in C. */
static FnDecl *parse_fn(P *p, bool is_static) {
    FnDecl *f = cx_alloc(sizeof(FnDecl));
    f->src = p->src;
    f->doc = cur(p)->doc;
    f->line = cur(p)->line;
    f->col = cur(p)->col;
    f->is_static = is_static;

    if (at(p, TK_FN)) {
        Token *c = cur(p);
        err_at(c->line, c->col, "Cub does not use `fn`");
        err_help("write what the function gives back first: "
                 "`int add(a: int, b: int)`, or `void` when it gives nothing");
        stop_if_errors();
    }

    f->ret = parse_type(p);

    Token *nm = cur(p);
    expect(p, TK_IDENT, "a name for the function");
    f->name = nm->lex;

    /* `T first<T>(items: [T])` -- names that stand in for any type.  They
     * go after the function name, where nothing else could start with `<`. */
    if (eat(p, TK_LT)) {
        while (!at(p, TK_GT) && !at(p, TK_EOF)) {
            Token *tn = cur(p);
            expect(p, TK_IDENT, "a name to stand in for a type");
            for (int i = 0; i < f->tparams.len; i++)
                if (strcmp((char *)f->tparams.items[i], tn->lex) == 0) {
                    err_at(tn->line, tn->col, "`%s` is named twice", tn->lex);
                    stop_if_errors();
                }
            vec_push(&f->tparams, tn->lex);
            if (!eat(p, TK_COMMA)) break;
        }
        expect(p, TK_GT, "`>` after the type names");
        if (f->tparams.len == 0) {
            err_at(nm->line, nm->col, "`%s` names no types between `<` and `>`",
                   f->name);
            err_help("write `%s<T>(...)`, or drop the `< >`", f->name);
            stop_if_errors();
        }
    }

    expect(p, TK_LPAREN, "`(` to start the parameter list");
    while (!at(p, TK_RPAREN)) {
        Token *pn = cur(p);
        expect(p, TK_IDENT, "a parameter name");
        if (!eat(p, TK_COLON)) {
            Token *c = cur(p);
            err_at(c->line, c->col, "parameter `%s` needs a type", pn->lex);
            err_help("write `%s: int`, `%s: string`, and so on", pn->lex, pn->lex);
            stop_if_errors();
        }
        VarSym *v = cx_alloc(sizeof(VarSym));
        v->name = pn->lex;
        v->type = parse_type(p);
        v->is_mut = false;
        vec_push(&f->params, v);
        if (!eat(p, TK_COMMA)) break;
    }
    expect(p, TK_RPAREN, "`)` to close the parameter list");

    if (at(p, TK_ARROW) || at(p, TK_COLON)) {
        Token *c = cur(p);
        err_at(c->line, c->col, "the return type goes before the name, not after");
        err_help("write `%s %s(...)` instead", ty_show(f->ret), f->name);
        stop_if_errors();
    }

    parse_block(p, &f->body);
    return f;
}

/* class Dog extends Animal {
 *     name: string;
 *     string speak() { ... }
 * }
 */
static void parse_class(P *p, Program *prog) {
    ClassDef *cd = cx_alloc(sizeof(ClassDef));
    cd->src = p->src;
    cd->doc = cur(p)->doc;
    cd->line = cur(p)->line;
    cd->col = cur(p)->col;
    p->i++;                                     /* class */

    Token *nm = cur(p);
    expect(p, TK_IDENT, "a name for the class");
    cd->name = nm->lex;

    if (at(p, TK_COLON)) {
        Token *c = cur(p);
        const char *base = at_next(p, TK_IDENT) ? p->t[p->i + 1].lex : "<name>";
        err_at(c->line, c->col, "a class names the one it builds on with `extends`");
        err_help("write `class %s extends %s { ... }`", cd->name, base);
        stop_if_errors();
    }
    if (at(p, TK_LT)) {
        Token *c = cur(p);
        err_at(c->line, c->col, "a class cannot name the types it works for yet");
        err_help("a `struct` can: write `struct %s<T> { ... }`, or hold the "
                 "type you need", cd->name);
        stop_if_errors();
    }
    if (eat(p, TK_EXTENDS)) {
        Token *bn = cur(p);
        expect(p, TK_IDENT, "the name of the class to build on");
        cd->base_name = bn->lex;
    }

    expect(p, TK_LBRACE, "`{` to start the class body");
    skip_semis(p);
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        /* `name: type;` is a field; a member that starts with `fn` is a
         * method.  Nothing else belongs in a class body. */
        if (at(p, TK_IDENT) && at_next(p, TK_COLON)) {
            Token *f = cur(p);
            p->i += 2;
            vec_push(&cd->fnames, f->lex);
            vec_push(&cd->fdocs, f->doc);
            vec_push(&cd->ftypes, parse_type(p));
            expect_semi(p, "field");
            skip_semis(p);
            continue;
        }

        char *member_doc = cur(p)->doc;
        bool is_static = eat(p, TK_STATIC);
        if (!starts_type(p) && !at(p, TK_FN)) {
            Token *c = cur(p);
            err_at(c->line, c->col, "expected a field or a method, but found `%s`",
                   tok_name(c->kind));
            err_help("a class holds `name: type;` fields and methods "
                     "written `float area()` or `void run()`");
            stop_if_errors();
        }
        FnDecl *m = parse_fn(p, is_static);
        if (!m->doc) m->doc = member_doc;
        m->is_init = strcmp(m->name, "init") == 0;
        if (m->is_init) {
            if (is_static) {
                err_at(m->line, m->col, "`init` sets up one object, so it is never static");
                err_help("drop the `static`");
            }
            if (cd->init)
                err_at(m->line, m->col, "`%s` already has an `init`", cd->name);
            cd->init = m;
        }
        if (is_static) vec_push(&cd->statics, m);
        else           vec_push(&cd->methods, m);
        skip_semis(p);
    }
    expect(p, TK_RBRACE, "`}` to close the class body");
    vec_push(&prog->classes, cd);
}

/* `struct Point { x: int; y: int; }` -- a plain bag of named values. */
static void parse_struct_decl(P *p, Program *prog) {
    StructDef *sd = cx_alloc(sizeof(StructDef));
    sd->src = p->src;
    sd->doc = cur(p)->doc;
    sd->line = cur(p)->line;
    sd->col = cur(p)->col;
    p->i++;                                     /* struct */

    Token *nm = cur(p);
    expect(p, TK_IDENT, "a name for the struct");
    sd->name = nm->lex;
    sd->show = nm->lex;
    sd->print_name = nm->lex;

    /* `struct Pair<A, B>` -- names standing in for whatever it is made with */
    if (eat(p, TK_LT)) {
        while (!at(p, TK_GT) && !at(p, TK_EOF)) {
            Token *tn = cur(p);
            expect(p, TK_IDENT, "a name to stand in for a type");
            vec_push(&sd->tparams, tn->lex);
            if (!eat(p, TK_COMMA)) break;
        }
        expect(p, TK_GT, "`>` after the type names");
        if (sd->tparams.len == 0) {
            err_at(nm->line, nm->col, "`%s` names no types between `<` and `>`",
                   sd->name);
            stop_if_errors();
        }
    }

    expect(p, TK_LBRACE, "`{` to start the field list");
    skip_semis(p);
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        Token *fn = cur(p);
        expect(p, TK_IDENT, "a field name");
        if (!eat(p, TK_COLON)) {
            Token *c = cur(p);
            err_at(c->line, c->col, "field `%s` needs a type", fn->lex);
            err_help("write `%s: int;`, `%s: string;`, and so on", fn->lex, fn->lex);
            stop_if_errors();
        }
        vec_push(&sd->fnames, fn->lex);
        vec_push(&sd->fdocs, fn->doc);
        vec_push(&sd->ftypes, parse_type(p));
        expect_semi(p, "field");
        skip_semis(p);
    }
    expect(p, TK_RBRACE, "`}` to close the field list");
    /* a generic one becomes nothing on its own; the C comes from what it
     * is made into */
    if (sd->tparams.len) vec_push(&prog->templates, sd);
    else                 vec_push(&prog->structs, sd);
}

/* `enum Colour { Red, Green, Blue }` -- a fixed list of named values. */
static void parse_enum_decl(P *p, Program *prog) {
    EnumDef *ed = cx_alloc(sizeof(EnumDef));
    ed->src = p->src;
    ed->doc = cur(p)->doc;
    ed->line = cur(p)->line;
    ed->col = cur(p)->col;
    p->i++;                                     /* enum */

    Token *nm = cur(p);
    expect(p, TK_IDENT, "a name for the enum");
    ed->name = nm->lex;

    expect(p, TK_LBRACE, "`{` to start the value list");
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        Token *vn = cur(p);
        expect(p, TK_IDENT, "a value name");
        vec_push(&ed->vals, vn->lex);

        /* `Circle(radius: float)` -- what this value carries */
        Vec *fields = cx_alloc(sizeof(Vec));
        if (eat(p, TK_LPAREN)) {
            while (!at(p, TK_RPAREN)) {
                Token *fn = cur(p);
                expect(p, TK_IDENT, "a name for what this value carries");
                if (!eat(p, TK_COLON)) {
                    Token *c = cur(p);
                    err_at(c->line, c->col, "`%s` needs a type", fn->lex);
                    err_help("write `%s(%s: int)`", vn->lex, fn->lex);
                    stop_if_errors();
                }
                VarSym *v = cx_alloc(sizeof(VarSym));
                v->name = fn->lex;
                v->type = parse_type(p);
                vec_push(fields, v);
                if (!eat(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN, "`)` after what the value carries");
            if (fields->len == 0) {
                err_at(vn->line, vn->col, "`%s` carries nothing, so it needs no `( )`",
                       vn->lex);
                stop_if_errors();
            }
            ed->tagged = true;
        }
        vec_push(&ed->vfields, fields);
        if (!eat(p, TK_COMMA)) break;
    }
    expect(p, TK_RBRACE, "`}` to close the value list");
    vec_push(&prog->enums, ed);
}

/* Cub used to spell these `type Point = struct { ... }`. */
static void parse_old_type_decl(P *p) {
    Token *kw = cur(p);
    p->i++;                                     /* type */
    const char *name = at(p, TK_IDENT) ? cur(p)->lex : "Name";
    const char *what = "struct";
    for (int j = p->i; j < p->n && j < p->i + 3; j++) {
        if (p->t[j].kind == TK_ENUM) { what = "enum"; break; }
    }
    err_at(kw->line, kw->col, "Cub declares a type with `%s`, not `type`", what);
    if (strcmp(what, "enum") == 0)
        err_help("write `enum %s { Red, Green, Blue }`", name);
    else
        err_help("write `struct %s { x: int; y: int; }`", name);
    stop_if_errors();
}

/* extern "math.h" {
 *     float pow(base: float, exponent: float);
 *     int getpid() = "getpid";      // when the C name differs
 * }
 *
 * A declaration with no body: Cub writes the call, the C library does the
 * work.  The header, if given, is included in the generated C.
 */
static void parse_extern(P *p, Program *prog) {
    Token *kw = cur(p);
    p->i++;                                     /* extern */
    char *header = NULL;

    /* The header is what makes this safe: with the real declaration in
     * scope, the C compiler converts the arguments and the result.  Cub
     * has one integer type and C has several, so a guessed declaration
     * would be wrong in ways nothing could catch. */
    if (!at(p, TK_STRLIT)) {
        err_at(kw->line, kw->col, "`extern` needs the header that declares "
               "these functions");
        err_help("write `extern \"math.h\" { ... }`; Cub cannot guess what "
                 "the C library says they look like");
        stop_if_errors();
    }
    {
        Token *h = cur(p);
        p->i++;
        if (h->parts.len != 1 || !((StrPart *)h->parts.items[0])->text) {
            err_at(h->line, h->col, "a header name cannot have `{ }` in it");
            stop_if_errors();
        }
        char *name = ((StrPart *)h->parts.items[0])->text;
        header = name;
        bool seen = false;
        for (int i = 0; i < prog->headers.len; i++)
            if (strcmp((char *)prog->headers.items[i], name) == 0) seen = true;
        if (!seen) vec_push(&prog->headers, name);
    }

    expect(p, TK_LBRACE, "`{` to start the list of C functions");
    skip_semis(p);
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        FnDecl *f = cx_alloc(sizeof(FnDecl));
        f->src = p->src;
        f->doc = cur(p)->doc;
        f->line = cur(p)->line;
        f->col = cur(p)->col;
        f->is_extern = true;
        f->header = header;

        f->ret = parse_type(p);
        Token *nm = cur(p);
        expect(p, TK_IDENT, "a name for the C function");
        f->name = nm->lex;
        f->c_name = nm->lex;

        expect(p, TK_LPAREN, "`(` to start the parameter list");
        while (!at(p, TK_RPAREN)) {
            Token *pn = cur(p);
            expect(p, TK_IDENT, "a parameter name");
            if (!eat(p, TK_COLON)) {
                Token *c = cur(p);
                err_at(c->line, c->col, "parameter `%s` needs a type", pn->lex);
                stop_if_errors();
            }
            VarSym *v = cx_alloc(sizeof(VarSym));
            v->name = pn->lex;
            v->type = parse_type(p);
            vec_push(&f->params, v);
            if (!eat(p, TK_COMMA)) break;
        }
        expect(p, TK_RPAREN, "`)` to close the parameter list");

        if (eat(p, TK_ASSIGN)) {                /* = "the real C name" */
            Token *cn = cur(p);
            expect(p, TK_STRLIT, "the name the C library uses, in quotes");
            if (cn->parts.len != 1 || !((StrPart *)cn->parts.items[0])->text) {
                err_at(cn->line, cn->col, "a C name cannot have `{ }` in it");
                stop_if_errors();
            }
            f->c_name = ((StrPart *)cn->parts.items[0])->text;
        }
        expect_semi(p, "declaration");
        skip_semis(p);
        vec_push(&prog->externs, f);
    }
    expect(p, TK_RBRACE, "`}` to close the list of C functions");
}

/* `link "sqlite3";` puts -lsqlite3 on the C compiler's command line. */
static void parse_link(P *p, Program *prog) {
    p->i++;                                     /* link */
    Token *t = cur(p);
    expect(p, TK_STRLIT, "the name of a library, in quotes");
    if (t->parts.len != 1 || !((StrPart *)t->parts.items[0])->text) {
        err_at(t->line, t->col, "a library name cannot have `{ }` in it");
        stop_if_errors();
    }
    expect_semi(p, "declaration");
    char *name = ((StrPart *)t->parts.items[0])->text;
    for (int i = 0; i < prog->links.len; i++)
        if (strcmp((char *)prog->links.items[i], name) == 0) return;
    vec_push(&prog->links, name);
}

static void parse_into(Program *prog, Token *toks, int ntoks, Source *src);

/* Everything after the last slash is the file; what precedes it is where
 * a relative import is resolved from. */
static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? cx_strndup(path, (size_t)(slash - path + 1)) : cx_strdup("");
}

static char *read_file_or_null(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = cx_alloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    return buf;
}

/* `import os;` names a module; `import "utils.cb";` pulls in a file. */
static void parse_import(P *p, Program *prog) {
    int line = cur(p)->line, col = cur(p)->col;
    p->i++;                                     /* import */

    if (at(p, TK_IDENT)) {
        Token *m = cur(p);
        p->i++;
        expect_semi(p, "import");
        for (int i = 0; i < prog->modules.len; i++)
            if (strcmp((char *)prog->modules.items[i], m->lex) == 0) return;
        vec_push(&prog->modules, m->lex);
        return;
    }

    if (!at(p, TK_STRLIT)) {
        Token *c = cur(p);
        err_at(c->line, c->col, "expected a module name or a file after `import`");
        err_help("write `import os;` for a built-in module, "
                 "or `import \"helpers.cb\";` for one of your files");
        stop_if_errors();
    }

    Token *t = cur(p);
    p->i++;
    expect_semi(p, "import");
    if (t->parts.len != 1 || !((StrPart *)t->parts.items[0])->text) {
        err_at(t->line, t->col, "a file name cannot have `{ }` in it");
        stop_if_errors();
    }
    char *rel = ((StrPart *)t->parts.items[0])->text;

    char *path = rel[0] == '/' ? cx_strdup(rel)
                               : cx_fmt("%s%s", dir_of(p->src->path), rel);

    for (int i = 0; i < prog->files.len; i++)
        if (strcmp(((Source *)prog->files.items[i])->path, path) == 0)
            return;                              /* already pulled in */

    char *text = read_file_or_null(path);
    if (!text) {
        err_at(line, col, "cannot find the file `%s`", rel);
        err_help("the path is taken relative to `%s`", p->src->path);
        stop_if_errors();
    }

    Source *sub = cx_alloc(sizeof(Source));
    sub->path = path;
    sub->text = text;
    vec_push(&prog->files, sub);

    Source *saved = g_source;
    g_source = sub;
    int n = 0;
    Token *sub_toks = lex_all(sub, 1, &n);
    parse_into(prog, sub_toks, n, sub);
    g_source = saved;
}

static void parse_into(Program *prog, Token *toks, int ntoks, Source *src) {
    P p = { toks, ntoks, 0, false, false, src };

    skip_semis(&p);
    while (!at(&p, TK_EOF)) {
        if (at(&p, TK_IMPORT)) { parse_import(&p, prog); skip_semis(&p); continue; }
        if (at(&p, TK_EXTERN))        parse_extern(&p, prog);
        else if (at(&p, TK_LINK))     parse_link(&p, prog);
        else if (at(&p, TK_CLASS))    parse_class(&p, prog);
        else if (at(&p, TK_STRUCT))   parse_struct_decl(&p, prog);
        else if (at(&p, TK_ENUM))     parse_enum_decl(&p, prog);
        else if (at(&p, TK_TYPE))     parse_old_type_decl(&p);
        else if (at(&p, TK_LET) || at(&p, TK_VAR)) {
            Stmt *g = parse_let(&p, at(&p, TK_VAR));
            expect_semi(&p, "declaration");
            vec_push(&prog->globals, g);
        }
        else if (at(&p, TK_FN) || starts_type(&p))
            vec_push(&prog->fns, parse_fn(&p, false));
        else {
            Token *c = cur(&p);
            if (at(&p, TK_STATIC)) {
                err_at(c->line, c->col, "only a class can hold a `static` function");
                err_help("drop the `static`, or move the function into a class");
            } else {
                err_at(c->line, c->col, "expected a declaration, but found `%s`",
                       tok_name(c->kind));
                err_help("the top level of a file holds functions, `class`, "
                         "`struct`, `enum`, `let`, and `var` declarations; "
                         "runnable code goes inside `void main()`");
            }
            stop_if_errors();
        }
        skip_semis(&p);
    }
}

Program *parse_program(Token *toks, int ntoks) {
    Program *prog = cx_alloc(sizeof(Program));
    vec_push(&prog->files, g_source);
    parse_into(prog, toks, ntoks, g_source);
    stop_if_errors();
    return prog;
}
