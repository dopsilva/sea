# Changelog

## Unreleased

- `libs/ssh`: an SSH 2 client in pure Sea (curve25519-sha256, ssh-ed25519, aes256-gcm, public-key login, `exec`), with
  `ssh.keygen`, `ssh.hostkey` and a small test server; checked against OpenSSH 9.7.
- `libs/ssl`: Ed25519 and X25519 (`keygen`, `key_raw`, `key_from_raw`, `x25519`, `sign`/`verify` with `"ed25519"`).
- `sea fmt`: a `for` or `while` loop whose body is one simple statement (or an `if` on one line) and has no
  comment goes on one line when it fits the width.
- `re`: Unicode classes `\p{L}` `\p{Lu}` `\p{Ll}` `\p{N}`/`\p{Nd}` `\p{P}` and the scripts Latin, Greek,
  Cyrillic, Hebrew, Arabic, Han, Hiragana, Katakana, Hangul (`\pL`, `\P{..}`, inside `[...]`); `re.each(p, s, fn)`
  calls `fn` for each match without building the array.
- `tensor.to_half` / `tensor.from_half`: float16 and bfloat16 stored as bytes (half the memory of float32).
- `sea check` / runtime hints for `string.has_suffix`, `string.has_prefix` and `io.stderr`.

## 0.3.1

- `sea fmt`: wraps one-line tables, arrays, calls (arguments packed row by row) and `and`/`or` chains wider than
  `--width` (default 100); `and`/`or` start the continuation line; a broken
  `if`/`while` condition goes one level deeper than the body; aligned `=` columns
  written by the author are kept. Idempotent, and the tree does not change.

## 0.3.0

A release about reliability and about helping people (and AI assistants) get
Sea code right on the first try. No language feature is added; one behavior
changes (see "Changed").

### Fixed
- **Scheduler lost a wake-up at exit.** `sea_sched_drain` (the main thread
  waiting for the last goroutine) shared a condition variable with the worker
  threads, so a `signal` meant for a worker could wake the drainer instead
  and be lost. A goroutine then sat in the run queue with every worker asleep
  and `sea` never exited (about 1 run in 20 of `128_websocket`, also seen in
  CI). The drainer now has its own condition variable; 360 runs under load
  no longer hang.
- `return f(x)` passes on every value `f` returns (it used to keep the first).
- `math.maxinteger` / `math.mininteger` are the real 48-bit limits.
- macOS: the listening-socket shutdown wakes `accept`, `memmem` with an empty
  needle, `TMPDIR` with a trailing slash, `getentropy` header.

### Added
- **macOS port**: kqueue instead of epoll (`sea_compat` layer), and CI on Linux
  and macOS.
- **Sandbox**: an embedded restricted VM (no `io`, `os`, `net`, `require`)
  with a step budget and value copying (`sea_vm_call` for embedders).
- **Errors that say the fix**: runtime errors name the value that failed and
  what to use instead (`tools/hints.txt`, shared by the runtime and `sea check`).
- `sea check`: hints for names from other languages (`table.sort`,
  `string.gsub`, `#x`), `longstring` (`[[a, b], [c, d]]` opens a long string),
  `sealed` (a field added to a `{a = 1}` literal, which fixes its fields) and
  `recover(fn)`; `sea check -` reads stdin.
- `sea doc <name>` also documents built-in functions (`sea doc string.split`).
- `math.atan2`, `os.time`.
- New libraries since 0.2.0: ndarray (float32, einsum, complex, FFT, eig),
  tensor (thread pool, attention, int8 weights), xlsx, pdf, smtp, truetype,
  image (PNG/BMP/PNM, JPEG decoder), cockroach, clickhouse, `sea/log`,
  `sea/dotenv`, `sea/flags`, `sea/stream`, time zones, `os.setenv/environ`.
- Language: keywords as field and method names (`t.in`), `local a, b = o:m()`,
  `local v, ok = t.name`, `load(src)`.
- `make test` prints only failures and the summary (`TEST_VERBOSE=1` lists
  every test); a test that hangs is killed after `TEST_TIMEOUT` seconds.

### Changed
- The error for adding a field to a sealed struct now says how to build the
  value instead (`{}` and assign, or a map `[=]`).
- `sea` library `sea/ref` (generated documentation index) is built into the
  binary.

## 0.2.0

First public version: the language, VM, JIT, netpoller, standard library,
`sea fmt/doc/check/test/build/lsp`, and the VS Code extension.
