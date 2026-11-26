---
name: sea
description: Sea language reference — keywords, operators, built-ins, stdlib (math, string, utf8, re, encoding, hash, io, os, fs, path, process, sync, time, net, sort, bigint, sea/parser), collection literals ([], [=], {}), concurrency, error handling, and external libraries (json, yaml, toml, csv, compress, xml, console, http, ssl, websocket, jwt, flags, test, template, tensor, duckdb, sqlite). Invoke when writing, reviewing, or debugging Sea code, or when you need the exact syntax/API for any Sea feature or library.
---

## Gotchas (what models and newcomers get wrong; the guides below have the rest)

- **Arrays are 0-indexed.** Create them with `[]` or `[a, b, c]` and append with `arr[] = v` (there is no `array()`/`push()`); nested: `[ [1, 2], [3] ]` with a space, since `[[` opens a long string. `{1, 2, 3}` is a positional *table* with keys 0, 1, 2, not an array. Use `len()`, not `#`. (`[n]` is a one-element array; the old capacity form is gone.)
- **Maps with known entries are a literal**: `local m = ["a" = 1, "b" = 2]`, one `key = value` per line (commas optional at line ends in `[...]`/`{...}`; a line starting with `-x` or `(x)` is a new element). Never `local m = [=]` followed by `m[k] = v` lines: `sea check` warns (`mapinit`). `[=]` is for maps filled later or in a loop.
- **`for-in`** works on arrays (`for i, v in arr do` → 0-based index, value), maps/tables/structs (key, value) and channels (value until closed).
- **Numeric for is inclusive on both ends:** `for i = 0, n-1 do` iterates n times. When n=0 this becomes `for i = 0, -1 do` — correctly executes zero iterations (fixed 2026-05; no guard needed).
- **Loop variables are fresh per iteration** (numeric `for` and `for-in`, as in Lua 5.4 / Go 1.22): closures and goroutines created in a loop body capture that iteration's value — no `local j = i` copy needed (changed 2026-10).
- **String indexing is 1-based** in `string.sub`, `string.find`, `string.byte` (matching Lua convention), but array/table indexing is 0-based.
- **Strings are bytes; source is UTF-8.** `len` and `string.*` count bytes (`len("é")` is 2); the built-in `utf8` module counts characters (`utf8.len`, `utf8.sub`, `utf8.upper`, ...). A `.sea` file with invalid UTF-8 is a compile error; `"\u{E9}"` writes a code point. Native code uses `src/sea_utf8.h`.
- **Cycles** are freed by the cycle collector (automatically, or `gc.collect()`), except cycles of objects shared between goroutines, which still leak; break those by setting a field to nil (see `docs/memory.md`).
- **`int` is 48-bit** (±140 737 488 355 327). Wider integers are `bigint`s, an explicit type: `bigint.new("…")` and `bigint.add/sub/mul/div/mod/pow` (the operators `+ - *` do not take bigints; comparisons, `==`, `tostring`, `..` and `sort` do).
- **`//` and `%` floor** (toward minus infinity) for ints and floats: `-7 // 2` is `-4`, `-7 % 2` is `1`.
- **Compound assignment** `+= -= *= /= //= %= ^= ..=`: for a simple target the parser builds `a = a op b` (assign.op_wrapped), so every analysis sees the real value; `t[f()] += 1` goes through compile_compound_assign (hidden locals `(o)`, `(k)`).
- **Template strings** use backquotes: `` `total: ${n}` `` (NODE_TEMPLATE, compiled to one OP_CONCAT). `"${x}"` is plain text (re.gsub uses `${name}` in replacements) and `f"x"` is a call.
- **`string.find` is plain text**; regular expressions are the built-in `re` module (RE2 syntax, linear time, no backreferences or lookaround).
- **`isnan` / `isinf`** are registered as bare globals (in addition to `math.isnan` / `math.isinf`).
- **Runtime errors are panics**: `recover()` in a deferred call catches them (Go semantics: the recovering function returns nil). The message starts with `file:line:`. Unrecovered, they print a traceback of `file:line: in main chunk` / `file:line: in function 'name'` lines (each function knows its source file: `SeaChunk.source`).
- **`{a = 1}` is a sealed struct**: adding a field fails at run time ("attempt to add field ... to a sealed struct"). Build with `{}` and assign every field (or give all fields up front); `sea check` reports it as `[sealed]`. This also bites option tables passed to natives such as `process.run` (`stdin`, `env`, `timeout`).
- **Small API facts that cost time**: there is no `string.rfind` (find the tail by length instead); stderr is `io.write_err(...)`, not `io.stderr`; `"\1"` is an invalid escape (use `\x01`); `\0` is valid; `[[` opens a long string, so a nested array literal needs a space (`[ [1], [2] ]`); `re.gsub(pattern, s, repl)` takes the subject second; captures are `m.groups[1]`; statements need their own line (`a() b()` on one line is an error); `local _ = f()?` can be repeated in one scope.
- **Ints are 48-bit**: a 64-bit protocol counter (SSH's AES-GCM invocation counter) must be kept as two 32-bit halves, not one int.
- **A project file with the same name as a lib shadows it** (`lib/ssh.sea` doing `require("ssh")` got itself back, with a nil upvalue). Name project modules differently (domus has `lib/remote.sea`).
- **`require("x").f(a)` alone on a line fails** ("attempt to call a nil value"): put the module in a `local` first.

## Where the full text is

Read these files (in sea_vm/):

1. `docs/sea-for-ai.md` — first: the rules models get wrong, syntax, verified examples, stdlib cheat sheet
2. `docs/reference.md` — authoritative Sea language reference (syntax, built-ins, stdlib)
3. `docs/libs.md` — external library reference (json, yaml, toml, csv, compress, xml, console, http, ssl, websocket, jwt, flags, test, template, tensor, duckdb, sqlite)

Use these documents as the single source of truth for all Sea syntax, built-in functions, stdlib APIs, collection operations, and library APIs. Do not rely on Lua or any other language's conventions — Sea has important differences documented there.

## Feedback loop

`bin/sea file.sea` runs, `bin/sea check` finds sealed-struct and other mistakes before running, `bin/sea test dir` runs `*_test.sea`, `bin/sea fmt` formats in place (`-l` only lists). When working in another project (promptus, domus), the docs live in the sea_vm checkout next to it (`../sea_vm/docs/`).
