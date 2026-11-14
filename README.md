# Sea

Sea is a small scripting language with Lua-like syntax and Go-like concurrency.
It compiles to bytecode for a register-based virtual machine written in C, with
an optional JIT compiler.

```sea
local function fetch(id)
    if id % 4 == 0 then return error("item " .. id .. " not found") end
    return {id = id, price = id * 2.5}
end

local results = chan(10)
for id = 1, 6 do
    go (function()
        local item = fetch(id)          -- each goroutine sees its own `id`
        if is_error(item) then results <- item.message
        else results <- string.format("item %d: %.2f", item.id, item.price) end
    end)()
end
for i = 1, 6 do print(<-results) end
```

## Highlights

- **Familiar syntax** — Lua-style blocks, closures, multiple returns, varargs.
- **Concurrency** — goroutines on an M:N scheduler, buffered/unbuffered
  channels, `select`, `sync.mutex` / `sync.waitgroup`.
- **Errors as values** — `error()`, `is_error()`, the `?` propagation operator,
  plus `defer`, `panic` and `recover`.
- **Data types** — arrays (`[]`), hash maps (`[=]`), sealed structs (`{x = 1}`),
  dynamic tables, typed `int_array` / `float_array`.
- **No tracing GC** — reference counting (atomic only for values shared between
  goroutines) plus a cycle collector; see [memory](docs/memory.md).
- **Fast** — NaN-boxed values, computed-goto dispatch, an optimizing compiler,
  and an optional JIT (libgccjit) that speeds hot numeric code up 2–4×.
- **Batteries** — `math`, `string`, `utf8`, `re`, `encoding`, `hash`, `io`, `os`, `fs`, `path`, `process`, `sync`, `time`, `net`, `sort`, `bigint`, and
  `sea/parser` (Sea's own parser: AST, tokens, checks, a formatter); external
  libraries for JSON, YAML, TOML, CSV, XML, HTTP, WebSocket, TLS, JWT, CLI flags, a terminal UI,
  SQLite and DuckDB (embedded SQL) and numeric kernels for training models.

## Quick start

**Platform: Linux and macOS, on x86-64 or aarch64 (ARM64).** The netpoller is
epoll on Linux and kqueue on macOS (`src/sea_compat.c` holds the few places where
the two differ). The macOS port is new: it is built and tested by the CI
workflow (`.github/workflows/ci.yml`). Not on macOS yet: the JIT, and the
`duckdb` and `embed` libraries (their vendored binaries are Linux builds).
Windows and the BSDs are not supported. On other CPU architectures the
interpreter builds without its pinned registers but is untested.

Requirements: GCC or Clang (computed goto), GNU Make, pthreads.
readline is optional: the REPL uses it for line editing when its headers are
installed (`make noreadline` leaves it out). The JIT additionally needs the
libgccjit headers (`libgccjit-13-dev` or similar). Native libraries have their
own: OpenSSL 3 for `libs/ssl`, zlib for `libs/http`, and DuckDB for
`libs/duckdb` (its build downloads the official `libduckdb` release, checksum
pinned).

```sh
make                              # build bin/sea
bin/sea examples/nbody/nbody.sea  # run a program
bin/sea -e 'print(`2^10 = ${2 ^ 10}`)'   # one-liner (also: sea - reads the program from stdin)
bin/sea                           # interactive REPL

bin/sea compile app.sea -o app.seac   # compile to bytecode …
bin/sea app.seac                      # … and run it

make sea-jit                                      # build bin/sea-jit
bin/sea-jit --jit examples/nbody/nbody.sea        # run with the JIT

bin/sea fmt src/                      # format .sea files in place (-l, -d, --check)
bin/sea doc libs/http/http.sea        # documentation from comments (-f markdown|html|json)
bin/sea doc -http :6060 -open .       # the same as web pages: index, modules, source, search
bin/sea test                          # run the *_test.sea files (-run, -bench, -v)
bin/sea check                         # undefined/unused variables, bad calls, ... without running
bin/sea build -o app main.sea         # standalone executable (program + modules + natives)
bin/sea prof app.sea                  # CPU profile: hot functions and lines (-folded for flame graphs)
bin/sea test -cover                   # line coverage of the code under test (-coverprofile out.lcov)
bin/sea new myapp                     # start a project (sea.toml, main.sea, a test)
bin/sea get https://host/repo@v1.0    # add a dependency: sea_modules/, sea.lock; require("repo")
bin/sea lsp                           # language server: diagnostics, format, hover, definition, completion
bin/sea utils/license/license.sea -l BSD-3-Clause -c "The Sea Authors"       # put copyright + SPDX license headers in the sources (-check, -n, -remove)
```

Native libraries are built per library, e.g. `make -C libs/json`.

### Installing

```sh
make -C libs/json && make -C libs/http ...   # the native libraries you want (optional)
sudo make install                 # /usr/local/bin/sea (+ sea-jit if built), /usr/local/lib/sea/<lib>/
make install PREFIX=$HOME/.local  # or anywhere else; DESTDIR=... for packaging
sudo make uninstall               # same PREFIX / DESTDIR as the install
```

`make install` copies the binary and the libraries (the pure Sea ones, and
the native ones that are built). An installed `sea` finds them by name from
any directory: `require("json")`, `require("http")`, `require("sqlite")`.

## Documentation

| Document | Contents |
|----------|----------|
| [Tour](docs/tour.md) | Example-driven introduction to the language |
| [Sea for AI assistants](docs/sea-for-ai.md) | One file for Claude, Gemini, GPT...: the rules they get wrong, syntax, stdlib, translations (also [llms.txt](llms.txt), [AGENTS.md](AGENTS.md)) |
| [Reference](docs/reference.md) | Syntax, operators, built-ins and the standard library |
| [Libraries](docs/libs.md) | `json`, `yaml`, `toml`, `csv`, `compress`, `xml`, `console`, `http`, `ssl`, `jwt`, `flags`, `tensor`, `ndarray`, `duckdb`, `sqlite`, `cockroach`, `clickhouse`, `websocket`, `test`, `template` |
| [Memory model](docs/memory.md) | Reference counting without a GC: ownership, cycles, goroutines |
| [Architecture](docs/architecture.md) | Compiler, VM, values, scheduler — how it fits together |
| [JIT design](docs/feature_jit.md) | The optional JIT: design, phases, measurements (pt-BR) |
| [Benchmarks](docs/benchmarks.md) | Sea, Sea+JIT, Lua 5.4 and Python 3.12 compared |
| [Everyday scripts](docs/scripts.md) | The same five scripts (CSV, logs, words, files, JSON) in Sea and Python: speed, length, same output |
| [Bugs & changes](docs/bugs.md) | Known issues and notable fixes (pt-BR) |

## Examples

30 runnable programs live in [`examples/`](examples/) — `bin/sea examples/<name>/<name>.sea`.
Those marked ⚡ are compute-bound and print their run time (compare `bin/sea`
with `bin/sea-jit --jit`).

| Area | Examples |
|------|----------|
| Numeric / JIT ⚡ | `nbody` (N-body simulation), `game_of_life` (Conway), `spectral_norm` (power method) |
| Algorithms | `graphs` (BFS maze + Dijkstra with a binary heap), `dynamic_programming` (LCS, edit distance, knapsack, memoization) |
| Closures | `generators` (generators, memoization, per-iteration loop variables), `closures` |
| Text & data | `word_count`, `calculator` (lexer + recursive-descent parser), `csv_report` (CSV → aggregation → JSON), `strings`, `verbos` |
| Language & stdlib | `collections`, `structs`, `errors`, `os_demo`, `time_demo`, `file_watcher`, `seafmt` (formatter/checker on `sea/parser`) |
| Concurrency | `pipeline`, `worker_pool`, `pubsub`, `timeout`, `rate_limiter`, `circuit_breaker`, `pool`, `logger`, `webhook`, `metrics_server` |
| Apps & testing | `users` (sign-up and login on `libs/sqlite`, PBKDF2 passwords, lockout, sessions; `users_test.sea` is a `sea test` suite with a fake clock and in-memory databases: `cd examples/users && ../../bin/sea test -v`) |

## Performance

A few results from [benchmarks.md](docs/benchmarks.md) (wall-clock, lower is better):

| Benchmark | Sea | Sea + JIT | Lua 5.4 | Python 3.12 |
|-----------|-----|-----------|---------|-------------|
| Insertion sort, 10K numbers | 837 ms | 246 ms | 777 ms | 7.96 s |
| Loop sum, 50M iterations | 262 ms | 78 ms | 383 ms | 6.86 s |
| Mandelbrot 400×400 | 410 ms | 165 ms | 518 ms | 4.58 s |
| Binary trees, depth 16 | 2.90 s | 2.43 s | 4.87 s | 2.67 s |

## Development

```sh
make test        # unit tests (tests/unit/*.sea vs *.expected)
make test-dwe    # tests with the optimizer's diagnostic checks
make test-seac   # tests run from compiled .seac bytecode
make test-jit    # tests with every chunk JIT-compiled
make test-asan   # tests under AddressSanitizer
make test-format # sea/parser's formatter round trip over every .sea file
bash tests/compare/run_benchmarks.sh   # regenerate docs/benchmarks.md
```

Layout: `src/` (compiler, VM, runtime, stdlib), `libs/` (external libraries),
`examples/`, `tests/` (unit tests, benchmarks), `docs/`, `tools/` (the `sea`
subcommands written in Sea), `extras/vscode/` (the VS Code extension: grammar,
snippets and a client for `sea lsp`; see its README).
