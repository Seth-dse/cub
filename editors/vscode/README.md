# Cub for VS Code

Syntax highlighting, formatting, and live error checking for the
[Cub language](../../README.md).

## What you get

**Highlighting.** Keywords, types, built-ins, numbers, comments, and escapes.
Interpolated expressions inside text are highlighted as code, so `{count + 1}`
in `"you have {count + 1} left"` reads as an expression, not as string
contents.

**Errors as you type.** The extension runs `cubc --check` in the background
and turns its output into squiggles. The compiler's `help:` line comes along
with the message, so hovering a mistake tells you how to fix it:

> `count` was declared with `let`, so it never changes
>
> help: declare it with `var count = ...` if it needs to change

**Formatting.** Format Document (`Shift+Alt+F`) runs `cubc fmt`. Because Cub
ends statements at ends of lines, the formatter only ever changes horizontal
whitespace — it cannot alter what your program does.

**Snippets.** `main`, `fn`, `if`, `ife`, `for`, `fore`, `while`, `struct`,
`enum`, `print`, `printv`.

**Commands** (Command Palette, or right-click in a `.cub` file):

| Command | What it does |
|---|---|
| Cub: Run File | saves, then runs it in a terminal |
| Cub: Check File for Errors | checks now, rather than waiting |
| Cub: Show Generated C | opens the C your program compiles to |

## Installing

From the project root:

```bash
make install-vscode
```

That links this folder into `~/.vscode/extensions`. Restart VS Code, or run
**Developer: Reload Window** from the Command Palette.

It needs `cubc` on your PATH. If it is somewhere else, set the path in
Settings:

```json
{ "cub.compilerPath": "/full/path/to/cubc" }
```

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `cub.compilerPath` | `cubc` | where to find the compiler |
| `cub.checkOnType` | `true` | underline errors while typing, not only on save |
| `cub.checkDelay` | `400` | milliseconds of quiet before checking |

To format every time you save:

```json
{
  "[cub]": {
    "editor.formatOnSave": true,
    "editor.defaultFormatter": "cub-lang.cub"
  }
}
```

## Build errors in the Problems panel

A `cubc` problem matcher is included. In `.vscode/tasks.json`:

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

## Uninstalling

```bash
rm ~/.vscode/extensions/cub-lang.cub-0.1.0
```
