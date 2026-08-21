/* lexer.c -- turns source text into a flat array of tokens.
 *
 * Line breaks carry no meaning in Cub: a statement ends at its semicolon,
 * so the lexer treats a newline as plain whitespace and the parser never
 * has to guess where an expression stopped.
 *
 * The one thing worth knowing about: string literals are split into parts
 * at scan time, so that "total: {a + b}" arrives at the parser as three
 * pieces -- the text "total: ", the expression source "a + b", and "".
 */
#include "cub.h"
#include <ctype.h>

typedef struct {
    const char *p;
    int         line, col;
    Buf         doc;        /* /// lines waiting for a declaration */
    bool        has_doc;
} Lexer;

static const char *tok_names[TK__COUNT] = {
    "end of file", "number", "number", "text", "name", "comment",
    "fn", "let", "var", "if", "else", "while", "for", "in",
    "return", "break", "continue", "type", "struct", "enum",
    "true", "false", "and", "or", "not",
    "class", "extends", "self", "super", "void", "static", "import",
    "nothing", "try",
    "(", ")", "{", "}", "[", "]",
    ",", ".", "..", "..=", ":", ";", "->", "?",
    "+", "-", "*", "/", "%",
    "=", "+=", "-=", "*=", "/=", "%=",
    "==", "!=", "<", "<=", ">", ">=",
    "&&", "||", "!"
};

const char *tok_name(TokKind k) {
    return (k >= 0 && k < TK__COUNT) ? tok_names[k] : "?";
}

static const struct { const char *word; TokKind kind; } keywords[] = {
    {"fn", TK_FN}, {"let", TK_LET}, {"var", TK_VAR}, {"if", TK_IF},
    {"else", TK_ELSE}, {"while", TK_WHILE}, {"for", TK_FOR}, {"in", TK_IN},
    {"return", TK_RETURN}, {"break", TK_BREAK}, {"continue", TK_CONTINUE},
    {"type", TK_TYPE}, {"struct", TK_STRUCT}, {"enum", TK_ENUM},
    {"true", TK_TRUE}, {"false", TK_FALSE},
    {"and", TK_AND}, {"or", TK_OR}, {"not", TK_NOT},
    {"class", TK_CLASS}, {"extends", TK_EXTENDS},
    {"self", TK_SELF}, {"super", TK_SUPER},
    {"void", TK_VOID}, {"static", TK_STATIC}, {"import", TK_IMPORT},
    {"nothing", TK_NOTHING}, {"try", TK_TRY},
    {NULL, TK_EOF}
};

static char peek(Lexer *L)      { return *L->p; }
static char peek2(Lexer *L)     { return *L->p ? L->p[1] : 0; }

static char advance(Lexer *L) {
    char c = *L->p++;
    if (c == '\n') { L->line++; L->col = 1; }
    else            L->col++;
    return c;
}

static bool keep_comments = false;

void lex_keep_comments(bool on) { keep_comments = on; }

static void skip_trivia(Lexer *L) {
    for (;;) {
        char c = peek(L);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(L); continue; }
        if (c == '/' && peek2(L) == '/') {
            if (keep_comments) return;
            /* `///` documents whatever comes next; `//` is an aside */
            bool is_doc = L->p[2] == '/' && L->p[3] != '/';
            if (is_doc) {
                advance(L); advance(L); advance(L);
                if (peek(L) == ' ') advance(L);
                if (!L->has_doc) { buf_init(&L->doc); L->has_doc = true; }
                else buf_putc(&L->doc, '\n');
                while (peek(L) && peek(L) != '\n') buf_putc(&L->doc, advance(L));
                continue;
            }
            while (peek(L) && peek(L) != '\n') advance(L);
            continue;
        }
        if (c == '/' && peek2(L) == '*') {
            if (keep_comments) return;
            int start = L->line;
            advance(L); advance(L);
            int depth = 1;
            while (depth > 0) {
                if (!peek(L)) {
                    err_at(start, 1, "this block comment is never closed");
                    err_help("add `*/` where the comment should end");
                    stop_if_errors();
                }
                if (peek(L) == '/' && peek2(L) == '*') { advance(L); advance(L); depth++; }
                else if (peek(L) == '*' && peek2(L) == '/') { advance(L); advance(L); depth--; }
                else advance(L);
            }
            continue;
        }
        return;
    }
}

/* Scan the body of a string literal, splitting out {interpolations}. */
static void scan_string(Lexer *L, Token *t) {
    Buf text;
    buf_init(&text);
    int line = L->line, col = L->col;

    for (;;) {
        if (!peek(L) || peek(L) == '\n') {
            err_at(t->line, t->col, "this text literal is never closed");
            err_help("text literals live on one line; close it with a quote, "
                     "or use \\n for a line break");
            stop_if_errors();
        }
        char c = advance(L);
        if (c == '"') break;

        if (c == '\\') {
            char e = advance(L);
            switch (e) {
            case 'n':  buf_putc(&text, '\n'); break;
            case 't':  buf_putc(&text, '\t'); break;
            case 'r':  buf_putc(&text, '\r'); break;
            case '0':  buf_putc(&text, '\0'); break;
            case '\\': buf_putc(&text, '\\'); break;
            case '"':  buf_putc(&text, '"');  break;
            case '{':  buf_putc(&text, '{');  break;
            case '}':  buf_putc(&text, '}');  break;
            default:
                err_at(L->line, L->col - 1, "`\\%c` is not an escape Cub knows", e);
                err_help("valid escapes are \\n \\t \\r \\0 \\\\ \\\" \\{ \\}");
                break;
            }
            continue;
        }

        if (c == '{') {
            /* flush the literal chunk collected so far */
            if (text.len > 0) {
                StrPart *sp = cx_alloc(sizeof(StrPart));
                sp->text = cx_strndup(text.data, text.len);
                sp->line = line; sp->col = col;
                vec_push(&t->parts, sp);
                text.len = 0; text.data[0] = 0;
            }
            /* capture the expression source up to the matching brace */
            int ex_line = L->line, ex_col = L->col;
            Buf ex;
            buf_init(&ex);
            int depth = 1;
            while (depth > 0) {
                if (!peek(L) || peek(L) == '\n') {
                    err_at(ex_line, ex_col, "this `{` in text is never closed");
                    err_help("write `{name}` to insert a value, or `\\{` for a real brace");
                    stop_if_errors();
                }
                char d = advance(L);
                if (d == '{') depth++;
                else if (d == '}') { depth--; if (!depth) break; }
                else if (d == '"') {          /* a nested string; copy verbatim */
                    buf_putc(&ex, d);
                    while (peek(L) && peek(L) != '"') {
                        if (peek(L) == '\\') buf_putc(&ex, advance(L));
                        buf_putc(&ex, advance(L));
                    }
                    if (peek(L)) buf_putc(&ex, advance(L));
                    continue;
                }
                buf_putc(&ex, d);
            }
            StrPart *sp = cx_alloc(sizeof(StrPart));
            sp->expr = cx_strndup(ex.data, ex.len);
            sp->line = ex_line; sp->col = ex_col;
            vec_push(&t->parts, sp);
            line = L->line; col = L->col;
            continue;
        }

        buf_putc(&text, c);
    }

    if (text.len > 0 || t->parts.len == 0) {
        StrPart *sp = cx_alloc(sizeof(StrPart));
        sp->text = cx_strndup(text.data, text.len);
        sp->line = line; sp->col = col;
        vec_push(&t->parts, sp);
    }
}

static void scan_number(Lexer *L, Token *t) {
    const char *start = L->p;
    bool is_float = false;

    if (peek(L) == '0' && (peek2(L) == 'x' || peek2(L) == 'X')) {
        advance(L); advance(L);
        while (isxdigit((unsigned char)peek(L)) || peek(L) == '_') advance(L);
        char *clean = cx_alloc((size_t)(L->p - start) + 1);
        size_t n = 0;
        for (const char *q = start; q < L->p; q++) if (*q != '_') clean[n++] = *q;
        t->kind = TK_INTLIT;
        t->ival = (int64_t)strtoull(clean, NULL, 16);
        return;
    }

    while (isdigit((unsigned char)peek(L)) || peek(L) == '_') advance(L);
    /* a dot is only a decimal point when a digit follows; `0..5` stays a range */
    if (peek(L) == '.' && isdigit((unsigned char)peek2(L))) {
        is_float = true;
        advance(L);
        while (isdigit((unsigned char)peek(L)) || peek(L) == '_') advance(L);
    }
    if (peek(L) == 'e' || peek(L) == 'E') {
        const char *save = L->p;
        int save_col = L->col;
        advance(L);
        if (peek(L) == '+' || peek(L) == '-') advance(L);
        if (isdigit((unsigned char)peek(L))) {
            is_float = true;
            while (isdigit((unsigned char)peek(L))) advance(L);
        } else { L->p = save; L->col = save_col; }
    }

    char *clean = cx_alloc((size_t)(L->p - start) + 1);
    size_t n = 0;
    for (const char *q = start; q < L->p; q++) if (*q != '_') clean[n++] = *q;

    if (is_float) { t->kind = TK_FLOATLIT; t->fval = strtod(clean, NULL); }
    else          { t->kind = TK_INTLIT;   t->ival = (int64_t)strtoll(clean, NULL, 10); }
}

static bool ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool ident_part(char c)  { return isalnum((unsigned char)c) || c == '_'; }

Token *lex_all(Source *src, int first_line, int *out_count) {
    Lexer L = { src->text, first_line, 1, {0}, false };
    Vec toks = {0};

    for (;;) {
        skip_trivia(&L);

        Token *t = cx_alloc(sizeof(Token));
        t->line = L.line;
        t->col = L.col;

        if (L.has_doc) {
            t->doc = cx_strndup(L.doc.data, L.doc.len);
            L.has_doc = false;
        }

        const char *tok_begin = L.p;
        /* Record the exact source text, then hand the token over.  The
         * formatter re-emits this verbatim, so 0xff and 1_000 keep the
         * shape you wrote them in. */
        #define PUSH()                                       \
            do {                                             \
                t->raw = tok_begin;                          \
                t->raw_len = (int)(L.p - tok_begin);         \
                vec_push(&toks, t);                          \
            } while (0)

        char c = peek(&L);
        if (!c) { t->kind = TK_EOF; t->raw = tok_begin; t->raw_len = 0;
                  vec_push(&toks, t); break; }

        if (keep_comments && c == '/' && (peek2(&L) == '/' || peek2(&L) == '*')) {
            t->kind = TK_COMMENT;
            if (peek2(&L) == '/') {
                while (peek(&L) && peek(&L) != '\n') advance(&L);
            } else {
                advance(&L); advance(&L);
                int depth = 1;
                while (depth > 0 && peek(&L)) {
                    if (peek(&L) == '/' && peek2(&L) == '*') { advance(&L); advance(&L); depth++; }
                    else if (peek(&L) == '*' && peek2(&L) == '/') { advance(&L); advance(&L); depth--; }
                    else advance(&L);
                }
            }
            PUSH();
            continue;
        }

        if (ident_start(c)) {
            const char *start = L.p;
            while (ident_part(peek(&L))) advance(&L);
            t->lex = cx_strndup(start, (size_t)(L.p - start));
            t->kind = TK_IDENT;
            for (int i = 0; keywords[i].word; i++)
                if (strcmp(keywords[i].word, t->lex) == 0) { t->kind = keywords[i].kind; break; }
            PUSH();
            continue;
        }

        if (isdigit((unsigned char)c)) { scan_number(&L, t); PUSH(); continue; }

        if (c == '"') { advance(&L); t->kind = TK_STRLIT; scan_string(&L, t); PUSH(); continue; }

        advance(&L);
        char d = peek(&L);
        switch (c) {
        case '(': t->kind = TK_LPAREN; break;
        case ')': t->kind = TK_RPAREN; break;
        case '{': t->kind = TK_LBRACE; break;
        case '}': t->kind = TK_RBRACE; break;
        case '[': t->kind = TK_LBRACK; break;
        case ']': t->kind = TK_RBRACK; break;
        case ',': t->kind = TK_COMMA;  break;
        case ':': t->kind = TK_COLON;  break;
        case ';': t->kind = TK_SEMI;   break;
        case '?': t->kind = TK_QUESTION; break;
        case '.':
            if (d == '.') {
                advance(&L);
                if (peek(&L) == '=') { advance(&L); t->kind = TK_RANGEEQ; }
                else t->kind = TK_RANGE;
            } else t->kind = TK_DOT;
            break;
        case '+': if (d == '=') { advance(&L); t->kind = TK_PLUSEQ; }  else t->kind = TK_PLUS;  break;
        case '-':
            if (d == '=')      { advance(&L); t->kind = TK_MINUSEQ; }
            else if (d == '>') { advance(&L); t->kind = TK_ARROW; }
            else               { t->kind = TK_MINUS; }
            break;
        case '*': if (d == '=') { advance(&L); t->kind = TK_STAREQ; }   else t->kind = TK_STAR;  break;
        case '/': if (d == '=') { advance(&L); t->kind = TK_SLASHEQ; }  else t->kind = TK_SLASH; break;
        case '%': if (d == '=') { advance(&L); t->kind = TK_PERCENTEQ; } else t->kind = TK_PERCENT; break;
        case '=': if (d == '=') { advance(&L); t->kind = TK_EQ; }       else t->kind = TK_ASSIGN; break;
        case '!': if (d == '=') { advance(&L); t->kind = TK_NE; }       else t->kind = TK_BANG;   break;
        case '<': if (d == '=') { advance(&L); t->kind = TK_LE; }       else t->kind = TK_LT;     break;
        case '>': if (d == '=') { advance(&L); t->kind = TK_GE; }       else t->kind = TK_GT;     break;
        case '&':
            if (d == '&') { advance(&L); t->kind = TK_ANDAND; }
            else { err_at(t->line, t->col, "Cub has no `&` operator");
                   err_help("use `and` (or `&&`) to combine two conditions"); t->kind = TK_ANDAND; }
            break;
        case '|':
            if (d == '|') { advance(&L); t->kind = TK_OROR; }
            else { err_at(t->line, t->col, "Cub has no `|` operator");
                   err_help("use `or` (or `||`) to combine two conditions"); t->kind = TK_OROR; }
            break;
        default:
            err_at(t->line, t->col, "`%c` is not a character Cub understands here", c);
            stop_if_errors();
            continue;
        }
        PUSH();
        #undef PUSH
    }

    stop_if_errors();

    Token *flat = cx_alloc(sizeof(Token) * (size_t)toks.len);
    for (int i = 0; i < toks.len; i++) flat[i] = *(Token *)toks.items[i];
    *out_count = toks.len;
    return flat;
}
