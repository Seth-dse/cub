/* format.c -- `cubc fmt`, the canonical formatter.
 *
 * It rewrites the horizontal whitespace -- indentation, spacing around
 * operators, spacing inside brackets -- and leaves every line break where
 * the author put it.  Semicolons mean it could safely rewrap code, but a
 * formatter that only tidies is one you can run on anything without
 * arguing with it, so this one tidies what you wrote.
 *
 * It works on the token stream rather than the syntax tree, which keeps
 * comments and the exact spelling of literals (0xff, 1_000, "a {b} c")
 * intact.  Afterwards it re-lexes its own output and checks that the token
 * stream is unchanged, so a formatting bug cannot silently damage a file.
 */
#include "cub.h"

#define INDENT "    "

static bool is_open(TokKind k)  { return k == TK_LPAREN || k == TK_LBRACK; }
static bool is_close(TokKind k) { return k == TK_RPAREN || k == TK_RBRACK; }

static bool is_binop(TokKind k) {
    switch (k) {
    case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH: case TK_PERCENT:
    case TK_EQ: case TK_NE: case TK_LT: case TK_LE: case TK_GT: case TK_GE:
    case TK_ANDAND: case TK_OROR: case TK_AND: case TK_OR:
        return true;
    default:
        return false;
    }
}

static bool is_assign(TokKind k) {
    switch (k) {
    case TK_ASSIGN: case TK_PLUSEQ: case TK_MINUSEQ:
    case TK_STAREQ: case TK_SLASHEQ: case TK_PERCENTEQ:
        return true;
    default:
        return false;
    }
}

/* Could a `-` or `!` right after this token be a prefix rather than an
 * operator?  After a value it is arithmetic; after anything else it is not. */
static bool prefix_position(const Token *prev) {
    if (!prev) return true;
    switch (prev->kind) {
    case TK_IDENT: case TK_INTLIT: case TK_FLOATLIT: case TK_STRLIT:
    case TK_RPAREN: case TK_RBRACK: case TK_TRUE: case TK_FALSE:
        return false;
    default:
        return true;
    }
}

/* A line that continues the previous one gets one extra level of indent.
 * That is true whether the operator was left at the end of the line above
 * or carried down to the start of this one:
 *
 *     let total = first
 *         + second;
 */
static bool continues_line(const Token *prev) {
    return prev && (is_binop(prev->kind) || is_assign(prev->kind));
}

static bool continued_line(const Token *cur) {
    return is_binop(cur->kind) || is_assign(cur->kind) || cur->kind == TK_DOT;
}

static bool needs_space(const Token *prev, const Token *cur, bool prev_was_prefix) {
    if (!prev) return false;
    if (prev->kind == TK_LBRACE && cur->kind == TK_RBRACE) return false;  /* {} */
    if (prev_was_prefix) return false;                                   /* -x, !x */
    if (is_open(prev->kind)) return false;                               /* (x, [x */
    if (is_close(cur->kind)) return false;                               /* x), x] */
    if (cur->kind == TK_COMMA || cur->kind == TK_SEMI || cur->kind == TK_COLON)
        return false;
    if (prev->kind == TK_DOT || cur->kind == TK_DOT) return false;        /* a.b   */
    if (prev->kind == TK_RANGE || prev->kind == TK_RANGEEQ) return false; /* 0..10 */
    if (cur->kind == TK_RANGE || cur->kind == TK_RANGEEQ) return false;

    /* `int?`, `int!`, and `parse(x)!` all attach to what comes before them;
     * a leading `!` is the other thing, and prev_was_prefix has that. */
    if (cur->kind == TK_QUESTION) return false;
    if (cur->kind == TK_BANG && !prefix_position(prev)) return false;

    /* a call or an index binds tight; a keyword does not */
    if (cur->kind == TK_LPAREN || cur->kind == TK_LBRACK) {
        switch (prev->kind) {
        case TK_IDENT: case TK_RPAREN: case TK_RBRACK: return false;
        default: return true;
        }
    }
    return true;
}

static int count_newlines(const char *s, int len) {
    int n = 0;
    for (int i = 0; i < len; i++) if (s[i] == '\n') n++;
    return n;
}

static bool is_opener(TokKind k) { return k == TK_LBRACE || is_open(k); }
static bool is_closer(TokKind k) { return k == TK_RBRACE || is_close(k); }

/* Indentation follows one rule: a line sits one level deeper than the line
 * that opened the bracket it is inside.  A line beginning with a closing
 * bracket lines up with the line that opened it instead.  So several
 * brackets opening together still cost one level:
 *
 *     print(total(
 *         1,
 *     ))
 */
static char *render(Token *toks, int n) {
    Buf out;
    buf_init(&out);

    enum { MAX_NEST = 256 };
    int open_level[MAX_NEST];      /* indent of the line each bracket opened on */
    int sp = 0;

    int cur_level = 0;
    int at_line = toks[0].line;
    Token *prev = NULL;
    bool prev_prefix = false;
    bool carried_on = false;       /* this line continues the one above */

    for (int i = 0; i < n && toks[i].kind != TK_EOF; i++) {
        Token *t = &toks[i];

        if (!prev) {
            /* first token in the file, always at column zero */
        } else if (t->line > at_line) {
            buf_putc(&out, '\n');
            if (t->line - at_line > 1) buf_putc(&out, '\n');   /* keep one blank line */

            if (sp > 0 && is_closer(t->kind))      cur_level = open_level[sp - 1];
            else if (sp > 0)                       cur_level = open_level[sp - 1] + 1;
            else                                   cur_level = 0;
            carried_on = !is_closer(t->kind) &&
                         (continues_line(prev) || continued_line(t));
            if (carried_on) cur_level++;

            for (int k = 0; k < cur_level; k++) buf_puts(&out, INDENT);
        } else if (t->kind == TK_COMMENT || needs_space(prev, t, prev_prefix)) {
            buf_putc(&out, ' ');
        }

        /* the token's own text, exactly as written */
        for (int k = 0; k < t->raw_len; k++) buf_putc(&out, t->raw[k]);

        /* A block opened at the end of a header that ran over several lines
         * belongs to the statement, not to the continuation, so its body
         * lines up with the statement:
         *
         *     if a < b
         *         and c < d {
         *         print("both");
         *     }
         */
        if (is_opener(t->kind) && sp < MAX_NEST)
            open_level[sp++] = (t->kind == TK_LBRACE && carried_on && cur_level > 0)
                             ? cur_level - 1 : cur_level;
        else if (is_closer(t->kind) && sp > 0)   sp--;

        prev_prefix = (t->kind == TK_MINUS || t->kind == TK_BANG) && prefix_position(prev);
        at_line = t->line + count_newlines(t->raw, t->raw_len);
        prev = t;
    }

    buf_putc(&out, '\n');
    return out.data;
}

/* Safety net: the formatted text must lex to exactly the same tokens. */
static bool same_tokens(Token *a, int na, Token *b, int nb) {
    if (na != nb) return false;
    for (int i = 0; i < na; i++) {
        if (a[i].kind != b[i].kind) return false;
        if (a[i].raw_len != b[i].raw_len) return false;
        if (a[i].raw_len && memcmp(a[i].raw, b[i].raw, (size_t)a[i].raw_len) != 0) return false;
    }
    return true;
}

char *format_source(Source *src) {
    lex_keep_comments(true);

    int n = 0;
    Token *toks = lex_all(src, 1, &n);
    char *text = render(toks, n);

    Source check = { src->path, text };
    int n2 = 0;
    Token *toks2 = lex_all(&check, 1, &n2);

    if (!same_tokens(toks, n, toks2, n2)) {
        fatal("the formatter would have changed `%s`, so nothing was written.\n"
              "       This is a bug in cubc; please report it.", src->path);
    }
    lex_keep_comments(false);
    return text;
}
