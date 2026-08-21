#!/usr/bin/env python3
"""Rewrite Cub 0.4 source in the 0.5 syntax.

    tools/migrate.py file.cb ...      print what would change
    tools/migrate.py -w file.cb ...   rewrite the files in place

Three things move:

    type P = struct { x: int }    ->  struct P { x: int; }
    type C = enum { Red }         ->  enum C { Red }
    class Dog : Animal { }        ->  class Dog extends Animal { }

and every statement gains the semicolon that used to be implied by the end
of the line.  Functions are untouched: they still lead with what they give
back, as `int add(a: int, b: int)` and `void run()`.

The tool parses the old grammar rather than matching patterns, and edits the
original text by offset, so comments, blank lines, and the exact spelling of
literals all survive untouched.  Run `cubc fmt -w` afterwards if you want the
spacing tidied as well.
"""
import sys

KEYWORDS = {
    "fn", "let", "var", "if", "else", "while", "for", "in", "return", "break",
    "continue", "type", "struct", "enum", "true", "false", "and", "or", "not",
    "class", "self", "super", "void", "static", "import",
}

PUNCT = [
    "..=", "..", "->", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=",
    "&&", "||", "(", ")", "{", "}", "[", "]", ",", ".", ":", ";", "+", "-",
    "*", "/", "%", "=", "<", ">", "!",
]

# a statement carries on when the line ends on one of these
TRAILING = {
    "+", "-", "*", "/", "%", "=", "+=", "-=", "*=", "/=", "%=", "==", "!=",
    "<", "<=", ">", ">=", "&&", "||", "and", "or", "not", ",", ".", "..",
    "..=", ":", "->", "(", "[", "{",
}

TYPE_START = {"void", "int", "float", "bool", "string"}


class Tok:
    __slots__ = ("kind", "text", "line", "start", "end")

    def __init__(self, kind, text, line, start, end):
        self.kind, self.text, self.line, self.start, self.end = kind, text, line, start, end

    def __repr__(self):
        return f"{self.kind}:{self.text!r}@{self.line}"


class Fail(Exception):
    pass


def lex(src, path):
    """Tokens, comments dropped.  A string keeps its interpolation intact."""
    toks, i, line, n = [], 0, 1, len(src)
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        if src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
            continue
        if src.startswith("/*", i):
            depth, i = 1, i + 2
            while i < n and depth:
                if src.startswith("/*", i):
                    depth, i = depth + 1, i + 2
                elif src.startswith("*/", i):
                    depth, i = depth - 1, i + 2
                else:
                    line += src[i] == "\n"
                    i += 1
            continue
        if c == '"':
            start, first, i = i, line, i + 1
            brace = 0
            while i < n:
                d = src[i]
                if d == "\\" and not brace:
                    i += 2
                    continue
                if d == '"' and not brace:
                    i += 1
                    break
                if d == "{" and not brace:
                    brace = 1
                    i += 1
                    continue
                if brace:
                    if d == "{":
                        brace += 1
                    elif d == "}":
                        brace -= 1
                    elif d == '"':          # a string inside {interpolation}
                        i += 1
                        while i < n and src[i] != '"':
                            i += 2 if src[i] == "\\" else 1
                    i += 1
                    continue
                line += d == "\n"
                i += 1
            toks.append(Tok("str", src[start:i], first, start, i))
            continue
        if c.isdigit():
            start = i
            while i < n and (src[i].isalnum() or src[i] == "_"):
                i += 1
            if i < n and src[i] == "." and i + 1 < n and src[i + 1].isdigit():
                i += 1
                while i < n and (src[i].isdigit() or src[i] == "_"):
                    i += 1
            toks.append(Tok("num", src[start:i], line, start, i))
            continue
        if c.isalpha() or c == "_":
            start = i
            while i < n and (src[i].isalnum() or src[i] == "_"):
                i += 1
            word = src[start:i]
            toks.append(Tok("kw" if word in KEYWORDS else "name", word, line, start, i))
            continue
        for p in PUNCT:
            if src.startswith(p, i):
                toks.append(Tok("punct", p, line, i, i + len(p)))
                i += len(p)
                break
        else:
            raise Fail(f"{path}: cannot read `{c}` on line {line}")
    toks.append(Tok("eof", "", line, n, n))
    return toks


class Migrator:
    def __init__(self, src, path):
        self.src, self.path = src, path
        self.t = lex(src, path)
        self.i = 0
        self.edits = []            # (start, end, replacement)

    # ---- token helpers ----
    def cur(self):
        return self.t[self.i]

    def at(self, text):
        return self.cur().text == text and self.cur().kind in ("kw", "punct")

    def ahead(self, k=1):
        return self.t[min(self.i + k, len(self.t) - 1)]

    def take(self):
        t = self.t[self.i]
        if t.kind != "eof":
            self.i += 1
        return t

    def expect(self, text):
        if not self.at(text):
            raise Fail(f"{self.path}:{self.cur().line}: expected `{text}`, "
                       f"found `{self.cur().text}`")
        return self.take()

    def prev(self):
        return self.t[self.i - 1] if self.i else self.t[0]

    # ---- edits ----
    def replace(self, start, end, text):
        self.edits.append((start, end, text))

    def semi_after(self, tok):
        self.edits.append((tok.end, tok.end, ";"))

    def apply(self):
        out, at = [], 0
        for start, end, text in sorted(self.edits, key=lambda e: (e[0], e[1])):
            if start < at:
                raise Fail(f"{self.path}: overlapping edits")
            out.append(self.src[at:start])
            out.append(text)
            at = end
        out.append(self.src[at:])
        return "".join(out)

    # ---- the old grammar ----
    def run(self):
        while self.cur().kind != "eof":
            here = self.i
            self.step()
            if self.i == here:
                raise Fail(f"{self.path}:{self.cur().line}: stuck at "
                           f"`{self.cur().text}`")
        return self.apply()

    def step(self):
            if self.at("import"):
                self.take()
                self.semi_after(self.take())          # module name or "file"
            elif self.at("class"):
                self.class_decl()
            elif self.at("type"):
                self.type_decl()
            elif self.at("let") or self.at("var"):
                self.let_stmt()
            else:
                self.fn_decl()

    def a_type(self):
        """Consume a type, returning its source text."""
        start = self.cur()
        if self.at("["):
            depth = 0
            while True:
                t = self.take()
                if t.text == "[":
                    depth += 1
                elif t.text == "]":
                    depth -= 1
                    if not depth:
                        break
                elif t.kind == "eof":
                    raise Fail(f"{self.path}:{start.line}: unclosed `[` in a type")
        elif self.cur().kind == "name" or self.cur().text in TYPE_START:
            self.take()
        else:
            raise Fail(f"{self.path}:{self.cur().line}: expected a type, "
                       f"found `{self.cur().text}`")
        return self.src[start.start:self.prev().end]

    def fn_decl(self):
        """`[static] <type> name(params) { }` -- walked, but left as written."""
        head = self.cur()
        if self.at("static"):
            self.take()
            head = self.cur()
        if self.at("fn"):
            raise Fail(f"{self.path}:{head.line}: Cub has no `fn` keyword")
        self.a_type()
        name = self.cur()
        if name.kind != "name":
            raise Fail(f"{self.path}:{name.line}: expected a function name, "
                       f"found `{name.text}`")
        self.take()

        self.expect("(")
        while not self.at(")"):
            self.take()
            self.expect(":")
            self.a_type()
            if not self.at(","):
                break
            self.take()
        self.expect(")")
        self.block()

    def type_decl(self):
        kw = self.expect("type")
        name = self.take()
        self.expect("=")
        if self.at("struct"):
            self.take()
            self.replace(kw.start, self.cur().start, f"struct {name.text} ")
            self.expect("{")
            while not self.at("}"):
                self.take()                       # field name
                self.expect(":")
                self.a_type()
                last = self.prev()
                if self.at(","):
                    comma = self.take()
                    self.replace(comma.start, comma.end, ";")
                else:
                    self.semi_after(last)
                    break
            self.expect("}")
        elif self.at("enum"):
            self.take()
            self.replace(kw.start, self.cur().start, f"enum {name.text} ")
            self.expect("{")
            while not self.at("}"):
                self.take()
                if not self.at(","):
                    break
                self.take()
            self.expect("}")
        else:
            raise Fail(f"{self.path}:{kw.line}: expected `struct` or `enum`")

    def class_decl(self):
        self.expect("class")
        self.take()                                # class name
        if self.at(":"):
            colon = self.take()
            self.replace(colon.start, colon.end, " extends")
            self.take()                            # base name
        self.expect("{")
        while not self.at("}") and self.cur().kind != "eof":
            here = self.i
            if self.cur().kind == "name" and self.ahead().text == ":":
                self.take()
                self.take()
                self.a_type()
                last = self.prev()
                if self.at(","):                   # fields once allowed commas
                    comma = self.take()
                    self.replace(comma.start, comma.end, ";")
                else:
                    self.semi_after(last)
                continue
            self.fn_decl()
            if self.i == here:
                raise Fail(f"{self.path}:{self.cur().line}: stuck in a class body")
        self.expect("}")

    # ---- statements ----
    def block(self):
        self.expect("{")
        while not self.at("}") and self.cur().kind != "eof":
            here = self.i
            self.statement()
            if self.i == here:
                raise Fail(f"{self.path}:{self.cur().line}: stuck at "
                           f"`{self.cur().text}` inside a block")
        self.expect("}")

    def statement(self):
        while self.at(";"):
            self.take()
        if self.at("let") or self.at("var"):
            self.let_stmt()
        elif self.at("if"):
            self.if_stmt()
        elif self.at("while"):
            self.take()
            self.header()
            self.block()
        elif self.at("for"):
            self.take()
            self.header()
            self.block()
        elif self.at("return"):
            kw = self.take()
            if not (self.at("}") or self.at(";") or self.cur().kind == "eof"
                    or self.cur().line > kw.line):
                self.expr()
            self.end_of_statement()
        elif self.at("break") or self.at("continue"):
            self.take()
            self.end_of_statement()
        elif self.at("{"):
            self.block()
        else:
            self.expr()
            if self.cur().text in ("=", "+=", "-=", "*=", "/=", "%="):
                self.take()
                self.expr()
            self.end_of_statement()

    def end_of_statement(self):
        if self.at(";"):
            self.take()
        else:
            self.semi_after(self.prev())

    def let_stmt(self):
        self.take()
        self.take()                                # name
        if self.at(":"):
            self.take()
            self.a_type()
        self.expect("=")
        self.expr()
        self.end_of_statement()

    def if_stmt(self):
        self.take()
        self.header()
        self.block()
        if self.at("else"):
            self.take()
            if self.at("if"):
                self.if_stmt()
            else:
                self.block()

    def header(self):
        """The condition of an if/while/for: everything up to its `{`."""
        depth = 0
        while self.cur().kind != "eof":
            t = self.cur()
            if depth == 0 and t.text == "{":
                return
            if t.text in ("(", "["):
                depth += 1
            elif t.text in (")", "]"):
                depth -= 1
            self.take()
        raise Fail(f"{self.path}: a loop or `if` header runs off the end of the file")

    def expr(self):
        """Consume one expression, stopping where the old parser would."""
        depth = 0
        started = False
        while True:
            t = self.cur()
            if t.kind == "eof":
                return
            if depth == 0 and started:
                if t.text in ("}", ";", ")", "]", ","):
                    return
                if t.text in ("=", "+=", "-=", "*=", "/=", "%="):
                    return
                # a line break ends the expression unless the line was left
                # hanging on an operator
                if self.i and t.line > self.prev().line and self.prev().text not in TRAILING:
                    return
            if t.text in ("(", "["):
                depth += 1
            elif t.text in (")", "]"):
                depth -= 1
            elif t.text == "{":
                self.value_braces()
                started = True
                continue
            self.take()
            started = True

    def value_braces(self):
        """A `{ ... }` inside an expression: a struct literal or an if-branch."""
        depth = 0
        while self.cur().kind != "eof":
            t = self.take()
            if t.text == "{":
                depth += 1
            elif t.text == "}":
                depth -= 1
                if not depth:
                    return


def migrate(path, src):
    return Migrator(src, path).run()


def main(argv):
    write = False
    files = []
    for a in argv[1:]:
        if a == "-w":
            write = True
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            files.append(a)
    if not files:
        print(__doc__)
        return 2

    failed = 0
    for path in files:
        with open(path) as f:
            src = f.read()
        try:
            out = migrate(path, src)
        except Fail as e:
            print(f"cannot migrate: {e}", file=sys.stderr)
            failed += 1
            continue
        if write:
            if out != src:
                with open(path, "w") as f:
                    f.write(out)
                print(f"migrated {path}")
        else:
            sys.stdout.write(out)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
