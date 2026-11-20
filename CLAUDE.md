# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```sh
make                  # Build bin/sea (optimized, -O2)
make debug            # Debug build (-g -O0 -DSEA_DEBUG)
make asan             # AddressSanitizer build
make clean            # Remove bin/ and obj/
make install          # bin/sea (+ sea-jit) to $(PREFIX)/bin, built libs to $(PREFIX)/lib/sea/<lib>/
                      # (PREFIX=/usr/local, DESTDIR); make uninstall removes them
make sea-jit          # Build bin/sea-jit (optional JIT, needs libgccjit headers)
bin/sea fmt -l .      # sea fmt / doc / test: Sea scripts in tools/, embedded at build
                      # time (tools/embed.sh -> gen/sea_tools.h, included by sea_main.c);
                      # MODULES in the Makefile embeds Sea modules for require("sea/...")
                      # (sea/test = libs/test/test.sea, sea/check = tools/lib/check.sea,
                      # sea/stream = libs/stream/stream.sea, sea/flags = libs/flags/flags.sea,
                      # sea/log = libs/log/log.sea, sea/dotenv = libs/dotenv/dotenv.sea;
                      # the tools parse their arguments with sea/flags)
                      # into gen/sea_modules.h

bin/sea utils/license/license.sea -l BSD-3-Clause -c "The Sea Authors" -check # license headers in .c/.h/.sea/.sh/Makefile
                      # (-n dry run, -update, -remove; skips vendor/, gen/, obj/)

# Native libraries under libs/ (e.g. json, yaml, toml, csv, compress, xml, console, http, ssl, jwt, tensor, ndarray, duckdb)
make -C libs/json     # Build a library's .so
make -C libs/duckdb test  # duckdb: first build downloads libduckdb into libs/duckdb/vendor/
make -C libs/sqlite test  # sqlite: first build downloads the SQLite amalgamation into libs/sqlite/vendor/
bin/sea test libs/cockroach libs/clickhouse  # pure Sea; COCKROACH_URL / CLICKHOUSE_URL add the tests against a server
```

## Test Commands

```sh
# Core VM tests (tests/unit/*.sea vs *.expected)
make test                        # Build + run all unit tests, then the license header check
make test-license                # Only the BSD-3-Clause header check (utils/license/license.sea -check)
make test-dwe                    # Unit tests with dead-write-elimination checks
make test-seac                   # Unit tests run as compiled .seac bytecode
make test-build                  # Unit tests run as `sea build` executables (src/sea_bundle.c)
SEA_COVER=/tmp/c.lcov bash tests/run_tests.sh  # with OP_COVER at each statement (src/sea_cover.c)
make test-jit                    # Unit tests with every chunk JIT-compiled (bin/sea-jit)
make test-asan                   # Run under AddressSanitizer
make test-format                 # sea/parser format round trip over every .sea file
bash tests/run_tests.sh 24       # Run only tests matching pattern "24"
bin/sea tests/run_tests.sea      # Run test runner directly
bash tests/arm64/run.sh          # Build + test on aarch64 in Docker (needs qemu binfmt; see the script)

# Run a single example
bin/sea examples/nbody/nbody.sea
bin/sea-jit --jit examples/nbody/nbody.sea
```

Tests in `tests/unit/` follow the pattern `NN_name.sea` + `NN_name.expected`; the runner diffs stdout against the expected file.

## Architecture

### VM Pipeline

Source `.sea` → **Lexer** (`sea_lexer.c`) → **Parser** (`sea_parser.c`, `sea_ast.c`) → **Compiler** (`sea_compiler.c`) → bytecode (`SeaChunk`) → **VM** (`sea_vm.c`)

The VM is a register-based interpreter using GCC computed-goto dispatch (`&&label` / `goto *dispatch_table[op]`). The `DISPATCH()` macro fetches the next instruction, saves `frame->ip`, and jumps — meaning `frame->ip` always points to the *next* instruction, so line-number lookups use `frame->ip - 1`.

### Value Representation

All values are NaN-boxed 64-bit words (`sea_value.h`). The quiet-NaN space encodes: nil, booleans, 48-bit signed integers, and heap object pointers. IEEE 754 doubles occupy the non-NaN space. Integer literals compile to `int` (distinct from `number`/float); `==` still compares them numerically (`1 == 1.0` is true).

### Memory Management

Objects are **reference-counted**, plus a **cycle collector** for reference cycles (`src/sea_gc.c`, no tracing GC); the full model is in `docs/memory.md`. Ownership is decided at compile time, not by the register file:

- **Named locals (and parameters) own their value** (+1). The compiler releases them at every scope exit: end of block, `break`/`continue`, `return`, `?`. Assignment uses `OP_SETLOCAL`, which stores the new value (retaining it if borrowed) and then releases the old one.
- **Temporaries** carry `is_owned` in `SeaRegMeta` when they hold a reference that must be consumed: moved into a local, passed, returned, stored with a `*_MOVE` op, or released (`own_temp` / `drop_temp` in `sea_compiler.c`).
- **Arguments are passed owned**; the callee owns its parameters. **Return values are owned.** The VM releases a native call's args after the call, and releases results nobody takes.
- `is_scalar` facts (numbers, bools, nil, pinned string constants) let the compiler skip RC entirely. They are tracked with the same sound dataflow as `is_int`.
- **Cycles:** a release that leaves a container (array/table/map/struct/closure/upvalue/error) with a non-zero count buffers it as a possible root, in the running goroutine's VM. At safe points (allocations, returns, JIT helpers, goroutine end, `gc.collect()`), the collector frees what only internal references keep alive (Bacon–Rajan trial deletion). It handles only objects that are not shared; `SeaObject.gc` and `gc_idx` hold its flags. Any VM code that frees objects outside a safe point must not hold borrowed pointers across a collection.
- **Storing into a shared container marks the value shared** (`sea_store_barrier`), so "not shared" really means "seen by one goroutine".
- **Small arrays are one allocation:** `sea_array_new` with a capacity up to `SEA_ARRAY_INLINE_MAX` (8) puts the elements right after the header (`SEA_ARRAY_IS_INLINE(a)`); growing moves them to their own buffer. Code that frees or reallocates `a->data` must check it (`sea_array.c`, `gc_free_white`).
- **The cycle collector's root buffer is compacted when full** (freed candidates leave NULL slots): a native looping over `sea_vm_call` (`csv.each`) reaches no safe point, and past 65535 slots (`gc_idx` is 16 bits) dying candidates would linger until the next collection.

**Native C functions** (`sea_obj_val(&x->obj)` makes a `SeaValue` *without* changing RC):
- Arguments are **borrowed**: never release them.
- **Return an owned (+1) value.** A fresh object (RC=1) is returned as is. An argument or a container element must be retained first: `return sea_val_ref(args[0]);`.
- `sea_array_push`, `sea_table_set`/`sea_table_set_str` and `sea_map_set` **retain** what they store, so drop your own reference afterwards.
- `sea_vm_call()` takes borrowed args and returns an owned result: release it if unused.

```c
SeaTable *entry = sea_table_new(TABLE_MAP);
sea_table_set_str(entry, "k", val);
sea_array_push(out, sea_obj_val(&entry->obj));
sea_obj_release(&entry->obj);  /* array retained it; drop our ref */
return sea_obj_val(&out->obj); /* new object: already +1 */
```

### Module / require() System

`require("path")` searches in order:
1. Directory of the requiring source file + path (`.sea` then `.so`)
2. Current working directory + path
3. Bare names: `sea_modules/` (packages from `sea get`) next to the requiring file or above
4. Each directory in `SEA_PATH` (colon-separated env var)
5. Bare names: `<dir of the sea binary>/../lib/sea/` (the libraries `make install` puts
   there: `require("json")` → `lib/sea/json/json.so`); not installed, a `bin/sea` in the source
   tree uses `<repo>/libs/` the same way (`require("http")` → `libs/http/http.sea`).
   tools/lib/check.sea, tools/build.sea and
   tools/lsp.sea repeat this search: keep them in sync with `src/sea_stdlib.c`

Before the search: built-in modules, the embedded `sea/test`/`sea/check`/`sea/stream`/`sea/flags`/`sea/log`/`sea/dotenv`, then (in a
`sea build` executable) the bundled modules.

Native libraries are `.so` files exposing a `sea_open` entry point. `json` and `csv` are also compiled into the binary (`EMBED_LIBS` in the Makefile: `-Dsea_open=sea_open_<name>`, registered by `sea_lib_embedded_register`), so they are globals. The `libs/` directory contains: `console`, `json`, `yaml`, `toml`, `csv`, `compress`, `xml`, `tensor` (native `.so`), `ndarray` (NumPy-like arrays: Sea + native), `http`, `ssl`, `jwt`, `websocket`, `duckdb`, `sqlite`, `embed` (Sea + native; `embed` runs GGUF embedding models with llama.cpp) and `flags`, `test`, `template`, `stream`, `log`, `dotenv`, `image` (PNG, BMP, PNM, JPEG), `truetype`, `pdf`, `cockroach` (PostgreSQL wire protocol), `clickhouse` (HTTP interface), `smtp` (e-mail), `ssh` (SSH client), `xlsx` (Excel workbooks) (pure Sea; `test`, `stream`, `flags`, `log` and `dotenv` are embedded as `sea/test`, `sea/stream`, `sea/flags`, `sea/log`, `sea/dotenv`) — see `docs/libs.md`.
TCP sockets are the built-in `net` module (`src/sea_lib_net.c`); `libs/ssl` and `libs/http` build on it.
The parser is also exposed to Sea code as the built-in `sea/parser` module (`src/sea_lib_parser.c`:
parse, tokenize, check, walk, format). Changing the AST (`sea_ast.h`) or the parser means updating
its conversion there, then `make test-format` (format every `.sea` file, reparse, compare the
trees, format again).

**Require paths are relative to the requiring file**: a lib's own examples use `"../libname"`; files under `examples/<name>/` reach a library as `"../../libs/json/json"`.

## Portability

Linux only (epoll/eventfd in the netpoller), tested on x86-64 and aarch64.
Keep arch-specific code behind `#if defined(__x86_64__)` / `__aarch64__` with a
plain fallback: the pinned dispatch registers (`SEA_PIN_REG_*` in `sea_vm.c`),
the SIMD map probe (SSE2/AVX2/NEON in `sea_map.c`), the spin-wait hint. `char`
is unsigned on aarch64: never rely on its sign (cast to `unsigned char` or
`int8_t`).

## Adding or Changing an Opcode

`sea_op_reads()` in `src/sea_chunk.c` is the single list of which registers each
opcode reads (derived from its handler in `src/sea_vm.c`). The compiler's dead-write
elimination and the JIT's register promotion both depend on it: a missing read lets
the compiler delete a live write — a silent miscompilation (Bugs 10 and 14 in
`docs/bugs.md`). When adding an opcode or changing what a handler reads, update it,
and if the JIT gets a helper for it, declare what the helper writes in `op_regs()`
(`src/sea_jit.c`). Check with `make test-dwe` (`SEA_DWE_CHECK=1` turns eliminated
writes into a visible poison value; `SEA_NO_DWE=1` disables elimination).

## Natives That Wait (netpoller)

A native that waits on a socket or timer must not block its scheduler thread.
It must go through `src/sea_netpoll.h`: `sea_io_begin` → `sea_io_wait` → return
`SEA_PARK_VAL` on `SEA_IO_PARK` → `sea_io_end` when done. The VM then re-executes
the whole `CALL` when the goroutine wakes, so the native must be **idempotent up
to the wait**: keep partial progress in `vm->io_call` (`progress`, `aux[]`), not
in locals, and allocate nothing before the last possible park. Close sockets with
`sea_netpoll_close(fd)` so parked waiters wake. The usage pattern is in the
header comment, and `src/sea_lib_net.c` and `libs/ssl/ssl_native.c` are worked
examples. Plain blocking calls (file I/O) still hold the thread; `net.connect`
resolves names on a resolver thread pool instead.

## Runtime Errors and Panics

`sea_vm_error_noreturn()` no longer exits: it raises a panic (`sea_vm_raise`)
that `longjmp`s to the innermost VM loop, which unwinds frames running their
defers until a `recover()` (see "Panics" in `docs/architecture.md`). So C code
in the VM that can raise must not hold a lock or own an unreleased resource
at that point. Natives should still report failures as error values.

## Keeping `docs/reference.md` Up-to-date

**Rule:** whenever you change a built-in library (`src/sea_lib_*.c`) or the language syntax (parser/compiler), also update `docs/reference.md` in the same commit/session. This includes:
- Adding or removing registered functions in any `sea_lib_*_register()` call
- Changing function signatures or behaviour
- Adding or removing keywords, operators, or syntax constructs

## Keeping `docs/libs.md` Up-to-date

**Rule:** whenever you change an external library under `libs/` (any `.sea` or `.c` file), also update `docs/libs.md` in the same commit/session. This includes:
- Adding or removing public functions in any `libs/*/` module
- Changing function signatures, option tables, or return shapes
- Adding a new library under `libs/`

## Sea Language — Key Gotchas

The gotchas of the language (0-based arrays, sealed structs, 48-bit ints, `[[`, `re.gsub` argument order, name shadowing, ...)
live in the skill `.claude/skills/sea/SKILL.md`; load it when writing or reviewing Sea. `docs/sea-for-ai.md` is the complete guide
(its examples are checked by `tests/unit/132_ai_guide`): keep it and the skill in sync when the language changes.

- **`libs/ssh`** (pure Sea SSH client, `ssh.serve` test server) rides on `libs/ssl`, which gained Ed25519/X25519 (`keygen`, `key_raw`, `key_from_raw`, `x25519`, `sign`/`verify` with `"ed25519"`). Its tests run against `ssh.serve`; `SSH_TEST_ADDR/USER/KEY` adds a real sshd (a Docker alpine/ubuntu container with `openssh-server` does, no root needed). Only curve25519-sha256, ssh-ed25519, aes256-gcm@openssh.com, public-key auth and `exec`; unencrypted OpenSSH keys. A host key is never trusted implicitly (`host_key` fingerprint or `verify`).
- **Shell hygiene when running Sea tools**: `pkill -f pattern` kills the shell that carries the pattern in its own command line (use `kill $(pgrep -f "[p]attern")`); run long test suites in the background and never two suites at once against the same test database (they time out each other); `sea fmt` rewrites files in place (`-l` only lists).
