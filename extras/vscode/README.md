# Sea Language — VS Code extension

Support for the [Sea](../../README.md) programming language: syntax
highlighting, snippets, and the language server built into the `sea` binary
(`sea lsp`).

## Features

From `sea lsp`:

- **Diagnostics as you type** — the checks of `sea check`: syntax errors,
  undefined and unused variables, wrong argument counts, bad format strings,
  unknown functions of built-in modules, ...
- **Formatting** — `sea fmt` (*Format Document*, or `editor.formatOnSave`)
- **Hover** — the declaration and its doc comment
- **Go to definition** (F12) — also into `require`d modules
- **Completion** — names in scope, built-in modules and their functions, the
  functions of required modules (after `.` / `:`)
- **Outline** — document symbols (*Go to Symbol*, breadcrumbs)

From the extension itself: the TextMate grammar, bracket matching and
comment toggling (`Ctrl+/`), and snippets (`fn`, `fnl`, `if`, `ife`, `for`,
`forin`, `while`, `sel`, `def`, `req`, `iferr`, `ch`, ...).

## Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `sea.executablePath` | `"sea"` | The `sea` binary (the extension runs `sea lsp`); use an absolute path if it is not in your `PATH` |
| `sea.trace.server` | `"off"` | `messages` / `verbose` log the LSP traffic in the *Sea Language Server* output |

Command: **Sea: Restart Language Server** (after rebuilding `bin/sea`, for
example).

## Building and installing

```sh
cd extras/vscode
npm install
npm run package                                   # → sea-language-0.3.0.vsix
code --install-extension sea-language-0.3.0.vsix
```

`npm run package` needs [`vsce`](https://github.com/microsoft/vscode-vsce)
(`npm install -g @vscode/vsce`).

## Testing

```sh
npm test
```

runs `test/suite.js` inside the installed VS Code, with this folder as a
development extension and a throwaway profile (a window opens for a few
seconds): it opens a Sea file and checks diagnostics, hover, go to
definition (also into a required module), completion, symbols and
formatting against `../../bin/sea` (or `$SEA`). To try it by hand, open this
folder in VS Code and press F5 (*Run Extension*).
