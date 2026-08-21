# Cub exercises

Fifteen problems, smallest first, each one checked against a known answer.
They follow [the tutorial](../docs/TUTORIAL.md) section by section, so you
can read a section and immediately use it.

## How it works

Every exercise is a `.cb` file that **already compiles and runs**. It just
does not do the right thing yet — the parts left for you are marked `TODO`.

Open one, run it, fill in a piece, run it again:

```bash
cubc run 01-greeting.cb
```

When you think it is right, check it:

```bash
./check.sh 1        # just this one
./check.sh          # all of them
```

You get one of four answers:

| | |
|---|---|
| `pass` | your output matches exactly |
| `todo` | still has `TODO` in it — not started |
| `wrong` | it runs, but the output differs; a diff shows where |
| `error` | it does not compile; the compiler's message follows |

Stuck? The worked answers are in [`solutions/`](solutions). Read one only
after you have had a real go — the struggle is the part that teaches.

```bash
./check.sh --solutions    # confirms the worked answers still pass
```

## The exercises

| | | Teaches | Tutorial |
|---|---|---|---|
| 1 | greeting | `let`, printing, putting values in text | §1–2 |
| 2 | temperature | `int` vs `float`, converting on purpose | §3 |
| 3 | grading | `if` / `else if`, and `if` as a value | §4 |
| 4 | countdown | `while`, `for`, `break`, `continue` | §5 |
| 5 | arrays | building, indexing, walking an array | §7 |
| 6 | text | `trim` `upper` `split` `join` `replace` | §8 |
| 7 | functions | returning a value, returning nothing | §6 |
| 8 | recursion | a function that calls itself | §6 |
| 9 | statistics | the array built-ins, and not changing your input | §7 |
| 10 | points | `struct`, and how copying one behaves | §9 |
| 11 | traffic | `enum`, and comparing choices | §9 |
| 12 | tally | maps, and `get` with a fallback | §10 |
| 13 | account | a class with state and methods | §11 |
| 14 | shapes | building on a class, replacing a method | §11 |
| 15 | catalogue | all of it, across two files, and text that may not parse | §12–15 |

Do 1 through 8 in one sitting if you like — they are short. Stop after 8
and write something of your own before starting 9.

## Three things worth knowing

**Read the errors.** Cub was built so its messages tell you what to do.
Getting something wrong on purpose is a fast way to learn:

```cub
let count = 0;
count = 1;          // error names the fix: use `var`
```

**Whitespace matters, wording does not.** `check.sh` compares output
exactly, so `"Hello, Ada!"` and `"Hello Ada!"` are different answers. If a
diff looks like nothing at all, it is probably a trailing space.

**There is more than one right answer.** The solutions are *a* way, not
*the* way. If yours passes and reads well, yours is right.

## When you finish

Write something you would actually use. You have classes, maps, files, and
the command line — that is enough for a real tool. The
[examples](../examples) are worth reading for how larger programs hang
together.
