/* parser.c -- recursive descent with precedence climbing.
 *
 * Cub has no statement terminators.  A statement ends where its expression
 * runs out, and an expression stops at a newline unless we are inside
 * brackets or the line ended on an operator:
 *
 *     let a = 1 + 2      // ends here
 *     let b = 1 +        // continues: the line ended on `+`
 *             2
 *
 * Semicolons are accepted and ignored, so nobody is ever punished for a
 * habit carried over from C.
 */
#include "cub.h"

typedef struct {
    Token  *t;
    int     n, i;
    int     depth;          /* > 0 while inside ( ) or [ ]        */
    bool    no_struct_lit;  /* while parsing if/while/for headers */
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

/* Does the current token continue the expression on the previous line? */
static bool continues(P *p) { return p->depth > 0 || !cur(p)->nl_before; }

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

static Type *parse_type(P *p) {
    if (eat(p, TK_VOID)) return ty_void();
    if (eat(p, TK_LBRACK)) {
        Type *el = parse_type(p);
        if (eat(p, TK_COLON)) {                 /* [key: value] is a map */
            Type *val = parse_type(p);
            expect(p, TK_RBRACK, "`]` to close the map type");
            return ty_map(el, val);
        }
        expect(p, TK_RBRACK, "`]` to close the array type");
        return ty_array(el);
    }
    Token *c = cur(p);
    if (!at(p, TK_IDENT)) {
        err_at(c->line, c->col, "expected a type name, but found `%s`", tok_name(c->kind));
        err_help("types are int, float, bool, string, [T], or a type you declared");
        stop_if_errors();
    }
    p->i++;
    const char *n = c->lex;
    if (strcmp(n, "int") == 0)    return ty_int();
    if (strcmp(n, "float") == 0)  return ty_float();
    if (strcmp(n, "bool") == 0)   return ty_bool();
    if (strcmp(n, "string") == 0) return ty_str();
    /* A user type; the checker decides later whether it is a struct or
     * an enum and swaps in the canonical Type. */
    return ty_named(TY_STRUCT, c->lex);
}

/* ---------------- expressions ---------------- */

static Expr *parse_expr(P *p);
static void  parse_block(P *p, Vec *out);
static FnDecl *parse_fn(P *p, bool is_static);

/* A declaration leads with its type, so this is what starts one. */
static bool starts_type(P *p) {
    return at(p, TK_VOID) || at(p, TK_LBRACK) || at(p, TK_IDENT);
}

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

    int saved_depth = p->depth;
    expect(p, TK_LBRACE, "`{` and one value for the `if` branch");
    p->depth++;
    e->b = parse_expr(p);
    expect(p, TK_RBRACE, "`}` after the value");
    p->depth = saved_depth;

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
    p->depth++;
    vec_push(&e->args, parse_expr(p));
    expect(p, TK_RBRACE, "`}` after the value");
    p->depth = saved_depth;
    return e;
}

static Expr *parse_primary(P *p) {
    Token *t = cur(p);

    if (at(p, TK_IF)) return parse_if_expr(p);

    if (eat(p, TK_SELF))  { Expr *e = mkexpr(p, EX_SELF);  return e; }
    if (eat(p, TK_SUPER)) { Expr *e = mkexpr(p, EX_SUPER); return e; }

    if (eat(p, TK_INTLIT))   { Expr *e = mkexpr(p, EX_INT);   e->ival = t->ival; return e; }
    if (eat(p, TK_FLOATLIT)) { Expr *e = mkexpr(p, EX_FLOAT); e->fval = t->fval; return e; }
    if (eat(p, TK_TRUE))     { Expr *e = mkexpr(p, EX_BOOL);  e->bval = true;    return e; }
    if (eat(p, TK_FALSE))    { Expr *e = mkexpr(p, EX_BOOL);  e->bval = false;   return e; }
    if (eat(p, TK_STRLIT))   { return parse_string(p, t); }

    if (eat(p, TK_LPAREN)) {
        p->depth++;
        bool saved = p->no_struct_lit;
        p->no_struct_lit = false;
        Expr *e = parse_expr(p);
        p->no_struct_lit = saved;
        expect(p, TK_RPAREN, "`)` to close the group");
        p->depth--;
        return e;
    }

    if (eat(p, TK_LBRACK)) {                  /* array or map literal */
        p->depth++;
        bool saved = p->no_struct_lit;
        p->no_struct_lit = false;
        Expr *e = mkexpr(p, EX_ARRAYLIT);
        e->line = t->line; e->col = t->col;

        if (eat(p, TK_COLON)) {                /* [:] -- an empty map */
            e->kind = EX_MAPLIT;
            p->no_struct_lit = saved;
            expect(p, TK_RBRACK, "`]` to close the map");
            p->depth--;
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
        p->depth--;
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
        if (!continues(p)) break;

        if (at(p, TK_LPAREN)) {
            p->i++;
            p->depth++;
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
            p->depth--;
            e = call;
            continue;
        }

        if (at(p, TK_LBRACK)) {
            p->i++;
            p->depth++;
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
            p->depth--;
            e = ix;
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
        if (!continues(p)) break;
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

    P sub = { toks, n, 0, 1, false, g_source };
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

    if (at(p, TK_LET)) return parse_let(p, false);
    if (at(p, TK_VAR)) return parse_let(p, true);

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
        if (!at(p, TK_RBRACE) && !at(p, TK_SEMI) && !at(p, TK_EOF) && !cur(p)->nl_before)
            s->rhs = parse_expr(p);
        return s;
    }

    if (at(p, TK_BREAK))    { Stmt *s = mkstmt(p, ST_BREAK);    p->i++; return s; }
    if (at(p, TK_CONTINUE)) { Stmt *s = mkstmt(p, ST_CONTINUE); p->i++; return s; }

    if (at(p, TK_LBRACE)) {
        Stmt *s = mkstmt(p, ST_BLOCK);
        parse_block(p, &s->body);
        return s;
    }

    if (at(p, TK_FN)) {
        err_at(t->line, t->col, "Cub does not use `fn`");
        err_help("write what the function gives back first, as in "
                 "`int add(a: int, b: int)`");
        stop_if_errors();
    }

    /* expression, possibly the target of an assignment */
    Expr *e = parse_expr(p);
    if (is_assign_op(cur(p)->kind) && continues(p)) {
        Stmt *s = cx_alloc(sizeof(Stmt));
        s->kind = ST_ASSIGN;
        s->line = cur(p)->line;
        s->col = cur(p)->col;
        s->op = cur(p)->kind;
        p->i++;
        s->lhs = e;
        s->rhs = parse_expr(p);
        return s;
    }
    Stmt *s = cx_alloc(sizeof(Stmt));
    s->kind = ST_EXPR;
    s->line = e->line;
    s->col = e->col;
    s->rhs = e;
    return s;
}

static void parse_block(P *p, Vec *out) {
    if (!at(p, TK_LBRACE)) {
        Token *c = cur(p);
        err_at(c->line, c->col, "expected `{` to start a block, but found `%s`", tok_name(c->kind));
        err_help("Cub always wraps the body of an if, loop, or function in braces");
        stop_if_errors();
    }
    p->i++;
    int saved_depth = p->depth;
    p->depth = 0;                 /* newlines matter again inside a block */
    skip_semis(p);
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        vec_push(out, parse_stmt(p));
        skip_semis(p);
    }
    expect(p, TK_RBRACE, "`}` to close the block");
    p->depth = saved_depth;
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

    expect(p, TK_LPAREN, "`(` to start the parameter list");
    p->depth++;
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
    p->depth--;

    if (at(p, TK_ARROW)) {
        Token *c = cur(p);
        err_at(c->line, c->col, "the return type goes before the name, not after");
        err_help("write `%s %s(...)` instead of `%s(...) -> %s`",
                 ty_show(f->ret), f->name, f->name, ty_show(f->ret));
        stop_if_errors();
    }
    parse_block(p, &f->body);
    return f;
}

/* class Name : Base {
 *     field: type
 *     fn method(...) -> type { ... }
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

    if (eat(p, TK_COLON)) {
        Token *bn = cur(p);
        expect(p, TK_IDENT, "the name of the class to build on");
        cd->base_name = bn->lex;
    }

    expect(p, TK_LBRACE, "`{` to start the class body");
    skip_semis(p);
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        /* `name: type` is a field; anything else leads with a type and is
         * therefore a method. */
        if (at(p, TK_IDENT) && at_next(p, TK_COLON)) {
            Token *f = cur(p);
            p->i += 2;
            vec_push(&cd->fnames, f->lex);
            vec_push(&cd->fdocs, f->doc);
            vec_push(&cd->ftypes, parse_type(p));
            eat(p, TK_COMMA);
            skip_semis(p);
            continue;
        }

        char *member_doc = cur(p)->doc;
        bool is_static = eat(p, TK_STATIC);
        if (!starts_type(p) && !at(p, TK_FN)) {
            Token *c = cur(p);
            err_at(c->line, c->col, "expected a field or a method, but found `%s`",
                   tok_name(c->kind));
            err_help("a class holds `name: type` fields and methods "
                     "written `int area()` or `void run()`");
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

static void parse_type_decl(P *p, Program *prog) {
    int line = cur(p)->line, col = cur(p)->col;
    char *doc = cur(p)->doc;
    p->i++;                                     /* type */
    Token *nm = cur(p);
    expect(p, TK_IDENT, "a name for the type");
    expect(p, TK_ASSIGN, "`=` after the type name");

    if (eat(p, TK_STRUCT)) {
        StructDef *sd = cx_alloc(sizeof(StructDef));
        sd->src = p->src;
        sd->doc = doc;
        sd->name = nm->lex;
        sd->line = line; sd->col = col;
        expect(p, TK_LBRACE, "`{` to start the field list");
        while (!at(p, TK_RBRACE)) {
            Token *fn = cur(p);
            expect(p, TK_IDENT, "a field name");
            if (!eat(p, TK_COLON)) {
                Token *c = cur(p);
                err_at(c->line, c->col, "field `%s` needs a type", fn->lex);
                err_help("write `%s: int`, `%s: string`, and so on", fn->lex, fn->lex);
                stop_if_errors();
            }
            vec_push(&sd->fnames, fn->lex);
            vec_push(&sd->fdocs, fn->doc);
            vec_push(&sd->ftypes, parse_type(p));
            if (!eat(p, TK_COMMA)) break;
        }
        expect(p, TK_RBRACE, "`}` to close the field list");
        vec_push(&prog->structs, sd);
        return;
    }

    if (eat(p, TK_ENUM)) {
        EnumDef *ed = cx_alloc(sizeof(EnumDef));
        ed->src = p->src;
        ed->doc = doc;
        ed->name = nm->lex;
        ed->line = line; ed->col = col;
        expect(p, TK_LBRACE, "`{` to start the value list");
        while (!at(p, TK_RBRACE)) {
            Token *vn = cur(p);
            expect(p, TK_IDENT, "a value name");
            vec_push(&ed->vals, vn->lex);
            if (!eat(p, TK_COMMA)) break;
        }
        expect(p, TK_RBRACE, "`}` to close the value list");
        vec_push(&prog->enums, ed);
        return;
    }

    Token *c = cur(p);
    err_at(c->line, c->col, "expected `struct` or `enum` after `=`");
    err_help("write `type %s = struct { ... }` or `type %s = enum { ... }`",
             nm->lex, nm->lex);
    stop_if_errors();
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

/* `import os` names a module; `import "utils.cub"` pulls in a file. */
static void parse_import(P *p, Program *prog) {
    int line = cur(p)->line, col = cur(p)->col;
    p->i++;                                     /* import */

    if (at(p, TK_IDENT)) {
        Token *m = cur(p);
        p->i++;
        for (int i = 0; i < prog->modules.len; i++)
            if (strcmp((char *)prog->modules.items[i], m->lex) == 0) return;
        vec_push(&prog->modules, m->lex);
        return;
    }

    if (!at(p, TK_STRLIT)) {
        Token *c = cur(p);
        err_at(c->line, c->col, "expected a module name or a file after `import`");
        err_help("write `import os` for a built-in module, "
                 "or `import \"helpers.cub\"` for one of your files");
        stop_if_errors();
    }

    Token *t = cur(p);
    p->i++;
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
    P p = { toks, ntoks, 0, 0, false, src };

    skip_semis(&p);
    while (!at(&p, TK_EOF)) {
        if (at(&p, TK_IMPORT)) { parse_import(&p, prog); skip_semis(&p); continue; }
        if (at(&p, TK_CLASS))      parse_class(&p, prog);
        else if (at(&p, TK_TYPE))  parse_type_decl(&p, prog);
        else if (at(&p, TK_LET))   vec_push(&prog->globals, parse_let(&p, false));
        else if (at(&p, TK_VAR))   vec_push(&prog->globals, parse_let(&p, true));
        else if (at(&p, TK_FN) || starts_type(&p))
            vec_push(&prog->fns, parse_fn(&p, false));
        else {
            Token *c = cur(&p);
            err_at(c->line, c->col, "expected a declaration, but found `%s`", tok_name(c->kind));
            err_help("the top level of a file holds functions, `class`, `type`, "
                     "`let`, and `var` declarations; runnable code goes inside "
                     "`void main()`");
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
