# Development notes

What is easy to forget when working on the compiler, the optimizer or the build. The architecture is in `architecture.md`, the
memory model in `memory.md`, the known bugs in `bugs.md`.

## Optimizer (src/sea_compiler.c)

The compiler keeps per-register facts in `SeaRegMeta`: `is_int`, `is_owned`, `is_const`/`const_int` (constant folding),
`has_copy`/`copy_of` (copy propagation: `local y = x` emits no `MOVE` until it must), `dwe_write_pos`/`dwe_read` (dead-write
elimination of temporaries). Rules that cost a debugging session each:

- **Clear `is_const` of an assignment's target before compiling its right side**, or `x = x + 1` folds to `LOAD_INT 1`.
- **Materialize deferred copies at merges**: `invalidate_locals_at_merge` runs at every branch/loop exit and *before* `loop_start`
  (a `MOVE` inside the loop body would reset the variable each iteration). Also in `compile_store`, `end_scope` and
  `resolve_upvalue` (a captured local that is a copy).
- **Facts follow the control flow**: `if`/`select` clear consts and materialize copies on entry; `is_int` is snapshotted and met
  (`IntSnap`); loops use `loop_head_ints` (a fixed point over the AST) and a meet on exit; captured locals carry no facts. A new
  control-flow construct must handle the facts. `SEA_TYPE_CHECK=1` aborts when `IIADD`/`IISUB` run on a non-int (in `make test-dwe`).
- **Which registers an opcode reads** comes from one table, `sea_op_reads()` (src/sea_chunk.c); the JIT's `op_regs` agrees with it.
  A missing read lets dead-write elimination delete a live load (Bugs 10 and 14). `SEA_DWE_CHECK=1` turns an eliminated write into a
  visible poison value, `SEA_NO_DWE=1` disables the elimination, `make test-dwe` runs the suite both ways.
- **Arena allocation** of non-escaping `local s = {k=v}` structs and `local a = []` arrays (`sea_scan_escapes`, `OP_NEW_STRUCT_LOCAL`,
  `OP_NEW_ARRAY_LOCAL`): an array's header is in the frame arena, its data buffer stays on the heap; anything referenced inside a
  closure body is marked escaped (`esc_walk_captures`); only top-level locals of the function are candidates (the arena resets per
  frame, not per iteration); a mutated struct field marks the struct escaped.

## Sanitizers

Build them out of tree by overriding Makefile variables, so `bin/` and `obj/` stay untouched:

- ASAN+UBSan of the JIT: `make JIT_OBJ_DIR=<dir>/obj-jit-asan JIT_TARGET=<dir>/sea-jit-asan CFLAGS="-std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wno-unused-parameter -fno-strict-aliasing -D_GNU_SOURCE" LDFLAGS="-lpthread -lm -ldl -rdynamic -fsanitize=address,undefined" <dir>/sea-jit-asan`.
  `ASAN_OPTIONS=detect_leaks=0` checks errors only. Leaks are measured by comparing N with 2N iterations (see `memory.md`).
- TSAN of the interpreter: the same with `OBJ_DIR=`/`TARGET=` and `-fsanitize=thread`. It needs `setarch -R` (ASLR entropy of this
  kernel: "unexpected memory mapping"); exit code 66 means there was a report.
- `make debug` does not rebuild when `obj/` is current (the flags are not dependencies): `make clean` or a separate `OBJ_DIR`.
- With `ptrace_scope=1`, gdb attaches to its own children only (`gdb -batch -x cmds bin/sea`), not with `-p`.

## Libraries and projects

- Libraries are required by bare name (`require("http")`); a non-installed `bin/sea` finds them in `<repo>/libs/`, and
  `tools/lib/check.sea`, `tools/build.sea` and `tools/lsp.sea` mirror the search. Dependencies between projects are in `sea.toml`
  (git) and fetched by `sea get` into `sea_modules/` (gitignored); `sea.lock` is committed. After pushing a dependency, `sea get -u <name>`
  in the projects that use it. Do not change the search order: native libraries rely on it (`require("embed_native")`).
- A file named like a lib it requires loads itself (the requiring file's directory is searched first): name project modules differently.

## Scripts instead of Python

Everyday scripts are written in Sea (one binary, fast start): array literals, template strings, compound assignment, `list`,
`json`/`csv` as globals, `sea -e 'code'`, `time.parse`, `sea/stream`, `sea/flags`. `docs/scripts.md` compares five everyday scripts
with Python (`tests/compare/scripts/run_scripts.sh`); benchmarks here are noisy (±10% from code layout alone): compare the minimum of
N runs in one process, and against a worktree build of the previous commit.

## Notes on some libraries

- **libs/cockroach and libs/clickhouse** (pure Sea): the PostgreSQL wire protocol over `net` (SCRAM, TLS by `ssl.handshake_client`, a pool,
  a retry on 40001) and ClickHouse's HTTP interface. Their tests run with `bin/sea test libs/cockroach libs/clickhouse`; the server parts need
  `COCKROACH_URL` / `CLICKHOUSE_URL` (a local `cockroach` binary with `--insecure`, or a clickhouse-server container).
- **libs/ndarray** (NumPy-like): an array is a table `{data, dtype, shape, strides (elements), offset, ndim, size}` plus methods copied into each
  array (the prototype is cloned by memcpy of its hash entries, keys and functions pinned: about 3 us per small op). Only methods, no operator
  overloading (`a:add(b)`, `a:get("1, 1:")`, `a:set(":, 0", v)`), by the user's choice. Options are struct literals (`{axis = 0}`). float32, complex128,
  einsum, as_strided and linalg.eig exist. Not done: fft, complex64, complex qr/svd/eigh, nan-min/max, a faster small-array path.
- **libs/embed**: GGUF embedding models in-process through llama.cpp (`fetch.sh` builds release b11119 into `vendor/`, needs cmake). Threads default
  to cores - 1 (all cores on a busy 4-core machine made a short text 10x slower). Ollama's embeddinggemma blob fails in llama.cpp ("wrong number of
  tensors"): the lib falls back to the HF ggml-org GGUF. A module first required inside a goroutine used to be freed with that goroutine's VM
  (fixed: chunks live in `SeaShared`).
- Moved out of this repo on purpose: the LLM experiments (`~/projects/conceitos`, not a git repo, uses `../sea_vm`); `libs/llm` and `libs/strands`
  are parked in `~/projects/sea_vm_parked/` (with the docs and the steps to restore them). Do not add LLM material back under `examples/`.
