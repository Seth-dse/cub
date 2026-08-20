/* main.c -- the cubc driver.
 *
 *   cubc hello.cb              compile to ./hello
 *   cubc hello.cb -o greet     choose the name
 *   cubc run hello.cb          compile and run in one step
 *   cubc build hello.cb --emit-c   write the generated C instead
 */
#include "cub.h"
#include <unistd.h>
#include <sys/wait.h>

static const char *USAGE =
"cubc " CUB_VERSION " -- the Cub compiler\n"
"\n"
"usage:\n"
"  cubc <file.cb> [-o <name>]     compile a program\n"
"  cubc run <file.cb>             compile and run it\n"
"  cubc build <file.cb>           same as the plain form\n"
"  cubc fmt <file.cb>             print the file, tidily formatted\n"
"  cubc fmt -w <file.cb>          format the file in place\n"
"  cubc fmt -                      format standard input (for editors)\n"
"  cubc doc <file.cb>             write a reference from its /// comments\n"
"\n"
"options:\n"
"  -o <name>     name of the program to write (default: the source name)\n"
"  --emit-c      write standalone C instead of a program\n"
"  --keep-c      keep the generated C next to the output\n"
"  --check       type-check only; with `fmt`, report unformatted files\n"
"  -w            with `fmt`, rewrite the file in place\n"
"  -v            show each step\n"
"  --version     print the version\n"
"  -h, --help    print this message\n";

static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) fatal("cannot open `%s`", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) fatal("cannot read `%s`", path);
    char *buf = cx_alloc((size_t)n + 2);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    return buf;
}

static void write_whole_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) fatal("cannot write `%s`", path);
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

/* strip directories and the .cb suffix */
static char *stem_of(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    return dot ? cx_strndup(base, (size_t)(dot - base)) : cx_strdup(base);
}

int main(int argc, char **argv) {
    const char *file = NULL, *outname = NULL;
    bool emit_c = false, keep_c = false, check_only = false, run_it = false, verbose = false;
    bool fmt_mode = false, in_place = false, doc_mode = false;
    Vec extra_files = {0};

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { fputs(USAGE, stdout); return 0; }
        if (strcmp(a, "--version") == 0) { printf("cubc %s\n", CUB_VERSION); return 0; }
        if (strcmp(a, "run") == 0 && !file)   { run_it = true;  continue; }
        if (strcmp(a, "fmt") == 0 && !file)   { fmt_mode = true; continue; }
        if (strcmp(a, "doc") == 0 && !file)   { doc_mode = true; continue; }
        if (strcmp(a, "-w") == 0)             { in_place = true; continue; }
        if (strcmp(a, "-") == 0 && fmt_mode)  { file = "-"; continue; }
        if (strcmp(a, "build") == 0 && !file) { continue; }
        if (strcmp(a, "--emit-c") == 0)  { emit_c = true;  continue; }
        if (strcmp(a, "--keep-c") == 0)  { keep_c = true;  continue; }
        if (strcmp(a, "--check") == 0)   { check_only = true; continue; }
        if (strcmp(a, "-v") == 0)        { verbose = true; continue; }
        if (strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) fatal("`-o` needs a name after it");
            outname = argv[++i];
            continue;
        }
        if (a[0] == '-') fatal("unknown option `%s`\n\nrun `cubc --help` to see the options", a);
        if (file) {
            if (!fmt_mode)
                fatal("cubc compiles one file at a time (got `%s` and `%s`)", file, a);
            vec_push(&extra_files, (void *)a);
            continue;
        }
        file = a;
    }

    if (!file) { fputs(USAGE, stderr); return 1; }

    /* ---- formatting works on text alone, so it runs before anything else ---- */
    if (fmt_mode && extra_files.len > 0) {
        /* several files at once, which is what `make fmt` and editors want */
        int bad = 0;
        for (int i = -1; i < extra_files.len; i++) {
            const char *path = i < 0 ? file : (const char *)extra_files.items[i];
            char *text = read_whole_file(path);
            Source fs_ = { path, text };
            g_source = &fs_;
            char *formatted = format_source(&fs_);
            if (check_only) {
                if (strcmp(text, formatted) != 0) {
                    fprintf(stderr, "%s is not formatted\n", path);
                    bad = 1;
                }
            } else if (in_place) {
                if (strcmp(text, formatted) != 0) {
                    write_whole_file(path, formatted);
                    printf("formatted %s\n", path);
                }
            } else {
                fputs(formatted, stdout);
            }
        }
        return bad;
    }

    if (fmt_mode) {
        bool from_stdin = strcmp(file, "-") == 0;
        char *text;
        if (from_stdin) {
            Buf in;
            buf_init(&in);
            int c;
            while ((c = fgetc(stdin)) != EOF) buf_putc(&in, (char)c);
            text = in.data;
        } else {
            text = read_whole_file(file);
        }
        Source fsrc = { from_stdin ? "<stdin>" : file, text };
        g_source = &fsrc;

        char *formatted = format_source(&fsrc);

        if (check_only) {
            if (strcmp(text, formatted) == 0) return 0;
            fprintf(stderr, "%s is not formatted\n", fsrc.path);
            return 1;
        }
        if (in_place && !from_stdin) {
            if (strcmp(text, formatted) != 0) {
                write_whole_file(file, formatted);
                printf("formatted %s\n", file);
            }
            return 0;
        }
        fputs(formatted, stdout);
        return 0;
    }

    size_t flen = strlen(file);
    if (flen < 3 || strcmp(file + flen - 3, ".cb") != 0)
        fprintf(stderr, "cubc: note: Cub source files usually end in .cb\n");

    /* ---- front end ---- */
    Source src = { file, read_whole_file(file) };
    g_source = &src;

    if (verbose) fprintf(stderr, "cubc: reading %s\n", file);
    int ntok = 0;
    Token *toks = lex_all(&src, 1, &ntok);
    if (verbose) fprintf(stderr, "cubc: %d tokens\n", ntok);

    Program *prog = parse_program(toks, ntok);
    if (verbose) fprintf(stderr, "cubc: %d function(s), %d type(s)\n",
                         prog->fns.len, prog->structs.len + prog->enums.len);

    check_program(prog, !doc_mode);
    if (verbose) fprintf(stderr, "cubc: types check out\n");
    if (check_only) { printf("%s: no problems found\n", file); return 0; }

    if (doc_mode) {
        char *md = docgen_program(prog, file);
        if (outname) {
            write_whole_file(outname, md);
            printf("wrote %s\n", outname);
        } else {
            fputs(md, stdout);
        }
        return 0;
    }

    /* ---- back end ---- */
    char *csrc = codegen_program(prog, file);

    char *stem = stem_of(file);
    /* `cubc run` is a throwaway build, so it goes to a temp path rather
     * than leaving a binary behind in whatever directory you are in. */
    char *exe = outname ? cx_strdup(outname)
              : (run_it ? cx_fmt("/tmp/cubc-%d-%s", (int)getpid(), stem) : stem);

    if (emit_c) {
        char *cpath = outname ? cx_strdup(outname) : cx_fmt("%s.c", stem);
        write_whole_file(cpath, csrc);
        printf("wrote %s\n", cpath);
        return 0;
    }

    char *cpath = keep_c ? cx_fmt("%s.c", exe) : cx_fmt("/tmp/cubc-%d-%s.c", (int)getpid(), stem);
    write_whole_file(cpath, csrc);

    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    char *cmd = cx_fmt("%s -std=c99 -O2 -w -o '%s' '%s' -lm", cc, exe, cpath);
    if (verbose) fprintf(stderr, "cubc: %s\n", cmd);

    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "cubc: the C compiler rejected the generated code.\n"
                        "      This is a bug in cubc, not in your program.\n"
                        "      The generated file is %s\n", cpath);
        return 1;
    }
    if (!keep_c) remove(cpath);

    if (run_it) {
        char *runcmd = exe[0] == '/' ? cx_fmt("'%s'", exe) : cx_fmt("./'%s'", exe);
        int prc = system(runcmd);
        if (!outname) remove(exe);
        return WIFEXITED(prc) ? WEXITSTATUS(prc) : 1;
    }

    if (verbose) fprintf(stderr, "cubc: wrote %s\n", exe);
    return 0;
}
