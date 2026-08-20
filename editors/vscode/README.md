# Cub for VS Code

Language support for [Cub](https://github.com/Seth-dse/cub) — a small
language in the C family that tells you what went wrong.

Syntax highlighting, formatting, and **errors underlined as you type**, with
the compiler's own suggestions carried into the hover.

---

## Requirements

This extension drives the Cub compiler, so you need `cubc` on your PATH:

```bash
git clone https://github.com/Seth-dse/cub.git
cd cub
make
make install          # or: make install PREFIX=$HOME/.local
```

If `cubc` lives somewhere else, point the extension at it:

```json
{ "cub.compilerPath": "/full/path/to/cubc" }
```

Without the compiler you still get highlighting and snippets; formatting and
error checking need it.

---

## What you get

### Errors as you type

`cubc --check` runs in the background and its output becomes squiggles. The
compiler's `help:` line comes along with the message, so hovering a mistake
tells you how to fix it:

> `count` was declared with `let`, so it never changes
>
> **help:** declare it with `var count = ...` if it needs to change

Misspell a name and it suggests the right one. Forget a struct field and it
names the one you forgot. Skip a `return` on one branch and it finds the
branch — before the program ever runs.

### Highlighting

Keywords, types, every built-in, classes, `self` and `super`, `import` and
the module names, `///` doc comments, numbers, escapes, and nested block
comments. Expressions inside interpolated text are
highlighted as code, so `{count + 1}` in `"you have {count + 1} left"` reads
as an expression rather than as string contents.

### Formatting

**Format Document** (`Shift+Alt+F`) runs `cubc fmt`. Because Cub ends
statements at ends of lines, the formatter only ever changes horizontal
whitespace — it cannot alter what your program does.

Format on save:

```json
{
  "[cub]": {
    "editor.formatOnSave": true,
    "editor.defaultFormatter": "Sethttech.cub"
  }
}
```

### Snippets

`main`, `fn`, `void`, `fnm`, `static`, `if`, `ife`, `ifx`, `for`, `fore`,
`while`, `struct`, `enum`, `class`, `classx`, `classmain`, `map`, `import`,
`importf`, `///`, `print`, `printv`.

### Commands

Command Palette, or right-click in a `.cub` file:

| Command | What it does |
|---|---|
| **Cub: Run File** | saves, then runs it in a terminal |
| **Cub: Check File for Errors** | checks now, rather than waiting |
| **Cub: Show Generated C** | opens the standalone C your program compiles to |

---

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `cub.compilerPath` | `cubc` | where to find the compiler |
| `cub.checkOnType` | `true` | underline errors while typing, not only on save |
| `cub.checkDelay` | `400` | milliseconds of quiet before checking |

---

## A taste of the language

```cub
class Animal {
    name: string

    void init(name: string) {
        self.name = name
    }

    string speak(){
        return "{self.name} makes a sound"
    }
}

class Dog: Animal {
    void init(name: string) { super.init(name) }
    string speak(){ return "{self.name} says woof" }
}

void main() {
    let pets: [Animal] = [Animal("Generic"), Dog("Rex")]
    for pet in pets {
        print(pet.speak())      // each one keeps its own voice
    }
}
```

---

## Build errors in the Problems panel

A `cubc` problem matcher ships with the extension. In `.vscode/tasks.json`:

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "cub: build",
      "type": "shell",
      "command": "cubc ${file}",
      "problemMatcher": "$cubc",
      "group": { "kind": "build", "isDefault": true }
    }
  ]
}
```

---

## Links

- [The language](https://github.com/Seth-dse/cub)
- [Tutorial](https://github.com/Seth-dse/cub/blob/main/docs/TUTORIAL.md)
- [Reference](https://github.com/Seth-dse/cub/blob/main/docs/LANGUAGE.md)
- [Report a problem](https://github.com/Seth-dse/cub/issues)

## License

MIT
