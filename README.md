# Cub

**A small language in the C family that tells you what went wrong.**

Cub keeps what makes C good — static types, structs, predictable machine
behavior, no runtime to install — and drops what makes it painful. No header
files. No pointers. No undefined behavior at the edges. No error message that
sends you to a search engine.

It compiles to standalone C99, so a Cub program runs anywhere C runs.

```cub
class Task {
    title: string;
    done: bool;

    void init(title: string) {
        self.title = title;
    }

    string to_string() {
        return "{if self.done { "[x]" } else { "[ ]" }} {self.title}";
    }
}

void main() {
    let tasks = [Task("write a language"), Task("write the docs")];
    tasks[0].done = true;

    for task in tasks {
        print(task);
    }
    print("{len(tasks)} tasks");
}
```

```
cubc run tasks.cb
```

---

## Build it

Nothing to install beyond a C compiler and `make`.

```bash
make
make test          # 87 tests: programs, compile errors, runtime failures
make check-linux   # optional: build and test under glibc in a container
make examples      # run everything in examples/
sudo make install  # optional: puts cubc in /usr/local/bin
make install-vscode   # optional: editor support
```

Then:

```bash
./cubc run examples/fizzbuzz.cb
```

---

## What it looks like

**Text that reads like text.**

```cub
let name = "Ada";
print("Hello, {name}. Next year you turn {age + 1}.");
```

**Errors written for a person.**

```
sum.cb:2:15: error: cannot add int and string
     2 |     let n = 1 + "two";
       |               ^
  help: turn the other side into text with `str(x)`, or write "...{value}..."
```

```
count.cb:3:5: error: `count` was declared with `let`, so it never changes
  help: declare it with `var count = ...;` if it needs to change
```

Misspell a name and it guesses what you meant. Forget a struct field and it
names the one you forgot. Skip a `return` on one branch and it finds the
branch before the program ever runs.

**Failures that stop, and say where.**

```
Runtime error: position 5 is outside the array, whose positions are 0 to 2
  at scores.cb:4
```

Array bounds, division by zero, arithmetic that overflows, conversions that
cannot fit, and a stack that runs out are all checked. Cub has no undefined
behavior at these edges — programs stop with an explanation instead of
corrupting memory or dying on a signal.

**No ceremony.**

```cub
// no #include, no header, no forward declarations,
// no `int argc, char **argv`
void main() {
    print("that is the whole program");
}
```

---

## The rules

1. **Nothing happens silently.** An `int` never becomes a `float` on its own.
   Nothing is quietly truthy. Conversions are written down.
2. **Mistakes are caught early, and explained.** Types, names, missing
   returns — checked before the program runs, in plain words.
3. **What the compiler cannot know, the program checks.** Bounds, division,
   overflow, conversions, stack depth. No undefined behavior, and no silent
   wraparound. What *can* fail says so in its type: `int(text)` is an `int!`,
   and there is no way to use it without deciding what an unreadable number
   means.
4. **One way to write things.** Every statement ends with `;`, every body is
   wrapped in braces, and a function says what it gives back before its name,
   the way C does. No headers, no preprocessor, no macros. Declaration order
   never matters.
5. **The output is honest C.** `--emit-c` gives you one readable C99 file
   with no dependencies.

---

## A tour in one page

```cub
// Values: let never changes, var does.
let limit = 100;
var count = 0;
count += 1;

// Types: int, float, bool, string, [T], and your own.
let ratio: float = 1.5;
var names: [string] = [];

// No implicit conversion, ever.
let half = float(7) / 2.0;        // 3.5

// Conditions are conditions -- there is no truthiness.
if count > 0 and count < limit {
    print("in range");
}

// Two kinds of loop.
for i in 0..5 { }                // 0 1 2 3 4
for i in 1..=5 { }               // 1 2 3 4 5
for name in names { }

// Functions: typed, in any order, checked for every return path.
float area(w: float, h: float) {
    return w * h;
}

// `void` is a function that gives nothing back.
void shout(text: string) {
    print(upper(text));
}

// Arrays and objects are shared. Structs are copied.
struct Point { x: int; y: int; }
enum Color { Red, Green, Blue }

// An enum value may carry things, and `match` takes them back out --
// leave a value unanswered and the compiler names it.
enum Shape { Circle(radius: float), Rect(w: float, h: float) }

let area = match Shape.Circle(2.0) {
    Circle(r) => 3.14159 * r * r,
    Rect(w, h) => w * h,
};

let p = Point { x: 1, y: 2 };
print(p);                        // Point{x: 1, y: 2}
print(Color.Green);              // Green

// Part of the library is always there; the rest is imported.
import math;
import fs;

print(math.sqrt(16.0));
print(fs.exists("notes.txt"));

// A function can work for any type; a call settles what T is.
T first<T>(items: [T]) { return items[0]; }
print(first(["a", "b"]));        // a

// A function is a value: hold one, pass one, write one where it is used.
let double = int(x: int) { return x * 2; };
print(map([1, 2, 3], double));   // [2, 4, 6]

// A program can span several files.
import "shapes.cb";

// And it can reach into C, because that is what it compiles to.
extern "math.h" {
    float cbrt(x: float);
}
print(cbrt(27.0));               // 3.0

// Maps, keyed by text or number.
var ages = ["ada": 36];
ages["grace"] = 45;
print(get(ages, "nobody", 0));   // 0

// if also works as a value.
let label = if count > 0 { "some" } else { "none" };

// A value that might not be there says so: `T?` for one that may be
// nothing, `T!` for one that may fail with a reason.
let typed = int(input()) or 0;           // a fallback
if let n = int(input()) {                // or take it apart
    print(n);
} else why {
    print(why);
}

// Classes: data with behaviour, and one parent at most.
class Animal {
    name: string;

    void init(name: string) { self.name = name; }
    string speak() { return "{self.name} makes a sound"; }
}

class Dog extends Animal {
    void init(name: string) { super.init(name); }
    string speak() { return "{self.name} says woof"; }
}

let pets: [Animal] = [Animal("Generic"), Dog("Rex")];
for pet in pets {
    print(pet.speak());          // each one keeps its own voice
}

// A class can hold the starting point, and methods of its own.
class App {
    static int twice(n: int) { return n * 2; }

    void main() {
        print(App.twice(21));
    }
}
```

Read [the tutorial](docs/TUTORIAL.md) to learn it, work through
[the exercises](exercises) to make it stick, or keep
[the reference](docs/LANGUAGE.md) open for the details and every built-in.

---

## How it works

```
your.cb ──▶ lexer ──▶ parser ──▶ checker ──▶ C generator ──▶ cc ──▶ program
```

The compiler is about 7,200 lines of C99 with no dependencies. It emits one
self-contained C file — runtime included — and hands it to your system C
compiler.

| | |
|---|---|
| `src/lexer.c` | text to tokens, including `{interpolation}` splitting |
| `src/parser.c` | recursive descent; statements end at `;` |
| `src/check.c` | types, names, mutability, return paths, and the messages |
| | including where a `T?` or `T!` must be dealt with |
| `src/codegen.c` | standalone C99 |
| `src/format.c` | `cubc fmt`, the canonical formatter |
| `src/docgen.c` | `cubc doc`, a reference built from `///` comments |
| `editors/vscode/` | highlighting, formatting, and live errors in VS Code |
| `runtime/cub_rt.h` | text, arrays, bounds checks, and the allocation registry |
| `tests/` | programs, compile errors, and runtime failures |
| `tools/migrate.py` | rewrites 0.4 source in the 0.5 syntax |

`extern "header.h" { ... }` borrows a C function; `link "name";` puts
`-lname` on the C compiler's command line. Everything Cub promises stops at
that boundary — see [the reference](docs/LANGUAGE.md#21-reaching-into-c).

Look at what your program becomes:

```bash
cubc program.cb --emit-c
```

The result compiles with plain `cc -std=c99 program.c -lm` and has no idea
Cub exists.

---

## Memory

No pointers, no `malloc`, no `free`. Text and arrays live on the heap; the
runtime tracks every allocation and releases all of it at exit. Programs
cannot leak, double-free, or touch freed memory — checked under
AddressSanitizer and UndefinedBehaviorSanitizer.

The honest limit: memory is reclaimed at exit, not when a value stops being
used. Right for scripts, tools, and batch programs; not yet right for a
server meant to run for weeks. Reference counting is the next step.

---

## Status

Version 0.10.0. The language described here works, and the test suite covers
it. Not yet built: generic types of your own, interfaces, and private
declarations.
[The reference](docs/LANGUAGE.md#20-what-cub-leaves-out-for-now) lists them
with what is planned.

---

## Editor support

A VS Code extension lives in [editors/vscode](editors/vscode). Install it
with:

```bash
make install-vscode
```

You get syntax highlighting (including expressions inside interpolated text),
**errors underlined as you type** — the compiler's `help:` line comes along
with them — formatting on `Shift+Alt+F`, snippets, and a **Cub: Run File**
command. It works in VS Code and its forks (Cursor, Windsurf, Antigravity).

## Formatting

```bash
cubc fmt program.cb          print it, tidily formatted
cubc fmt -w program.cb       rewrite it in place
cubc fmt --check program.cb  exit non-zero if it needs formatting (for CI)
```

The formatter **only changes horizontal whitespace** — indentation and
spacing. It never moves code between lines, so a run of it can never
rearrange your program into something you did not write. It also re-lexes its
own output and refuses to write if the token stream changed, so a bug in it
cannot damage your file.

```cub
// before
struct Point {x:int;y:int;}
int dist( a:Point,b:Point ){
let dx=a.x-b.x;
return dx*dx;
}

// after
struct Point { x: int; y: int; }
int dist(a: Point, b: Point) {
    let dx = a.x - b.x;
    return dx * dx;
}
```

The flip side of that guarantee: if you write a whole function on one line,
the formatter leaves it on one line. It tidies what you wrote rather than
rearranging it.

## Coming from 0.4

The syntax changed in 0.5, and one tool does the whole move:

```bash
tools/migrate.py -w your/*.cb
```

| 0.4 | 0.5 |
|---|---|
| a statement ends at the end of the line | a statement ends at `;` |
| `type P = struct { x: int }` | `struct P { x: int; }` |
| `type C = enum { Red }` | `enum C { Red }` |
| `class Dog : Animal { }` | `class Dog extends Animal { }` |

Functions are unchanged: they still lead with what they give back, as
`int add(a: int, b: int)` and `void main()`.

The compiler recognises each of the old shapes and names the replacement, so
anything the tool misses still tells you what to write:

```
old.cb:1:1: error: Cub declares a type with `struct`, not `type`
     1 | type Point = struct { x: int, y: int }
       | ^
  help: write `struct Point { x: int; y: int; }`
```

---

## Commands

```
cubc program.cb              compile to ./program
cubc program.cb -o name      choose the output name
cubc run program.cb          compile and run, leaving nothing behind
cubc --check program.cb      check for errors only
cubc program.cb --emit-c     write standalone C99
cubc program.cb --keep-c     compile, and keep the C
cubc -v program.cb           show each step
cubc fmt program.cb          format it
cubc fmt -w program.cb       format it in place
cubc doc program.cb          write a reference from its /// comments
```

---

## License

MIT. See [LICENSE](LICENSE).
