/* cub.h -- shared definitions for the Cub compiler.
 *
 * Cub is a small, friendly, statically typed language in the C family.
 * The compiler is a straightforward pipeline:
 *
 *     source -> lexer -> parser -> checker -> C code generator -> cc
 *
 * Everything the phases share lives in this header.
 */
#ifndef CUB_H
#define CUB_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#define CUB_VERSION "0.5.0"

/* ------------------------------------------------------------------ */
/* small utilities                                                     */
/* ------------------------------------------------------------------ */

void *cx_alloc(size_t n);              /* zeroed, never returns NULL   */
char *cx_strdup(const char *s);
char *cx_strndup(const char *s, size_t n);
char *cx_fmt(const char *fmt, ...);

typedef struct {
    char  *data;
    size_t len, cap;
} Buf;

void buf_init(Buf *b);
void buf_putc(Buf *b, char c);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);

typedef struct {
    void **items;
    int    len, cap;
} Vec;

void vec_push(Vec *v, void *p);

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *path;
    char       *text;
} Source;

extern Source *g_source;
extern int     cub_errors;

void err_at(int line, int col, const char *fmt, ...);
void err_help(const char *fmt, ...);   /* attaches a hint to the last error */
void fatal(const char *fmt, ...);      /* unrecoverable, exits             */
void stop_if_errors(void);

/* ------------------------------------------------------------------ */
/* tokens                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    TK_EOF, TK_INTLIT, TK_FLOATLIT, TK_STRLIT, TK_IDENT, TK_COMMENT,
    /* keywords */
    TK_FN, TK_LET, TK_VAR, TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_IN,
    TK_RETURN, TK_BREAK, TK_CONTINUE, TK_TYPE, TK_STRUCT, TK_ENUM,
    TK_TRUE, TK_FALSE, TK_AND, TK_OR, TK_NOT,
    TK_CLASS, TK_EXTENDS, TK_SELF, TK_SUPER, TK_VOID, TK_STATIC, TK_IMPORT,
    /* punctuation */
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACK, TK_RBRACK,
    TK_COMMA, TK_DOT, TK_RANGE, TK_RANGEEQ, TK_COLON, TK_SEMI, TK_ARROW,
    /* operators */
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_ASSIGN, TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_PERCENTEQ,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_ANDAND, TK_OROR, TK_BANG,
    TK__COUNT
} TokKind;

const char *tok_name(TokKind k);

/* One piece of a string literal: either literal text or an interpolated
 * expression written between braces, as in "hi {name}". */
typedef struct {
    char *text;   /* non-NULL for a literal chunk      */
    char *expr;   /* non-NULL for a {...} chunk        */
    int   line, col;
} StrPart;

typedef struct {
    TokKind  kind;
    int      line, col;
    char    *lex;         /* identifier text                      */
    int64_t  ival;
    double   fval;
    Vec      parts;       /* StrPart* for TK_STRLIT               */
    const char *raw;      /* exact source text of this token      */
    int      raw_len;
    char    *doc;         /* /// lines gathered just above it     */
} Token;

Token *lex_all(Source *src, int first_line, int *out_count);
void   lex_keep_comments(bool on);   /* the formatter needs them */

/* ------------------------------------------------------------------ */
/* formatter                                                           */
/* ------------------------------------------------------------------ */

char *format_source(Source *src);

/* ------------------------------------------------------------------ */
/* types                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    TY_ERR, TY_VOID, TY_INT, TY_FLOAT, TY_BOOL, TY_STR,
    TY_ARRAY, TY_STRUCT, TY_ENUM, TY_CLASS, TY_MAP
} TypeKind;

typedef struct Type      Type;
typedef struct StructDef StructDef;
typedef struct EnumDef   EnumDef;
typedef struct ClassDef  ClassDef;

struct Type {
    TypeKind   kind;
    Type      *elem;   /* TY_ARRAY element, TY_MAP value */
    Type      *key;    /* TY_MAP key */
    char      *name;   /* TY_STRUCT / TY_ENUM / TY_CLASS */
    StructDef *sdef;
    EnumDef   *edef;
    ClassDef  *cdef;
};

Type *ty_err(void);
Type *ty_void(void);
Type *ty_int(void);
Type *ty_float(void);
Type *ty_bool(void);
Type *ty_str(void);
Type *ty_array(Type *elem);          /* interned: same elem -> same Type* */
Type *ty_map(Type *key, Type *val);  /* interned the same way            */
Type *ty_named(TypeKind k, char *name);
bool  ty_same(Type *a, Type *b);
const char *ty_show(Type *t);        /* human readable, e.g. "[string]"   */
const char *ty_mangle(Type *t);      /* identifier-safe, e.g. "arr_string"*/
bool  ty_is_num(Type *t);
bool  ty_assignable(Type *from, Type *to);  /* allows a subclass for a base */

/* ------------------------------------------------------------------ */
/* symbols                                                             */
/* ------------------------------------------------------------------ */

typedef struct VarSym {
    char *name;
    Type *type;
    bool  is_mut;
    bool  is_global;
    char *cname;      /* unique name used in the generated C */
} VarSym;

/* ------------------------------------------------------------------ */
/* AST                                                                 */
/* ------------------------------------------------------------------ */

typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct FnDecl FnDecl;

typedef enum {
    EX_INT, EX_FLOAT, EX_BOOL, EX_STR, EX_INTERP, EX_IDENT,
    EX_UNARY, EX_BINARY, EX_CALL, EX_INDEX, EX_FIELD,
    EX_ARRAYLIT, EX_MAPLIT, EX_STRUCTLIT, EX_ENUMVAL, EX_IFEXPR,
    EX_SELF, EX_SUPER, EX_NEW, EX_METHOD
} ExprKind;

struct Expr {
    ExprKind kind;
    int      line, col;
    Type    *type;        /* filled in by the checker */

    int64_t  ival;
    double   fval;
    bool     bval;
    char    *sval;
    char    *name;        /* identifier, field name, callee, type name   */

    TokKind  op;
    Expr    *a, *b;       /* operands / target / index                   */
    Vec      args;        /* Expr*: call args, array items, interp parts */
    Vec      fnames;      /* char*: struct literal field names           */

    /* resolution results */
    VarSym   *var;
    FnDecl   *fn;
    int       builtin;     /* BI_* or -1 */
    int       enum_index;
    ClassDef *cls;         /* EX_NEW: the class being made          */
    Type     *obj_type;    /* EX_METHOD / EX_FIELD: type of the target */
};

typedef enum {
    ST_LET, ST_ASSIGN, ST_EXPR, ST_IF, ST_WHILE,
    ST_FORRANGE, ST_FORIN, ST_RETURN, ST_BREAK, ST_CONTINUE, ST_BLOCK
} StmtKind;

struct Stmt {
    StmtKind kind;
    int      line, col;

    char    *name;        /* declared / loop variable    */
    bool     is_mut;      /* `var` rather than `let`     */
    Type    *decl_type;   /* annotation, else inferred   */
    VarSym  *var;

    Source  *src;
    Expr    *lhs, *rhs, *cond, *from, *to;
    bool     inclusive;   /* `..=` range                 */
    TokKind  op;          /* compound assignment         */

    Vec      body;        /* Stmt* */
    Vec      els;         /* Stmt* */
};

struct FnDecl {
    char     *doc;
    Source   *src;
    char     *name;
    Vec       params;     /* VarSym* */
    Type     *ret;
    Vec       body;       /* Stmt*   */
    int       line, col;
    char     *cname;

    /* methods only */
    bool      is_static;  /* belongs to the class, not to an object */
    ClassDef *owner;      /* the class that declares this body     */
    ClassDef *slot_owner; /* the class whose table holds the slot   */
    bool      is_init;
    bool      is_override;
    VarSym   *self_sym;
};

/* A class: named fields plus methods, held by reference, with single
 * inheritance and methods that can be replaced by a subclass. */
struct ClassDef {
    char     *doc;
    Source   *src;
    char     *name;
    char     *base_name;   /* NULL when there is no parent */
    ClassDef *base;
    int       depth;       /* 0 for a class with no parent */

    Vec       fnames;      /* char*   -- fields declared here */
    Vec       ftypes;      /* Type*   */
    Vec       fdocs;       /* char*   -- one per field, may be NULL */
    Vec       methods;     /* FnDecl* -- declared here, init included */
    Vec       own_slots;   /* FnDecl* -- new table slots added here   */

    FnDecl   *init;
    Vec       statics;     /* FnDecl* -- methods without an object */
    int       line, col;
    bool      checked;     /* guards against inheritance loops */
};

struct StructDef {
    char *doc;
    Source *src;
    char *name;
    Vec   fnames;         /* char* */
    Vec   ftypes;         /* Type* */
    Vec   fdocs;          /* char* -- one per field, may be NULL */
    int   line, col;
};

struct EnumDef {
    char *doc;
    Source *src;
    char *name;
    Vec   vals;           /* char* */
    int   line, col;
};

typedef struct {
    Vec fns;              /* FnDecl*    */
    Vec structs;          /* StructDef* */
    Vec enums;            /* EnumDef*   */
    Vec classes;          /* ClassDef*  */
    Vec globals;          /* Stmt* (ST_LET) */

    Vec modules;          /* char*   -- `import os` and friends   */
    Vec files;            /* Source* -- every file that was read  */

    /* where the program starts, worked out by the checker */
    FnDecl   *entry;
    ClassDef *entry_class;   /* set when the entry point is a method */
} Program;

Program *parse_program(Token *toks, int ntoks);
Expr    *parse_expr_source(const char *src, int line, int col);

/* ------------------------------------------------------------------ */
/* built-in functions                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    BI_NONE = -1, BI_PRINT, BI_WRITE, BI_LEN, BI_PUSH, BI_POP, BI_REMOVE,
    BI_INSERT, BI_STR, BI_INT, BI_FLOAT, BI_ABS, BI_MIN, BI_MAX, BI_SQRT,
    BI_POW, BI_FLOOR, BI_CEIL, BI_ROUND, BI_RAND_INT, BI_RAND_SEED,
    BI_INPUT, BI_UPPER, BI_LOWER, BI_TRIM, BI_SPLIT, BI_JOIN, BI_FIND,
    BI_SLICE, BI_CONTAINS, BI_STARTS_WITH, BI_ENDS_WITH, BI_REPLACE,
    BI_REPEAT, BI_CHAR_AT, BI_CODE_AT, BI_FROM_CODE, BI_SORT, BI_REVERSE,
    BI_READ_FILE, BI_WRITE_FILE, BI_PANIC, BI_ASSERT, BI_TIME_MS, BI_HAS,
    BI_GET, BI_KEYS, BI_VALUES, BI_CLEAR, BI_SIN, BI_COS, BI_TAN, BI_ASIN,
    BI_ACOS, BI_ATAN, BI_ATAN2, BI_LOG, BI_LOG10, BI_EXP, BI_SIGN, BI_CLAMP,
    BI_IS_NAN, BI_IS_INF, BI_RAND_FLOAT, BI_PAD_START, BI_PAD_END,
    BI_TRIM_START, BI_TRIM_END, BI_LINES, BI_CHARS, BI_COUNT, BI_INDEX_OF,
    BI_LAST_INDEX_OF, BI_CAPITALIZE, BI_IS_DIGIT, BI_IS_ALPHA, BI_IS_ALNUM,
    BI_IS_SPACE, BI_IS_UPPER, BI_IS_LOWER, BI_SUM, BI_COPY, BI_CONCAT,
    BI_SHUFFLE, BI_SWAP, BI_MIN_OF, BI_MAX_OF, BI_EPRINT, BI_EXIT, BI_ARGS,
    BI_ENV, BI_SLEEP_MS, BI_CLOCK_MS, BI_FILE_EXISTS, BI_APPEND_FILE,
    BI_DELETE_FILE, BI_READ_LINES, BI_PLATFORM
} Builtin;

int  builtin_lookup(const char *name);   /* -1 when not a builtin */

/* Some built-ins live in a module and are reached through it. */
const char *builtin_module(int bi);      /* NULL when it is global */

/* ------------------------------------------------------------------ */
/* phases                                                              */
/* ------------------------------------------------------------------ */

void  check_program(Program *p, bool require_main);
char *codegen_program(Program *p, const char *unit_name);
char *docgen_program(Program *p, const char *unit_name);

extern const char *CUB_RUNTIME_SRC;      /* generated from runtime/cub_rt.h */

#endif /* CUB_H */
