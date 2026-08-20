# Changelog

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
