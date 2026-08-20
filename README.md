# Cub

**A small language in the C family that tells you what went wrong.**

Cub keeps what makes C good — static types, structs, predictable machine
behavior, no runtime to install — and drops what makes it painful. No header
files. No pointers. No undefined behavior at the edges. No error message that
sends you to a search engine.

It compiles to standalone C99, so a Cub program runs anywhere C runs.

```cub
class Task {
    title: string
    done: bool

    void init(title: string) {
        self.title = title
    }

    string to_string(){
        return "{if self.done { "[x]" } else { "[ ]" }} {self.title}"
    }
}

void main() {
    let tasks = [Task("write a language"), Task("write the docs")]
    tasks[0].done = true

    for task in tasks {
        print(task)
    }
    print("{len(tasks)} tasks")
}
```

```
cubc run tasks.cub
```

---

## Build it

Nothing to install beyond a C compiler and `make`.

```bash
make
make test          # 48 tests: programs, compile errors, runtime failures
make examples      # run everything in examples/
sudo make install  # optional: puts cubc in /usr/local/bin
make install-vscode   # optional: editor support
```

Then:

```bash
./cubc run examples/fizzbuzz.cub
```

---

## What it looks like

**Text that reads like text.**

```cub
let name = "Ada"
print("Hello, {name}. Next year you turn {age + 1}.")
```

**Errors written for a person.**

```
sum.cub:2:15: error: cannot add int and string
     2 |     let n = 1 + "two"
       |               ^
  help: turn the other side into text with `str(x)`, or write "...{value}..."
```

```
count.cub:3:5: error: `count` was declared with `let`, so it never changes
  help: declare it with `var count = ...` if it needs to change
```

Misspell a name and it guesses what you meant. Forget a struct field and it
names the one you forgot. Skip a `return` on one branch and it finds the
branch before the program ever runs.

**Failures that stop, and say where.**

```
Runtime error: position 5 is outside the array, whose positions are 0 to 2
  at scores.cub:4
```

Array bounds, integer division by zero, and failed conversions are all
checked. Cub has no undefined behavior at these edges — programs stop with an
explanation instead of corrupting memory.

**No ceremony.**

```cub
// no #include, no header, no forward declarations,
// no semicolons, no `int argc, char **argv`
void main() {
    print("that is the whole program")
}
```

---

## The rules

1. **Nothing happens silently.** An `int` never becomes a `float` on its own.
   Nothing is quietly truthy. Conversions are written down.
2. **Mistakes are caught early, and explained.** Types, names, missing
   returns — checked before the program runs, in plain words.
3. **What the compiler cannot know, the program checks.** Bounds, division,
   parsing. No undefined behavior.
4. **One way to write things.** No headers, no preprocessor, no macros.
   Declaration order never matters.
5. **The output is honest C.** `--emit-c` gives you one readable C99 file
   with no dependencies.

---

## A tour in one page

```cub
// Values: let never changes, var does.
let limit = 100
var count = 0
count += 1

// Types: int, float, bool, string, [T], and your own.
let ratio: float = 1.5
var names: [string] = []

// No implicit conversion, ever.
let half = float(7) / 2.0        // 3.5

// Conditions are conditions -- there is no truthiness.
if count > 0 and count < limit {
    print("in range")
}

// Two kinds of loop.
for i in 0..5 { }                // 0 1 2 3 4
for i in 1..=5 { }               // 1 2 3 4 5
for name in names { }

// Functions: typed, in any order, checked for every return path.
float area(w: float, h: float){
    return w * h
}

// Arrays and objects are shared. Structs are copied.
type Point = struct { x: int, y: int }
type Color = enum { Red, Green, Blue }

let p = Point { x: 1, y: 2 }
print(p)                         // Point{x: 1, y: 2}
print(Color.Green)               // Green

// Maps, keyed by text or number.
var ages = ["ada": 36]
ages["grace"] = 45
print(get(ages, "nobody", 0))    // 0

// if also works as a value.
let label = if count > 0 { "some" } else { "none" }

// Classes: data with behaviour, and one parent at most.
class Animal {
    name: string
    void init(name: string) { self.name = name }
    string speak(){ return "{self.name} makes a sound" }
}

class Dog: Animal {
    void init(name: string) { super.init(name) }
    string speak(){ return "{self.name} says woof" }
}

let pets: [Animal] = [Animal("Generic"), Dog("Rex")]
for pet in pets {
    print(pet.speak())           // each one keeps its own voice
}

// A class can hold the starting point, and methods of its own.
class App {
    static int twice(n: int) { return n * 2 }

    void main() {
        print(App.twice(21))
    }
}
```

Read [the tutorial](docs/TUTORIAL.md) to learn it, or
[the reference](docs/LANGUAGE.md) for the details and every built-in.

---

## How it works

```
your.cub ──▶ lexer ──▶ parser ──▶ checker ──▶ C generator ──▶ cc ──▶ program
```

The compiler is about 7,200 lines of C99 with no dependencies. It emits one
self-contained C file — runtime included — and hands it to your system C
compiler.

| | |
|---|---|
| `src/lexer.c` | text to tokens, including `{interpolation}` splitting |
| `src/parser.c` | recursive descent; newlines end statements |
| `src/check.c` | types, names, mutability, return paths, and the messages |
| `src/codegen.c` | standalone C99 |
| `src/format.c` | `cubc fmt`, the canonical formatter |
| `editors/vscode/` | highlighting, formatting, and live errors in VS Code |
| `runtime/cub_rt.h` | text, arrays, bounds checks, and the allocation registry |
| `tests/` | programs, compile errors, and runtime failures |

Look at what your program becomes:

```bash
cubc program.cub --emit-c
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

Version 0.3.0. The language described here works, and the test suite covers
it. Not yet built: modules, optional values, generics, closures, interfaces,
and private fields.
[The reference](docs/LANGUAGE.md#19-what-cub-leaves-out-for-now) lists them
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
cubc fmt program.cub          print it, tidily formatted
cubc fmt -w program.cub       rewrite it in place
cubc fmt --check program.cub  exit non-zero if it needs formatting (for CI)
```

Because Cub ends statements at ends of lines, the formatter **only changes
horizontal whitespace** — indentation and spacing. It never moves code
between lines, so it cannot change what a program does. It also re-lexes its
own output and refuses to write if the token stream changed, so a bug in it
cannot damage your file.

```cub
// before
type   Point=struct{x:int,y:int}
int dist( a:Point,b:Point ){
let dx=a.x-b.x
return dx*dx
}

// after
type Point = struct { x: int, y: int }
int dist(a: Point, b: Point){
    let dx = a.x - b.x
    return dx * dx
}
```

The flip side of that guarantee: if you write a whole function on one line,
the formatter leaves it on one line. It tidies what you wrote rather than
rearranging it.

## Commands

```
cubc program.cub              compile to ./program
cubc program.cub -o name      choose the output name
cubc run program.cub          compile and run, leaving nothing behind
cubc --check program.cub      check for errors only
cubc program.cub --emit-c     write standalone C99
cubc program.cub --keep-c     compile, and keep the C
cubc -v program.cub           show each step
cubc fmt program.cub          format it
cubc fmt -w program.cub       format it in place
```

---

## License

MIT. See [LICENSE](LICENSE).
