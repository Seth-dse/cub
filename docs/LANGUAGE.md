# The Cub Language Reference

Version 0.5.0

Cub is a small statically typed language in the C family. It compiles to
standalone C99, so a Cub program runs anywhere a C compiler runs and starts
up as fast as one.

This document is the reference. If you are new to Cub, read
[TUTORIAL.md](TUTORIAL.md) first and come back here for the details.

---

## 1. The rules Cub plays by

Every design decision in Cub follows from five rules.

**1. Nothing happens silently.** An `int` never becomes a `float` on its own.
A value is never quietly truthy. If a conversion should happen, you write it.

**2. Mistakes are caught early, and explained.** Anything the compiler can
know, it checks: types, names, missing returns, unreachable fields. Every
error names the problem in plain words and suggests a fix.

**3. What the compiler cannot know, the program checks.** Array indexes are
bounds-checked. Integer division by zero stops the program with a message
naming the file and line. Cub has no undefined behavior at these edges.

**4. There is one way to write things.** No header files, no forward
declarations, no macros, no preprocessor. Functions see each other regardless
of order.

**5. The output is honest C.** `cubc --emit-c` gives you a single readable
C99 file with no dependencies. Nothing is hidden in a runtime you cannot read.

---

## 2. Program structure

A file holds declarations in any order: functions, types, and globals.
Execution starts at `main`.

```cub
// Globals are declared outside any function.
let GREETING = "hello";

struct Point { x: int; y: int; }

fn main() {
    print(GREETING);
}
```

`main` takes no arguments. It may return nothing, or return an `int` used as
the process exit code.

```cub
fn main(): int {
    return 0;
}
```

There is no `#include` and no header. A program may span several files, and
`import` is how they meet — see [section 14](#14-imports).

---

## 3. Comments

```cub
// A line comment.

/* A block comment,
   /* which may nest, */
   so commenting out a region always works. */
```

Three slashes document whatever follows. `cubc doc` turns these into a
reference, and everything else ignores them:

```cub
/// Adds two whole numbers together.
///
/// Returns their sum.
fn add(a: int, b: int): int {
    return a + b;
}

/// A creature that can make a sound.
class Animal {
    /// What the creature is called.
    name: string;
}
```

Doc comments attach to functions, classes, methods, fields, structs, and
enums. Anywhere else they are just comments.

---

## 4. Statements and semicolons

Every statement ends with a semicolon.

```cub
let a = 1;
let b = 2;
print(a + b);
```

Because the semicolon says where a statement stops, line breaks mean nothing
to the compiler. Write an expression across as many lines as it reads best
on:

```cub
let total = 1 +
            2 +
            3;

let items = [
    10,
    20,
];

if temperature > 30
    and humidity > 60
    and not raining {
    print("close the blinds");
}
```

Declarations that end in a block — `fn`, `class`, `struct`, `enum`, and the
body of an `if`, `while`, or `for` — end at their closing brace and take no
semicolon:

```cub
fn twice(n: int): int {
    return n * 2;
}                       // no semicolon here

for i in 0..3 {
    print(i);
}                       // nor here
```

The fields of a struct or a class are declarations too, so each one ends with
a semicolon:

```cub
struct Point {
    x: int;
    y: int;
}
```

A trailing comma is allowed in every list: arrays, arguments, parameters, and
enum values.

---

## 5. Types

Cub has six kinds of type.

| Type | Meaning | Example |
|---|---|---|
| `int` | 64-bit signed whole number | `42`, `-7`, `1_000_000`, `0xff` |
| `float` | 64-bit floating point | `3.14`, `1e3`, `2.0` |
| `bool` | a condition | `true`, `false` |
| `string` | immutable UTF-8 text | `"hello"` |
| `[T]` | an array of `T` | `[1, 2, 3]`, `[[1], [2]]` |
| `[K: V]` | a map from `K` to `V` | `["ada": 36]`, `[:]` |
| `struct` / `enum` | data you declare | `Point { x: 1, y: 2 }`, `Color.Red` |
| `class` | an object with methods | `Dog("Rex")` |

Underscores may be used to group digits: `1_000_000`. Hexadecimal literals
are written `0xff`.

There is no implicit conversion between any two types, including `int` and
`float`. Convert explicitly:

```cub
let n = 7;
let half = float(n) / 2.0;      // 3.5
let back = int(half);           // 3, rounded toward zero
```

---

## 6. Variables

`let` binds a name once. `var` binds a name that can be reassigned.

```cub
let limit = 100;        // never changes
var count = 0;          // may change
count += 1;
```

The type is inferred from the value. Write it explicitly when you want to,
and when the value alone is not enough to decide:

```cub
let ratio: float = 1.5;
var names: [string] = [];      // an empty array needs a type
```

**Scope.** A variable is visible from its declaration to the end of the
enclosing block. An inner block may shadow an outer name; the inner one is a
separate variable.

```cub
let x = 1;
{
    let x = x + 10;     // a new variable, built from the outer one
    print(x);           // 11
}
print(x);               // 1
```

Redeclaring a name *in the same block* is an error, because that is almost
always a mistake rather than an intention.

---

## 7. Operators

From tightest to loosest binding:

| Level | Operators | Notes |
|---|---|---|
| 1 | `f(x)` `a[i]` `a.b` | call, index, field |
| 2 | `-x` `not x` / `!x` | unary |
| 3 | `*` `/` `%` | `%` works on `int` and `float` |
| 4 | `+` `-` | `+` also joins two strings |
| 5 | `<` `<=` `>` `>=` | numbers and text |
| 6 | `==` `!=` | any two values of the same type |
| 7 | `and` / `&&` | short-circuits |
| 8 | `or` / `\|\|` | short-circuits |

`and`/`or`/`not` and `&&`/`||`/`!` are the same operators spelled two ways.
Use whichever reads better.

Assignment is a statement, not an expression, so `if x = 1` cannot be written
by mistake. The compound forms are `+=`, `-=`, `*=`, `/=`, `%=`.

Conditions must be `bool`. There is no truthiness:

```cub
if count > 0 { }     // yes
if count { }         // error: `if` needs a condition, but this is int
```

Comparison rules:

- `==` and `!=` work on `int`, `float`, `bool`, `string`, and enums.
- `<` `<=` `>` `>=` work on `int`, `float`, and `string` (lexicographic).
- Arrays and structs cannot be compared yet; compare their parts.

---

## 8. Control flow

```cub
if temperature > 30 {
    print("hot");
} else if temperature > 15 {
    print("mild");
} else {
    print("cold");
}
```

Braces are always required; the condition needs no parentheses.

```cub
while queue_size > 0 {
    queue_size -= 1;
}
```

An `if` can also *produce* a value, in which case the `else` is required and
both branches hold one expression:

```cub
let label = if score >= 90 { "A" } else if score >= 80 { "B" } else { "C" };
```

The two forms are the same keyword doing two jobs: as a statement it chooses
what to *do*, as an expression it chooses what a value *is*. Both branches
must produce the same type.

Two kinds of `for` loop:

```cub
for i in 0..5 { }        // 0 1 2 3 4   -- the end is excluded
for i in 0..=5 { }       // 0 1 2 3 4 5 -- the end is included

for name in names { }    // each item of an array, in order
```

The loop variable is a fresh, immutable binding on each pass. `break` and
`continue` work as in C.

Iterating text goes through positions, because a byte is not a character:

```cub
for i in 0..len(word) {
    print(char_at(word, i));
}
```

---

## 9. Functions

```cub
fn area(width: float, height: float): float {
    return width * height;
}

fn shout(message: string) {        // no `: type`, so it gives nothing back
    print(upper(message));
}
```

A function reads in the order you would say it out loud: the name, what goes
in, then what comes back. Every parameter is typed, written `name: type`, and
the return type is written the same way after the parameters. Leave it off
and the function gives nothing back — there is no `void` to write.

Declaration order does not matter — functions may call each other freely,
including mutual recursion:

```cub
fn is_even(n: int): bool {
    if n == 0 { return true; }
    return is_odd(n - 1);
}

fn is_odd(n: int): bool {
    if n == 0 { return false; }
    return is_even(n - 1);
}
```

A function that declares a return type must return on every path. The
compiler proves this before your program runs:

```
error: `biggest` must return int on every path
  help: add a `return` at the end, or an `else` branch that returns
```

Parameters are immutable inside the function. Cub has no function values,
closures, or generics yet.

---

## 10. Text

Text is immutable. `+` joins, and `{ }` inserts a value:

```cub
let name = "Ada";
let age = 36;
print("Hello, {name}. You are {age}.");
print("Next year: {age + 1}");          // any expression fits
print("Loud: {upper(name)}");
```

Interpolation calls `str` on whatever you insert, so it works for every type,
including arrays and structs.

Escapes: `\n` `\t` `\r` `\0` `\\` `\"` `\{` `\}`.

```cub
print("a real brace: \{ }");
```

`len` returns the length in bytes. For ASCII that is the number of
characters; for other UTF-8 text it is not.

---

## 11. Arrays

An array holds any number of values of one type, and grows on demand.

```cub
var scores = [10, 20, 30];
push(scores, 40);
print(scores[0]);          // 10
print(len(scores));        // 4
scores[1] = 25;
```

**Arrays are shared, not copied.** Assigning an array, or passing one to a
function, gives you another name for the same array — the same model as
Python lists or Java arrays.

```cub
var a = [1, 2];
let b = a;
push(b, 3);
print(a);              // [1, 2, 3] -- a and b are the same array
```

This is why `let` on an array still allows the contents to change. `let`
promises the *name* keeps pointing at the same array, not that the array
stops changing:

```cub
let fixed = [1, 2, 3];
fixed[0] = 9;          // fine: the contents may change
push(fixed, 4);        // fine
fixed = [5, 6];        // error: `fixed` was declared with `let`
```

**Every index is checked.** Reading or writing outside an array stops the
program with a message rather than corrupting memory:

```
Runtime error: position 5 is outside the array, whose positions are 0 to 2
  at scores.cb:4
```

---

## 12. Structs

A struct groups named fields into one value.

```cub
struct Point {
    x: int;
    y: int;
}

let origin = Point { x: 0, y: 0 };
print(origin.x);
```

A struct that fits on one line may stay on one line:

```cub
struct Point { x: int; y: int; }
```

Every field must be given a value when you build one — there is no partially
initialized struct. The compiler names any field you forget.

**Structs are values, not references.** Assigning one copies it:

```cub
var a = Point { x: 1, y: 2 };
var b = a;
b.x = 99;
print(a.x);          // 1 -- b is a separate copy
```

To change a struct's fields, the variable must be a `var`. Structs may
contain other structs and arrays, and printing one shows its contents:

```cub
print(origin);       // Point{x: 0, y: 0}
```

A struct cannot contain itself directly, because that value would have no
size. Hold it in an array instead — the compiler tells you so:

```cub
struct Node {
    value: int;
    children: [Node];     // fine: an array is a reference
}
```

---

## 13. Enums

An enum is a fixed set of named values.

```cub
enum Status { Todo, Doing, Done }

let state = Status.Doing;
if state == Status.Done {
    print("finished");
}
print(state);                 // Doing
```

Enum values are always written with the type name in front, so it is obvious
where a name comes from. Printing an enum gives its name.

---

## 14. Imports

`import` does two jobs: it brings in part of the standard library, and it
brings in another one of your files.

### Modules

Everyday built-ins — `print`, `len`, `push`, `str`, the text and array
functions — are always there. Anything that reaches outside the program, or
belongs to a recognisable corner of the library, lives in a module:

```cub
import os;
import fs;
import math;
import rand;

fn main() {
    print(os.platform());              // "macos", "linux", "windows"
    print(math.round(math.PI));
    if fs.exists("notes.txt") {
        for line in fs.read_lines("notes.txt") {
            print(line);
        }
    }
    print(rand.int(1, 6));
}
```

| Module | Holds |
|---|---|
| `os` | `args` `env` `exit` `platform` `sleep_ms` `clock_ms` `time_ms` |
| `fs` | `read` `write` `append` `exists` `delete` `read_lines` |
| `math` | `sqrt` `pow` `floor` `ceil` `round` `sin` `cos` `tan` `asin` `acos` `atan` `atan2` `log` `log10` `exp` `is_nan` `is_inf`, and `PI` `TAU` `E` `INF` `NAN` |
| `rand` | `int` `float` `seed` |

Reaching for one you have not imported, or calling a module function as if
it were global, is an error that names the fix:

```
error: `sqrt` lives in the `math` module
  help: add `import math;`, then write `math.sqrt(...)`
```

### Your own files

```cub
import "shapes.cb";
import "lib/util.cb";
```

The path is taken relative to the file doing the importing. Everything the
imported file declares — functions, classes, types, globals — becomes
available, and a file is read once however many times it is asked for, so
two files importing the same third one is fine.

Only the file you hand to `cubc` may declare `main`. There is no privacy
yet: an imported file shares everything it declares.

---

## 15. Maps

A map holds values under keys. Keys are `string` or `int`; values are any
type.

```cub
var ages = ["ada": 36, "alan": 41];
ages["grace"] = 45;          // adds it
ages["ada"] += 1;            // changes it

print(ages["ada"]);          // 37
print(len(ages));            // 3
print(has(ages, "alan"));    // true
print(get(ages, "nobody", 0));   // 0 -- the fallback
remove(ages, "alan");
```

An empty map is written `[:]`, and needs a type:

```cub
var counts: [string: int] = [:];
var byId: [int: [string]] = [:];
```

Reading a key that is not there stops the program, the same way an array
index does. Use `has` to check, or `get` to supply a fallback:

```
Runtime error: this map has no key "zzz"
  at counts.cb:7
```

To walk a map, walk its keys. The order is not meaningful, so sort them when
you want a stable listing:

```cub
var names = keys(ages);
sort(names);
for name in names {
    print("{name} is {ages[name]}");
}
```

Like arrays, maps are shared rather than copied.

---

## 16. Classes

A `struct` is data. A **class** is data with behaviour: fields, methods, and
the option to build on another class.

```cub
class Animal {
    name: string;

    fn init(name: string) {
        self.name = name;
    }

    fn speak(): string {
        return "{self.name} makes a sound";
    }
}

let a = Animal("Generic");
print(a.speak());
```

The class name is also how you make one: `Animal("Generic")` runs `init`.
Inside a method, `self` is the object. Cub never fills `self` in for you --
`self.name` and `self.speak()` are always written out, so you can always see
where a name comes from.

**Objects are shared, like arrays.** Assigning one, or passing it to a
function, gives another name for the same object. `==` asks whether two names
refer to the same object, not whether their contents match.

**Fields start from a known value** -- `0`, `""`, an empty array -- so an
object is never full of rubbish. A field that holds another object starts
*empty*, and reading it before it is set stops the program:

```
Runtime error: there is no Engine here yet
  at car.cb:4
```

### Building on another class

```cub
class Dog extends Animal {
    tricks: [string];

    fn init(name: string) {
        super.init(name);          // set up the Animal part first
    }

    fn speak(): string {       // replaces Animal's version
        return "{self.name} says woof";
    }

    fn learn(trick: string) {
        push(self.tricks, trick);
    }
}
```

A `Dog` may be used wherever an `Animal` is expected, and it keeps its own
behaviour when it gets there:

```cub
let pets: [Animal] = [Animal("Generic"), Dog("Rex")];
for p in pets {
    print(p.speak());        // "Generic makes a sound", then "Rex says woof"
}
```

That is the whole point of building on a class: the code holding an `Animal`
does not need to know which kind it has.

Rules the compiler enforces:

- A method that replaces another must take the same arguments and return the
  same type. Getting it wrong is an error, not a silently separate method.
- If the class you build on has an `init`, yours must call `super.init(...)`.
- `super.speak()` calls the version you replaced.
- A field cannot share a name with an inherited field, or with a method.

### Methods that belong to the class

A method marked `static` belongs to the class rather than to any object. It
has no `self`, and you call it through the class name:

```cub
class MathKit {
    static fn double(n: int): int {
        return n * 2;
    }
}

print(MathKit.double(21));       // 42
```

Reaching for `self` inside one is an error, and so is calling a static
method through an object — the compiler tells you which form to use.

### Starting from a class

A program starts at `main`. That `main` can be a plain function, or it can
live inside a class:

```cub
class App {
    greeting: string;

    fn init() {
        self.greeting = "hello";
    }

    fn main() {
        print(self.greeting);
    }
}
```

When `main` is an ordinary method, Cub makes one object and runs `main` on
it, so `init` must take no arguments. Mark it `static` if you would rather
no object were made:

```cub
class Program {
    static fn main() {
        print("started");
    }
}
```

A program needs exactly one starting point. Two `main`s — whether in two
classes or in a class and at the top level — is an error that names both.

### Printing an object

If a class declares `fn to_string(): string`, then `print` and `str` use it.
Otherwise you get every field, inherited ones first:

```cub
print(dog);      // Dog{name: "Rex", tricks: ["sit"]}
```

### Classes and structs, side by side

| | `struct` | `class` |
|---|---|---|
| holds | fields | fields and methods |
| assigning it | copies | shares |
| can build on another | no | yes |
| methods on the type itself | no | `static` |
| made with | `Point { x: 1 }` | `Dog("Rex")` |
| `==` | not yet | same object? |

Reach for a `struct` when you want a value, like a point or a colour. Reach
for a `class` when the thing has behaviour, or when sharing it is the point.

---

## 17. Errors

**Compile-time errors** point at the exact spot, explain the problem in
words, and suggest a fix:

```
sum.cb:2:15: error: cannot add int and string
     2 |     let n = 1 + "two"
       |               ^
  help: turn the other side into text with `str(x)`, or write "...{value}..."
```

Misspelled names get a suggestion:

```
  help: did you mean `total`?
```

**Runtime errors** stop the program, name the file and line, and exit with
status 70. These are the failures no compiler can predict:

- indexing outside an array or past the end of text
- `pop` on an empty array
- integer division or remainder by zero
- `int("twelve")` and other failed conversions
- a file that cannot be opened
- `panic(message)` and a failed `assert`

```cub
assert(age >= 18, "you must be 18 or older");
panic("this should be unreachable");
```

---

## 18. The standard library

Ninety-seven built-ins. The everyday ones are always in scope; the rest live
in a module you `import` (see [section 14](#14-imports)) and are marked here
with the module they belong to.

### Output and input

| Call | Result | Notes |
|---|---|---|
| `print(values...)` | nothing | joins the values, then a newline |
| `write(values...)` | nothing | the same, without the newline |
| `eprint(values...)` | nothing | writes to the error stream instead |
| `input()` | `string` | reads one line, without the newline |

### Text

| Call | Result |
|---|---|
| `len(text)` | `int`, the length in bytes |
| `upper(t)` / `lower(t)` / `capitalize(t)` | `string` |
| `trim(t)` / `trim_start(t)` / `trim_end(t)` | `string` |
| `pad_start(t, width, fill)` / `pad_end(t, width, fill)` | `string` |
| `find(t, part)` / `index_of(t, part)` | `int`, the position, or `-1` |
| `last_index_of(t, part)` | `int` |
| `count(t, part)` | `int`, how many times it appears |
| `contains(t, part)` | `bool` |
| `starts_with(t, part)` / `ends_with(t, part)` | `bool` |
| `replace(t, from, to)` | `string`, every occurrence |
| `slice(t, from, to)` | `string`, positions clamped to the ends |
| `repeat(t, times)` | `string` |
| `split(t, separator)` | `[string]`; an empty separator splits into characters |
| `lines(t)` / `chars(t)` | `[string]` |
| `join(parts, separator)` | `string` |
| `char_at(t, i)` / `code_at(t, i)` | one-byte `string` / `int` |
| `from_code(n)` | `string` of one byte |
| `is_digit(t)` `is_alpha(t)` `is_alnum(t)` | `bool`, true when every byte qualifies |
| `is_space(t)` `is_upper(t)` `is_lower(t)` | `bool` |

### Arrays

| Call | Result |
|---|---|
| `len(a)` | `int` |
| `push(a, v)` / `pop(a)` | append / take the last one off |
| `insert(a, i, v)` / `remove(a, i)` | nothing |
| `slice(a, from, to)` / `copy(a)` / `concat(a, b)` | a new array |
| `contains(a, v)` / `index_of(a, v)` / `count(a, v)` | `bool` / `int` / `int` |
| `sort(a)` / `reverse(a)` / `shuffle(a)` | nothing; in place |
| `swap(a, i, j)` / `clear(a)` | nothing; in place |
| `sum(a)` | `int` or `float` |
| `min_of(a)` / `max_of(a)` | one item, from `[int]` `[float]` `[string]` |

### Maps

| Call | Result |
|---|---|
| `len(m)` | `int` |
| `has(m, key)` | `bool` |
| `get(m, key, fallback)` | the value, or the fallback |
| `remove(m, key)` / `clear(m)` | nothing |
| `keys(m)` / `values(m)` | an array, in no particular order |

### Numbers

| Call | Result |
|---|---|
| `abs(n)` / `sign(n)` | same type / `int` |
| `min(a, b)` / `max(a, b)` | same type; both sides must match |
| `clamp(v, lo, hi)` | same type |
| `math.sqrt` `floor` `ceil` `round` `exp` `log` `log10` | `float` |
| `math.sin` `cos` `tan` `asin` `acos` `atan` | `float` |
| `math.pow(base, exp)` / `math.atan2(y, x)` | `float` |
| `math.is_nan(f)` / `math.is_inf(f)` | `bool` |
| `rand.int(lo, hi)` | `int`, inclusive at both ends |
| `rand.float()` | `float` from 0 up to but not including 1 |
| `rand.seed(n)` | nothing; makes the random numbers repeatable |

Constants: `INT_MAX` and `INT_MIN` are always there. `math.PI`, `math.TAU`,
`math.E`, `math.INF`, and `math.NAN` come with `import math`.

### Conversion

| Call | Result |
|---|---|
| `str(value)` | `string`, for any type |
| `int(value)` | `int`, from `float`, `string`, or `bool` |
| `float(value)` | `float`, from `int` or `string` |

### Files

All of these need `import fs`.

| Call | Result |
|---|---|
| `fs.read(path)` | `string` |
| `fs.read_lines(path)` | `[string]` |
| `fs.write(path, text)` / `fs.append(path, text)` | nothing |
| `fs.exists(path)` | `bool` |
| `fs.delete(path)` | nothing |

### The world outside

All of these need `import os`.

| Call | Result |
|---|---|
| `os.args()` | `[string]`, the command line, program name first |
| `os.env(name)` | `string`, empty when it is not set |
| `os.platform()` | `string`: `macos`, `linux`, `windows`, or `unknown` |
| `os.exit(code)` | never returns |
| `os.clock_ms()` | `int`, wall-clock milliseconds |
| `os.time_ms()` | `int`, processor milliseconds used |
| `os.sleep_ms(n)` | nothing |

### Stopping on purpose

| Call | Result |
|---|---|
| `assert(condition)` / `assert(condition, message)` | stops if false |
| `panic(message)` | stops the program |

You cannot define a function with a built-in's name; the compiler says so
rather than silently shadowing it.

---

## 19. How memory works

Cub has no manual memory management and no pointers. Text and arrays live on
the heap; the runtime records every allocation and releases all of it when
the program ends. A Cub program cannot leak, double-free, or use freed
memory — verified under AddressSanitizer and UndefinedBehaviorSanitizer.

The honest limitation: memory a long-running program stops using is not
reclaimed until the program exits. For scripts, tools, and batch programs
this is exactly right and costs nothing. For a server meant to run for weeks
it is not enough — reference counting is the planned next step, and the
allocation registry is already the hook for it.

---

## 20. What Cub leaves out, for now

Left out deliberately, and not missed: pointers, pointer arithmetic, manual
`malloc`/`free`, header files, the preprocessor, `goto`, implicit
conversions, truthiness, uninitialized variables, and undefined behavior.

Genuinely missing, and planned:

- **Private declarations** — an imported file shares everything it declares.
- **Renaming on import** — no `import os as system` yet.
- **Optional values** — no way yet to express "a `Point`, or nothing". Class
  fields fill the gap awkwardly, by starting empty and being checked on use.
- **Generics** — arrays and maps are the only generic types.
- **Closures and function values** — functions cannot yet be passed around,
  so there is no `map`/`filter`/`sort_by`.
- **Interfaces** — a class can build on one parent; there is no way to say
  "anything with an `area()`" without a shared parent.
- **Asking an object what it is** — no `is` test and no downcast, so once a
  `Dog` is held as an `Animal` you can call it but not narrow it back.
- **Private fields** — every field is readable and writable from anywhere.
- **Reference counting** — see section 19.
- **`==` on structs, arrays, and maps** — compare the parts for now.

---

## 21. Grammar

```ebnf
program     = { import | declaration } ;
import      = "import" ( IDENT | STRING ) ";" ;
declaration = function | classdecl | structdecl | enumdecl | binding ";" ;

function    = [ "static" ] "fn" IDENT "(" [ params [ "," ] ] ")"
              [ ":" type ] block ;
params      = param { "," param } ;
param       = IDENT ":" type ;

classdecl   = "class" IDENT [ "extends" IDENT ] "{" { member } "}" ;
member      = function | field ;
field       = IDENT ":" type ";" ;

structdecl  = "struct" IDENT "{" { field } "}" ;
enumdecl    = "enum" IDENT "{" [ IDENT { "," IDENT } [ "," ] ] "}" ;

binding     = ( "let" | "var" ) IDENT [ ":" type ] "=" expr ;

type        = "int" | "float" | "bool" | "string"
            | "[" type "]" | "[" type ":" type "]" | IDENT ;

block       = "{" { statement } "}" ;
statement   = binding ";" | assignment ";" | expr ";"
            | ifstmt | whilestmt | forstmt
            | "return" [ expr ] ";" | "break" ";" | "continue" ";"
            | block ;

assignment  = expr ( "=" | "+=" | "-=" | "*=" | "/=" | "%=" ) expr ;
ifstmt      = "if" expr block [ "else" ( ifstmt | block ) ] ;
whilestmt   = "while" expr block ;
forstmt     = "for" IDENT "in" ( expr ( ".." | "..=" ) expr | expr ) block ;

expr        = or ;
or          = and { ( "or" | "||" ) and } ;
and         = equality { ( "and" | "&&" ) equality } ;
equality    = comparison { ( "==" | "!=" ) comparison } ;
comparison  = sum { ( "<" | "<=" | ">" | ">=" ) sum } ;
sum         = product { ( "+" | "-" ) product } ;
product     = unary { ( "*" | "/" | "%" ) unary } ;
unary       = [ "-" | "not" | "!" ] postfix ;
postfix     = primary { "(" [ args ] ")" | "[" expr "]" | "." IDENT } ;
primary     = INT | FLOAT | STRING | "true" | "false" | IDENT
            | "self" | "super" | ifexpr
            | "(" expr ")" | "[" [ args ] "]" | maplit
            | IDENT "{" [ inits ] "}" ;
maplit      = "[" ( ":" | expr ":" expr { "," expr ":" expr } [ "," ] ) "]" ;
ifexpr      = "if" expr "{" expr "}" "else" ( ifexpr | "{" expr "}" ) ;
```

Whitespace, including line breaks, separates tokens and means nothing else.
A `;` on its own is an empty statement and is allowed anywhere a statement
is.

---

## 22. The compiler

```
cubc program.cb              compile to ./program
cubc program.cb -o name      choose the output name
cubc run program.cb          compile and run, leaving nothing behind
cubc --check program.cb      check for errors, produce nothing
cubc program.cb --emit-c     write standalone C99 instead
cubc program.cb --keep-c     compile, and keep the generated C
cubc -v program.cb           show each step

cubc fmt program.cb          print it, tidily formatted
cubc fmt -w program.cb       format it in place
cubc fmt --check program.cb  exit non-zero if it needs formatting
cubc fmt -w a.cb b.cb       several files at once

cubc doc program.cb          write a reference from its /// comments
cubc doc program.cb -o api.md
```

### Formatting

`cubc fmt` produces one canonical layout: four-space indentation, single
spaces around binary operators, none inside brackets, one blank line kept
where you left blank lines, and comments untouched.

The formatter never moves code between lines — it rewrites horizontal
whitespace only, so formatting cannot rearrange a program into a shape you
did not write. As a second check, it re-lexes its own output and refuses to
write anything if the token stream is not identical.

This means `cubc fmt` will not break a long line, join two short ones, or
move a trailing `}` onto a line of its own. Where the lines go is yours to
decide; the spacing within them is the formatter's.

### Editors

`editors/vscode` holds a VS Code extension: syntax highlighting, snippets,
formatting through `cubc fmt`, and errors underlined as you type via
`cubc --check`. Install it with `make install-vscode`.

`cubc` compiles to C and hands it to your system C compiler (`cc`, or
whatever `$CC` names). The generated file carries its own runtime, so it
depends on nothing but libc and libm:

```
cubc program.cb --emit-c
cc -std=c99 -O2 program.c -o program -lm
```
