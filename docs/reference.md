# Sea Language Reference

Keywords, operators, and built-in functions available in the Sea scripting language.

---

## Source Encoding

Source files are **UTF-8**. A byte-order mark at the start is skipped; any
byte that is not valid UTF-8 (a Latin-1 `é`, say), in code, strings or
comments, is a compile error that names its line and column:
`file.sea:2:15: invalid UTF-8 in source (byte 0xE9)`. Identifiers are ASCII;
non-ASCII text goes in strings and comments. Strings themselves are byte
strings: escapes such as `\xFF` can still put any byte in one.

---

## Command Line

```sh
sea                              # REPL
sea file.sea [args...]           # run (args in os.args)
sea run file.sea                 # the same
sea ./tool [args...]             # any file name, e.g. a script starting with #!/usr/bin/env sea
sea -e 'code' [args...]          # run code from the command line (like python -c)
sea - [args...]                  # run code read from stdin (cat script.sea | sea -)
sea compile file.sea -o out.seac # bytecode; run it with `sea out.seac`
sea bytecode file.sea            # disassembly
sea --jit file.sea               # with the JIT (bin/sea-jit)
sea fmt [flags] [path...]        # format source code
sea doc [flags] [path...] [name] # documentation from comments
sea test [flags] [path...]       # run the *_test.sea files
sea check [flags] [path...]      # find likely mistakes without running
sea build [-o out] file.sea      # a standalone executable
sea prof [flags] file.sea [args] # run with the CPU profiler
sea new [--lib] name             # start a project
sea get [URL[@ref]]              # dependencies (sea.toml, sea.lock, sea_modules/)
sea lsp                          # language server for editors (stdin/stdout)
sea version
```

### `sea fmt`

Formats Sea source in the canonical layout of `sea/parser`'s `format` (see
that module): one statement per line, blocks indented by 4 spaces, spaced
operators; comments, single blank lines and literals as written are kept.
Paths are files or directories (searched for `*.sea`, skipping hidden
directories); without paths, stdin is formatted to stdout.

| Flag | |
|------|--|
| (none) | Rewrite the files that change (atomically, keeping their permissions) and print their names |
| `-l` | List the files whose formatting differs; change nothing |
| `-d` | Print a unified diff for each file that would change (uses `diff(1)`) |
| `--check` | Like `-l`, and exit with status 1 if any file would change (for CI) |
| `-p` | Print the formatted code instead of writing files |
| `--indent N` | Spaces per level (1-16) |

A file with a syntax error is reported as `file:line:col: message` (on
stderr) and left alone; the exit status is then 2.

### `sea doc`

Prints the documentation written in comments. A **doc comment** is the
block of `--` lines (or a `--[[ ]]` comment) right above a declaration;
the **module comment** is the comment block at the top of a file, when a
blank line separates it from the first statement (a license header block,
one with `SPDX-License-Identifier`, is skipped). Documented: functions
stored in the table the file returns (`function M.f(...)`, shown as
`file.f`), functions and methods of other tables (`function Conn:send(...)`,
grouped under `Conn`), global functions, and documented fields
(`M.VERSION = "1.0"`); local functions only with `--all`. Lines of a comment
indented by two or more spaces are shown as code.

```sea
-- greet — friendly greetings
--
--   local greet = require("greet")

local M = {}

-- greet.hello(name) → "Hello, <name>!"
function M.hello(name)
    return "Hello, " .. name .. "!"
end

return M
```

`sea doc greet.sea` prints the module comment and `greet.hello(name)` with
its comment. Arguments are files or directories (default: the current
directory; files that do not parse are reported and skipped) and,
optionally, a name: `sea doc http.sea get` shows only the matching
functions. The name of a built-in module (`sea doc fs`) lists its functions.

| Flag | |
|------|--|
| `-f`, `--format F` | `text` (default), `markdown`, `html` (one page with an index, light and dark) or `json` |
| `-o FILE` | Write to a file instead of stdout |
| `--all` | Include local functions |
| `-http [HOST]:PORT` | Serve the documentation as web pages instead (see below) |
| `-open` | With `-http`, open the index in the browser (`xdg-open`) |

When a doc comment starts with the signature written by hand (`greet.hello(name)
→ ...`), the HTML pages use that line as the heading instead of the bare
parameter names.

`sea doc -http :6060 [path ...]` is a documentation server, like the old
`godoc -http`: an index of the modules grouped by directory (with the first
line of each module comment), a page per module with links to the source,
the source with line numbers and highlighting (`/src/file.sea#L42`), search
over names and comments (`/search?q=login`) and the built-in modules
(`/builtin/fs`). Files are read again on each request, so editing a comment
and reloading shows it. The host defaults to `127.0.0.1` (`-http 0.0.0.0:6060`
to serve the network); port `0` picks a free one, printed on start. Only the
`.sea` files under the given paths are served.

### `sea test`

Runs the tests of a project: every `*_test.sea` file under the paths
(default: the current directory; hidden directories are skipped), each in its
own process started in the file's directory, several at a time. A test file
defines tests with the built-in `sea/test` module (see `test` in
`docs/libs.md`); `t.main()` at its end is optional.

```sea
-- stack_test.sea
local t = require("sea/test")
local Stack = require("./stack")

t.it("pops what was pushed", function()
    local s = Stack.new()
    s:push(1)
    t.eq(s:pop(), 1)
end)
```

```
$ sea test
ok    stack_test.sea  1 passed (0.01 s)
FAIL  parser/lexer_test.sea  4 passed, 1 failed (0.02 s)

2 files, 1 failed; 5 passed, 1 failed (0.03 s)
```

The output of a file (its report and anything it prints) is shown when it
fails, or always with `-v`. A file that does not parse, panics outside a
test or is killed fails. The exit status is 1 when anything failed.

| Flag | |
|------|--|
| `-v` | Show every file's output |
| `-run TEXT` | Only the tests whose full name (`group > test`) contains `TEXT` |
| `-bench TEXT` | Also run the benchmarks (`t.bench`) whose name contains `TEXT` (`.` for all); their results are shown |
| `-benchtime S` | Seconds per benchmark (default 1) |
| `-j N` | Files run at the same time (default: the number of CPUs) |
| `-timeout S` | Seconds a file may take before it is killed (default 600) |
| `-cover` | Report which lines of the code under test ran: per file, the share of statement lines run and the ranges not run (the `_test.sea` files and built-in modules are left out) |
| `-coverprofile FILE` | Also write the coverage as an lcov file (for `genhtml` and editor plugins); implies `-cover` |

```
coverage:
  stack.sea   91.7%  11/12  not run: 30
  total       91.7%  11/12
```

Coverage works for any run: with `SEA_COVER=FILE` in the environment, the
compiler adds a marker at each statement and the program appends an lcov
record per source file to `FILE` when it ends (also through `os.exit`).
Without the variable nothing is added, so there is no cost. (JIT-compiled
code does not record coverage; the markers keep loops in the interpreter.)

### `sea check`

Finds likely mistakes without running the code: each problem is printed as
`file:line:col: message [code]`, and the exit status is 1 when there is any
(2 for a bad flag or path). Paths are files or directories (searched for
`*.sea`, hidden directories skipped; default: the current directory). A
global assigned in one of the checked files (`function helper()`, `x = 1`)
counts as defined in all of them.

```
$ sea check
app.sea:4:1: undefined variable 'pritn' (did you mean 'print'?) [undefined]
app.sea:9:10: 'add' takes 2 arguments, called with 3 [args]
app.sea:12:7: variable 'tmp' is never used [unused]
```

| Code | Severity | Reports |
|------|----------|---------|
| `syntax` | error | The file does not parse or compile |
| `undefined` | error | A variable that is not local, not built in and not assigned as a global in any checked file; suggests a close name, and the Sea way for names from other languages (`array`, `push`, `pairs`, ...) |
| `require` | error | `require("literal")` of a module that cannot be found (searched like `require`: the file's directory, the working directory, `SEA_PATH`) |
| `args` | error | A call to a local function with more arguments than it takes |
| `format` | error | `string.format` / `printf` with a literal format whose `%` directives do not match the number of arguments |
| `unused` | warning | A local variable or function that is never read (names starting with `_` are exempt) |
| `redeclared` | warning | `local x` twice in the same block |
| `shadow` | warning | A local that hides a local of an enclosing block or function |
| `unreachable` | warning | Code after `break`, `continue`, `panic`, `os.exit` or an `if` whose branches all leave |
| `dupkey` | warning | The same key twice in a table literal |
| `selfassign` | warning | `x = x` |

A line ending with the comment `-- check:ignore` is not checked; with
`-- check:ignore unused shadow` only those codes are skipped on it.

| Flag | |
|------|--|
| `-disable CODES` | Skip these checks (comma separated) |
| `-only CODES` | Run only these checks |
| `-g NAMES` | Globals defined elsewhere (comma separated) |
| `-all` | Also report unused parameters and loop variables |
| `-f text\|json` | Output format; `json` is an array of `{file, line, col, code, severity, message}` |
| `-w` | Exit with status 0 when there are only warnings |

The checks are the built-in module `sea/check`:
`check.file(path [, opts])` and `check.source(src [, opts])` return the
diagnostics as an array of `{file, line, col, code, severity, message}`
(opts: `name`, `dir` for `require`, `globals` (table of names), `disable`
(table of codes), `all_unused`); `check.defined_globals(src)` gives the
globals a source assigns; `check.CODES` lists the codes.

### `sea build`

Makes a standalone executable of a program: a copy of the `sea` binary with
the program appended. It holds the main file and every module it requires
with a literal name, followed recursively — Sea sources and native `.so`
libraries (loaded from memory when required). Built-in modules need nothing.
The executable runs anywhere the same `sea` binary would (same
architecture; the system libraries a native library links to, such as
libssl, must be installed), from any directory, without the sources.

```
$ sea build -o hello app/main.sea
hello: 4 files (1 native), 0.8 MB
$ ./hello world          # os.args[1] is "world"; every argument is the program's
```

| Flag | |
|------|--|
| `-o FILE` | The executable (default: the main file's name without `.sea`) |
| `-i PATH` | Also include this module (`.sea` or `.so`; may repeat): for `require` with a computed name, which cannot be followed (they are reported) |
| `-v` | List the files included |

Inside the executable, modules keep their paths relative to the directory
that holds them all (`main.sea`, `lib/util.sea`; tracebacks show these), and
`require` looks in the bundle first, then on disk as usual. Files the
program reads at run time (templates, data) are not included. A module that
cannot be found fails the build.

### `sea prof`

Runs a program under a sampling CPU profiler and prints where its time goes
(on stderr when it ends, or through `os.exit`): functions by their own time
(`self`) and by the time of everything they call (`total`), and the hottest
lines.

```
$ sea prof app.sea
sea prof: 157 samples, 1.57 s of CPU at 100 Hz

   self%  total%   self s  function
   98.7%   98.7%     1.55  sum_squares  app.sea:12
    1.3%    1.3%     0.02  fib  app.sea:1

  total%  total s  function (including what it calls)
  100.0%     1.57  main chunk  app.sea:1
   98.7%     1.55  sum_squares  app.sea:12
   ...

   self%  line
   98.7%  app.sea:14  (sum_squares)
```

| Flag | |
|------|--|
| `-o FILE` | Write the report to a file |
| `-folded` | Folded stacks instead (`main chunk (app.sea:1);f (app.sea:3) 42` per line), for flame graphs (`flamegraph.pl`, speedscope); also when the `-o` file ends in `.folded` |
| `-hz N` | Samples per second of CPU time (default 100) |
| `-top N` | Rows per table (default 20) |

Every thread that runs Sea code is sampled (goroutines show up with their
own stacks), on CPU time: a program waiting on the network or a timer
costs nothing. Time inside a native function counts for the Sea line that
called it. The innermost line is approximate (the interpreter saves its
position before calls and operations that may fail, not at every
instruction); the functions are exact. Run it on `bin/sea`, without
`--jit`.

### `sea new`

`sea new name` creates the directory `name/` with an application:
`sea.toml` (the manifest), `main.sea`, a module it uses (`greet.sea`) and
its test, `README.md` and `.gitignore`, and runs `git init` there.
`sea new --lib name` makes a library instead: `name.sea` (what
`require("name")` loads once the library is installed with `sea get`) and
`name_test.sea`. `--no-git` skips `git init`. The result passes `sea test`,
`sea check` and `sea fmt` as it is, and `sea build main.sea` makes `./name`
(`sea build` names the executable after the package in `sea.toml`).

### `sea get`

Dependencies of a project, the nearest directory (from the current one up)
with a `sea.toml`:

```toml
[package]
name = "app"
version = "0.1.0"

[dependencies]
json5 = { git = "https://github.com/someone/sea-json5", tag = "v1.2.0" }
utils = { git = "https://example.com/utils.git", branch = "main" }
tools = { git = "https://example.com/tools.git", rev = "4f2a9c1" }
mine = { path = "../mine" }
```

| Command | |
|---------|--|
| `sea get` | Install what `sea.toml` lists, at the commits recorded in `sea.lock` (the newest commit of each ref for a dependency that is not there yet); remove packages no longer listed |
| `sea get URL[@ref] [name]` | Add a git dependency and install it. `ref` is a tag (`v1.2.0`, anything starting with a digit or `v` and a digit), a commit (hex) or a branch; without it, the default branch. The name defaults to the repository's (`sea-json5` → `json5`: a `sea-`/`sea_` prefix is dropped, `-` becomes `_`) |
| `sea get -path DIR [name]` | Add a local dependency: a link to `DIR` |
| `sea get -u [name ...]` | Move dependencies (all, or those named) to the newest commit of their ref |
| `sea get -rm name ...` | Remove dependencies |
| `sea get -l` | List them, with the locked commits |

Packages are cloned into `sea_modules/` (add it to `.gitignore`), where
`require("json5")` finds them from any file of the project:
`sea_modules/json5/init.sea` or `sea_modules/json5/json5.sea`. A package's
own `sea.toml` dependencies are installed too, in the same `sea_modules/`
(when two packages want different versions of one, the first wins, with a
warning). A package with a `Makefile` is built with `make` after it is
fetched (for its native `.so` parts). `sea.lock` records the commit of
every git package (`name url ref commit` per line): commit it with the
project so that `sea get` installs the same code everywhere. `sea get`
needs `git`. `sea build` and `sea check` find packages the same way.

### `sea lsp`

A [Language Server Protocol](https://microsoft.github.io/language-server-protocol/)
server on stdin/stdout, for editors. It provides:

- **diagnostics** as you type: the checks of `sea check` (globals assigned
  in other files of the workspace count as defined);
- **formatting**: `sea fmt` (the editor's tab size is the indent);
- **hover**: a function's signature and doc comment (also of functions in
  required modules and other files), or the functions of a built-in module;
- **go to definition**: locals, functions, globals of other files, and
  `mod.f` where `local mod = require("...")` (into the module's file);
- **completion**: names in scope, built-in modules and their functions after
  `math.`, the functions a required module exports after `mod.`, keywords;
- **document symbols**: the file's functions, methods, fields and top-level
  locals.

Neovim (0.11+):

```lua
vim.lsp.config("sea", { cmd = { "sea", "lsp" }, filetypes = { "sea" }, root_markers = { "sea.toml", ".git" } })
vim.lsp.enable("sea")
vim.filetype.add({ extension = { sea = "sea" } })
```

Helix (`languages.toml`):

```toml
[language-server.sea]
command = "sea"
args = ["lsp"]

[[language]]
name = "sea"
scope = "source.sea"
file-types = ["sea"]
roots = ["sea.toml"]
comment-token = "--"
language-servers = ["sea"]
```

VS Code needs a small extension that starts `sea lsp` (any generic LSP
client extension that takes a command works).

These tools are Sea programs (`tools/fmt.sea`, `tools/doc.sea`,
`tools/test.sea`, `tools/check.sea`, `tools/build.sea`, `tools/new.sea`,
`tools/get.sea`, `tools/lsp.sea`) built into the binary.

---

## Comments & Shebang

```sea
-- single-line comment
--[[ multi-line
     comment ]]
--[=[ also multi-line, level-1 delimiter ]=]
#!/usr/bin/env sea    -- shebang only at the very first line of a file
```

---

## Number Literals

```sea
42          -- integer
3.14        -- float
1e6         -- float (scientific notation)
0xFF        -- hex integer (0x or 0X prefix)
0xDEAD_BEEF -- (any hex digits)
```

An integer literal must fit an `int` (48 bits, up to 140737488355327): a
larger one is a compile error (`integer literal too large`), not a number
wrapped around. Write it as a float (`1e15`, `999999999999999.0`) or with
`bigint.new("...")`.

Integer (`int`) and float (`number`) are **distinct types**. `1 == 1.0` is **false**.

---

## String Literals

### Quoted strings

```sea
"hello\nworld"   -- double-quoted
'it\'s fine'     -- single-quoted
```

**Escape sequences inside quoted strings:**

| Escape | Meaning |
|--------|---------|
| `\n` | newline |
| `\t` | horizontal tab |
| `\r` | carriage return |
| `\\` | backslash |
| `\'` | single quote |
| `\"` | double quote |
| `\0` | null byte (a literal keeps it: `len("a\0b")` is 3) |
| `\a` | bell |
| `\b` | backspace |
| `\f` | form feed |
| `\v` | vertical tab |
| `\xHH` | byte with hex value `HH` (exactly two hex digits) |
| `\u{H…}` | the code point `H…` (1 to 6 hex digits, up to `10FFFF`, not a surrogate) as UTF-8: `"\u{E9}"` is `"é"`, `"\u{1F600}"` is `"😀"`. Anything else after `\u` is a compile error |
| `\` at the end of a line | the newline itself (the string goes on on the next line) |

Any other character after a backslash (`"\d"`, `"\1"`, `"\x4"`) is a
compile error: `invalid escape '\d'`. Write a backslash as `\\` (regular
expressions: `"\\d+"`, or a long string `[[\d+]]`).

### Template strings

Backquoted strings insert the value of each `${expr}`, converted as
`tostring` does (`nil`, booleans, numbers and other values too):

```sea
local name, n = "Ana", 3
print(`hello ${name}, you have ${n} new ${n == 1 and "message" or "messages"}`)
print(`sum = ${a + b}; upper = ${string.upper(name)}; nested = ${`<${n}>`}`)
```

A template may span lines, and takes the escapes of quoted strings plus
`` \` `` (a backquote) and `\$` (a `$` that does not start `${`). Each
`${...}` holds one expression (it may contain strings, tables and other
templates). The result is always a string: `` `${n}` `` is `tostring(n)`.
They compile to one concatenation, as fast as `..`.

### Long strings (no escape processing)

```sea
[[
raw text
no escapes here: \n is literal
]]

[=[
level-1: safe to contain ]] inside
]=]

[==[
level-2: safe to contain ]=] inside
]==]
```

An initial newline immediately after the opening bracket is ignored. The number of `=` signs must match between the opening and closing delimiters.

---

## Keywords

Keywords cannot name variables or parameters, but they can name fields and
methods: after `.` or `:`, in `function t.name` / `function t:name`, and as a
table key before `=`, the position says a name comes next.

```sea
local Query = {}
function Query:in(field, values) ... end      -- q:in("uf", ["SP", "RJ"])
function Query:or(fn) ... end                 -- q:or(function(g) ... end)
local t = {select = 1, end = 2}               -- t.select, t.end
```

### Literals / values

| Keyword | Description |
|---------|-------------|
| `nil` | The nil value (falsy) |
| `true` | Boolean true |
| `false` | Boolean false |

### Control flow

| Keyword | Description |
|---------|-------------|
| `if` | Conditional — `if cond then ... end` |
| `elseif` | Additional branch in an `if` chain |
| `else` | Fallback branch |
| `then` | Separates condition from body in `if`/`elseif` |
| `end` | Closes `if`, `for`, `while`, `function`, `do` blocks |
| `while` | While loop — `while cond do ... end` |
| `for` | Numeric for — `for i = start, limit [, step] do ... end` |
| `in` | For-in loop — `for k, v in expr do ... end` |
| `do` | Opens a block (`while`/`for`/bare `do...end`) |
| `break` | Exit the nearest `for` or `while` loop |
| `continue` | Skip to the next iteration |
| `return` | Return values from a function |

### Definitions / scope

| Keyword | Description |
|---------|-------------|
| `local` | Declare a local variable — `local x = expr`; multiple: `local a, b = e1, e2`; block form: `local (\n  a = e1\n  b = e2\n)` |
| `const` | Declare a read-only local — `const x = expr`; block form: `const (\n  a = e1\n  b = e2\n)`. Assignment after declaration is a compile error. |
| `function` | Define a function — `function name(params) ... end` or as expression |

### Logic operators

| Keyword | Description |
|---------|-------------|
| `and` | Logical AND (short-circuits, returns one of the operands) |
| `or` | Logical OR (short-circuits, returns one of the operands) |
| `not` | Logical NOT — `not expr` |

### Concurrency / channels

| Keyword | Description |
|---------|-------------|
| `go` | Spawn a goroutine — `go fn(args)` |
| `select` | Select over channel operations |
| `case` | A branch inside a `select` block |
| `default` | Default (non-blocking) branch in `select` |

### Error / resource management

| Keyword | Description |
|---------|-------------|
| `panic` | Start a panic (unwinds, running deferred calls) |
| `recover` | Recover from a panic inside a deferred call |
| `defer` | Schedule a call to run when the enclosing function returns |

### Collections

| Keyword | Description |
|---------|-------------|
| `del` | Delete an element — `del arr[i]` or `del map[k]` |

---

## Operators

### Arithmetic

| Operator | Description |
|----------|-------------|
| `+` | Addition |
| `-` | Subtraction / unary negation |
| `*` | Multiplication |
| `/` | Float division |
| `//` | Floor division: rounds toward minus infinity (`-7 // 2` is `-4`), for ints and floats |
| `%` | Modulo, with the sign of the divisor (`-7 % 2` is `1`): `a == (a // b) * b + a % b` |
| `^` | Exponentiation |

The arithmetic operators take `int` and `number`. A `bigint` needs the
`bigint` module's functions (`bigint.add(a, b)`); see [Module: `bigint`](#module-bigint).

### Bitwise

| Operator | Description |
|----------|-------------|
| `&` | Bitwise AND |
| `\|` | Bitwise OR |
| `~` | Bitwise XOR (binary) / bitwise NOT (unary) |
| `<<` | Left shift |
| `>>` | Right shift |

### Comparison

| Operator | Description |
|----------|-------------|
| `==` | Equal (integers and floats are distinct types) |
| `~=` | Not equal |
| `<` | Less than |
| `<=` | Less than or equal |
| `>` | Greater than |
| `>=` | Greater than or equal |

Comparisons and `==` also take a `bigint`, against another bigint, an int or
a number (by value: `bigint.new(5) == 5`).

### String

| Operator | Description |
|----------|-------------|
| `..` | String concatenation |
| `...` | Variadic arguments (inside a function body) |

### Channel

| Operator | Description |
|----------|-------------|
| `ch <- v` | Send `v` to channel `ch` |
| `<- ch` | Receive from channel `ch` |

### Error propagation

| Operator | Description |
|----------|-------------|
| `expr?` | If `expr` is an error, return it immediately from the current function; otherwise unwrap the value. A call with `?` is also a statement: `fs.write(p, data)?` |

### Assignment

| Operator | Description |
|----------|-------------|
| `=` | Assign |
| `arr[] = v` | Append `v` to array `arr` |
| `+=` `-=` `*=` `/=` `//=` `%=` `^=` `..=` | Compound assignment: `x += 1` is `x = x + 1` (`s ..= t` appends in place). One target and one value; a target with effects (`t[f()] += 1`) is evaluated once |

---

## Built-in Functions (globals)

### Core

| Function | Signature | Description |
|----------|-----------|-------------|
| `print` | `print(...)` | Print values separated by tabs, followed by newline (as `tostring` shows them) |
| `dump` | `dump(...)` | Print each value on its own line for inspection: strings quoted, containers indented over several lines when they don't fit in 80 columns |
| `printf` | `printf(fmt, ...)` | Format and print to stdout using `string.format` specifiers (`%d`, `%s`, `%f`, `%x`, etc.); no implicit newline |
| `type` | `type(v) → string` | Return the type name of `v`: `"nil"`, `"bool"`, `"int"`, `"number"`, `"string"`, `"array"`, `"table"`, `"map"`, `"struct"`, `"function"`, `"channel"`, `"int_array"`, `"float_array"`, `"float32_array"`, `"bigint"` |
| `tostring` | `tostring(v) → string` | Convert any value to its string representation. Containers show their contents on one line: `[1, "two"]`, `{name = "ana", n = 1}` (keys sorted; a struct in its literal's order), `{"a", "b"}`, `[="k" = 1]` (map), `int_array[1, 2]`; at most 100 items per container (`... (N more)`), 4 levels deep (`[...]`), `<cycle>` for a container inside itself |
| `tonumber` | `tonumber(v) → int\|number\|nil` | Parse a string to a number (an int when it fits Sea's 48-bit range, else a number): decimal, or hex with `0x`; leading zeros are not octal (`"010"` is 10); a bigint becomes a number; `nil` on failure |
| `len` | `len(v) → int` | Length of string, array, table, map, struct, int_array, float_array or float32_array |
| `error` | `error(msg) → error` | Create an error value |
| `is_error` | `is_error(v) → bool` | Check whether a value is an error |
| `assert` | `assert(v, msg?)` | Abort with `msg` if `v` is falsy |
| `require` | `require(name) → module` | Load a Sea or native module by path/name |
| `load` | `load(src [, name]) → function\|error` | Compile a string of Sea code into a function that runs it: its arguments are the code's `...`, its top-level `return` is the result. It sees globals, not the caller's locals. `name` is the source name in errors and tracebacks (default `"load"`). A syntax or compile error is an error value |
| `int_array` | `int_array(cap?) → IntArray` | Create a typed integer array (no per-element RC) |
| `float_array` | `float_array(cap?) → FloatArray` | Create a typed float array (no per-element RC) |
| `float32_array` | `float32_array(cap?) → Float32Array` | Create a typed array of 32-bit floats: half the memory of a float_array; values are rounded to float when stored and read back as numbers |

### Channels

| Function | Signature | Description |
|----------|-----------|-------------|
| `chan` | `chan(cap?) → channel` | Create a channel with optional buffer capacity |
| `close` | `close(ch)` | Close a channel |

### Error handling

| Function | Signature | Description |
|----------|-----------|-------------|
| `panic` | `panic(v)` | Start a panic with value `v` (see [panic and recover](#panic-and-recover)); unrecovered, it prints the traceback and exits with code 1 |
| `recover` | `recover() → value\|nil` | Inside a deferred call, recover the panic value; otherwise returns `nil` |

### Math (global aliases)

| Function | Signature | Description |
|----------|-----------|-------------|
| `isnan` | `isnan(v) → bool` | True if `v` is a float NaN |
| `isinf` | `isinf(v) → bool` | True if `v` is a float ±Inf |

---

## Module: `math`

### Constants

| Name | Value |
|------|-------|
| `math.pi` | π (3.14159…) |
| `math.huge` | Positive infinity |
| `math.maxinteger` | Maximum 64-bit signed integer |
| `math.mininteger` | Minimum 64-bit signed integer |

### Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `math.abs` | `abs(x) → x` | Absolute value |
| `math.ceil` | `ceil(x) → int` | Round up to nearest integer |
| `math.floor` | `floor(x) → int` | Round down to nearest integer |
| `math.round` | `round(x) → int` | Round to nearest integer |
| `math.trunc` | `trunc(x) → int` | Truncate toward zero |
| `math.sqrt` | `sqrt(x) → number` | Square root |
| `math.exp` | `exp(x) → number` | e^x |
| `math.log` | `log(x [, base]) → number` | Natural log, or log with given base |
| `math.log2` | `log2(x) → number` | Base-2 logarithm |
| `math.log10` | `log10(x) → number` | Base-10 logarithm |
| `math.pow` | `pow(x, y) → number` | x raised to the power y |
| `math.fmod` | `fmod(x, y) → number` | Floating-point remainder |
| `math.sin` | `sin(x) → number` | Sine (radians) |
| `math.cos` | `cos(x) → number` | Cosine (radians) |
| `math.tan` | `tan(x) → number` | Tangent (radians) |
| `math.asin` | `asin(x) → number` | Arc-sine |
| `math.acos` | `acos(x) → number` | Arc-cosine |
| `math.atan` | `atan(x [, y]) → number` | Arc-tangent; two-arg form computes `atan2(x, y)` |
| `math.max` | `max(...) → x` | Maximum of all arguments |
| `math.min` | `min(...) → x` | Minimum of all arguments |
| `math.random` | `random() → number`<br>`random(n) → int ∈ [1,n]`<br>`random(lo, hi) → int ∈ [lo,hi]` | Pseudo-random number (xorshift64; one shared generator, safe to call from several goroutines) |
| `math.randomseed` | `randomseed([seed])` | Seed the RNG; defaults to `time()` |
| `math.isnan` | `isnan(x) → bool` | Same as global `isnan` |
| `math.isinf` | `isinf(x) → bool` | Same as global `isinf` |

---

## Module: `string`

> **Note:** Index arguments (`i`, `j`) are **1-based** and support negative indices (−1 = last character), following Lua convention.
> The string functions work on **bytes** (`len("é")` is 2); module [`utf8`](#module-utf8) works on characters.

| Function | Signature | Description |
|----------|-----------|-------------|
| `string.len` | `len(s) → int` | Byte length of string |
| `string.sub` | `sub(s, i [, j]) → string` | Substring from byte `i` to `j` (inclusive, 1-based) |
| `string.upper` | `upper(s) → string` | Uppercase copy |
| `string.lower` | `lower(s) → string` | Lowercase copy |
| `string.reverse` | `reverse(s) → string` | Byte-reversed copy |
| `string.rep` | `rep(s, n [, sep]) → string` | Repeat `s` `n` times, optionally joined by `sep` |
| `string.byte` | `byte(s [, i]) → int` | Byte value at position `i` (1-based, default 1) |
| `string.char` | `char(...) → string` | Build a string from byte values |
| `string.format` | `format(fmt, ...) → string` | Printf-style formatting: `%d`, `%i`, `%u`, `%f`, `%e`, `%g`, `%s`, `%c`, `%x`, `%X`, `%%`, with flags, width and precision (`%-8s`, `%05.1f`). No length limit; a plain `%s` copies any bytes. `%d`/`%x` print a bigint with all its digits. An unknown conversion is written as is |
| `string.find` | `find(s, pattern [, init]) → int\|nil` | Return 1-based start position of first occurrence of `pattern` (plain text, not a regex: see module `re`), or `nil` |
| `string.split` | `split(s, sep [, n]) → array` | Split `s` by separator string; returns an array of strings: `k` separators give `k + 1` pieces, empty ones included (`split("a,,b,", ",")` is `["a", "", "b", ""]`, `split("", ",")` is `[""]`; for lines use `splitlines`). An empty `sep` splits into bytes. With `n`, at most `n` pieces (the last holds the rest: `split("k=v=w", "=", 2)` is `["k", "v=w"]`) |
| `string.splitlines` | `splitlines(s) → array` | The lines, without `\n` / `\r\n` (a final newline adds no empty line) |
| `string.format_number` | `format_number(n [, decimals [, sep [, point]]]) → string` | Digits grouped by thousands: `format_number(1234567.891)` is `"1,234,567.89"` (decimals default 0 for ints, 2 for numbers); `format_number(1234.5, 1, ".", ",")` is `"1.234,5"` |
| `string.contains` | `contains(s, sub) → bool` | True if `sub` is present in `s` |
| `string.trim` | `trim(s) → string` | Strip leading and trailing whitespace |
| `string.starts_with` | `starts_with(s, prefix) → bool` | True if `s` begins with `prefix` |
| `string.ends_with` | `ends_with(s, suffix) → bool` | True if `s` ends with `suffix` |
| `string.replace` | `replace(s, old, new) → string` | Replace all occurrences of `old` with `new` |
| `string.pad_left` | `pad_left(s, width [, char]) → string` | Right-align `s` in a field of `width`, padding with `char` (default `' '`) |
| `string.pad_right` | `pad_right(s, width [, char]) → string` | Left-align `s` in a field of `width` |
| `string.pad_center` | `pad_center(s, width [, char]) → string` | Center `s` in a field of `width` |
| `string.join` | `join(arr [, sep]) → string` | Join the elements of an array (or a positional table `{"a", "b"}`) with a separator; numbers and other values join as `tostring` shows them. O(N), preferred over incremental `..` |
| `string.normalize_ws` | `normalize_ws(s) → string` | Collapse internal whitespace runs to a single space and strip leading/trailing whitespace |

---

## Module: `io`

### stdin / stdout

| Function | Signature | Description |
|----------|-----------|-------------|
| `io.read` | `read([fmt]) → string\|number\|nil` | Read from stdin. `fmt`: `"*l"` / `"l"` (line, default), `"*n"` / `"n"` (number), `"*a"` / `"a"` (all) |
| `io.write` | `write(...)` | Write the values to stdout with no separator and no line break (strings byte for byte, others as `tostring` shows them) |
| `io.write_err` | `write_err(...)` | The same on stderr |

### Buffered file I/O

Handles are opaque integer values returned by `io.open` and `io.scanner`. Close them with `io.close` when done.

| Function | Signature | Description |
|----------|-----------|-------------|
| `io.open` | `open(path [, mode]) → handle\|nil` | Open a file. `mode`: `"r"` (default), `"w"`, `"a"`, `"rb"`, `"wb"`, `"ab"`, and `"r+b"` / `"w+b"` to read and write (with `io.seek`). Returns `nil` if the file cannot be opened |
| `io.seek` | `seek(handle, offset [, whence]) → int\|nil` | Move to `offset` from the start (`whence` `"set"`, the default), the current position (`"cur"`) or the end (`"end"`); the new position (`seek(h, 0, "cur")` tells it), `nil` on failure |
| `io.stdin` | `stdin() → handle\|nil` | A handle on standard input for `read_line` / `read_bytes` / `read_all` (works when stdin is a pipe, file, terminal or socket; closing it leaves stdin open) |
| `io.close` | `close(handle)` | Close a file or scanner handle and free its resources |
| `io.read_line` | `read_line(handle) → string\|nil` | Read one line, stripping the trailing newline. Returns `nil` on EOF |
| `io.read_bytes` | `read_bytes(handle, n) → string\|nil` | Read up to `n` bytes. Returns `nil` on immediate EOF |
| `io.read_some` | `read_some(handle, max) → string\|error` | One `read(2)`: what is available now, up to `max` bytes, without waiting for more (pipes, terminals, stdin); `""` at the end. It bypasses the handle's buffer: don't mix it with `read_line`/`read_bytes` on one handle (`sea/stream` uses it) |
| `io.read_all` | `read_all(handle) → string` | Read the entire remaining contents of the file as a string |
| `io.write_string` | `write_string(handle, s)` | Write string `s` to the file (no newline appended) |
| `io.write_line` | `write_line(handle, s)` | Write string `s` followed by a newline |
| `io.flush` | `flush(handle)` | Flush the file's write buffer to the OS |

### Scanner (line-by-line iteration)

`io.scanner` is the idiomatic way to iterate over lines without loading the whole file.

| Function | Signature | Description |
|----------|-----------|-------------|
| `io.scanner` | `scanner(path) → scanner\|nil` | Open a file for line-by-line scanning. Returns `nil` if the file cannot be opened |
| `io.scan` | `scan(scanner) → bool` | Advance to the next line. Returns `true` if a line was read, `false` on EOF |
| `io.scan_text` | `scan_text(scanner) → string` | Return the text of the current line (call after `io.scan` returns `true`) |

```sea
local sc = io.scanner("data.txt")
if sc then
    while io.scan(sc) do
        print(io.scan_text(sc))
    end
    io.close(sc)
end
```

---

## Module: `os`

### Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `os.exit` | `exit([code])` | Terminate the process with exit code (default 0) |
| `os.getenv` | `getenv(name) → string\|nil` | Read an environment variable |
| `os.setenv` | `setenv(name, value) → true\|error` | Set an environment variable, for this process and the processes it starts afterwards (`process.run`, `process.spawn`). A name with `=` or a NUL byte is an error. The environment is shared by every goroutine |
| `os.unsetenv` | `unsetenv(name) → true\|error` | Remove an environment variable |
| `os.environ` | `environ() → map` | A copy of the whole environment, name → value |
| `os.exec` | `exec(cmd) → {code=int, output=string}` | Run a shell command; captures stdout+stderr (blocks its thread; module `process` runs commands without a shell, with stdin, env, timeouts and streaming) |
| `os.readfile` | `readfile(path) → string\|nil` | Read entire file contents as a string (module `fs` has the full set: `fs.read` reports why a read failed) |
| `os.filesize` | `filesize(path) → int\|nil` | File size in bytes, or `nil` if not found |
| `os.glob` | `glob(dir, suffix) → array` | Sorted array of full paths in `dir` whose names end with `suffix` |
| `os.executable` | `executable() → string\|nil` | absolute path of the running `sea` binary (what `/proc/self/exe` gave on Linux) |
| `os.urandom` | `urandom(n) → string` | `n` random bytes from the system's cryptographic generator (`getrandom`, `getentropy` on macOS): for keys, salts, nonces (`math.random` is not secure) |
| `os.token` | `token([n]) → string` | `n` random bytes (default 16) as lowercase hex: session tokens, salts, ids |
| `os.uuid` | `uuid() → string` | A random UUID, version 4: `"1b4e28ba-2fa1-41d2-883f-0016d3cca427"` |

### Script arguments (`os.args`)

When a `.sea` file is run with `sea file.sea arg1 arg2 ...`, the arguments are available via `os.args`:

| Index | Value |
|-------|-------|
| `os.args[0]` | `nil` (reserved) |
| `os.args[1]` | First argument after the script filename |
| `os.args[N]` | Nth argument |

Use `len(os.args) - 1` to get the count of arguments passed. `os.script` is the
running script's path as given on the command line (`nil` for `sea -e` and
standard input), to find files that come with it.

```sea
-- sea myscript.sea hello world
local nargs = len(os.args) - 1   -- 2
print(os.args[1])                 -- "hello"
print(os.args[2])                 -- "world"
```

---

## Module: `sync`

| Function | Signature | Description |
|----------|-----------|-------------|
| `sync.mutex` | `mutex() → Mutex` | Create a mutex; methods: `acquire(mu)`, `release(mu)`, `try_lock(mu) → bool` |
| `sync.waitgroup` | `waitgroup() → WaitGroup` | Create a wait-group; methods: `add(wg, n)`, `done(wg)`, `wait(wg)` |
| `sync.once` | `once() → Once` | Create a once-object; method: `do_once(once, fn)` — runs `fn` exactly once across all goroutines |

A goroutine waiting in `acquire` or `wait` parks (retrying every millisecond) instead of blocking its
scheduler thread, so a holder that sleeps or does I/O while holding the lock always gets a thread to
resume on. Any goroutine may `release` a mutex (it has no owner). Outside a goroutine, `acquire` sleeps
briefly between attempts and `wait` blocks.

> **Note:** Sea methods take the receiver as the first explicit argument.
> `mu.acquire(mu)` / `mu.release(mu)`, `wg.add(wg, 1)` / `wg.done(wg)` / `wg.wait(wg)`.

---

## Module: `net`

TCP sockets. Sockets are non-blocking and every wait goes through the netpoller:
inside a goroutine, a call that has to wait **parks only that goroutine** (its
scheduler thread runs other goroutines meanwhile); on the main thread it blocks.
Thousands of idle connections therefore cost no threads. Sockets are plain
integer fds. Failures return an error value (`is_error(x)`, `x.message`); a timeout's
message is `"timeout"`. Timeouts are in milliseconds; omitted or `nil` means wait forever.

| Function | Signature | Description |
|----------|-----------|-------------|
| `net.connect` | `connect(host, port [, timeout]) → fd \| error` | Resolve `host` and connect (tries each address; TCP_NODELAY on). Name lookups run on a small resolver thread pool, so they park too, and the timeout covers them |
| `net.listen` | `listen(port [, host [, backlog]]) → fd \| error` | Bind and listen (SO_REUSEADDR). `port` 0 picks a free port; `host` defaults to all interfaces |
| `net.accept` | `accept(fd [, timeout]) → fd \| error` | Wait for and accept one connection |
| `net.read` | `read(fd, max [, timeout]) → string \| error` | Up to `max` bytes as soon as any arrive; `""` at end of stream |
| `net.write` | `write(fd, data [, timeout]) → int \| error` | Write all of `data` (the timeout applies to each wait); returns `len(data)` |
| `net.close` | `close(fd)` | Close; a goroutine waiting on `fd` wakes with an error |
| `net.shutdown` | `shutdown(fd)` | End both directions without freeing the fd: a goroutine reading or accepting on it wakes (EOF / error) and closes it itself. Use this, not `close`, to stop a socket another goroutine owns — closing it lets the fd number be reused under that goroutine's next read |
| `net.local_port` | `local_port(fd) → int` | Port the socket is bound to |
| `net.peer` | `peer(fd) → string` | `"ip:port"` of the remote end |

`SIGPIPE` is ignored once the module is loaded, so writing to a closed peer
returns an error instead of killing the process.

```sea
local lfd = net.listen(0)
local port = net.local_port(lfd)
go (function()
    local c = net.accept(lfd)
    net.write(c, "echo: " .. net.read(c, 1024, 5000))
    net.close(c)
end)()
local fd = net.connect("127.0.0.1", port, 2000)
net.write(fd, "hi")
print(net.read(fd, 1024, 2000))   -- echo: hi
net.close(fd)
```

Higher-level: `libs/ssl` (TLS over these sockets) and `libs/http` — see [libs.md](libs.md).

---

## Module: `time`

### Duration constants

| Constant | Value |
|----------|-------|
| `time.Nanosecond` | 1 |
| `time.Microsecond` | 1 000 |
| `time.Millisecond` | 1 000 000 |
| `time.Second` | 1 000 000 000 |
| `time.Minute` | 60 000 000 000 |
| `time.Hour` | 3 600 000 000 000 |

| `time.Day` | 86 400 000 000 000 |

> **Durations are int nanoseconds**, and Sea's int holds 48 bits: about 39
> hours (`time.Day` fits, `2 * time.Day` does not). For longer spans use
> `t:add_seconds(s)`, `t:add_date(years, months, days)`, or pass a number
> (`t:add(30 * 86400 * 1e9)`).

```sea
local t = time.parse("2026-10-18 14:30")?            -- ISO 8601, local zone
local d = time.parse("18/10/2026", "02/01/2006", "UTC")?   -- a Go layout
print(t:add_date(0, 1, 0), t:add_seconds(7 * 86400))   -- a month / a week later
print(t:iso(), t:utc():iso(), t:in_zone("+09:00"):format("15:04 MST"))
print(time.date(2026, 12, 25, "UTC"), time.days(time.date(2026, 12, 25):sub(time.now())))
local call = time.date(2026, 11, 3, 9, 0, 0, "America/New_York")
print(call:in_zone("America/Sao_Paulo"):format("15:04 MST"))   -- 11:00 -03
```

A **zone** argument is `"local"` (the default: `TZ` or `/etc/localtime`),
`"UTC"`, a fixed offset (`"+03:00"`, `"-0530"`), or a name of the IANA tz
database (`"America/Sao_Paulo"`, `"Europe/Lisbon"`), read from the system's
files (`$TZDIR`, `/usr/share/zoneinfo`; install `tzdata` in minimal
containers). A time in a named zone follows its summer-time rules: the offset
is the one in effect at that instant, including dates after the last change
the files list (from the POSIX rule they end with). A wall time skipped when
clocks move forward is moved forward by the gap; a repeated one is the first.

### Module functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `time.now` | `now() → Time` | Current wall-clock time (local zone) |
| `time.unix` | `unix(sec [, ns]) → Time` | Construct a Time from Unix seconds + optional nanoseconds |
| `time.date` | `date(year, month, day [, hour, min, sec] [, zone]) → Time` | A Time from its fields (out-of-range ones are normalized: month 13 is January of the next year) |
| `time.parse` | `parse(s [, layout [, zone]]) → Time\|error` | Without a layout, ISO 8601 / RFC 3339: `2026-10-18`, `2026-10-18 14:30`, `2026-10-18T14:30:05.25Z`, `...-03:00`. With a Go layout (tokens below): `parse("18/10/2026", "02/01/2006")`. A text without a zone is in `zone` (default local). A date that does not exist (`2026-02-30`) is an error |
| `time.days` | `days(ns) → number` | Convert a nanosecond duration to days |
| `time.time` | `time() → int` | Current Unix timestamp in seconds (raw integer) |
| `time.sleep` | `sleep(ns)` | Sleep for `ns` nanoseconds. In a goroutine it parks only that goroutine (the scheduler thread keeps running others); on the main thread it blocks |
| `time.clock` | `clock() → number` | CPU time in seconds since program start |
| `time.since` | `since(t) → number` | Nanoseconds elapsed since Time `t` |
| `time.until` | `until(t) → number` | Nanoseconds until Time `t` |
| `time.hours` | `hours(ns) → number` | Convert nanosecond duration to hours |
| `time.minutes` | `minutes(ns) → number` | Convert nanosecond duration to minutes |
| `time.seconds` | `seconds(ns) → number` | Convert nanosecond duration to seconds |
| `time.milliseconds` | `milliseconds(ns) → int` | Convert nanosecond duration to milliseconds |
| `time.microseconds` | `microseconds(ns) → int` | Convert nanosecond duration to microseconds |
| `time.nanoseconds` | `nanoseconds(ns) → int` | Identity — return nanoseconds as integer |

### Time object methods

Returned by `time.now()`, `time.unix()`, etc. All methods take the Time object as the first argument.

| Method | Signature | Description |
|--------|-----------|-------------|
| `t.unix(t)` | `() → int` | Unix timestamp in seconds |
| `t.unix_nano(t)` | `() → number` | Unix timestamp in nanoseconds (float) |
| `t.year(t)` | `() → int` | Year (local time) |
| `t.month(t)` | `() → int` | Month 1–12 (local time) |
| `t.day(t)` | `() → int` | Day of month 1–31 (local time) |
| `t.hour(t)` | `() → int` | Hour 0–23 (local time) |
| `t.minute(t)` | `() → int` | Minute 0–59 (local time) |
| `t.second(t)` | `() → int` | Second 0–59 (local time) |
| `t.nanosecond(t)` | `() → int` | Sub-second nanoseconds |
| `t.weekday(t)` | `() → int` | Day of week 0=Sunday…6=Saturday |
| `t.yearday(t)` | `() → int` | Day of year 1–366 |
| `t.add(t, ns)` | `(ns) → Time` | New Time shifted by `ns` nanoseconds (an int up to ~39 h, or a number) |
| `t.add_seconds(t, s)` | `(s) → Time` | Shifted by `s` seconds (any size, may be fractional) |
| `t.add_date(t, years, months, days)` | `(...) → Time` | Calendar arithmetic in the time's zone, keeping the time of day (Jan 31 + 1 month is Mar 3, as in Go) |
| `t.utc(t)`, `t.local_time(t)`, `t.in_zone(t, zone)` | `() → Time` | The same instant in UTC, the local zone, or another zone (`"+03:00"`, `"-0530"`, `"UTC"`, `"local"`, `"Asia/Tokyo"`) |
| `t.zone(t)` | `() → string` | `"local"`, `"UTC"`, the offset (`"-03:00"`) or the zone's name (`"Europe/Lisbon"`) |
| `t.offset(t)` | `() → int` | Seconds east of UTC at that instant |
| `t.iso(t [, nanos])` | `() → string` | RFC 3339: `2026-10-18T14:30:00-03:00` (`Z` in UTC); with `true`, the fraction of a second |
| `t.sub(t, other)` | `(other) → number` | Duration between two Times in nanoseconds |
| `t.before(t, other)` | `(other) → bool` | True if `t` is earlier than `other` |
| `t.after(t, other)` | `(other) → bool` | True if `t` is later than `other` |
| `t.equal(t, other)` | `(other) → bool` | True if the two Times are equal |
| `t.format(t, layout)` | `(layout) → string` | Format with Go's reference time `Mon Jan 2 15:04:05 MST 2006`: `2006` `06` (year), `01` `1` `Jan` `January` (month), `02` `2` `_2` (day), `15` `03` `3` (hour), `04` `4`, `05` `5`, `PM` `pm`, `Mon` `Monday`, `MST`, `-0700` `-07:00` `Z07:00` (offset; `Z` for UTC), `.000` / `.999` (fraction, trailing zeros dropped with 9s). `time.parse` takes the same tokens |
| `t.string(t)` | `() → string` | Format as `"YYYY-MM-DD HH:MM:SS"` |

---

## Module: `gc`

Memory is reference counted; the cycle collector frees objects that only keep
each other alive (see [memory.md](memory.md#cycles)). It runs on its own; these
functions are for forcing and inspecting it.

| Function | Signature | Description |
|----------|-----------|-------------|
| `gc.collect()` | `() → int` | Run a cycle collection now (current goroutine); returns the number of objects freed |
| `gc.stats()` | `() → table` | For the current goroutine: `collections`, `freed` (objects), `traversed` (objects examined), `candidates` (pending now), `buffered` (candidates recorded in total), `threshold` (`-1` when the collector is off) |

```sea
local a = {}
local b = {}
a.peer = b
b.peer = a
a = nil
b = nil
print(gc.collect())        -- 2
```

Environment: `SEA_GC=0` turns the cycle collector off (cycles leak, but no
candidates are tracked); `SEA_GC_THRESHOLD=n` sets the minimum number of
candidates before a collection runs (default 10000).

---

## Module: `sea/parser`

The Sea lexer and parser as a library, built into the VM — the same parser the
compiler uses, so its trees always match the language. For linters,
formatters, code generators and editor tooling (`examples/seafmt` is a small
formatter/checker built on it).

```sea
local parser = require("sea/parser")

local ast = parser.parse("local x = 1 + 2")
print(ast.stmts[0].values[0].op)          -- +

local d = parser.check(src, {name = "app.sea"})
if d ~= nil then print(d.text) end        -- app.sea:3:9: expected 'then' but got 'end'

print(parser.format("local   t={a=1}"))   -- local t = {a = 1}
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `parse` | `parse(src [, opts]) → node \| error` | The tree of `src` (a `"block"` node). A syntax error is an error value `"name:line:col: message"` |
| `parse_file` | `parse_file(path [, opts]) → node \| error` | The same, reading a file (`name` defaults to the path) |
| `tokenize` | `tokenize(src [, opts]) → array \| error` | The tokens: `{kind, text, line, col, value}` — `kind` is `"name"`, `"keyword"`, `"number"`, `"string"`, `"op"` or `"comment"`; `value` is the number or the decoded string |
| `check` | `check(src [, opts]) → nil \| table` | Parse and compile without running: `nil`, or the first error as `{phase, message, line, col, text}` — `phase` is `"syntax"` or `"compile"` (compile errors have no `col`); `text` is `"name:line[:col]: message"` |
| `children` | `children(node) → array` | The child nodes, in source order |
| `walk` | `walk(node, fn)` | Calls `fn(node, parent)` on every node, parents first; returning `false` skips that node's children. The children are read after `fn` returns, so `fn` may change them. A panic in `fn` propagates |
| `format` | `format(src_or_node [, opts]) → string \| error` | Source code printed in the canonical layout (below) |
| `globals` | `globals() → array` | The names of the global variables defined now (built-in functions and modules, and any the program set), sorted |

Options: `name` (used in error messages, default `"<input>"`); for `parse` and
`parse_file`, `comments = true` adds the comments to the root as
`comments: [{text, line, col, inline}]` (`inline`: code precedes it on its
line); for `tokenize`, `comments = true` includes comment tokens; for `format`,
`indent` (number of spaces, or a string; default 4).

**Nodes** are tables with `type`, `line` and `col` (1-based; columns count
bytes). A statement's position is its first token; an operator's, call's or
access's is the operator token (`+`, `(`, `[`, `.`, `:`). Blocks, tables and
calls also have `end_line`, the line of the token that closes them (`end`,
`else`, `elseif`, `case`, `}`, `)`). A node written in parentheses has
`paren = true`.

| `type` | Fields |
|--------|--------|
| `"nil"`, `"break"`, `"continue"`, `"vararg"`, `"map_lit"` | — |
| `"bool"` | `value` |
| `"number"` | `value` (int or float), `is_float`, `raw` (the source text, e.g. `0x1F`) |
| `"string"` | `value` (decoded), `raw` (the literal as written, with its quotes) |
| `"ident"` | `name` |
| `"unop"` | `op` (`-`, `not`, `~`), `operand` |
| `"binop"` | `op`, `left`, `right` |
| `"index"` | `object`, `index` (`nil` in `t[] = v`) |
| `"field"` | `object`, `field` (string) |
| `"call"` | `callee`, `args`, `end_line` |
| `"method_call"` | `object`, `method` (string), `args`, `end_line` |
| `"func"` | `params: [{name, is_vararg}]`, `body`, `name` (string or `nil`), `is_method` |
| `"table"` | `fields: [{key, value}]` (`key` is `nil` for positional fields; `x = 1` has a `"string"` key), `end_line` |
| `"array_lit"` | `cap` (node or `nil`) |
| `"chan_recv"` | `channel` |
| `"chan_send"` | `channel`, `value` |
| `"propagate"` | `expr` (`expr?`) |
| `"block"`, `"do"` | `stmts`, `end_line` |
| `"local"` | `names` (strings), `values`, `is_const` |
| `"assign"` | `targets`, `values` |
| `"if"` | `cond`, `then_body`, `else_body` (`nil`, a block, or an `"if"` node for `elseif`) |
| `"while"` | `cond`, `body` |
| `"for_num"` | `var` (string), `start`, `limit`, `step` (or `nil`), `body` |
| `"for_in"` | `vars` (strings), `iter`, `body` |
| `"return"` | `values` |
| `"go"`, `"defer"` | `call` |
| `"select"` | `cases: [{stmt, body, is_default}]` |
| `"panic"` | `value` |
| `"del"` | `target` |
| `"expr_stmt"` | `expr` |

Binary operators: `or and == ~= < <= > >= | ~ & << >> .. + - * / // % ^` (`!=`
in the source is `~=` in the tree). Some syntax is sugar in the tree:
`local function f` is a `"local"` whose value is a `"func"` named `f`;
`function a.b:m(x)` is an `"assign"` to `a.b.m` of a `"func"` with `name = "a"`,
`is_method = true` and `self` added as its first parameter; `f"s"` is a call.
Names declared by `local`, `for` and parameters are strings, not `"ident"`
nodes, so `walk` does not visit them.

**`format`** takes source text or a node (a tree from `parse`, or one built by
hand — nodes may be tables or structs, lists must be arrays). It prints one
statement per line, indents blocks, spaces operators and adds parentheses only
where precedence needs them, keeping the ones written in the source. From
source text (or a tree parsed with `comments = true`) it keeps the comments,
single blank lines, one-line blocks (`if x then return end`), line breaks
inside long expressions and argument lists, and literals as written (`raw`,
when it still matches `value`); trailing comments on consecutive lines are
aligned. Formatting is idempotent, and the formatted code parses to the same
tree.

Limits: the parser rejects code nested more than 1000 levels (expressions and
blocks) or with more than 100 nested functions, with an error rather than a
stack overflow; these limits apply to every program, not just this library.

---

## Module: `list`

Helpers over arrays (also positional tables `{a, b}`, `int_array` and
`float_array`) and over the keys and values of tables, maps and structs —
what Python's built-ins and list comprehensions do. Every function returns a
new value and leaves its input unchanged. Callbacks receive `(value, index)`;
a function may take just the value.

```sea
local nums = [3, 1, 4, 1, 5, 9]
print(list.map(nums, function(x) return x * 2 end))       -- [6, 2, 8, 2, 10, 18]
print(list.filter(nums, function(x) return x > 2 end))    -- [3, 4, 5, 9]
print(list.sum(nums), list.max(nums), list.unique(nums))  -- 23  9  [3, 1, 4, 5, 9]
local people = [{name = "ana", age = 31}, {name = "bob", age = 25}]
print(list.max(people, function(p) return p.age end).name)            -- ana
print(list.group_by(people, function(p) return p.age > 30 and "senior" or "junior" end))
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `list.map` | `map(a, fn) → array` | `fn(v, i)` for each element |
| `list.filter` | `filter(a, fn) → array` | The elements for which `fn(v, i)` is true |
| `list.reduce` | `reduce(a, fn [, init]) → value` | `acc = fn(acc, v, i)` over the elements, from `init` (default: the first element; an empty array without `init` is an error) |
| `list.find` | `find(a, fn) → value\|nil` | The first element for which `fn(v, i)` is true |
| `list.index` | `index(a, x) → int\|nil` | The index of the first element `== x` |
| `list.contains` | `contains(a, x) → bool` | Whether an element `== x` |
| `list.any`, `list.all` | `any(a, fn) → bool` | Whether `fn(v, i)` is true for some / every element (`all` of `[]` is true) |
| `list.count` | `count(a, fn_or_value) → int` | How many elements `fn` accepts, or are `== value` |
| `list.sum` | `sum(a [, fn]) → number` | The sum of the elements (or of `fn(v, i)`): an int while every term is an int, `0` for `[]` |
| `list.min`, `list.max` | `min(a [, fn]) → value\|nil` | The element with the smallest / largest value (or key `fn(v, i)`), numbers or strings in the natural order; the first one on ties; `nil` for `[]` |
| `list.mean`, `list.median` | `mean(a) → number\|nil` | The arithmetic mean / the middle value (mean of the two middle ones for an even count); `nil` for `[]` |
| `list.stdev` | `stdev(a) → number\|nil` | The sample standard deviation (as Python's `statistics.stdev`); `nil` with fewer than 2 elements |
| `list.range` | `range(n)`, `range(from, to [, step]) → array` | `[0, ..., n-1]`; or `from` to `to` **inclusive**, like a numeric `for` (`range(1, 10, 3)` is `[1, 4, 7, 10]`) |
| `list.unique` | `unique(a) → array` | Without repeats, first occurrences kept (by map key: `1` and `1.0` differ) |
| `list.slice` | `slice(a, from [, to]) → array` | Elements `from` to `to - 1` (**`to` exclusive**, as in Python/JS; negative indexes count from the end; `to` defaults to the end) |
| `list.concat` | `concat(a, b, ...) → array` | The elements of all |
| `list.flatten` | `flatten(a) → array` | The elements of nested arrays, one level |
| `list.reversed` | `reversed(a) → array` | A reversed copy (`sort.reverse` reverses in place) |
| `list.copy` | `copy(a) → array` | A shallow copy |
| `list.chunk` | `chunk(a, n) → array` | Arrays of `n` elements (the last may be shorter) |
| `list.zip` | `zip(a, b, ...) → array` | `[[a[0], b[0]], [a[1], b[1]], ...]`, as long as the shortest |
| `list.group_by` | `group_by(a, fn) → map` | `fn(v, i)` → the array of elements with that key |
| `list.count_by` | `count_by(a, fn) → map` | `fn(v, i)` → how many elements have that key (like Python's `Counter`) |
| `list.inc` | `inc(m, k [, n]) → number` | Counter update: `m[k] = (m[k] or 0) + n` (`n` defaults to 1) on a table, map or struct (a struct only for a field it has); returns the new value. Changes `m` |
| `list.most_common` | `most_common(m [, n]) → array` | `[[key, count], ...]` of a table, map or struct of numbers: largest count first, ties by key; only the first `n` when given (Python's `Counter.most_common`) |
| `list.keys`, `list.values` | `keys(t) → array` | The keys / values of a table or map (sorted by key: numbers, then strings), a struct (its literal's order) or an array (indexes) |
| `list.items` | `items(t) → array` | `[[key, value], ...]` in the same order |

A panic or runtime error in a callback stops the function and goes on as
any panic.

```sea
local words = [=]
for _, w in string.split("a b a c b a", " ") do list.inc(words, w) end
print(list.most_common(words, 2))               -- [ ["a", 3], ["b", 2] ]
```

---

## Module: `sort`

Sorting and binary search. Works on arrays, `int_array` and `float_array`.
Without a comparator the order is the natural one: numbers by value (ints and
floats and bigints together, NaN last) and strings by bytes. Every sort is **stable**:
equal elements keep their order.

| Function | Signature | Description |
|----------|-----------|-------------|
| `sort.sort` | `sort(a [, less]) → a` | Sort in place. `less(x, y)` returns true when `x` goes before `y` |
| `sort.by` | `by(a, key [, reverse]) → a` | Sort in place by a key: a function (called once per element), a field name of tables/structs/maps, or an array of field names (`"-name"`: that field descending). Keys compare in the natural order; a key that is an array (from a field list, or returned by the function) compares element by element, like a Python tuple |
| `sort.sorted` | `sorted(a [, less]) → array` | A sorted copy (same kind of array); `a` is untouched |
| `sort.reverse` | `reverse(a) → a` | Reverse in place |
| `sort.is_sorted` | `is_sorted(a [, less]) → bool` | Whether `a` is already in order |
| `sort.search` | `search(a, x [, less]) → int` | Binary search in a sorted array: the first index whose element is not less than `x` (`len(a)` if none) — where `x` is, or would be inserted |
| `sort.asc`, `sort.desc` | comparators | The natural order and its reverse: `sort.sort(a, sort.desc)`. Passed to a sort function they run in C, without calling back into Sea |

```sea
local scores = []
scores[] = {name = "ana", points = 7}
scores[] = {name = "bia", points = 9}
scores[] = {name = "caio", points = 7}
sort.by(scores, "points", true)                 -- bia 9, ana 7, caio 7 (stable)
sort.by(scores, ["-points", "name"])            -- bia 9, ana 7, caio 7
sort.by(scores, function(s) return [-s.points, s.name] end)   -- the same

local words = string.split("pear apple fig", " ")
sort.sort(words, function(a, b) return len(a) < len(b) end)   -- fig pear apple
local i = sort.search(sort.sorted(words), "grape")             -- 1
```

Errors are runtime errors (panics), raised before the array is changed:
values the natural order cannot compare (a number with a string, tables
without a comparator), a comparator that is not a function. A comparator
written in Sea works on a copy, so a panic inside it leaves the array as it
was, and a comparator that changes the array is an error.

---

## Module: `bigint`

Integers of any size. Sea's `int` holds 48 bits (±140 737 488 355 327) and a
`number` is exact only up to 2^53; a `bigint` keeps every digit. It is an
explicit type: values come from `bigint.new`, from this module's functions,
and from libraries that read wide integers (`libs/duckdb`, `json.decode` with
`bigint = true`).

| Function | Signature | Description |
|----------|-----------|-------------|
| `bigint.new` | `new(v [, base]) → bigint\|error` | From an int, an integral number, a bigint or a string: decimal, or `0x`/`0o`/`0b` prefixed, or in `base` 2–36; a sign and `_` between digits are allowed. A string that is not an integer, or a fractional number, is an error value |
| `bigint.add`, `sub`, `mul` | `add(a, b) → bigint` | Sum, difference, product |
| `bigint.div`, `mod` | `div(a, b) → bigint` | Floor division and modulo, like `//` and `%` on ints (`div(-7, 2)` is `-4`, `mod(-7, 2)` is `1`); a zero divisor is a runtime error |
| `bigint.pow` | `pow(a, e) → bigint` | `a` to the power `e` (`e >= 0`). Results over 128 Mbit are an error |
| `bigint.neg`, `abs` | `neg(a) → bigint` | Negation, absolute value |
| `bigint.cmp` | `cmp(a, b) → int\|nil` | -1, 0 or 1 (`nil` against NaN) |
| `bigint.tostring` | `tostring(a [, base]) → string` | Digits in base 2–36 (default 10) |
| `bigint.toint` | `toint(a) → int\|nil` | The int, or `nil` when it does not fit Sea's 48-bit range |
| `bigint.tonumber` | `tonumber(a) → number` | The nearest number |

Operands may be bigints, ints or integral numbers; results are always
bigints. The arithmetic operators do **not** take bigints (`a + 1` is a
runtime error that names these functions): the compiler counts on `+ - * /`
producing plain numbers. What does take them: `==` and `~=`, `< <= > >=`
(against bigints, ints and numbers, by value), `tostring`, `..`, `print`,
`string.format("%d")`, `tonumber`, the natural order of `sort`, and table and
map keys (by value; `bigint.new(5)` and `5` are different keys).

```sea
local f = bigint.new(1)
for i = 2, 30 do f = bigint.mul(f, i) end
print(f)                                   -- 265252859812191058636308480000000
print(f > 1e30, bigint.mod(f, 7) == 0)     -- true  true
local id = bigint.new("9223372036854775807")
print(bigint.tostring(id, 16), bigint.toint(id))   -- 7fffffffffffffff  nil
```

---

## Module: `utf8`

Strings read as UTF-8. The `string` module and `len()` count bytes; these
functions count characters (code points). Character indexes are 1-based and
negative ones count from the end, as in `string.sub`. A byte that is not part
of a valid UTF-8 sequence counts as one character, and the functions keep it
as it is (`utf8.valid`/`utf8.invalid` find such bytes, `utf8.sanitize`
replaces them). Valid means RFC 3629: no overlong forms, no surrogates,
nothing above U+10FFFF.

| Function | Signature | Description |
|----------|-----------|-------------|
| `utf8.len` | `len(s) → int` | Number of characters |
| `utf8.valid` | `valid(s) → bool` | Whether `s` is valid UTF-8 |
| `utf8.invalid` | `invalid(s) → int\|nil` | Byte position (1-based) of the first invalid byte, `nil` if none |
| `utf8.sanitize` | `sanitize(s [, repl]) → string` | `s` with each invalid byte replaced by `repl` (default U+FFFD `"�"`) |
| `utf8.char` | `char(cp, ...) → string`, `char(array) → string` | The UTF-8 encoding of code points; an invalid code point is a runtime error |
| `utf8.codepoint` | `codepoint(s [, i]) → int\|nil` | Code point of character `i` (default 1); `nil` past the end or at an invalid byte |
| `utf8.codepoints` | `codepoints(s) → array` | Every code point (invalid bytes give 0xFFFD) |
| `utf8.chars` | `chars(s) → array` | The characters as one-character strings |
| `utf8.sub` | `sub(s, i [, j]) → string` | Characters `i` to `j` (inclusive; `j` defaults to -1) |
| `utf8.offset` | `offset(s, n) → int\|nil` | Byte position (1-based) where character `n` starts; `len + 1` gives the position after the end; `nil` out of range. Use it to pass character positions to `string.*` |
| `utf8.upper`, `utf8.lower` | `upper(s) → string` | Simple case mapping of Latin (with Latin-1, Extended-A/B and Vietnamese letters), Greek, Cyrillic, Armenian and fullwidth letters; other characters unchanged. One code point maps to one (`ß` stays `ß`) |
| `utf8.reverse` | `reverse(s) → string` | Characters in reverse order |
| `utf8.width` | `width(s) → int` | Terminal columns: East Asian wide and fullwidth characters and emoji take 2, combining marks and zero-width characters 0, control characters 0 |

```sea
local s = "Ação 日本 😀"
print(len(s), utf8.len(s))              -- 18  9
print(utf8.upper(s), utf8.sub(s, -4))   -- AÇÃO 日本 😀  日本 😀
print(utf8.codepoint("€"), utf8.char(0x48, 0xE9))   -- 8364  Hé
print(utf8.width("日本"))                -- 4
```

Native libraries use the same code through `src/sea_utf8.h` (`sea_utf8_decode`,
`sea_utf8_encode`, `sea_utf8_check`, `sea_utf8_len`, `sea_utf8_next`,
`sea_utf8_upper`/`lower`, `sea_utf8_cpwidth`), included by `sea.h`.

---

## Module: `fs`

The file system. Failures are **error values** naming the call, the path and
the system's reason (`fs.read: data.txt: No such file or directory`), so
`fs.read(p)?` passes them up; `exists`, `isfile` and `isdir` answer with a
bool. Calls block their scheduler thread, like other file I/O. Modes are
ints (`0x1A4` is 0644; Sea has no octal literals).

| Function | Signature | Description |
|----------|-----------|-------------|
| `fs.read` | `read(p) → string\|error` | The whole file (any bytes) |
| `fs.write` | `write(p, data [, opts]) → true\|error` | Create or replace the file. `opts`: `append` (bool), `mode` (int, default 0666 less the umask, for a new file), `atomic` (bool: write a temporary file in the same directory, `fsync`, then rename it over `p`, so readers see the old or the new content, never part) |
| `fs.append` | `append(p, data) → true\|error` | Add to the end (creating the file) |
| `fs.exists` | `exists(p) → bool` | Something is there (a dangling link counts) |
| `fs.isfile`, `fs.isdir` | `isfile(p) → bool` | A regular file / a directory (links followed) |
| `fs.stat`, `fs.lstat` | `stat(p) → table\|error` | `type` (`"file"`, `"dir"`, `"link"`, `"fifo"`, `"socket"`, `"char"`, `"block"`), `size`, `mode` (permission bits), `mtime`/`atime`/`ctime` (seconds as numbers), `uid`, `gid`, `ino`, `nlink`. `lstat` does not follow a link |
| `fs.list` | `list(p) → array\|error` | Names in a directory, sorted (no `.`/`..`) |
| `fs.walk` | `walk(p [, opts]) → array\|error` | Every path under `p` (joined to it), depth first, sorted in each directory. `opts`: `files` (only non-directories), `max_depth` (1 = like `list`), `follow_links` (descend into linked directories; loops are skipped) |
| `fs.mkdir` | `mkdir(p [, opts]) → true\|error` | `opts`: `parents` (create missing parents; an existing directory is fine), `mode` (default 0777 less the umask) |
| `fs.remove` | `remove(p [, opts]) → true\|error` | A file, link or empty directory. `opts`: `recursive` (a whole tree; links are removed, not followed), `force` (a missing path is fine) |
| `fs.rename` | `rename(from, to) → true\|error` | Move or rename (same file system) |
| `fs.copy` | `copy(from, to [, opts]) → true\|error` | Copy a file, keeping its mode. `opts`: `recursive` (a directory tree; links copied as links), `overwrite` (default `true`) |
| `fs.symlink` | `symlink(target, link) → true\|error` | Create `link` pointing at `target` |
| `fs.readlink` | `readlink(p) → string\|error` | Where a link points |
| `fs.realpath` | `realpath(p) → string\|error` | Absolute path with links and `..` resolved (the path must exist) |
| `fs.chmod` | `chmod(p, mode) → true\|error` | Set the permission bits |
| `fs.touch` | `touch(p) → true\|error` | Create an empty file, or set an existing one's times to now |
| `fs.tempdir`, `fs.tempfile` | `tempdir([prefix]) → string\|error` | Create a new directory / empty file under `$TMPDIR` (or `/tmp`) with a unique name; removing it is up to you |
| `fs.cwd`, `fs.chdir` | `cwd() → string`, `chdir(p) → true\|error` | The working directory (process-wide: every goroutine sees the change) |
| `fs.glob` | `glob(pattern) → array\|error` | Paths matching a shell pattern (`*`, `?`, `[...]`, `{a,b}`, `~`), sorted; none: `[]` |

```sea
local dir = fs.tempdir("build-")?
fs.mkdir(path.join(dir, "out/logs"), {parents = true})?
fs.write(path.join(dir, "out/config.json"), json.encode(cfg), {atomic = true})?
for _, f in fs.walk(dir, {files = true})? do print(path.relative(dir, f), fs.stat(f).size) end
fs.remove(dir, {recursive = true})?
```

---

## Module: `path`

Paths as strings, with the rules of Go's `path/filepath` on Unix. Nothing here
touches the file system except `path.abs`, which reads the working directory.

| Function | Signature | Description |
|----------|-----------|-------------|
| `path.join` | `join(a, b, ...) → string` | Parts joined with `/` and normalized; an absolute part starts over (`join("a", "/b")` is `"/b"`); empty parts are skipped; no parts give `""` |
| `path.normalize` | `normalize(p) → string` | One `/` between elements, no `.`, `..` removes the element before it (dropped at the root, kept at the start of a relative path); `""` becomes `"."`: `"a//b/./c/.."` → `"a/b"` |
| `path.dirname` | `dirname(p) → string` | Everything before the last element, normalized (`"a/b"` → `"a"`, `"a"` → `"."`, `"/a"` → `"/"`) |
| `path.basename` | `basename(p [, suffix]) → string` | The last element (`"a/b/"` → `"b"`, `""` → `"."`, `"/"` → `"/"`); a matching `suffix` is removed |
| `path.ext` | `ext(p) → string` | The extension with its dot (`"a/b.tar.gz"` → `".gz"`); `""` when there is none. Leading dots do not start one (`".bashrc"` has none) |
| `path.stem` | `stem(p) → string` | The last element without its extension |
| `path.parts` | `parts(p) → array` | The elements of the normalized path, `"/"` first when absolute |
| `path.isabs` | `isabs(p) → bool` | Starts with `/` |
| `path.abs` | `abs(p) → string` | Absolute and normalized (joined to the working directory; links are not resolved, see `fs.realpath`) |
| `path.relative` | `relative(base, target) → string\|error` | A path that joined to `base` gives `target` (`relative("/a/b/c", "/a/x")` is `"../../x"`); both absolute or both relative, else an error |
| `path.sep` | `"/"` | The separator |

---

## Module: `re`

Regular expressions with RE2's syntax and guarantees: matching takes time
linear in the input for **every** pattern (a bounded backtracker that never
revisits a state for short texts, a Pike VM for long ones: no exponential
backtracking, so
`(a*)*b` on a long string is as fast as any other), and the first match is the
leftmost, preferring earlier alternatives and greedy repeats (as in Perl, Go,
Python). There are no backreferences and no lookaround. Patterns and subjects
are UTF-8: `.` and classes match characters; an invalid byte is one
character. Results are checked against Go's `regexp` (same semantics).

| Function | Signature | Description |
|----------|-----------|-------------|
| `re.compile` | `compile(pattern [, flags]) → regex\|error` | Compile once, reuse. `flags`: a string of `i` (case-insensitive), `m` (`^`/`$` at line breaks), `s` (`.` matches `\n`), `U` (repeats lazy by default). A bad pattern is an error value: `re: missing closing ) at position 4 in /(ab/` |
| `re.test` | `test(p, s) → bool` | Whether `p` matches anywhere in `s` |
| `re.match` | `match(p, s [, init]) → match\|nil` | The first match at or after byte `init` (1-based, negative from the end) |
| `re.captures` | `captures(p, s [, init]) → array\|nil` | The first match's strings only: `[whole, group 1, ...]` (`nil` for a group that did not take part), like Go's `FindStringSubmatch` or Python's `m.groups()`. About twice as fast as `match`, which also builds `spans` and `named`: use it to parse lines |
| `re.all` | `all(p, s [, n]) → array` | Successive non-overlapping matches (at most `n`), as match tables. An empty match right after the previous match is skipped |
| `re.findall` | `findall(p, s [, n]) → array` | The same, as strings (the whole match) |
| `re.gsub` | `gsub(p, s, repl [, n]) → string` | Replace every match (or the first `n`). `repl`: a string where `$0`/`$1`/`${12}`/`${name}` insert groups and `$$` is a `$`; a function `fn(match)` returning the replacement (`nil` or `false` keeps the match; other values go through `tostring`); or a table from matched text to replacement (missing: kept) |
| `re.split` | `split(p, s [, n]) → array` | The pieces between matches (`n > 0`: at most `n` pieces, the last holding the rest; `n = 0`: none), like Go's `regexp.Split`: `split("x*", "axbc")` is `{"a", "b", "c"}` |
| `re.escape` | `escape(s) → string` | `s` with the metacharacters `\ . + * ? ( ) \| [ ] { } ^ $` escaped, to match it literally |

`p` is a pattern string (compiled once and kept in a small per-thread cache) or a regex from `re.compile`,
whose methods are the same functions: `rx:test(s)`, `rx:match(s [, init])`,
`rx:captures`, `rx:all`, `rx:findall`, `rx:gsub`, `rx:split`. A regex also has `pattern`,
`flags`, `groups` (number of capture groups) and `names` (`{name = index}`).
A pattern string that does not compile is a runtime error in these functions.

A **match** is a table: `text` (the matched text), `start`/`stop` (1-based
byte positions, inclusive, like `string.find`), `groups` (array: `[0]` the
whole match, `[i]` group `i`, `nil` when it did not take part), `spans`
(`[i] = [start, stop]` or `nil`) and `named` (`{name = text}`).

**Syntax:**

| | |
|-|-|
| Characters | literal; `.` any but `\n`; `\n \t \r \f \v \a \e \0`; `\x41`, `\x{263A}`, `\u{1F600}`; `\` before punctuation makes it literal |
| Classes | `[abc]`, `[^a-z]`, `[\d\-.]`, `[[:alpha:]]` (also `alnum ascii blank cntrl digit graph lower print punct space upper word xdigit`, `[[:^digit:]]`); `\d \w \s` (ASCII: `[0-9]`, `[0-9A-Za-z_]`, `[\t\n\f\r ]`) and `\D \W \S` |
| Anchors | `^` `$` (text start/end; line start/end with `m`), `\A` `\z` (always text), `\b` `\B` (ASCII word boundary) |
| Groups | `(x)` capturing, `(?P<name>x)` or `(?<name>x)` named, `(?:x)` non-capturing, `x\|y` alternation |
| Repeats | `*` `+` `?` `{n}` `{n,}` `{n,m}` (counts up to 1000), lazy with a trailing `?` (`*?`, `{2,5}?`); `{` that is not a valid repeat is literal |
| Flags | `(?i)` `(?m)` `(?s)` `(?U)`, clearing with `-` (`(?i-s)`), until the end of the enclosing group; `(?i:x)` for `x` only. Case-insensitive matching folds Latin, Greek, Cyrillic and Armenian letters (`utf8.lower`/`upper`) |

```sea
local m = re.match("(?P<user>[\\w.]+)@(?P<host>[\\w.]+)", "write to ana@sea.dev")
print(m.named.user, m.named.host, m.start)            -- ana  sea.dev  10
print(re.captures("(\\w+)=(\\d+)", "x a=1"))             -- ["a=1", "a", "1"]
local date = re.compile("(\\d{4})-(\\d\\d)-(\\d\\d)")
print(date:gsub("on 2026-10-16", "$3/$2/$1"))           -- on 16/10/2026
print(re.gsub("\\d+", "a1b22", function(m) return tonumber(m.text) * 2 end))   -- a2b44
print(string.join(re.split("\\s*;\\s*", "a ; b;c"), ","))  -- a,b,c
```

---

## Module: `process`

Child processes. Pipes are non-blocking and waited on through the netpoller,
so in a goroutine a wait parks only that goroutine (`os.exec` is the simple
blocking form). A command is an **array of arguments**, run directly and
searched in `PATH` (no shell, no quoting problems), or a **string**, run with
`/bin/sh -c`. Failing to start (`process.run: rg: No such file or directory`)
is an error value; a command that runs and fails is a result with `ok = false`.

| Function | Signature | Description |
|----------|-----------|-------------|
| `process.run` | `run(cmd [, opts]) → result\|error` | Run to completion, collecting output. `result`: `code` (exit code, `nil` when killed by a signal), `signal` (the signal number, or `nil`), `ok` (`code == 0`), `stdout`, `stderr`, `timed_out`, `pid` |
| `process.spawn` | `spawn(cmd [, opts]) → proc\|error` | Start a process and talk to it through its pipes (methods below) |
| `process.pid` | `pid() → int` | This process's id |
| `process.kill` | `kill(pid [, sig]) → true\|error` | Send a signal (default `TERM`) |
| `process.signals` | table | `HUP INT QUIT KILL USR1 USR2 PIPE ALRM TERM CHLD CONT STOP TSTP` → numbers |

**Options** (both functions):

| Key | Description |
|-----|-------------|
| `cwd` | Working directory of the child |
| `env` | Table of variables to set over the current environment; `false` removes one (`{PATH = "/bin", HOME = false}`) |
| `clear_env` | Start from an empty environment (then `env`) |
| `stdin` | `run`: a string to feed the child, or `"null"` (default) / `"inherit"`. `spawn`: `"pipe"` (default), `"null"`, `"inherit"` |
| `stdout`, `stderr` | `"pipe"` (default), `"null"`, `"inherit"` (the parent's); `stderr` also `"stdout"` (merged into stdout) |
| `timeout` | `run` only: milliseconds; the child is then killed (`SIGKILL`) and the result has `timed_out = true` |

**A spawned process** (`proc`) has `pid` and the pipe fds `stdin`, `stdout`,
`stderr` (`-1` when not a pipe), and methods:

| Method | Description |
|--------|-------------|
| `proc:write(data [, timeout_ms]) → int\|error` | Write all of `data` to its stdin |
| `proc:close_stdin()` | Close its stdin (most filters then finish) |
| `proc:read([max [, timeout_ms]]) → string\|error` | Up to `max` bytes (default 65536) of its stdout as they arrive; `""` at the end |
| `proc:read_stderr([max [, timeout_ms]])` | The same for stderr |
| `proc:read_line([timeout_ms]) → string\|nil\|error` | The next line of stdout without `\n` (or `\r\n`); `nil` at the end |
| `proc:wait([timeout_ms]) → status\|error` | Wait for it to exit: `{code, signal, ok}` (the same table on later calls); closes its pipes. A timeout is the error `"timeout"` and the process keeps running |
| `proc:kill([sig]) → true\|error` | Send a signal (default `TERM`) |

Call `wait` for every spawned process (until then it stays a zombie).

```sea
local r = process.run({"git", "log", "--oneline", "-3"}, {cwd = repo, timeout = 5000})?
if r.ok then print(r.stdout) else print("git failed:", r.code, r.stderr) end

local p = process.spawn({"sort"})?
p:write("pear\napple\nfig\n")
p:close_stdin()
local line = p:read_line()
while line != nil do
    print(line)
    line = p:read_line()
end
print(p:wait().code)
```

---

## Module: `encoding`

Binary-to-text encodings, in C. Decoders accept what other tools write
(whitespace is skipped; padding is optional) and return an **error value**
that says where the input went wrong: `encoding.base64_decode: invalid
character at byte 9`.

| Function | Signature | Description |
|----------|-----------|-------------|
| `encoding.base64_encode` | `base64_encode(s [, opts]) → string` | RFC 4648. `opts`: `url` (the `-_` alphabet, unpadded unless `pad = true`), `pad` |
| `encoding.base64_decode` | `base64_decode(s) → string\|error` | Either alphabet, with or without padding |
| `encoding.base32_encode` | `base32_encode(s [, pad]) → string` | RFC 4648 (`pad` default `true`) |
| `encoding.base32_decode` | `base32_decode(s) → string\|error` | Case-insensitive; `-` and whitespace skipped |
| `encoding.hex_encode` | `hex_encode(s [, upper]) → string` | Two hex digits per byte (lowercase by default) |
| `encoding.hex_decode` | `hex_decode(s) → string\|error` | Either case; whitespace and `:` between bytes skipped |
| `encoding.url_encode` | `url_encode(s [, opts]) → string` | Percent-encode every byte but `A-Z a-z 0-9 - . _ ~` (a URL component); `opts.plus` (or `true`): spaces as `+`, as in forms |
| `encoding.url_decode` | `url_decode(s [, opts]) → string\|error` | `%XX` sequences; with `plus`, `+` is a space |
| `encoding.html_escape` | `html_escape(s) → string` | `& < > " '` as entities |

---

## Module: `hash`

Digests, MACs and checksums, in C (no OpenSSL; `libs/ssl` has TLS and
public-key crypto). Digests are lowercase hex strings, or the raw bytes with
`raw = true` (a bool argument or `{raw = true}`).

| Function | Signature | Description |
|----------|-----------|-------------|
| `hash.md5`, `sha1`, `sha224`, `sha256`, `sha384`, `sha512` | `sha256(s [, raw]) → string` | The digest of `s` (MD5 and SHA-1 only for compatibility: they are broken for security) |
| `hash.hmac` | `hmac(alg, key, data [, raw]) → string` | HMAC (RFC 2104) with `alg` one of the names above (`"sha256"`, `"SHA-256"`) |
| `hash.new` | `new(alg) → hasher` | Incremental: `h:update(s)` (returns `h`, so calls chain), `h:digest([raw])` (the hasher can go on). Fields `algorithm`, `size` (digest bytes) |
| `hash.crc32` | `crc32(s [, crc]) → int` | CRC-32 (zlib, PNG, gzip); pass the previous value to continue over pieces |
| `hash.adler32` | `adler32(s [, adler]) → int` | Adler-32 (zlib) |
| `hash.fnv1a` | `fnv1a(s) → int` | FNV-1a, 32 bits (fast, not cryptographic) |
| `hash.equal` | `equal(a, b) → bool` | Compare in time that depends only on the lengths: use it to check MACs and tokens |

```sea
local sig = hash.hmac("sha256", secret, body)
if not hash.equal(sig, request.headers["x-signature"]) then return error("bad signature") end
local h = hash.new("sha256")
for _, chunk in chunks do h:update(chunk) end
print(h:digest(), encoding.base64_encode(hash.sha256(data, true), {url = true}))
```

---

## Collection Literals

| Syntax | Type | Description |
|--------|------|-------------|
| `{}` | table (struct-mode) | Fixed-layout struct; field names set at compile time with `{ k = v }` |
| `[]` | array | Empty dynamic array |
| `[a, b, c]` | array | Array with these elements (a trailing comma is allowed). Write nested arrays with a space after the first bracket, `[ [1, 2], [3] ]`: `[[` starts a long string |
| `[=]` | map | Swiss-table hash map (any-type keys) |

### Array operations

```sea
local a = []
local b = [1, 2, 3]   -- with elements (a call in the list gives one value)
a[] = v          -- append
a[i] = v         -- set at index i (0-based; negative: from end, -1 = last)
v = a[i]         -- get (negative indices work)
del a[i]         -- remove element at i and shift down
len(a)           -- number of elements

-- for-in on arrays yields (index, value)
for i, v in a do
    ...
end
```

### Map operations

```sea
local m = [=]
m[k] = v              -- set key k to value v (any type for k)
v = m[k]              -- get; returns nil if missing
v, ok = m[k]          -- two-value lookup: ok=true if key exists (even holding nil)
v, ok = t.name        -- the same with a field name (tables, maps, structs)
del m[k]              -- delete key k
len(m)                -- number of entries

for k, v in m do      -- iterate all key-value pairs
    ...
end
```

### Struct / table operations

```sea
local s = { x = 1, y = 2 }   -- struct literal (compile-time fields)
s.x = 10
local v = s["x"]              -- string-key indexing also works

for k, v in s do              -- iterates all fields
    ...
end
```

### Typed arrays (int_array / float_array / float32_array)

No per-element reference counting — more efficient for numeric data.
`float32_array` stores 32-bit floats (half the memory of `float_array`):
a stored value is rounded to float (`0.1` reads back as `0.10000000149012`,
`16777217` as `16777216`, `1e40` as `inf`) and read back as a `number`.

```sea
local ia = int_array()      -- typed int64 array
local fa = float_array(64)  -- with initial capacity hint
local f32 = float32_array() -- typed float32 array (rounded on store)
ia[] = 42                   -- append
ia[0] = 99                  -- set (0-based); past the end grows, filling with 0
v = ia[0]                   -- get
len(ia)                     -- count
-- Note: for-in is NOT supported on typed arrays; use numeric for
for i = 0, len(ia)-1 do ... end
```

---

## Functions

### Basic definitions

```sea
function add(a, b) return a + b end

-- Expression form
local f = function(x) return x * 2 end
```

### Method definition and call

```sea
-- Definition: 'self' is added implicitly as first parameter
function obj:method(a, b)
    -- self is available here
end

-- Call: obj is passed as first argument automatically
obj:method(a, b)       -- equivalent to obj.method(obj, a, b)

-- A chain may break before ':' or '.': a line that starts with one
-- continues the expression above (sea fmt keeps the breaks, one level in)
local sql = query:from("clientes")
    :where("ativo = 1")
    :order_by("nome")
    :build()
```

### Multiple return values

```sea
function divmod(a, b) return a // b, a % b end
local q, r = divmod(10, 3)
```

### Variadic functions

```sea
function sum(...)
    local args = [...]   -- all the varargs in an array ({...}: a positional table)
    local total = 0
    for i = 0, len(args)-1 do total = total + args[i] end
    return total
end

-- Pass-through
function wrap(...) return inner(...) end

-- The first ones by name
function pair(...)
    local a, b = ...      -- nil when fewer were passed
    return a, b
end
```

### Bare `do...end` blocks (scope isolation)

```sea
do
    local tmp = expensive()
    use(tmp)
end
-- tmp not accessible here
```

### Closures and captured variables

A closure captures variables **by reference**: it sees later assignments made
by the enclosing function, and assignments made inside the closure are visible
outside.

**Loop variables are fresh per iteration** (as in Lua 5.4 and Go 1.22): each
iteration of a numeric `for` or a `for-in` has its own loop variable(s) — and
its own body locals — so closures and goroutines created in the body keep that
iteration's values, also when the iteration ends with `continue`.

```sea
local fns = []
for i = 1, 3 do
    fns[] = function() return i end
end
print(fns[0](), fns[1](), fns[2]())   -- 1  2  3

for i = 1, 10 do
    go (function() ch <- i end)()     -- each goroutine sends its own i
end
```

A variable captured by a goroutine may be reassigned by its owner after the
goroutine started; the new value is safely shared with the goroutine (as for
values sent on a channel).

### Semicolons

Semicolons are optional statement separators (permitted but not required).

---

## Error Handling

```sea
-- Create an error value (not a panic)
local err = error("something went wrong")
if is_error(err) then
    print(err.message)   -- access the message field
end

-- Propagate with ? (returns error to caller if result is an error)
local v = might_fail()?

-- Panic + recover
function safe()
    defer function()
        local e = recover()
        if e ~= nil then print("recovered:", e) end
    end
    panic("oops")
    print("not reached")
end
safe()          -- prints "recovered: oops"; safe() returns nil
```

### panic and recover

`panic(v)` and **runtime errors** (indexing `nil`, arithmetic on a table,
calling `nil`, …) unwind the call stack the same way, as in Go:

- Execution stops at the panic. Each function on the stack, innermost first,
  runs its deferred calls (LIFO) and returns.
- A deferred call that calls `recover()` stops the unwinding and gets the panic
  value. The function whose deferred call recovered then returns normally to
  its caller, **with nil results**, after running its remaining deferred calls.
  The caller continues as after any call.
- For a runtime error, `recover()` returns an error value whose message starts
  with the location: `is_error(e)`, `e.message == "main.sea:12: attempt to
  index a nil value"`. For `panic(v)` it returns `v`.
- `recover()` returns `nil` when there is no panic.
- A panic inside a deferred call replaces the one being unwound.
- Panics cross native callbacks (`once.do_once(once, f)`), module bodies run by
  `require`, and JIT-compiled code.
- An unrecovered panic, in the main script or in any goroutine, prints the
  traceback and ends the process (exit code 1).

```sea
local function try(f, ...)
    local err = nil
    local function run(...)
        defer function() err = recover() end
        return f(...)
    end
    local result = run(...)
    return result, err
end

local r, e = try(function()
    local t = nil
    return t.x
end)
print(r, e.message)     -- nil   main.sea:13: attempt to index a nil value
```

Functions unwound by a panic do not release their local variables (the
values leak). Panics are for exceptional paths; use error values and `?` for
expected failures.

**Error object field:**

| Field | Description |
|-------|-------------|
| `err.message` | The string message passed to `error()` |

### defer

`defer` schedules a call to run when the enclosing function returns (normally
or by panic). Deferred calls run in LIFO order — including ones registered in a
loop.

```sea
local function f()
    local x = 1
    defer print("x was", x)           -- call: f and its args evaluated NOW
    defer obj:close("bye")            -- method call: obj, obj.close and args evaluated NOW
    defer function() print(x) end     -- closure: runs later, sees x by reference
    x = 2
end
-- prints: 2   (closure), "bye" via obj:close, then "x was 1"
```

- In the call forms the callee and every argument are evaluated when the
  `defer` statement runs (like Go), so later changes to `x` don't affect them.
- A deferred call takes at most **8 arguments** (for `obj:m(...)`, `self`
  counts as one); more is a compile error.

### Syntax and compile errors

Code that does not parse or compile stops before running, with the position:
`file:line:col: message` for syntax errors (`app.sea:3:9: expected 'then' but
got 'end'`) and `file:line: message` for compile errors (`app.sea:7: cannot
assign to const 'a'`). `require` reports a module's errors the same way.
`sea/parser`'s `check` returns them as data.

---

## Concurrency

```sea
-- Goroutines
go my_func(arg1, arg2)

-- Channels
local ch = chan()       -- unbuffered
local ch = chan(16)     -- buffered (capacity 16)
ch <- value             -- send
local v = <- ch         -- receive
close(ch)               -- close

-- Range over channel (terminates when channel is closed)
for v in ch do
    ...
end

-- Select
select
    case v = <-ch1 then
        -- received from ch1
    case ch2 <- x then
        -- sent to ch2
    case default then
        -- non-blocking fallback
end
```

Goroutines run on a pool of scheduler threads (one per CPU). Blocking on a
channel, `time.sleep` and every wait in the `net` module park the goroutine
without holding its thread. Other blocking calls — file I/O, `io.read`, a
native library that blocks — do hold the thread, so enough of them at
once can stall the other goroutines.

---

## `for-in` Support by Type

| Type | Yields | Notes |
|------|--------|-------|
| `array` | `(index int, value)` | 0-based index |
| `table` / struct | `(key string, value)` | all fields |
| `map` (`[=]`) | `(key, value)` | any-type key |
| `channel` | `(value)` | blocks per value; exits on `close()` |
| `int_array`, `float_array`, `float32_array` | — | **not supported**; use numeric `for` |

---

## Gotchas

| Wrong | Right |
|-------|-------|
| expecting `for k, v in arr` to yield 1-based keys | it yields `(index, value)` with 0-based indexes; `for i = 0, len(arr)-1 do` is equivalent |
| `#arr` | `len(arr)` |
| `{1, 2, 3}` as array | `[1, 2, 3]` (`{1, 2, 3}` is a positional table) |
| `[[1, 2], [3]]` | `[ [1, 2], [3] ]` (`[[` starts a long string) |
| `1 == 1.0` is `false` | it is `true`: `==` compares int and float numerically (they remain distinct types: `type(1)` is `"int"`, `type(1.0)` is `"number"`) |
| `!=` | `~=` |
| `string.sub(s, 0, 2)` | `string.sub(s, 1, 2)` (1-based) |
| `len("é")` expecting 1 | `len` and `string.*` count bytes (2); `utf8.len("é")` is 1 |
| `mu.lock(mu)` | `mu.acquire(mu)` |
| `mu.unlock(mu)` | `mu.release(mu)` |
| Long ns arithmetic (weeks) | use `time.unix(sec, 0)` instead of `base.add(base, 7*24*time.Hour)` |
| `json.encode([=])` | works — `[=]` maps are serialized as JSON objects |
| expecting a cycle of objects shared between goroutines to be collected | the cycle collector only frees cycles of objects a single goroutine owns; break shared cycles yourself (`a.peer = nil`, see [memory.md](memory.md#cycles)) |

---

## Module: `require`

```sea
local json = require("./libs/json/json")   -- relative to the requiring file
local http = require("./libs/http/http")
```

Built-in modules (`math`, `string`, `utf8`, `re`, `encoding`, `hash`, `io`,
`os`, `fs`, `path`, `process`, `sync`, `net`, `time`, `gc`, `sort`, `bigint`,
`json`, `csv`, `list`,
`sea/parser`, and the Sea modules `sea/test`, `sea/check`, `sea/stream`, `sea/flags`, `sea/log` and `sea/dotenv`) are found
first, then, in an executable made by `sea build`, the modules bundled in
it. An absolute path (`require("/opt/sea/libs/yaml/yaml")`) names the file
itself, tried as `.sea`, `/init.sea`, then `.so`. Otherwise the search order
is:
1. Directory of the requiring source file + path (`.sea`, `/init.sea`, then `.so`)
2. Current working directory + path
3. For a name that does not start with `.`: a package installed by
   `sea get`, in the `sea_modules/` directory next to the requiring file or
   in any directory above it (`sea_modules/name.sea`, `name/init.sea`,
   `name.so`, or `name/name.sea`)
4. Each directory in `SEA_PATH` (colon-separated environment variable)
5. For a name that does not start with `.`: the libraries installed by
   `make install`, in `lib/sea/` next to the `bin/` directory of the running
   `sea` binary (`/usr/local/lib/sea` for `/usr/local/bin/sea`):
   `lib/sea/name.sea`, `name/init.sea`, `name.so`, `name/name.sea` or
   `name/name.so`. So an installed `sea` loads `require("json")`,
   `require("http")` or `require("sqlite")` from anywhere. A `sea` run from
   its source tree (`bin/sea` next to `src/`, not installed) looks in `libs/`
   of that tree instead, in the same layout: `require("http")` is
   `libs/http/http.sea`. Projects should require the libraries by name, not
   by a path to the sea_vm checkout
