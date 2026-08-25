# Changelog

## 0.9.0

Functions are values.

A function can be kept in a variable, handed to another function, and
written where a value is wanted -- which looks like a declaration with the
name left out:

    int apply(f: (int) -> int, n: int) { return f(n); }

    let double = int(x: int) { return x * 2; };
    print(apply(double, 5));            // 10

The type is written `(what goes in) -> what comes back`. The name of a
function you declared is a value too, so `apply(add_one, 5)` works.

**What a function written inline uses from around it is copied in when it is
made.** Changing one of those copies would not change the original, so Cub
refuses rather than letting it look as though it worked:

    error: `count` is copied into this function when it is made, so changing
           it here would not change the one outside
      help: hold what changes in an array or an object, which are shared
            rather than copied

Arrays, maps, and objects are shared whether captured or passed, exactly as
they are everywhere else, so a closure can still add to one. What a closure
captures goes on the heap, so it may outlive the call that made it.

**Six built-ins now take a function of yours:** `map`, `filter`, `any`,
`all`, `find_by`, and `sort_by`. `find_by` gives back a `T?`, which the
optional machinery from 0.6 already knows what to do with:

    print(find_by(nums, bool(n: int) { return n % 2 == 0; }) or -1);

`sort_by` is stable, and wants a negative number when the first item comes
first. A built-in cannot be held as a value yet; wrapping one takes a line.

## 0.8.0

Enum values can carry things, and `match` takes them back out.

    enum Shape {
        Circle(radius: float),
        Rect(width: float, height: float),
        Empty,
    }

    float area(s: Shape) {
        return match s {
            Circle(r) => 3.14159 * r * r,
            Rect(w, h) => w * h,
            Empty => 0.0,
        };
    }

What a value carries is named where the enum is declared, checked when the
value is made, and reachable only through `match` -- so there is no way to
read a `Circle`'s radius out of a `Rect`.

**Every value must be answered.** A `match` that leaves one out is a compile
error naming it, and `_` answers the rest. This is what makes an enum worth
using: add a value later and the compiler walks you to every place that has
to decide what it means.

    error: this `match` does not say what to do about `Rect`, `Empty`
      help: give them an arm, or add `_ =>` for everything else

An arm is a statement or a block, a trailing `,` is optional, and a `match`
can produce a value the way an `if` can. A `_` that could never run is an
error too.

**Equality looks at what is carried.** Two enum values are the same when
they are the same value carrying the same things. Where something carried
cannot be compared -- an array, say -- the compiler says so and points at
`match` rather than quietly comparing tags.

Enums whose values carry nothing are unchanged, and still compile to a plain
C enum. Printing a value that carries something shows what it holds:
`Circle(2.0)`.

## 0.7.0

Cub can call C.

The language compiles to C99 and always could, but there was no way to use
any of it. `extern` borrows a function from a header:

    extern "math.h" {
        float cbrt(x: float);
        float hypot(a: float, b: float);
    }

    void main() {
        print(cbrt(27.0));        // 3.0
    }

They are called like any other function and their arguments are checked the
same way. `= "name"` gives the C name when it differs, and `link "sqlite3";`
puts `-lsqlite3` on the C compiler's command line.

**The header is required.** Cub has one integer type where C has several, so
a declaration Cub guessed at would be wrong in ways nothing on this side
could catch. With the real header in scope the C compiler converts the
arguments and the result, which is what makes the call safe.

`int`, `float`, `bool`, `string`, and `void` may cross. Arrays, maps,
structs, enums, objects, `T?`, and `T!` are refused -- C has no shape for
them. Text goes out as the bytes Cub holds, NUL-terminated, and comes back
measured and copied so the runtime owns it.

Everything else in the reference describes what Cub guarantees. Across an
`extern` none of it holds, and the reference says so plainly.

## 0.6.0

Values that may not be there.

Until now a Cub program had no way to say "this might not work". Reading a
number out of text either produced a number or killed the program, and a
class field holding an object was quietly empty until something touched it.
Both are gone.

**`T?` is a `T`, or nothing. `T!` is a `T`, or a failure with a reason.**

    int? first_even(numbers: [int]) {
        for n in numbers {
            if n % 2 == 0 { return n; }
        }
        return nothing;
    }

    int! to_number(text: string) {
        let clean = trim(text);
        if len(clean) == 0 { return fail("there is nothing here"); }
        return try int(clean);
    }

Neither nests, and a plain value goes wherever either is wanted.

**Four ways in, and the compiler names them.** `or` supplies a fallback and
only works it out if it is needed. `if let` binds the value where it exists,
and can name the reason in the `else`. `try` hands a failure to the caller.
`!` insists, and stops the program with the reason if it was wrong.

    let n = to_number(line) or 0;

    if let n = to_number(line) {
        print(n);
    } else why {
        print("no: {why}");
    }

Using one as a plain value, doing arithmetic on it, or throwing away a `T!`
returned by a statement are all errors -- the last because discarding it
discards whether the work happened at all.

**The library now says which calls can fail.** `int(text)` and
`float(text)` give back `int!` and `float!`; `fs.read` gives `string!`,
`fs.read_lines` gives `[string]!`, and `fs.write`, `fs.append`, and
`fs.delete` give `void!` carrying whatever the system said. Converting a
number to another number cannot fail and has not changed.

**An object field is no longer quietly empty.** Every other type starts from
an empty value -- `0`, `""`, an empty array -- but an object has none, so a
class field holding one is now either set by `init` or declared `Engine?`:

    class Car {
        engine: Engine?;
        spare: Engine;
        void init() { self.spare = Engine(); }
    }

The runtime error "there is no Engine here yet" was the old way of finding
this out. It is a compile error now.

### Upgrading

Existing code keeps working except where it used one of the calls above.
Each one is a compile error naming the fix; adding `!` reproduces the old
behaviour exactly, and `or` or `if let` is usually what you actually wanted.

## 0.5.1

Two edges that used to be undefined are now checked.

**Integer overflow stops the program.** `+`, `-`, `*`, and negation on an
`int` were emitted as plain C operators, where signed overflow is undefined
behaviour -- so a program could wrap silently, and an optimiser was entitled
to delete an overflow check written in Cub. Every one of them is now checked,
the same way division by zero already was:

    Runtime error: 9223372036854775807 + 1 does not fit in an int;
    an int runs from -9223372036854775808 to 9223372036854775807
      at counter.cb:3

`int(1e300)` and other out-of-range float conversions are checked too, and
cubc passes `-fwrapv` to the C compiler so the generated code cannot be
optimised on the assumption that overflow never happens.

On GCC and Clang the checks compile to the add-and-check-the-flag pair the
hardware already provides. An arithmetic-saturated loop runs about 40%
slower; anything that also touches text, arrays, or files is far less
affected.

**Running out of stack says so.** Recursion that went too deep used to kill
the process with a segmentation fault and no message at all. Each function
now checks on entry that there is room beneath it:

    Runtime error: `deeper` ran out of stack space, so the calls were nested
    too deeply to finish
      at walk.cb:4

The floor comes from `getrlimit` at startup, with enough headroom left for
the message itself. The check is one comparison and measures as free.

One consequence worth knowing: a deeply recursive program that only fitted
because the optimiser flattened it will now stop instead. It was never
guaranteed to fit -- at `-O0` it would have crashed.

## 0.5.0

Statements end with a semicolon, and types declare themselves.

**Statements end at `;`.** A line break used to end a statement, which meant
the parser had to guess whether an expression had finished and the formatter
was forbidden from touching line breaks at all. Now the semicolon says where
a statement stops, whitespace means nothing to the grammar, and an
expression can be laid out however it reads best:

    if temperature > 30
        and humidity > 60
        and not raining {
        print("close the blinds");
    }

Declarations that end in a block take no semicolon, and the fields of a
struct or a class each end with one.

**`struct` and `enum` declare themselves.**

    struct Point { x: int; y: int; }
    enum Colour { Red, Green, Blue }

rather than `type Point = struct { ... }`.

**A class names its parent with `extends`.** `class Dog extends Animal`,
where it used to be `class Dog : Animal`. `:` now means one thing only: the
type of the thing named to its left.

Functions are unchanged. They still lead with what they give back, as
`int add(a: int, b: int)` and `void main()`.

**Every 0.4 shape is recognised and named.** The compiler does not report a
puzzling token when it meets old code; it says what the line should be:

    old.cb:1:1: error: Cub declares a type with `struct`, not `type`
         1 | type Point = struct { x: int, y: int }
           | ^
      help: write `struct Point { x: int; y: int; }`

**`tools/migrate.py` does the move for you.** It parses the old grammar and
edits by offset, so comments, blank lines, and the spelling of literals come
through untouched:

    tools/migrate.py -w src/*.cb

The whole of `examples/`, `exercises/`, and `tests/` went through it, and
every program produces byte-identical output to the version before the
change.

## 0.4.2

Fixes a build failure on Linux.

`nanosleep`, `clock_gettime`, and `struct timespec` are POSIX rather than
ISO C. Because cubc compiles the C it emits with `-std=c99`, glibc hid
them, and any program using `os.sleep_ms` or `os.clock_ms` failed to
build with "storage size of 'ts' isn't known". macOS headers expose them
regardless, which is why this was not caught sooner.

The generated runtime now asks for POSIX before it includes anything, and
falls back to the Windows equivalents where those calls do not exist. The
compiler's own sources do the same for `isatty` and `getpid`.

Also fixes a misleading-indentation warning that gcc reports and clang
does not, and adds `make check-linux`, which builds and tests the whole
project under glibc in a container so this class of slip is caught
without needing a Linux machine.

## 0.4.0

Imports and documentation comments.

**`import` brings in the standard library.** The everyday built-ins stay
global; anything that reaches outside the program now lives in a module:

    import os
    import fs
    import math
    import rand

    void main() {
        print(os.platform())
        print(math.round(math.PI))
        if fs.exists("notes.txt") { print(fs.read("notes.txt")) }
    }

Calling one of these globally is an error that names the module and the new
spelling, so an older program is told what to change.

**`import` also brings in your own files.** `import "shapes.cb"` reads a
path relative to the importing file, and a file is read once however many
times it is asked for. Errors name the file the code was written in, not the
one on the command line. Only the file handed to `cubc` may declare `main`.

**Doc comments.** `///` documents whatever follows -- functions, classes,
methods, fields, structs, enums. `cubc doc` turns them into a markdown
reference, and everything else ignores them.

Also: `os.platform()`, `cubc fmt` on several files at once, and `void` now
prints as `void` rather than `nothing`.

Tests grew from 48 to 52.

## 0.3.0

C-style declarations, and classes that can start a program.

**Functions lead with what they give back**, the way C does. `fn` and `->`
are gone:

    int add(a: int, b: int) { return a + b }
    void greet(name: string) { print("hello {name}") }
    [string] tags(item: Item) { return item.tags }

`void` means a function gives nothing back, so it means in Cub what it means
in C. Writing `fn` now produces an error that shows the new form, and so does
a trailing `-> type`.

**Static methods.** A method marked `static` belongs to the class rather than
to an object, has no `self`, and is called through the class name:

    class MathKit {
        static int double(n: int) { return n * 2 }
    }

    print(MathKit.double(21))

**A class can hold the entry point.** `main` may be an ordinary method, in
which case Cub makes one object and runs it, or `static`, in which case it
does not. A program still needs exactly one `main`; two of them is an error
that names both.

This is a breaking change. Every `fn` in an existing program becomes the
type it returns, or `void`.

Tests grew from 43 to 48.

## 0.2.0

Objects, maps, and a standard library worth the name.

**Classes.** Fields and methods, `init` constructors, `self`, and single
inheritance with `super`. Methods are replaceable by a subclass and dispatch
on the real type, so a `Dog` held as an `Animal` still barks. Objects are
shared like arrays, unlike structs, which stay values. A class that declares
`to_string` controls how `print` shows it.

**Maps.** `[K: V]`, keyed by text or whole numbers, written `["ada": 36]` or
`[:]` when empty. Reading a key that is not there stops the program, the same
way an array index does; `has` and `get` are there when a key is optional.

**`if` as an expression.** `let label = if n > 0 { "some" } else { "none" }`.
Both branches must produce the same type, and the `else` is required.

**The standard library grew from 43 built-ins to 96**, adding maps, the rest
of the maths functions, the constants (`PI`, `TAU`, `E`, `INF`, `INT_MAX`),
text padding and classification, array `sum`/`min_of`/`index_of`/`concat`,
file helpers, and access to the command line and environment.

Tests grew from 30 to 43. Everything stays clean under AddressSanitizer and
UndefinedBehaviorSanitizer, with no leaks.

## 0.1.0

First release.

The language: `let`/`var` with type inference, `int` `float` `bool` `string`,
arrays, structs, enums, functions, `if`/`while`/`for`, and string
interpolation. No implicit conversions, no truthiness, no pointers, no
headers.

The compiler: about 4,000 lines of C99 with no dependencies, emitting one
standalone C99 file per program.

Also shipped: `cubc fmt`, a formatter that only ever changes horizontal
whitespace, and a VS Code extension with highlighting, live error checking,
and formatting.
