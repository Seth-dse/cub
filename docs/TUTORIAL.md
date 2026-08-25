# Learning Cub

This walks you from nothing to a working program. It assumes you have
written a little code before, in any language, but nothing about C.

Everything here is runnable. Save the code, run `cubc run whatever.cb`, and
watch what happens.

---

## 1. Hello

Put this in `hello.cb`:

```cub
void main() {
    print("Hello, world!");
}
```

Run it:

```
cubc run hello.cb
```

Every Cub program starts at `main`. Code lives inside functions; the file
itself holds only declarations.

Notice what is not there: no `#include`, no header file, no
`int argc, char **argv`, no `return 0`. What is there is the semicolon:
every statement in Cub ends with one, and the compiler says so plainly when
you forget.

---

## 2. Values and names

```cub
void main() {
    let name = "Ada";
    let age = 36;

    print("Hello, {name}. You are {age}.");
}
```

`let` names a value. The braces in the text insert a value — anything can go
inside them, including arithmetic:

```cub
print("Next year you will be {age + 1}.");
```

`let` names never change. When you need one that does, use `var`:

```cub
void main() {
    var count = 0;
    count += 1;
    count += 1;
    print("count is {count}");      // count is 2
}
```

Try changing a `let` and see what the compiler says. Cub's errors are meant
to be read:

```
error: `count` was declared with `let`, so it never changes
  help: declare it with `var count = ...;` if it needs to change
```

---

## 3. Types

Cub works out the type of a value for you, but it never changes one type
into another behind your back. This is the rule that surprises people coming
from Python or JavaScript, so it is worth meeting early:

```cub
void main() {
    let whole = 7;
    let ratio = whole / 2;         // 3, because both sides are whole numbers
    print(ratio);

    let exact = float(whole) / 2.0;
    print(exact);                  // 3.5
}
```

`7 / 2` is `3` because both sides are `int`. To get `3.5`, say so with
`float(...)`. If you mix them by accident, the compiler stops you rather
than guessing:

```
error: cannot divide int and float
  help: Cub never mixes int and float silently; use `float(x)` or `int(x)`
```

The types are `int`, `float`, `bool`, `string`, arrays written `[int]`, and
the types you declare yourself.

You can always write a type down if you want it visible:

```cub
let temperature: float = 21.5;
```

---

## 4. Deciding

```cub
void main() {
    let temperature = 22;

    if temperature > 30 {
        print("hot");
    } else if temperature > 15 {
        print("mild");
    } else {
        print("cold");
    }
}
```

No parentheses around the condition, and braces always. The condition has to
be a real condition — a number is not "true enough":

```cub
if temperature { }      // error: `if` needs a condition, but this is int
```

Write `if temperature > 0 { }`. Cub would rather ask you what you meant than
guess wrong.

Conditions combine with `and`, `or`, and `not` (`&&`, `||`, `!` mean the
same thing, if you prefer them):

```cub
if temperature > 15 and not raining {
    print("go outside");
}
```

---

## 5. Repeating

Counting loops use a range. The end is not included:

```cub
for i in 0..5 {
    print(i);               // 0 1 2 3 4
}

for i in 1..=5 {
    print(i);               // 1 2 3 4 5 -- `..=` includes the end
}
```

`while` repeats as long as something is true:

```cub
var countdown = 3;
while countdown > 0 {
    print(countdown);
    countdown -= 1;
}
print("liftoff");
```

`break` leaves a loop early and `continue` skips to the next pass.

---

## 6. Functions

```cub
int double(n: int) {
    return n * 2;
}

void announce(message: string) {
    print("** {message} **");
}

void main() {
    print(double(21));
    announce("done");
}
```

A function reads left to right in the order you would say it: `fn`, the
name, what goes in, then `: int` for what comes back. Leave that last part
off — as `announce` does — and the function gives nothing back. Parameters
are always typed, written `name: type`, the same shape as everywhere else in
the language.

Order does not matter — `main` can call a function declared below it, and two
functions can call each other. There are no forward declarations in Cub.

If a function promises to return something, it has to do so on every path.
This is checked before your program runs:

```cub
int biggest(a: int, b: int) {
    if a > b {
        return a;
    }
}
```

```
error: `biggest` must return int on every path
  help: add a `return` at the end, or an `else` branch that returns
```

---

## 7. Lists of things

An array holds many values of one type:

```cub
void main() {
    var scores = [10, 20, 30];

    push(scores, 40);              // add to the end
    print(scores);                 // [10, 20, 30, 40]
    print(len(scores));            // 4
    print(scores[0]);              // 10

    scores[0] = 15;
    sort(scores);
    print(scores);                 // [15, 20, 30, 40]
}
```

Walk one with `for ... in`:

```cub
var total = 0;
for score in scores {
    total += score;
}
print("total is {total}");
```

An empty array needs a type, because there is nothing to work it out from:

```cub
var names: [string] = [];
push(names, "Ada");
```

**Two things worth knowing.**

First, reaching outside an array stops the program instead of reading
whatever happened to be in memory:

```
Runtime error: position 5 is outside the array, whose positions are 0 to 3
  at scores.cb:8
```

Second, arrays are *shared*. Two names can refer to one array:

```cub
var a = [1, 2];
let b = a;
push(b, 3);
print(a);          // [1, 2, 3] -- a and b are the same array
```

This is why an array declared with `let` can still change its contents:
`let` promises the *name* keeps pointing at the same array.

---

## 8. Text

```cub
void main() {
    let title = "  the Cub language  ";
    let clean = trim(title);

    print(upper(clean));                   // THE CUB LANGUAGE
    print(len(clean));                     // 16
    print(replace(clean, "Cub", "cub"));
    print(contains(clean, "Cub"));         // true

    let words = split(clean, " ");
    print(words);                          // ["the", "Cub", "language"]
    print(join(words, "-"));               // the-Cub-language
}
```

Text does not change in place — every one of these gives you a new string.

To look at text one character at a time, walk the positions:

```cub
let word = "cub";
for i in 0..len(word) {
    print(char_at(word, i));
}
```

---

## 9. Your own types

Group related values into a `struct`:

```cub
struct Point {
    x: int;
    y: int;
}

int distance_squared(a: Point, b: Point) {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    return dx * dx + dy * dy;
}

void main() {
    let origin = Point { x: 0, y: 0 };
    let target = Point { x: 3, y: 4 };

    print(distance_squared(origin, target));    // 25
    print(target);                              // Point{x: 3, y: 4}
    print(target.x);                            // 3
}
```

Every field must be filled in. Leave one out and the compiler names it.

Structs are **copied** when you assign them, unlike arrays:

```cub
var a = Point { x: 1, y: 2 };
var b = a;
b.x = 99;
print(a.x);        // still 1
```

When a value is one of a fixed set of choices, use an `enum`:

```cub
enum Status { Todo, Doing, Done }

void main() {
    var state = Status.Todo;
    state = Status.Doing;

    if state == Status.Done {
        print("finished");
    } else {
        print("still {state}");     // still Doing
    }
}
```

Writing `Status.Doing` rather than a bare `Doing` means you can always see
where a name comes from.

---

## 10. Values that carry things

An enum value can carry values of its own. Say what each one carries where
the enum is declared:

```cub
enum Shape {
    Circle(radius: float),
    Rect(width: float, height: float),
    Empty,
}

void main() {
    let s = Shape.Circle(2.0);
    print(s);                    // Circle(2.0)
}
```

To get at what a value carries, use `match`:

```cub
float area(s: Shape) {
    return match s {
        Circle(r) => 3.14159 * r * r,
        Rect(w, h) => w * h,
        Empty => 0.0,
    };
}
```

Each arm names a value on the left of `=>` and what to do on the right. The
names in `Circle(r)` are yours to choose, and only exist inside that arm.

**You have to answer every value.** Leave one out and the compiler tells you
which:

```
error: this `match` does not say what to do about `Rect`, `Empty`
  help: give them an arm, or add `_ =>` for everything else
```

That is the point of `match`, and it is worth more than it looks. Add a
value to `Shape` a month from now and the compiler walks you to every place
that has to decide what the new value means — instead of the program
guessing at runtime.

Use `_` for "everything else":

```cub
match s {
    Circle(r) => print("round, {r} across"),
    _ => print("not round"),
}
```

An arm can be a block when one line is not enough, and `match` works on
plain enums too:

```cub
match light {
    Red => {
        stop();
        wait();
    }
    _ => go(),
}
```

---

## 11. Maps

An array finds things by position. A map finds them by name:

```cub
void main() {
    var ages = ["ada": 36, "alan": 41];

    ages["grace"] = 45;           // add
    ages["ada"] += 1;             // change

    print(ages["ada"]);           // 37
    print(len(ages));             // 3
    print(has(ages, "turing"));   // false
}
```

Asking for a key that is not there stops the program, just like reading past
the end of an array. When a key might be missing, say what to do instead:

```cub
print(get(ages, "turing", 0));    // 0
```

Keys can be text or whole numbers; values can be anything. An empty map is
`[:]`, and needs a type so Cub knows what will go in it:

```cub
var scores: [string: int] = [:];
```

To go through a map, go through its keys. They come out in no particular
order, so sort them when the order matters:

```cub
var names = keys(ages);
sort(names);
for name in names {
    print("{name} is {ages[name]}");
}
```

---

## 12. Functions as values

A function is a value like any other. You can keep one in a variable, hand
one to another function, or write one on the spot.

Writing one on the spot looks like a declaration with the name left out:

```cub
void main() {
    let double = int(x: int) { return x * 2; };
    print(double(21));           // 42
}
```

A function that takes one says so in its type, written
`(what goes in) -> what comes back`:

```cub
int apply(f: (int) -> int, n: int) {
    return f(n);
}

int add_one(n: int) { return n + 1; }

void main() {
    print(apply(add_one, 5));                          // 6
    print(apply(int(x: int) { return x * x; }, 5));     // 25
}
```

### It can use what is around it

```cub
void main() {
    let factor = 10;
    let scale = int(x: int) { return x * factor; };
    print(scale(3));             // 30
}
```

`factor` is **copied in** when `scale` is made. That means a function written
inline cannot change a number from outside it — and rather than letting you
write something that quietly does nothing, Cub stops you:

```
error: `count` is copied into this function when it is made, so changing it
       here would not change the one outside
```

Arrays and objects are shared rather than copied, in a closure exactly as
everywhere else, so this works:

```cub
void main() {
    var seen: [int] = [];
    let note = void(n: int) { push(seen, n); };
    note(1);
    note(2);
    print(seen);                 // [1, 2]
}
```

### Where this pays off

Six built-ins take a function and walk an array with it:

```cub
void main() {
    let nums = [5, 3, 8, 1, 9, 2];

    print(map(nums, int(n: int) { return n * 2; }));
    print(filter(nums, bool(n: int) { return n > 3; }));
    print(any(nums, bool(n: int) { return n > 8; }));
    print(find_by(nums, bool(n: int) { return n % 2 == 0; }) or -1);

    var words = ["pear", "fig", "banana"];
    sort_by(words, int(a: string, b: string) { return len(a) - len(b); });
    print(words);
}
```

`sort_by` wants a negative number when the first item comes first, a
positive one when it comes second, and `0` when they are equal.

---

## 13. Working for any type

Some functions do not care what they are working with. Taking the first item
of an array is the same job whether the array holds numbers or names:

```cub
T first<T>(items: [T]) {
    return items[0];
}

void main() {
    print(first([1, 2, 3]));       // 1
    print(first(["a", "b"]));      // a
}
```

`<T>` after the name says "T stands for a type". You never write it at the
call — Cub works it out from what you pass.

More than one name is fine:

```cub
[U] convert<T, U>(items: [T], f: (T) -> U) {
    var out: [U] = [];
    for item in items { push(out, f(item)); }
    return out;
}

void main() {
    print(convert([1, 2, 3], string(n: int) { return "n{n}"; }));
}
```

Inside the function you can do anything that does not depend on which type
`T` is: hold it, pass it, put it in an array, print it. What you cannot do is
add two of them or compare them — `T` might be a `Point`, and Cub has no way
yet to ask for "any type that can be added". It tells you at the declaration
rather than at some call:

```
error: two `T` values cannot be compared, because `T` could be anything
```

Each set of types you call it with gets its own copy of the function, with
the real types filled in, so it is as fast as one you wrote by hand.

---

## 14. Classes

A struct holds data. A **class** holds data *and* the things you can do with
it:

```cub
class Counter {
    count: int;

    void bump() {
        self.count += 1;
    }

    int value() {
        return self.count;
    }
}

void main() {
    let c = Counter();
    c.bump();
    c.bump();
    print(c.value());      // 2
}
```

`self` is the object the method was called on. Cub always makes you write it
out — `self.count`, not `count` — so you can see at a glance what belongs to
the object and what is a local.

To take values when the object is made, write an `init`:

```cub
class Animal {
    name: string;

    void init(name: string) {
        self.name = name;
    }

    string speak() {
        return "{self.name} makes a sound";
    }
}

let a = Animal("Generic");
print(a.speak());          // Generic makes a sound
```

The class name is how you make one: `Animal("Generic")` runs `init`.

### Building on a class

A class can build on another, keeping its fields and methods and replacing
the ones it wants to do differently:

```cub
class Dog extends Animal {
    void init(name: string) {
        super.init(name);         // set up the Animal part
    }

    string speak() {       // Dog's own version
        return "{self.name} says woof";
    }
}
```

Now the useful part. A `Dog` can go anywhere an `Animal` is expected, and it
still behaves like a dog:

```cub
let pets: [Animal] = [Animal("Generic"), Dog("Rex")];
for p in pets {
    print(p.speak());
}
```

```
Generic makes a sound
Rex says woof
```

The loop does not know or care which kind of animal it has. That is what
building on a class buys you.

The compiler holds you to the deal: a replaced method must take the same
arguments and return the same type, and if the class you build on has an
`init`, yours has to call `super.init(...)`. Forget, and you are told:

```
error: `Dog.init` never sets up the `Animal` it is built on
  help: call `super.init(...)` first
```

### Objects are shared

Assigning an object does not copy it — both names point at the same one.
This is the same rule arrays follow, and the opposite of structs:

```cub
let a = Counter();
let b = a;
b.bump();
print(a.value());      // 1 -- a and b are the same counter
```

### Methods on the class itself

Sometimes a function belongs to a class without belonging to any particular
object of it. Mark it `static` and call it through the class name:

```cub
class MathKit {
    static int double(n: int) {
        return n * 2;
    }
}

print(MathKit.double(21));       // 42
```

There is no `self` inside a static method, because there is no object.

### Starting from a class

Your `main` can live inside a class instead of at the top level:

```cub
class App {
    greeting: string;

    void init() {
        self.greeting = "hello";
    }

    void main() {
        print(self.greeting);
    }
}
```

Cub makes one `App` and runs `main` on it. If you would rather it made
nothing, mark `main` as `static`.

### Printing an object

Give a class a `to_string` and `print` will use it:

```cub
class Point {
    x: int;
    y: int;

    void init(x: int, y: int) { self.x = x; self.y = y; }
    string to_string() { return "({self.x}, {self.y})"; }
}

print(Point(3, 4));        // (3, 4)
```

Without one you still get something useful: `Point{x: 3, y: 4}`.

---

## 15. When there might be nothing

Ask a program to read a number out of whatever someone typed, and there are
two answers: the number, or an explanation. Cub makes you say which one you
are holding.

```cub
void main() {
    let typed = "seven";
    let n = int(typed);       // this does not compile
    print(n + 1);
}
```

```
error: `n` expects int, but this is int!
  help: `int!` may fail: supply a fallback with `or`, take it apart with
        `if let`, or insist with `!`
```

`int!` means "an int, or a failure with a reason". The `!` in the type is the
compiler telling you that this one needs a decision. There are two shapes:

- `T?` — a `T`, or **nothing**
- `T!` — a `T`, or a **failure**, which carries a reason as text

### Four ways to get the value out

The quickest is `or`, which gives a fallback:

```cub
void main() {
    print(int("42") or 0);       // 42
    print(int("oops") or 0);     // 0
}
```

When you want to know *why*, take it apart with `if let`. The name only
exists in the branch where the value does, and the `else` can name the
reason:

```cub
void main() {
    if let n = int(input()) {
        print("that is {n * 2} doubled");
    } else why {
        print("I could not use that: {why}");
    }
}
```

If your own function can fail too, `try` hands the failure straight up:

```cub
int! doubled(text: string) {
    let n = try int(text);       // a failure here returns from doubled
    return n * 2;
}
```

And when you are certain, `!` insists — the program stops with the reason if
you were wrong:

```cub
let port = int("8080")!;
```

### Writing your own

Say so in the return type, and hand back `nothing` or `fail(...)`:

```cub
int? first_even(numbers: [int]) {
    for n in numbers {
        if n % 2 == 0 { return n; }
    }
    return nothing;
}

int! age_of(text: string) {
    let n = try int(text);
    if n < 0 { return fail("{n} is not an age"); }
    return n;
}
```

Returning a plain number where `int?` is expected is fine — a value that is
there is a value that may be there.

### Where else this turns up

Reading files can fail for reasons no program can foresee, so those say so
too:

```cub
import fs;

void main() {
    print(fs.read("notes.txt") or "(no notes yet)");

    if let _ = fs.write("notes.txt", "hello\n") {
        print("saved");
    } else why {
        print(why);
    }
}
```

`_` as the name means "I only want to know whether it worked".

One last thing the compiler insists on: a `T!` that a statement throws away
is an error, because that discards whether the work happened at all.

---

## 16. Imports

Some of the library is always there — `print`, `len`, `push`, `str`, and the
text and array functions. Anything that reaches outside your program lives in
a module you import:

```cub
import math;
import fs;

void main() {
    print(math.sqrt(16.0));        // 4.0
    print(math.round(math.PI));    // 3.0

    if fs.exists("notes.txt") {
        print(fs.read("notes.txt"));
    }
}
```

The modules are `os`, `fs`, `math`, and `rand`. Forget the import and the
compiler tells you exactly what to add:

```
error: `sqrt` lives in the `math` module
  help: add `import math`, then write `math.sqrt(...)`
```

### Splitting a program across files

A program does not have to be one file. Put the shared parts somewhere and
import them by name:

```cub
// shapes.cb
class Circle {
    radius: float;
    void init(radius: float) { self.radius = radius; }
    float area() { return 3.14159 * self.radius * self.radius; }
}
```

```cub
// main.cb
import "shapes.cb";

void main() {
    print(Circle(2.0).area());
}
```

The path is relative to the file doing the importing. Only the file you hand
to `cubc` may have a `main`.

---

## 17. Writing things down

Three slashes document what comes next:

```cub
/// Works out how much space a circle covers.
///
/// The radius is measured from the middle to the edge.
float circle_area(radius: float) {
    return 3.14159 * radius * radius;
}
```

Ordinary `//` comments are for whoever is reading the code; `///` comments
are for whoever is *using* it. `cubc doc` collects them into a reference:

```bash
cubc doc shapes.cb -o shapes.md
```

You can document classes, methods, fields, structs, and enums the same way.

---

## 18. Putting it together

A program that reads numbers, and reports on them. It uses almost everything
above.

```cub
struct Stats {
    count: int;
    total: int;
    lowest: int;
    highest: int;
}

Stats summarize(numbers: [int]) {
    if len(numbers) == 0 {
        return Stats { count: 0, total: 0, lowest: 0, highest: 0 };
    }

    var stats = Stats {
        count: len(numbers),
        total: 0,
        lowest: numbers[0],
        highest: numbers[0],
    };

    for n in numbers {
        stats.total += n;
        stats.lowest = min(stats.lowest, n);
        stats.highest = max(stats.highest, n);
    }
    return stats;
}

[int] parse_all(line: string) {
    var numbers: [int] = [];
    for piece in split(line, " ") {
        let clean = trim(piece);
        if len(clean) > 0 {
            push(numbers, int(clean));
        }
    }
    return numbers;
}

void main() {
    let numbers = parse_all("8 3 17 4 9 12");
    let stats = summarize(numbers);

    print("count: {stats.count}");
    print("total: {stats.total}");
    print("lowest: {stats.lowest}");
    print("highest: {stats.highest}");
    print("mean: {float(stats.total) / float(stats.count)}");
}
```

```
count:   6
total:   53
lowest:  3
highest: 17
mean:    8.83333333333333
```

Change the input to include a word — `"8 three 17"` — and run it again. The
program stops with a message naming the line, rather than quietly treating it
as zero:

```
Runtime error: cannot read a whole number from "three"
  at stats.cb:33
```

That is the trade Cub makes everywhere: say what you mean, and be told
clearly when something does not add up.

---

## Where to go next

- [the exercises](../exercises) — fifteen problems that follow these
  sections, each checked against a worked answer
- [LANGUAGE.md](LANGUAGE.md) — the full reference, including every built-in
- `examples/` — larger programs: a sieve, a word counter, a task list,
  shapes built with classes, a shop inventory built with classes and maps,
  and `geometry/`, a program split across three files
- `cubc program.cb --emit-c` — read the C your program turns into
