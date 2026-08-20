# Changelog

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
