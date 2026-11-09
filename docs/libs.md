# Sea External Libraries Reference

External libraries live under `libs/`. Native libraries (`.so`) must be built before use;
pure-Sea libraries work out of the box.

---

## Library Overview

| Library | Path | Kind | Build |
|---------|------|------|-------|
| `json` | built in: the global `json` (or `require("json")`); also `libs/json/json` | native, compiled into `sea` | — |
| `yaml` | `libs/yaml/yaml` | native `.so` | `make -C libs/yaml` |
| `toml` | `libs/toml/toml` | native `.so` | `make -C libs/toml` |
| `csv` | built in: the global `csv` (or `require("csv")`); also `libs/csv/csv` | native, compiled into `sea` | — |
| `compress` | `libs/compress/compress` | native `.so` (zlib) | `make -C libs/compress` (needs `zlib1g-dev`) |
| `xml` | `libs/xml/xml` | native `.so` | `make -C libs/xml` |
| `console` | `libs/console/console` | native `.so` | `make -C libs/console` |
| `http` | `libs/http/http` | Sea + native | `make -C libs/http` |
| `ssl` | `libs/ssl/ssl` | Sea + native | `make -C libs/ssl` |
| `jwt` | `libs/jwt/jwt` | Sea + native | `make -C libs/jwt` |
| `flags` | built in: `require("sea/flags")` (`libs/flags/flags.sea`) | pure Sea | — |
| `test` | `libs/test/test` | pure Sea | — |
| `stream` | built in: `require("sea/stream")` (`libs/stream/stream.sea`) | pure Sea | — |
| `log` | built in: `require("sea/log")` (`libs/log/log.sea`) | pure Sea | — |
| `dotenv` | built in: `require("sea/dotenv")` (`libs/dotenv/dotenv.sea`) | pure Sea | — |
| `template` | `libs/template/template` | pure Sea | — |
| `image` | `libs/image/image` | pure Sea (PNG through `compress`) | `make -C libs/compress` for PNG |
| `truetype` | `libs/truetype/truetype` | pure Sea (uses `image`) | — |
| `tensor` | `libs/tensor/tensor` | native `.so` | `make -C libs/tensor` |
| `ndarray` | `libs/ndarray/ndarray` | Sea + native | `make -C libs/ndarray` |
| `duckdb` | `libs/duckdb/duckdb` | Sea + native | `make -C libs/duckdb` (downloads libduckdb) |
| `sqlite` | `libs/sqlite/sqlite` | Sea + native | `make -C libs/sqlite` (downloads the SQLite source) |
| `embed` | `libs/embed/embed` | Sea + native | `make -C libs/embed` (downloads and builds llama.cpp; needs cmake) |
| `pdf` | `libs/pdf/pdf` | pure Sea (uses `compress`) | `make -C libs/compress` |
| `cockroach` | `libs/cockroach/cockroach` | pure Sea (PostgreSQL protocol; `libs/ssl` for TLS) | — |
| `clickhouse` | `libs/clickhouse/clickhouse` | pure Sea (HTTP interface, uses `libs/http`) | `make -C libs/http` |
| `websocket` | `libs/websocket/websocket` | Sea + native | `make -C libs/websocket` (and `libs/http`, `libs/ssl`) |
| `smtp` | `libs/smtp/smtp` | pure Sea (`net`; `libs/ssl` for TLS) | `make -C libs/ssl` |

Require a library by its name: `require("http")`, `require("yaml")`. An installed `sea`
(`make install`, see the README) finds it in `lib/sea/`; a `sea` run from the source tree
(`bin/sea`, not installed) finds it in `libs/`, so the same code works in both and in any
project, without a path to the sea_vm checkout. Paths in `require` are relative to the
requiring file, for a project's own modules (`require("./util")`); a lib's own examples
reach it as `"../libname"`.

Writing a native library: see `src/sea.h` for the module convention and
[memory.md](memory.md#writing-native-functions) for the ownership rules. In
short, functions borrow their arguments and return an owned value
(`sea_val_ref()` for an argument or a container element). For UTF-8, use
`src/sea_utf8.h` (decode, encode, validate, case mapping, terminal widths)
instead of a private copy.

---

## `json`

JSON (RFC 8259) in C, compiled into the `sea` binary: `json` is a global
(like `string` or `fs`), and `require("json")` returns it. The same code
also builds as `libs/json/json.so` (`make -C libs/json`), for a program
that loads it from that path.

```sea
print(json.encode({ok = true, items = [1, 2]}))   -- {"items":[1,2],"ok":true} (key order may vary)
local data = json.decode(fs.read("data.json")?)?
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `json.encode` | `encode(v [, opts]) → string\|error` | Serialize a Sea value |
| `json.decode` | `decode(s [, opts]) → value\|error` | Parse a JSON document |
| `json.valid` | `valid(s) → bool` | Check that `s` is valid JSON without building values |

Failures are **error values** (`is_error(e)`, `e.message`), so a JSON `null`
(→ `nil`) is never confused with a failure and `json.decode(s)?` passes a
failure up. Messages say where: `json.decode: invalid number (leading zero) at
line 3, column 8`, `json.encode: cycle detected (a value contains itself)`.

**`encode` options** (table, map or struct):

| Key | Default | Description |
|-----|---------|-------------|
| `indent` | — | Pretty-print: a number of spaces (up to 16) or a string such as `"\t"` |
| `sort_keys` | `false` | Sort object keys (byte order). Tables, maps and structs have no key order of their own, so use this for stable output |
| `ascii` | `false` | Escape every non-ASCII character as `\uXXXX` (a surrogate pair above U+FFFF) |
| `max_depth` | `512` | Deepest nesting allowed |

**`decode` options:** `max_depth` (default 512, at most 10000). Deeper input is
an error ("nesting too deep"), never a stack overflow. `bigint = true` turns
integers beyond Sea's int range into exact `bigint`s (IDs of 64 bits and more)
instead of numbers, which lose digits past 2^53.

**Type mapping — encode:**

| Sea value | JSON |
|-----------|------|
| `nil` | `null` |
| `bool` | `true` / `false` |
| `int` | integer |
| `bigint` | integer, every digit |
| `number` | shortest form that reads back exactly (`0.1`, `2.5e-08`); integral values keep a `.0` (`3.0`) so they decode as floats again; `NaN`/`Inf` → `null` |
| `string` | `"..."` with `\"`, `\\`, `\n`, `\r`, `\t`, `\b`, `\f` and `\u00XX` for other control characters; invalid UTF-8 bytes become U+FFFD |
| `array`, `int_array`, `float_array` | array |
| table with keys `0..n-1` (`{a, b, c}`) | array |
| other table, map `[=]`, struct | object. Integer, number and bool keys become strings (`{"1": …}`); other keys are skipped |
| function, channel, error, goroutine | left out of objects, `null` in arrays, an error at the top level |

A value that contains itself is an error; the same value appearing twice (not
nested in itself) is fine.

**Type mapping — decode:**

| JSON | Sea value |
|------|-----------|
| `null` | `nil` |
| `true` / `false` | `bool` |
| integer within ±2^47 | `int` |
| other numbers (fraction, exponent, larger integers) | `number` (larger integers: `bigint` with `bigint = true`) |
| string | `string` (escapes and surrogate pairs decoded to UTF-8; a lone surrogate becomes U+FFFD) |
| array | `array` (**0-indexed**) |
| object | table (string keys; for a repeated key the last value wins) |

Decoding is strict: leading zeros, `1.`, `-`, trailing commas, single quotes,
raw control characters or unknown escapes in strings, and data after the value
are rejected. A UTF-8 byte-order mark at the start is skipped. Object keys are
shared: one string per distinct key per document, and the program's own
constant when it has one, so `obj.name` lookups compare pointers.

---

## `yaml`

YAML 1.2 in C: a complete reader (block and flow styles, every scalar style,
anchors and aliases, tags and `%TAG` directives, multiple documents, merge
keys), an encoder, and a low-level event API to read or write YAML with its
styles, tags and anchors kept. Conformance: every case of the official
[yaml-test-suite](https://github.com/yaml/yaml-test-suite) passes (events and
errors). Build: `make -C libs/yaml`.

```sea
local yaml = require("yaml")

local cfg = yaml.decode(os.readfile("config.yaml"))?
print(cfg.server.port)
print(yaml.encode(cfg, {sort_keys = true}))
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `yaml.decode` | `decode(s [, opts]) → value\|error` | Parse a stream holding one document (none: `nil`; more than one: an error) |
| `yaml.decode_all` | `decode_all(s [, opts]) → array\|error` | Parse every document of a stream |
| `yaml.valid` | `valid(s) → bool` | Check syntax, anchors and aliases without building values |
| `yaml.encode` | `encode(v [, opts]) → string\|error` | Serialize a Sea value as one document |
| `yaml.encode_all` | `encode_all(docs [, opts]) → string\|error` | Serialize an array of values as a stream of documents (`---` between them) |
| `yaml.events` | `events(s) → array\|error` | The parser's events, as tables |
| `yaml.emit` | `emit(events [, opts]) → string\|error` | Write a stream from events (the inverse of `events`) |

Failures are **error values** with the position: `yaml.decode: mapping values
are not allowed here at line 3, column 9`, `yaml.decode: duplicate mapping key
'port' at line 7, column 3`, `yaml.decode: found undefined alias 'base' at
line 2, column 8`. Input must be UTF-8 (a byte-order mark is skipped);
non-printable characters are rejected. A tag handler that panics stops
the decode, and the panic goes on.

**`decode` / `decode_all` options:**

| Key | Default | Description |
|-----|---------|-------------|
| `schema` | `"core"` | How plain scalars resolve: `"core"` (YAML 1.2), `"json"` (only JSON's literals; anything else is a string), `"failsafe"` (every scalar is a string) or `"1.1"` (YAML 1.1 as PyYAML reads it: `yes`/`no`/`on`/`off`, `0777` octal, `0b1010`, `1_000`, `190:20:30`). A document starting with `%YAML 1.1` uses `"1.1"` unless a schema is given |
| `bigint` | `false` | Integers beyond Sea's int range (±2^47) become exact `bigint`s instead of numbers |
| `string_keys` | `false` | Mapping keys stay strings as written (`1: a` → key `"1"`, `~: b` → key `"~"`) |
| `duplicate_keys` | `false` | Allow a key twice in a mapping (the last wins) instead of an error |
| `merge` | `true` | Merge keys: `<<: *base` or `<<: [*a, *b]` copy the keys the mapping does not set itself (earlier sources win) |
| `tags` | — | Handlers for custom tags: a table `{["!point"] = function(value, tag) ... end}`; `["*"]` catches any other. The handler's result replaces the value (an error value it returns stays in the data) |
| `tagged` | `false` | Without a handler, a value with a custom tag becomes `{tag = "!point", value = ...}` (by default the tag is dropped) |
| `max_depth` | `512` | Deepest nesting allowed (at most 10000) |

**Type mapping — decode** (core schema):

| YAML | Sea value |
|------|-----------|
| `null`, `Null`, `NULL`, `~`, empty | `nil` |
| `true`/`True`/`TRUE`, `false`/… | `bool` |
| `12`, `-3`, `0o17`, `0x1F` within ±2^47 | `int` (larger: `number`, or `bigint` with `bigint = true`) |
| `1.5`, `1e3`, `.inf`, `-.inf`, `.nan` | `number` |
| any other plain scalar, quoted or block scalar | `string` |
| sequence | `array` (**0-indexed**) |
| mapping | table; keys are strings, ints, numbers or bools as resolved. A `null` or collection key is an error (use `string_keys` for the first; `yaml.events` reads anything) |
| `&a` / `*a` | the same Sea object: `a: &x [1]` and `b: *x` share one array. A recursive structure (`&r [*r]`) is a cycle the cycle collector frees |

Standard tags: `!!str`, `!!int`, `!!float`, `!!bool`, `!!null` convert the
scalar (an invalid value is an error: `!!int abc`); `!!binary` decodes base64
into a string of bytes; `!!timestamp` stays a string; `!!set` (`? a`) gives
a table with `true` values; `!!seq`, `!!map`, `!!omap`, `!!pairs` keep the
collection as is. The non-specific `!` makes a string. Every other tag goes to
`tags`/`tagged` (or is dropped). Integer keys and string keys are distinct:
`1: a` and `"1": b` are two keys.

**`encode` / `encode_all` options:**

| Key | Default | Description |
|-----|---------|-------------|
| `indent` | `2` | Spaces per level (1 to 9). Sequences inside mappings are indented |
| `sort_keys` | `false` | Sort mapping keys (byte order of their text). Tables, maps and structs have no key order of their own, so use this for stable output |
| `flow` | `false` | Write everything in flow style on one line (`{a: [1, 2]}`) |
| `anchors` | `false` | A table or array reached more than once is written once with `&id001` and then as `*id001`; this also encodes cycles (otherwise an error) |
| `explicit_start` | `false` | Start each document with `---` |
| `explicit_end` | `false` | End each document with `...` |
| `max_depth` | `512` | Deepest nesting allowed |

**Type mapping — encode:**

| Sea value | YAML |
|-----------|------|
| `nil` | `null` |
| `bool` | `true` / `false` |
| `int`, `bigint` | integer (every digit) |
| `number` | shortest form that reads back exactly; integral values keep `.0` (`3.0`), exponents get a dot (`1.0e+20`, as YAML 1.1 needs); `.inf`, `-.inf`, `.nan` |
| `string` | plain when that reads back as the same string under the core schema and YAML 1.1 (`'true'`, `'123'`, `'yes'`, `'2001-12-14'`, `''` are quoted); multi-line text as a literal block (`|`, with `-`/`+` chomping and an indentation indicator when needed); otherwise single- or double-quoted with escapes. Bytes that are not UTF-8 become `!!binary` (base64) |
| `array`, `int_array`, `float_array`, table with keys `0..n-1` | sequence (empty: `[]`) |
| other table, map `[=]`, struct | mapping (empty: `{}`); string, int, number, bool and bigint keys; other keys are skipped |
| function, channel, error, goroutine | left out of mappings, `null` in sequences, an error at the top level |

Output is never wrapped. Decoding what `encode` writes gives the same value
back, and other YAML readers (PyYAML, libyaml) read it the same way.

**Events.** `yaml.events(s)` returns one table per event, in order:
`stream_start`, `document_start`, `mapping_start` … `mapping_end`,
`sequence_start` … `sequence_end`, `scalar`, `alias`, `document_end`,
`stream_end`. Each has `type`, `line` and `column` (1-based, where the node
starts), and as they apply:

| Field | Events | Meaning |
|-------|--------|---------|
| `value` | scalar | The text after escapes, folding and chomping |
| `style` | scalar | `"plain"`, `"single"`, `"double"`, `"literal"`, `"folded"` |
| `style` | mapping/sequence start | `"block"` or `"flow"` |
| `anchor` | scalar, mapping/sequence start | `&name` |
| `alias` | alias | `*name` |
| `tag` | scalar, mapping/sequence start | The resolved tag: `tag:yaml.org,2002:str` for `!!str`, `!local`, `!` |
| `implicit` | document start/end | `false` when written with `---` / `...` |
| `version`, `tags` | document start | `%YAML` version (`"1.2"`) and `%TAG` directives (`{["!e!"] = "tag:example.com,2000:"}`) |

`yaml.emit(events [, opts])` writes events back (the `stream_*` events are
optional; `implicit`, `version` and `tags` are honoured). Styles are kept
where they are valid for the value and the place (a plain scalar that would
not parse is quoted, a block scalar inside a flow collection becomes double
quoted); a scalar without a `style` is quoted when its text would read as
something other than a string. Options: `indent`, `flow`. Comments are not
events, so `events` → `emit` drops them; everything else (key order, styles,
tags, anchors) survives.

```sea
-- rewrite a file keeping its order and styles, changing one value
local evs = yaml.events(src)?
for _, e in evs do
    if e.type == "scalar" and e.value == "8080" then e.value = "9090" end
end
print(yaml.emit(evs))
```

---

## `toml`

TOML 1.0 in C: decode and encode. Conformance: every case of the official
[toml-test](https://github.com/toml-lang/toml-test) suite for TOML 1.0 passes
(708 of 709 compare exactly; the other differs only in how its reference
writes `.6` seconds as `.600`). Build: `make -C libs/toml`.

```sea
local toml = require("toml")

local cfg = toml.decode(fs.read("config.toml")?)?
print(cfg.database.port, cfg.servers.alpha.ip)
fs.write("out.toml", toml.encode(cfg, {sort_keys = true})?)
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `toml.decode` | `decode(s [, opts]) → table\|error` | Parse a document |
| `toml.encode` | `encode(t [, opts]) → string\|error` | Write a table as a document |
| `toml.valid` | `valid(s) → bool` | Whether `s` is valid TOML |
| `toml.datetime` | `datetime(s) → value\|error` | A date/time value (written bare by `encode`) from its RFC 3339 text |

Failures are error values with the position: `toml.decode: duplicate key
'port' at line 7, column 1`, `toml.decode: table 'a' defined twice at line 9,
column 2`. Every rule of the spec is checked: keys and tables defined once,
tables made by dotted keys or inline tables closed to later additions, `[[ ]]`
only on arrays of tables, valid UTF-8, no control characters, integers within
64 bits, real calendar dates.

**Decode** (`opts`: `bigint`, `datetimes`):

| TOML | Sea value |
|------|-----------|
| table, inline table | table |
| array, array of tables | array (**0-indexed**) |
| string (all four kinds) | `string` |
| integer (decimal, `0x`, `0o`, `0b`, `_`) | `int` within ±2^47; beyond, `number` (exact `bigint` with `bigint = true`) |
| float, `inf`, `nan` | `number` |
| boolean | `bool` |
| offset/local date-time, local date, local time | `string`, normalized to RFC 3339 (`T` separator, uppercase `Z`; fractions as written): `"1979-05-27T07:32:00Z"`, `"1979-05-27"`, `"07:32:00"`. With `datetimes = "values"`: a table `{__toml_datetime = text, kind = "datetime" \| "datetime-local" \| "date-local" \| "time-local"}` |

**Encode** (`opts.sort_keys`): the top level must be a table (or map or
struct). Plain keys come first, then each sub-table as a `[section]` and each
list of tables as `[[section]]` entries, nested sections with dotted names.
Keys are bare when they can be (`A-Za-z0-9_-`), quoted otherwise; strings
are basic strings with escapes; integral floats keep `.0`; `nan`, `inf`;
bigints within 64 bits; lists of other values (or mixed) are inline arrays,
records inside them inline tables; datetime values (from `toml.datetime` or
`datetimes = "values"`) are written bare. TOML has no null: `nil` fields are
left out and a `nil` inside an array is an error, as are functions and other
values with no TOML form. Decoding what `encode` writes gives the same data
back, and Python's `tomllib` reads it the same way.

---

## `csv`

CSV (RFC 4180) in C: decode a string, encode rows, or stream a file of any
size row by row. Compiled into the `sea` binary: `csv` is a global, and
`require("csv")` returns it (`make -C libs/csv` builds the same code as a
`.so`).

```sea

for _, r in csv.decode(fs.read("people.csv")?, {header = true, numbers = true})? do
    print(r.name, r.age + 1)
end
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `csv.decode` | `decode(s [, opts]) → array\|error` | Every row: arrays of strings, or tables keyed by the header with `header = true` |
| `csv.encode` | `encode(rows [, opts]) → string\|error` | Rows as CSV text |
| `csv.each` | `each(path, fn [, opts]) → int\|error` | Read a file in 64 KB chunks and call `fn(row)` for each row (`false` from `fn` stops); returns the number of rows. Memory stays flat however big the file is |

Fields may be quoted with `"`: a quoted field can hold the separator, line
breaks and `""` for a quote. `\n`, `\r\n` and `\r` end a record; a final
line break does not make an empty row; a UTF-8 byte-order mark is skipped.
Failures are error values with the line: `csv.decode: unterminated quoted
field at line 12`, `csv.decode: line 7 has 4 fields, the header has 3`.

**`decode` / `each` options:**

| Key | Default | Description |
|-----|---------|-------------|
| `sep` | `","` | Field separator (one character: `";"`, `"\t"`, ...) |
| `quote` | `"\""` | Quote character |
| `header` | `false` | The first row names the columns: rows become tables `{name = value}`, and a row with a different number of fields is an error |
| `numbers` | `false` | Unquoted fields in decimal syntax become numbers (`ints` within ±2^47, else floats: `31`, `-2.5`, `1e3`); `"42"` (quoted), `007`, `0x10` and empty fields stay strings |
| `trim` | `false` | Strip spaces and tabs around fields (outside quotes) |
| `comment` | — | A character that starts a comment line (skipped whole) |
| `skip_empty` | `true` | Skip blank lines (`false`: they are rows with one empty field) |
| `strict` | `false` | Reject text after a closing quote, quotes inside unquoted fields, and (without a header) rows whose field count differs from the first row's. Without it, such text is kept as written (like Python's reader) |

**`encode`**: a row is a list (array, or table with keys `0..n-1`), written in
order, or a record (table, map, struct), written in the order of
`opts.columns` (default: every key of every record, sorted) under a header
line (`header = false` leaves it out). Strings, numbers (shortest form that
reads back exactly), bools and bigints are written; `nil`, NaN and infinities
as empty fields; other values are an error. A field is quoted when it holds
the separator, a quote, a line break, or leading or trailing spaces. Options:
`sep`, `quote`, `columns`, `header`, `crlf` (end lines with `\r\n`, as RFC 4180
asks; default `\n`), `quote_all`.

```sea
local out = csv.encode(people, {columns = {"name", "age", "city"}})
fs.write("people.csv", out)?
local adults = 0
local n = csv.each("big.csv", function(r)
    if r.age >= 18 then adults = adults + 1 end
end, {header = true, numbers = true})?
```

---

## `compress`

gzip, zlib and raw DEFLATE, and ZIP and tar archives, on the system's zlib. Build: `make -C
libs/compress` (needs zlib's headers: `zlib1g-dev` on Debian/Ubuntu).

```sea
local compress = require("compress")

local gz = compress.gzip(body)               -- e.g. for Content-Encoding: gzip
local back = compress.gunzip(gz)?
compress.gzip_file("access.log", "access.log.gz", {level = 9})?
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `compress.gzip` | `gzip(data [, opts]) → string` | The gzip file format (RFC 1952) |
| `compress.gunzip` | `gunzip(data [, opts]) → string\|error` | Decompress gzip data; several members back to back (as `cat a.gz b.gz` makes) are joined |
| `compress.deflate` | `deflate(data [, opts]) → string` | zlib format (RFC 1950) by default; `format = "raw"` (RFC 1951, as in ZIP and HTTP `deflate`) or `"gzip"` |
| `compress.inflate` | `inflate(data [, opts]) → string\|error` | Decompress; `format = "auto"` (default: zlib or gzip, by the header), `"zlib"`, `"gzip"` or `"raw"` |
| `compress.gzip_file` | `gzip_file(src, dst [, opts]) → int\|error` | Compress a file into another in 64 KB steps (memory stays flat); the compressed size |
| `compress.gunzip_file` | `gunzip_file(src, dst [, opts]) → int\|error` | Decompress a file into another; the decompressed size |
| `compress.version` | `version() → string` | zlib's version |

**Options:** `level` (0-9, default 6; a bare number works too:
`compress.gzip(data, 9)`), `format` (see above), `max` (the largest output
allowed when decompressing, default 1 GiB in memory and no limit for files:
decompressing more is the error `output larger than the limit (opts.max)`,
a guard against decompression bombs). Damaged data is an error value with
zlib's reason (`incorrect header check`, `invalid distance too far back`) or
`truncated compressed data`. CRC-32 and Adler-32 checksums are in the
built-in `hash` module.

### Archives: ZIP and tar

```sea
compress.zip_write("site.zip", ["public", {name = "VERSION", data = "1.4\n"}])?
for _, e in compress.zip_list("site.zip")? do print(e.name, e.size, e.compressed) end
local index = compress.zip_read("site.zip", "public/index.html")?
compress.zip_extract("site.zip", "/tmp/site")?

compress.tar_write("backup.tar.gz", ["etc", "notes.txt"], {base = os.getenv("HOME")})?   -- gzip: by the name
compress.tar_extract("backup.tar.gz", "restore", {names = ["notes.txt"]})?
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `compress.zip_write`, `compress.tar_write` | `(dst, entries [, opts]) → int\|error` | Write an archive; the number of entries. A `.gz` / `.tgz` name (or `opts.gzip`) makes a gzipped tar |
| `compress.zip_list`, `compress.tar_list` | `(src) → array\|error` | The members: `{name, type, size, mode, mtime}` (`type`: `"file"`, `"dir"`, `"symlink"`, `"link"`, `"other"`; a link has `link`; ZIP adds `compressed`) |
| `compress.zip_read`, `compress.tar_read` | `(src, name [, opts]) → string\|error` | One file's contents |
| `compress.zip_extract`, `compress.tar_extract` | `(src, dir [, opts]) → array\|error` | Extract files and directories under `dir` (created as needed), with their modes and times; the names extracted. `opts.names`: only these |

**Entries** (writing): an array of paths, where a directory adds every file
under it (in name order, empty directories kept), and of tables `{name =
"in/archive.txt", data = "..."}` or `{name = ..., path = "on/disk"}`; a
single path works too. Names in the archive are the paths as given
(`{base = dir}`: read relative to `dir`). **Options:** `base`, `level`
(0-9), `mtime` (one time for every entry, for reproducible archives), `max`
(largest member read, default 1 GiB), `names` (extract).

Reading takes ZIPs from any tool (stored or deflated) and tar archives in
ustar, PAX and GNU formats (long names, gzipped or not). Archives are read
whole into memory. Extraction refuses an archive with an absolute name or a
`..` part before writing anything, and skips links. Not supported: ZIP64
(archives over 4 GB or 65535 entries) and encrypted ZIPs.

---

## `xml`

XML 1.0 in C: a strict parser, an encoder, queries (a small XPath subset), a
SAX mode and helpers to build and edit trees. Build: `make -C libs/xml`.

```sea
local xml = require("xml")
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `xml.parse` | `parse(s [, opts]) → element\|document\|error` | Parse a document into a tree |
| `xml.valid` | `valid(s) → bool` | Check well-formedness without building a tree |
| `xml.sax` | `sax(s, handlers [, opts]) → bool\|error` | Call handlers as the document is read (no tree) |
| `xml.encode` | `encode(node [, opts]) → string\|error` | Serialize an element or a document |
| `xml.escape` | `escape(s [, attr]) → string` | Escape `& < >` (and `" ' \n \t` when `attr` is true) |
| `xml.find` | `find(node, tag) → element\|nil` | First child element with that tag (`"*"`: any) |
| `xml.findall` | `findall(node, tag) → array` | Child elements with that tag (`"*"`: all) |
| `xml.descendants` | `descendants(node [, tag]) → array` | Every element under `node` in document order, optionally only one tag |
| `xml.query` | `query(node, path) → array\|error` | Elements, attribute values or text matching a path (see below) |
| `xml.query_one` | `query_one(node, path) → value\|nil\|error` | First match of `query` |
| `xml.attr` | `attr(node, name [, default]) → value` | An attribute, or `default` (nil) when absent |
| `xml.text` | `text(node [, deep]) → string` | The element's own text; with `deep`, all text under it in order |
| `xml.element` | `element(tag [, attrs [, content...]]) → element` | Build an element; content is strings, elements, comment/cdata/pi/text nodes or arrays of them |
| `xml.append` | `append(node, x) → x` | Append a string, element or comment/cdata/pi/text node |
| `xml.set_text` | `set_text(node, s) → node` | Replace the whole content with the text `s` |
| `xml.set_attr` | `set_attr(node, name, value) → node` | Set an attribute (string, number or bool) keeping `attr_order`; `nil` removes it |
| `xml.remove` | `remove(node, child) → bool` | Remove a child (element or node by identity, a string by value) |

**Elements** are tables:

```sea
{ tag = "item",
  attrs = {id = "1", lang = "pt"},   -- attribute values are strings
  attr_order = ["id", "lang"],       -- names in document order
  children = [ ... ],                -- child elements only
  nodes = [ ... ],                   -- all content, in document order
  text = "..." }                     -- the element's own text, joined
```

`nodes` holds text as plain strings, the same element tables as `children`,
and (when kept) `{type = "comment", value}`, `{type = "cdata", value}` and
`{type = "pi", target, value}`. So `<p>Hello <b>world</b>!</p>` has
`nodes = ["Hello ", <b>, "!"]`, `children = [<b>]` and `text = "Hello !"`.
Test an entry with `type(n) == "string"` (text), `n.tag` (element) or `n.type`.

**`parse` options** (table, map or struct):

| Key | Default | Description |
|-----|---------|-------------|
| `ignore_whitespace` | `false` | Drop text that is only whitespace (indentation) from `nodes` and `text` |
| `comments` | `false` | Keep comments as nodes |
| `cdata` | `false` | Keep CDATA sections as nodes (otherwise they are text) |
| `pi` | `false` | Keep processing instructions as nodes |
| `namespaces` | `false` | Resolve namespaces: each element gets `ns` (the URI, absent when none) and `name` (the local name); `tag` keeps the prefix. An unbound prefix is an error |
| `document` | `false` | Return a document: `{type = "document", root, nodes, doctype, version, encoding, standalone}`; `nodes` holds the top-level comments/PIs, a `{type = "doctype", value}` node and the root |
| `max_depth` | `512` | Deepest nesting allowed (deeper input is an error; the parser never recurses) |

Parsing is strict: one root element, matching end tags, quoted and unique
attributes, no `<` in attribute values, no bare `&`, only the five predefined
entities and valid character references, no `]]>` in text or `--` in comments,
valid UTF-8 (other encodings are rejected) and no control characters. Line ends
become `"\n"` and whitespace characters in attribute values become spaces, as
the spec says. A DOCTYPE is skipped, never interpreted: its entities are not
expanded, so using one is an error. A UTF-8 byte-order mark is skipped.
Failures are **error values** with a position, e.g.
`xml.parse: expected </b>, found </a> at line 1, column 7`.

**`encode` options:**

| Key | Default | Description |
|-----|---------|-------------|
| `indent` | — | Pretty-print with a number of spaces (up to 16) or a string such as `"\t"`. Whitespace-only text is replaced by the indentation; elements with mixed content are written inline |
| `declaration` | `false` | Start with `<?xml version="1.0" encoding="UTF-8"?>` |
| `sort_attrs` | `false` | Write attributes sorted by name instead of `attr_order` first |
| `max_depth` | `512` | Deepest nesting allowed (at most 10000); also stops a node that contains itself |

`encode` takes an element or a document; nodes may be tables, maps or structs,
so `{tag = "a", attrs = {k = 1}, text = "hi"}` works. Attributes come in
`attr_order` and then the rest by name, so output is stable. Attribute values
may be strings, numbers or bools. Content comes from `nodes` when present,
otherwise from `text` followed by `children`. When `nodes` disagrees with
`children` or `text` (an element added to `children` by hand, say), `encode`
fails rather than drop the edit. The exception is a node whose `nodes` holds
only text, as in `leaf.text = "new"`: `text` and `children` are used. Edit
through `append`, `set_text` and `remove`, or set `nodes = nil`. Invalid names,
control characters, invalid UTF-8, `--` in a comment and `?>` in a PI are
errors; `]]>` in CDATA is split across two sections.

**Paths** for `query` start at the given node:

| Path | Selects |
|------|---------|
| `book`, `shelf/book` | child elements by tag (qualified names such as `d:title` as written) |
| `*` | any child element |
| `//book`, `shelf//title` | descendants at any depth |
| `/lib/book` | from the top: the first step must match the node itself (or a document's root) |
| `.` | the node itself |
| `@id`, `@*` | attribute values (last step only) |
| `text()` | text nodes (last step only) |
| `[2]`, `[last()]` | position among the matches for each parent (1-based) |
| `[@id]`, `[@id='2']`, `[@id!='2']` | has the attribute / its value equals / differs |
| `[year]`, `[year='1881']` | has a child element / one whose text equals |
| `[text()='x']` | the element's text equals |

Predicates can be chained (`book[@lang][2]`); unquoted numbers are compared as
text (`[year=1881]`). There is no `..` (nodes do not link to their parent).
A bad path returns an error value.

**`sax` handlers** (all optional): `start(tag, attrs [, ns])`, `finish(tag)`,
`text(s)`, `cdata(s)`, `comment(s)`, `pi(target, data)`, `doctype(s)`. CDATA,
comments and PIs are reported only when their handler exists (CDATA otherwise
counts as text). The options are those of `parse` (`ignore_whitespace`,
`namespaces` adds the `ns` argument, `max_depth`). A handler returning `false`
stops the parse and `sax` returns `false`; a panic in a handler propagates.
The checks and errors are the same as for `parse`.

```sea
local doc = xml.parse(src, {ignore_whitespace = true})?
for _, book in xml.query(doc, "//book[@lang='pt']") do
    print(xml.attr(book, "id"), xml.text(xml.find(book, "title")))
end

local feed = xml.element("feed", {version = 2}, xml.element("title", nil, "News & more"))
xml.append(feed, xml.element("item", {id = 1}, "first"))
print(xml.encode(feed, {indent = 2, declaration = true}))
```

---

## `console`

```sea
local console = require("./libs/console/console")
```

### Cursor

| Function | Signature | Description |
|----------|-----------|-------------|
| `console.goto` | `goto(row, col)` | Move cursor to absolute position (1-based) |
| `console.up` | `up([n])` | Move cursor up `n` lines (default 1) |
| `console.down` | `down([n])` | Move cursor down `n` lines |
| `console.left` | `left([n])` | Move cursor left `n` columns |
| `console.right` | `right([n])` | Move cursor right `n` columns |
| `console.save` | `save()` | Save cursor position |
| `console.restore` | `restore()` | Restore saved cursor position |
| `console.home` | `home()` | Move cursor to top-left (1,1) |
| `console.clear` | `clear()` | Clear entire screen |
| `console.clear_line` | `clear_line()` | Clear current line |
| `console.clear_eol` | `clear_eol()` | Clear from cursor to end of line |
| `console.hide_cursor` | `hide_cursor()` | Hide cursor |
| `console.show_cursor` | `show_cursor()` | Show cursor |
| `console.size` | `size() → {width, height}` | Terminal dimensions |

### Text & colors

#### `console.write(text [, opts])` / `console.writeln(text [, opts])`

Print with optional styling. `writeln` appends a newline.

**`opts` fields:**

| Key | Type | Description |
|-----|------|-------------|
| `color` | string / `"#RRGGBB"` / `{r,g,b}` | Foreground color |
| `bg` | string / `"#RRGGBB"` / `{r,g,b}` | Background color |
| `bold` | bool | Bold |
| `dim` | bool | Dim/faint |
| `italic` | bool | Italic |
| `underline` | bool | Underline |
| `blink` | bool | Blink |
| `reverse` | bool | Swap fg/bg |

Named colors: `black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `white`,
`bright_black` … `bright_white`, `default`.

#### `console.reset()`

Reset all text attributes to default.

### Input

| Function | Signature | Description |
|----------|-----------|-------------|
| `console.readline` | `readline([prompt]) → string\|nil` | Read a line from stdin with optional prompt |
| `console.readkey` | `readkey() → string` | Read a single keypress (raw mode, no Enter needed) |
| `console.readpass` | `readpass([prompt]) → string\|nil` | Read a line with echo disabled (passwords) |

### Progress bar

#### `console.progress(current, total [, opts])`

Renders a progress bar inline (no newline). Overwrite with `\r` in a loop.

| Option | Default | Description |
|--------|---------|-------------|
| `width` | `30` | Bar width in characters |
| `fill` | `"█"` | Filled character |
| `empty` | `"░"` | Empty character |
| `color` | `"green"` | Bar color |
| `show_percent` | `true` | Show percentage |
| `show_count` | `false` | Show `current/total` |

### Shimmer / glow

#### `console.shimmer(text [, opts])`

One-shot animation: a brightness wave sweeps across the text, then leaves it dim.

| Option | Default | Description |
|--------|---------|-------------|
| `duration` | `1.5` | Animation length in seconds |
| `fps` | `30` | Frames per second |
| `wave_width` | `0.30` | Wave width as fraction of text length |
| `color` | `{r=255,g=255,b=255}` | Peak (brightest) color |
| `base` | `{r=70,g=70,b=70}` | Trough (darkest) color |

#### Background shimmer (non-blocking)

Runs in a background thread while your code does other work.

| Function | Signature | Description |
|----------|-----------|-------------|
| `console.shimmer_start` | `shimmer_start(text [, opts])` | Start background shimmer. `opts`: `fps`, `wave_width`, `bar_width`, `bar_total`, `color`, `base` |
| `console.shimmer_update` | `shimmer_update(text [, cur, total])` | Update text and optional progress counters |
| `console.shimmer_stop` | `shimmer_stop()` | Stop the background thread and clear the line |
| `console.shimmer_run` | `shimmer_run(text [, opts])` | Alias for `shimmer_start` |

```sea
console.shimmer_start("Loading model...", { bar_total = 100 })
for i = 0, 99 do
    -- do real work
    console.shimmer_update("Loading model...", i + 1, 100)
end
console.shimmer_stop()
```

---

## `http`

HTTP/1.1 client and server. Requires `http_native.so` (strict request/response parsers, URL coding), plus `libs/ssl` for HTTPS and `libs/json` for the JSON helpers. Build: `make -C libs/http libs/ssl libs/json` (one `make -C` each); `http_native.so` links zlib (`zlib1g-dev` / `zlib-devel`) for gzip.

```sea
local http = require("http")
```

Sockets come from the built-in `net` module: every network wait parks only the goroutine doing it (netpoller), so a slow or idle peer never stalls the others. All timeouts are in milliseconds.

### Client

| Function | Signature | Description |
|----------|-----------|-------------|
| `http.request` | `request(method, url [, opts]) → response` | Any method. The older `request(method, url, body, content_type, headers)` form still works |
| `http.get` / `head` / `delete` | `get(url [, opts]) → response` | Requests without a body |
| `http.post` / `put` / `patch` | `post(url, body [, content_type [, opts]]) → response` | `content_type` defaults to `application/octet-stream` when a `body` is given; `body` may be `nil` when `opts.json`/`opts.form` is used |

**Client `opts`** (table or map):

| Key | Default | Description |
|-----|---------|-------------|
| `headers` | — | Extra request headers (a value may be an array for a repeated header). CR/LF in names or values is rejected |
| `body` | — | Request body (string) |
| `json` | — | Value to send JSON-encoded (sets `Content-Type: application/json`) |
| `form` | — | Map sent as `application/x-www-form-urlencoded` |
| `content_type` | — | Content-Type of `body` |
| `query` | — | Map appended to the URL as a query string |
| `timeout` | `30000` | Per network operation (connect, TLS handshake, each read/write) |
| `follow_redirects` | `true` | `false`, `true` (up to 5 hops) or a max hop count. 303 — and 301/302 after a POST — become GET; `Authorization`/`Cookie` are dropped when redirected to another host |
| `keep_alive` | `true` | Reuse connections (pool of up to 8 idle per host, 30 s max age; a stale reused connection is retried once, on a new connection). A connection is reused only by requests with the same `verify` and `tls` options |
| `basic_auth` | — | `{user, password}` |
| `verify` | `true` | `false` skips TLS certificate checks |
| `tls` | — | Extra `ssl.connect` opts (`cert`, `key`, `sni`, `min_version`); `cert`/`key` a path or the PEM text (mutual TLS with a certificate kept in a database) |
| `max_body_bytes` | 100 MB | Larger responses fail (also the limit for a decompressed body) |
| `decompress` | `true` | Send `Accept-Encoding: gzip` and decode gzip/deflate responses. Off when you set `Accept-Encoding` yourself (the body is then yours to decode) |
| `on_data` | — | `function(piece)`: a 2xx body is passed in pieces as they arrive (chunked, `Content-Length` or until close) instead of being collected; `resp.body` is then `""`. Returning `false` stops reading (the connection is closed). Other statuses still fill `resp.body`, so an error message can be read. Turns off `Accept-Encoding: gzip`. For server-sent events and LLM streams |

The client sends `Host` (with the port when it is not the default), `User-Agent: sea-http/1.1`, `Accept: */*` and `Accept-Encoding: gzip` (unless given), and `Content-Length` (a non-string body is converted with `tostring`); it decodes chunked responses and skips 1xx interim responses.

**Response:**

| Field | Description |
|-------|-------------|
| `status` | Status code; `0` when the request failed (connection, TLS, timeout, malformed response) |
| `ok` | `true` for 2xx |
| `reason` | Reason phrase |
| `headers` | Map with lowercase keys (repeated headers joined with `", "`) |
| `body` | Body string (`""` for HEAD / 204 / 304) |
| `url` | Final URL after redirects |
| `error` | Message, only when `status == 0` |
| `uncompressed` | `true` when a gzip/deflate body was decoded (`Content-Encoding` and `Content-Length` are then removed from `headers`) |
| `resp:json()` | Decodes the body as JSON (an error value if it is not valid JSON) |

### Server

| Function | Signature | Description |
|----------|-----------|-------------|
| `http.listen` | `listen(port, handler [, opts]) → server\|error` | Start serving in the background and return at once. `port` 0 picks a free port |
| `http.serve` | `serve(port, handler [, opts])` | `listen` + print a "listening" line (unless `opts.quiet`) + block until the server is closed |

`handler(req) → response` is a function or a router. Every connection runs in its own goroutine.

**Server object:** `srv.port` (bound port), `srv.tls` (bool), `srv.close()` (stop accepting; idle keep-alive connections are closed, busy ones finish their request), `srv.wait()` (block until closed).

**Server `opts`:**

| Key | Default | Description |
|-----|---------|-------------|
| `host` | all interfaces | Address to bind |
| `tls` | — | Enable HTTPS: `{cert, key [, ca]}` (`ca` requires client certificates) |
| `read_timeout` | `30000` | Time to receive a whole request once it starts (also the TLS handshake); 408 after it |
| `idle_timeout` | `60000` | Time a keep-alive connection may wait for its next request |
| `write_timeout` | `30000` | Per response write |
| `max_header_bytes` | `65536` | Larger request heads get 431 |
| `max_body_bytes` | 10 MB | Larger bodies get 413 |
| `max_requests` | `1000` | Requests per connection before it is closed |
| `keep_alive` | `true` | `false` closes after every response |
| `log` | `false` | Print one line per request: method, target, status, time |
| `gzip` | `false` | `true`: gzip text-like responses (`text/*`, JSON, JavaScript, XML, SVG) of 1 KB or more for clients that accept it, adding `Vary: Accept-Encoding`; a number sets the minimum size. Responses that already have a `Content-Encoding` are left alone |
| `proxy_protocol` | `false` | `true`: behind a load balancer, every connection starts with a PROXY protocol v1 line (`PROXY TCP4 <client ip> <dest ip> <client port> <dest port>`, before TLS); `req.remote_addr` is then the client's address (`PROXY UNKNOWN` keeps the peer). A connection without a valid line is closed: with the option on, only the balancer may connect, or anyone could claim any address |
| `on_error` | prints | `on_error(err, req)`: called when a handler panics or hits a runtime error (the client gets a 500) |
| `quiet` | `false` | (`serve` only) no "listening" line |

The server supports keep-alive and pipelining, chunked request bodies, `Expect: 100-continue`, and HEAD (headers only). It adds `Date`, `Content-Length` and `Connection`. Malformed requests get 400, 501 (unknown Transfer-Encoding) or 505, including request-smuggling shapes: `Content-Length` together with `Transfer-Encoding`, conflicting lengths, obsolete line folding, and bare CR/LF.

**Request** (the handler's argument, a map):

| Field | Description |
|-------|-------------|
| `method` | `"GET"`, `"POST"`, … |
| `target` | Raw request target, e.g. `"/search?q=a%20b"` |
| `path` | Path **without** the query string (`"/search"`), not percent-decoded |
| `query` | Map of decoded query parameters (`req.query["q"] == "a b"`) |
| `headers` | Map with lowercase keys |
| `body` | Body string (chunked bodies already decoded) |
| `version` | `"HTTP/1.1"` or `"HTTP/1.0"` |
| `remote_addr` | `"ip:port"` of the peer |
| `tls` | `true` over HTTPS |
| `cookies` | Map from the `Cookie` header |
| `params` | Route parameters (router only) |
| `req:json()` | Decodes the body as JSON (an error value if it is not valid JSON) |

**Response** (what the handler returns): a table/map `{status = 200, body = "", headers = {...}, close = false}`, or just a string (200, `text/plain`). A non-string `body` is converted with `tostring`. Header values may be arrays (repeated headers). `close = true` closes the connection after the response. Anything else — `nil`, an invalid status, a header with CR/LF — becomes a 500 and is logged. A handler that panics or hits a runtime error (indexing nil, …) also gets the client a 500; the error goes to `on_error` (by default printed with the request) and the server keeps running. Without a `Content-Type`, `text/plain; charset=utf-8` is used.
**Streaming responses:** a response with `stream = function(write) ... end` (and no `body`) is sent with `Transfer-Encoding: chunked`: each `write(data)` sends one chunk at once and returns `false` once the client is gone. The response ends when the function returns; if it fails, the connection is closed without the final chunk, so the client sees a cut body. For server-sent events: `headers = {["Content-Type"] = "text/event-stream"}`.


**Protocol upgrades:** a response with `status = 101` and an `upgrade`
function switches protocols: after writing it (with `Connection: Upgrade`)
the server calls `upgrade(stream)` in the connection's goroutine and closes
the connection when it returns. `stream` has `read(max, timeout)`,
`write(data, timeout)` (plain or TLS, waiting through the netpoller),
`shutdown()`, `buffered` (bytes already read past the request),
`remote_addr`, `tls` and `request`. `srv.close()` shuts upgraded connections
down too. `libs/websocket` is built on this.

**Response helpers:**

| Function | Description |
|----------|-------------|
| `http.text(body [, status])` | `text/plain` response |
| `http.html(body [, status])` | `text/html` response |
| `http.json(value [, status [, headers]])` | JSON-encoded `value`, `application/json` (a value that cannot be encoded, e.g. a cycle, panics, so the server answers 500) |
| `http.redirect(url [, status])` | Redirect (302 by default) with `Location` |
| `http.set_cookie(resp, name, value [, opts]) → resp` | Adds a `Set-Cookie`; `opts`: `path, domain, max_age, expires, secure, http_only, same_site` |
| `http.static(dir [, opts]) → handler` | Serves files under `dir` with a MIME type by extension; `opts`: `index` (`"index.html"`), `prefix` (URL prefix to strip). GET/HEAD only; `..` and NUL in paths are refused |

### Router

```sea
local r = http.router()
r.use(function(req, next) ... return next(req) end)
r.get("/users/:id", function(req) return http.json({ id = req.params["id"] }) end)
http.serve(8080, r)
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `router.get` / `post` / `put` / `delete` / `patch` / `head` | `get(path, [mws,] fn)` | Register a handler for that method |
| `router.any` | `any(path, [mws,] fn)` | Handler for any method |
| `router.use` | `use(mw)` | Global middleware: `mw(req, next) → resp` |
| `router.group` | `group(prefix, {mws}) → group` | Sub-router with a shared prefix and middlewares (same methods) |
| `router.handle` | `handle(req) → resp` | Dispatch (what the server calls) |

**Path patterns:** exact segments (`/users`), parameters (`:name` → `req.params["name"]`, percent-decoded), and a trailing `*` (the rest of the path in `req.params["*"]`).

**Automatic responses:** 404 when no path matches; 405 with an `Allow` header when the path matches but the method does not. HEAD falls back to the GET route.

**Middleware:** call `next(req)` to continue, or return a response to short-circuit. Responses are turned into mutable maps, so middleware can change `status`, `body` and `headers` (a `stream` or `upgrade` function is kept); a `nil` from a handler becomes 500.

### Utilities

| Function | Description |
|----------|-------------|
| `http.parse_url(url) → {scheme, host, port, path, query, target}\|nil` | Splits an http/https URL (IPv6 literals in `[...]`, default ports, fragment dropped) |
| `http.parse_query(s) → map` | Decodes `a=1&b=x%20y` (the part after `?` if `s` is a path or URL); `+` means space |
| `http.build_query(t) → string` | Encodes a map (keys sorted; an array value repeats the key) |
| `http.url_encode(s)` / `http.url_decode(s)` | Percent-encoding (everything but `A-Z a-z 0-9 - _ . ~`); decoding turns `+` into a space |
| `http.status_text(code) → string` | Reason phrase, e.g. `"Not Found"` |

```sea
-- Client
local r = http.get("https://api.example.com/items", { query = { page = 2 }, timeout = 5000 })
if r.ok then print(r:json().total) else print(r.status, r.error) end

local r2 = http.post("https://api.example.com/items", nil, nil, { json = { name = "sea" } })

-- Server, stopped from code
local srv = http.listen(8080, function(req)
    if req.path == "/hello" then
        return http.text("Hello, " .. (req.query["name"] or "world") .. "!\n")
    end
    return { status = 404, body = "Not found\n" }
end, { log = true })
-- ...
srv.close()

-- HTTPS
http.serve(8443, function(req) return "Secure hello!\n" end,
    { tls = { cert = "cert.pem", key = "key.pem" } })
```

**Breaking changes (2026-10):** `req.path` no longer includes the query string (use `req.query`, or `req.target` for the raw form). Handlers returning `nil`, panicking or failing with a runtime error now produce a 500 instead of stopping the server.

---

## `ssl`

Requires `ssl_native.so` (OpenSSL). Build: `make -C libs/ssl`.

```sea
local ssl = require("./libs/ssl/ssl")
```

Full SSL/TLS + cryptography library. All handles (connections, certs, keys, CSRs) are opaque integers; call the matching `_free` function when done.

TLS runs over sockets from the built-in `net` module (see `docs/reference.md`): every wait — handshake, read, write, accept — parks only the calling goroutine through the netpoller (on the main thread it blocks), so one slow peer never holds up the others. Timeouts are in milliseconds and apply per operation.

### TLS Client

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.connect` | `connect(host, port [, opts]) → conn\|nil` | Open a TLS client connection. `opts` (table or map): `{verify, verify_host, ca, cert, key, sni, min_version, timeout}`: `ca` is a PEM file of trusted CAs (instead of the system's), `verify_host = false` checks the chain but not the host name, `cert`/`key` a client certificate, each a PEM file's path or the PEM text itself (starting with `-----BEGIN`: a certificate kept in a database, as an A1 from `pkcs12_parse`; the certificate's PEM may carry its chain after it; one that cannot be loaded is an error). On failure `ssl.last_error()` says why |
| `ssl.dial` | `dial(host, port [, opts]) → conn\|error` | Same as `connect`, but returns an error value (`is_error`) — safe from many goroutines at once |
| `ssl.handshake_client` | `handshake_client(fd, host [, opts]) → conn\|error` | TLS client handshake on a socket already connected with `net.connect`, for protocols that switch to TLS after a plain exchange (PostgreSQL's SSLRequest, STARTTLS). Takes the `connect` opts; closes the socket on failure |
| `ssl.last_error` | `last_error() → string\|nil` | Message of the last failure of a `nil`-returning function (shared, not per goroutine) |
| `ssl.read` | `read(conn [, n [, timeout]]) → string\|nil` | Without `n`: read until EOF. With `n`: read exactly `n` bytes (fewer if the stream ends) |
| `ssl.recv` | `recv(conn [, max [, timeout]]) → string\|error` | One read of whatever is available (up to `max`, default 65536); `""` at EOF |
| `ssl.read_request` | `read_request(conn [, timeout]) → string\|nil` | Read one full HTTP request (headers + Content-Length body). Prefer `libs/http` for real servers |
| `ssl.write` | `write(conn, data [, timeout]) → n\|nil` | Write all of `data` |
| `ssl.send` | `send(conn, data [, timeout]) → n\|error` | As `write`, with an error value on failure |
| `ssl.stream` | `stream(conn) → source` | The connection as a source/destination for `sea/stream`: `stream.reader(ssl.stream(conn))`, `stream.writer(ssl.stream(conn))` |
| `ssl.close` | `close(conn)` | Send close_notify (without waiting) and close the socket; wakes any goroutine waiting on it |
| `ssl.peer_cert` | `peer_cert(conn) → cert\|nil` | Peer certificate handle (inspect with `cert_info`, free with `cert_free`) |
| `ssl.version` | `version(conn) → string` | Negotiated TLS version, e.g. `"TLSv1.3"` |
| `ssl.cipher` | `cipher(conn) → string` | Negotiated cipher suite |

### TLS Server

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.server` | `server(opts) → ctx\|nil` | Create server context. `opts`: `{cert, key, ca, min_version}`. `ca` enables mTLS |
| `ssl.server_free` | `server_free(ctx)` | Free server context |
| `ssl.listen` | `listen(port [, host]) → fd\|nil` | Bind a TCP socket (a `net` fd) |
| `ssl.accept` | `accept(ctx, fd [, timeout]) → conn\|nil` | Accept one connection + TLS handshake (10 s default) |
| `ssl.handshake_server` | `handshake_server(ctx, fd [, timeout]) → conn\|error` | TLS server handshake on a socket from `net.accept`; closes it on failure |
| `ssl.wrap_server` | `wrap_server(ctx, fd [, timeout]) → conn\|nil` | As `handshake_server`, `nil` + `last_error()` on failure |
| `ssl.serve` | `serve(ctx, port, handler)` | Blocking accept loop; runs `handler(conn)` in a new goroutine per connection |

### Certificates

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.cert_load` | `cert_load(path) → cert\|nil` | Load X.509 cert from PEM file |
| `ssl.cert_parse` | `cert_parse(pem) → cert\|nil` | Parse X.509 cert from PEM string |
| `ssl.cert_info` | `cert_info(cert) → table` | `{subject, issuer, not_before, not_after, serial, fingerprint, san}`. `san` is a 0-indexed array |
| `ssl.pkcs12_parse` | `pkcs12_parse(bytes, password) → {cert_pem, key_pem, chain}\|error` | Read a PKCS#12 file (`.pfx`/`.p12`, an A1 certificate): the certificate, its private key (PEM, not encrypted) and the chain (array of PEM). Older files (RC2, 3DES) work too (OpenSSL 3's legacy provider). Errors: `pkcs12: wrong password`, `pkcs12: not a PKCS#12 (.pfx/.p12) file` |
| `ssl.cert_pem` | `cert_pem(cert) → string\|nil` | Serialize cert to PEM |
| `ssl.cert_save` | `cert_save(cert, path) → bool` | Save cert to PEM file |
| `ssl.cert_free` | `cert_free(cert)` | Free cert handle |
| `ssl.cert_verify` | `cert_verify(cert, ca_cert) → bool` | Verify cert was signed by ca_cert |
| `ssl.cert_pubkey` | `cert_pubkey(cert) → key\|nil` | The certificate's public key, for `ssl.verify` (free it with `key_free`) |
| `ssl.cert_self_signed` | `cert_self_signed(key, opts) → cert\|nil` | Self-signed cert. `opts`: `{cn, org, country, state, city, email, san=[], days=365, is_ca=false}` |
| `ssl.cert_sign` | `cert_sign(csr, ca_key, ca_cert, opts) → cert\|nil` | CA-sign a CSR. `opts`: `{days=365, serial=1}` |

### Keys

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.keygen` | `keygen(opts) → key\|nil` | Generate key pair. `opts`: `{type="rsa"\|"ec", bits=2048, curve="P-256"\|"P-384"\|"P-521"}` |
| `ssl.key_load` | `key_load(path [, password]) → key\|nil` | Load private key from PEM file |
| `ssl.key_parse` | `key_parse(pem [, password]) → key\|nil` | Parse private key from PEM string |
| `ssl.key_pem` | `key_pem(key [, password]) → string\|nil` | Serialize private key to PEM (AES-256-CBC encrypted if password given) |
| `ssl.key_pub_pem` | `key_pub_pem(key) → string\|nil` | Serialize public key to PEM |
| `ssl.key_save` | `key_save(key, path [, password]) → bool` | Save private key to PEM file |
| `ssl.key_free` | `key_free(key)` | Free key handle |

### CSR

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.csr_new` | `csr_new(key, opts) → csr\|nil` | Create a CSR. `opts`: `{cn, org, country, state, city, email, san=[]}` |
| `ssl.csr_pem` | `csr_pem(csr) → string\|nil` | Serialize CSR to PEM |
| `ssl.csr_free` | `csr_free(csr)` | Free CSR handle |

### Hash / HMAC

Supported `algo` strings: `"sha256"`, `"sha384"`, `"sha512"`, `"sha1"`, `"md5"`, `"sha3-256"`, `"sha3-512"`.

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.hash` | `hash(algo, data) → string\|nil` | Hex-encoded digest |
| `ssl.hash_raw` | `hash_raw(algo, data) → string\|nil` | Binary digest |
| `ssl.hmac` | `hmac(algo, key, data) → string\|nil` | Hex-encoded HMAC |
| `ssl.hmac_raw` | `hmac_raw(algo, key, data) → string\|nil` | Binary HMAC |

### Symmetric Encryption

Supported `algo` strings: `"aes-128-gcm"`, `"aes-256-gcm"`, `"aes-128-cbc"`, `"aes-256-cbc"`, `"chacha20-poly1305"`.

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.encrypt` | `encrypt(algo, key, plaintext [, iv [, aad]]) → {data, iv, tag}\|nil` | Encrypt. For GCM/ChaCha20: IV auto-generated if omitted (or nil); `tag` is always filled; `aad` (additional authenticated data, e.g. the record the value belongs to) is authenticated, not encrypted |
| `ssl.decrypt` | `decrypt(algo, key, iv, ciphertext [, tag [, aad]]) → string\|nil` | Decrypt. `tag` required for authenticated ciphers; nil when the tag, the `aad`, the key or the IV do not match (a key or IV shorter than the cipher's is refused) |

### Asymmetric Sign / Verify

Supported `algo` strings: `"rsa-sha1"` (still required by XMLDSig in the Brazilian NF-e), `"rsa-sha256"`, `"rsa-sha384"`, `"rsa-sha512"`, `"rsa-pss-sha256"`, `"rsa-pss-sha384"`, `"rsa-pss-sha512"`, `"ecdsa-sha256"`, `"ecdsa-sha384"`, `"ecdsa-sha512"`.

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.sign` | `sign(algo, key, data) → string\|nil` | Sign data; returns binary DER signature |
| `ssl.verify` | `verify(algo, key, data, sig) → bool` | Verify signature |

### Utilities

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.rand_bytes` | `rand_bytes(n) → string` | `n` cryptographically random bytes |
| `ssl.pbkdf2` | `pbkdf2(algo, password, salt, iterations, key_len) → string\|nil` | PKCS#5 PBKDF2 key derivation; returns binary |
| `ssl.hkdf` | `hkdf(algo, ikm, salt, info, length) → string\|nil` | RFC 5869 HKDF; returns binary |
| `ssl.base64_encode` | `base64_encode(data) → string` | Base64 encode (no newlines) |
| `ssl.base64_decode` | `base64_decode(str) → string\|nil` | Base64 decode |
| `ssl.hex_encode` | `hex_encode(data) → string` | Hex encode (lowercase) |
| `ssl.hex_decode` | `hex_decode(str) → string\|nil` | Hex decode |

```sea
-- TLS client
local conn = ssl.connect("example.com", 443)
ssl.write(conn, "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n")
print(ssl.read(conn))
ssl.close(conn)

-- TLS server with mTLS
local ctx = ssl.server({cert="server.pem", key="server.key", ca="ca.pem"})
ssl.serve(ctx, 8443, function(conn)
    local data = ssl.read_request(conn)
    ssl.write(conn, "HTTP/1.0 200 OK\r\n\r\nHello\n")
    ssl.close(conn)
end)

-- Self-signed cert + key
local key = ssl.keygen({type="rsa", bits=2048})
local san = []; san[] = "localhost"; san[] = "127.0.0.1"
local cert = ssl.cert_self_signed(key, {cn="localhost", san=san, days=365})
ssl.key_save(key, "server.key")
ssl.cert_save(cert, "server.pem")
ssl.cert_free(cert)
ssl.key_free(key)

-- AES-256-GCM round-trip
local k = ssl.rand_bytes(32)
local r = ssl.encrypt("aes-256-gcm", k, "secret")
local plain = ssl.decrypt("aes-256-gcm", k, r.iv, r.data, r.tag)

-- SHA-256 hash
print(ssl.hash("sha256", "hello"))

-- RSA sign / verify
local k2 = ssl.keygen({type="rsa", bits=2048})
local sig = ssl.sign("rsa-sha256", k2, "data")
print(ssl.verify("rsa-sha256", k2, "data", sig))  -- true
ssl.key_free(k2)
```

---

## `websocket`

WebSocket (RFC 6455) server and client: the server side upgrades requests
of the `http` server, the client connects to `ws://` and `wss://` URLs.
Sockets are the built-in `net` module (TLS from `libs/ssl`), so a connection
waiting for a message parks only its goroutine. Conformance: the
[Autobahn testsuite](https://github.com/crossbario/autobahn-testsuite) passes
for both the server and the client (all cases but compression: 296 OK,
3 informational, 2 non-strict, 0 failed). Build: `make -C libs/websocket`
(with `libs/http` and `libs/ssl`).

```sea
local http = require("http")
local ws = require("websocket")

http.serve(8080, function(req)
    if req.path == "/chat" then
        return ws.upgrade(req, function(conn)
            while true do
                local msg = conn:receive()
                if msg == nil or is_error(msg) then break end    -- closed, or failed
                conn:send("you said: " .. msg)
            end
        end)
    end
    return http.text("not found", 404)
end)
```

```sea
local conn = ws.connect("wss://example.com/chat", {protocols = {"chat"}})?
conn:send("hello")?
print(conn:receive(5000))
conn:close()
```

| Function | Description |
|----------|-------------|
| `ws.upgrade(req, fn [, opts]) → response` | Return it from an `http` handler: after the 101 response, `fn(conn, req)` runs in the connection's goroutine, and the connection is closed when it returns. A request that is not a valid WebSocket handshake gets a 400 (a wrong version, 426). `opts`: `protocols` (the subprotocols the server speaks; the first the client offers that is in this list is chosen, and `conn.protocol` names it), `max_message`, `close_timeout`, `write_timeout` |
| `ws.is_upgrade(req) → bool` | Whether the request asks for a WebSocket |
| `ws.connect(url [, opts]) → conn\|error` | Connect and do the handshake. `opts`: `protocols` (offered to the server), `headers` (extra request headers, e.g. `Authorization`), `timeout` (ms for connecting and the handshake; 10000), `verify` (`false` accepts any TLS certificate), `max_message`, `close_timeout`, `write_timeout` |
| `ws.accept_key(key) → string` | The `Sec-WebSocket-Accept` value for a key |

**A connection** (`conn`, from either side):

| Method / field | Description |
|----------------|-------------|
| `conn:receive([timeout_ms]) → string\|nil\|error` | The next message (fragments joined); `nil` when the connection closed (`conn.close_code`, `conn.close_reason`; 1006 if it dropped without a close frame); an error value on timeout (`"timeout"`) or failure. `conn.last_type` is `"text"` or `"binary"`. Pings are answered and pongs consumed on the way |
| `conn:send(data [, binary]) → true\|error` | A text message (`data` must be UTF-8), or binary with `binary = true` |
| `conn:send_binary(data) → true\|error` | A binary message |
| `conn:ping([data]) → true\|error` | Send a ping (at most 125 bytes) |
| `conn:close([code [, reason]])` | The closing handshake: send a close frame (1000 by default), wait up to `close_timeout` ms (1000) for the peer's, then close the socket |
| `conn.protocol`, `conn.closed`, `conn.close_code`, `conn.close_reason` | |

**Options** shared by both sides: `max_message` (bytes, default 16 MiB:
a bigger message closes the connection with 1009), `close_timeout` (ms),
`write_timeout` (ms per frame, 30000).

The protocol rules are enforced: client frames masked and server frames not,
no reserved bits or unknown opcodes (close 1002), whole control frames of at
most 125 bytes, valid close codes, and text messages (and close reasons) in
valid UTF-8, checked as fragments arrive (1007). Sends from several
goroutines are serialized per connection. Not supported: extensions such as
permessage-deflate (none is negotiated).

---

## `jwt`

Requires `jwt_native.so` (OpenSSL). Build: `make -C libs/jwt`.

```sea
local jwt = require("./libs/jwt/jwt")
```

Supported algorithms: `HS256`, `HS384`, `HS512` (symmetric HMAC), `RS256` (RSA).

| Function | Signature | Description |
|----------|-----------|-------------|
| `jwt.sign` | `sign(claims, key [, opts]) → string\|nil` | Create a signed JWT token |
| `jwt.verify` | `verify(token, key [, opts]) → claims, err` | Verify and decode a token |
| `jwt.decode` | `decode(token) → header, payload` | Decode without verifying signature |
| `jwt.algs` | `algs() → array` | List of supported algorithm names |

**`sign` opts:**

| Key | Default | Description |
|-----|---------|-------------|
| `alg` | `"HS256"` | Algorithm |
| `exp` | — | Expiry in seconds from now (adds `exp` claim) |
| `iss` | — | Issuer |
| `sub` | — | Subject |
| `aud` | — | Audience |
| `nbf` | — | Not-before Unix timestamp |

**`verify` opts:**

| Key | Default | Description |
|-----|---------|-------------|
| `verify_exp` | `true` | Validate `exp` claim |

`verify` returns `claims, nil` on success and `nil, "reason"` on failure.

```sea
-- HS256
local token = jwt.sign({ sub = "user123", role = "admin" }, "my-secret", {
    alg = "HS256",
    exp = 3600,
})

local claims, err = jwt.verify(token, "my-secret")
if claims then
    print(claims.sub)   -- "user123"
else
    print("invalid:", err)
end

-- Inspect without verifying
local header, payload = jwt.decode(token)
print(header.alg)   -- "HS256"
```

---

## `flags`

Command-line flags in the style of Go's `flag` package, with aliases,
repeatable flags, allowed values and independent sets. Pure Sea, built into
the `sea` binary as `require("sea/flags")` (the file is
`libs/flags/flags.sea`; `require("flags")` still works). The
`sea` tools (`sea fmt`, `sea test`, ...) parse their arguments with it.

```sea
local flags = require("sea/flags")

local host    = flags.string("host", "localhost", "server address")
local port    = flags.int("port", 8080, "server port", {alias = "p"})
local verbose = flags.bool("v", false, "verbose output", {alias = "verbose"})
local tags    = flags.list("tag", "a tag (repeat it)", {alias = "t"})
local format  = flags.string("format", "text", "output format", {choices = {"text", "json"}})

local files = flags.parse()          -- the positional arguments, an array
print(host.value, port.value, verbose.value, len(tags.value), files[0])
```

**Syntax:** `-flag value`, `-flag=value`, `--flag value`, `--flag=value`
(one or two dashes, always); `-bool` sets a bool flag, `-bool=false` clears
it; `--` ends the flags; `-` alone is a positional argument. Flags and
positional arguments may be mixed (`tool file -v`). `-h` / `--help` prints
the usage on stdout and exits 0 (unless the set defines `h`). A bad flag
(unknown, missing or invalid value, a value not in `choices`) prints
`name: message` and the usage on stderr and exits 2.

**Defining flags** — each returns a flag whose `.value` holds the parsed
value (and `.set` whether it was given):

| Function | Description |
|----------|-------------|
| `string(name, default, usage [, opts])` | A string |
| `int(name, default, usage [, opts])` | An integer (`1.5` is an error) |
| `number(name, default, usage [, opts])` | A number |
| `bool(name, default, usage [, opts])` | A bool |
| `list(name, usage [, opts])` | Repeatable: `.value` is an array of every value given (`-i a -i b`) |

`opts`: `alias` (another name, or a list of names: `{alias = "n"}`), `arg`
(the value's name in the usage: `{arg = "file"}` shows `-o FILE`), `choices`
(the values allowed, for strings and lists).

**Parsing and results:** `parse([args]) → array` (the positional arguments;
`args` defaults to `os.args` from 1; parsing again starts from the
defaults), `get(name)` (by name or alias), `is_set(name)`, `args()` (the
positional arguments of the last parse, an array), `narg()`, `nflag()`.

**Usage:** `usage([to_stderr])` prints it, `help()` returns it,
`set_usage(fn)` replaces it, `reset()` forgets every flag.

```
usage: tool [flags] [arguments...]

flags:
  -p, --port INT    server port (default 8080)
  -v, --verbose     verbose output
  --tag, -t VALUE   a tag (repeat it)
```

**Sets of their own:** `flags.new([name [, opts]])` returns a set with the
same functions as methods (`set:string(...)`, `set:parse()`, ...), so a
program can have several, and its messages and usage carry its name:

```sea
local cli = flags.new("sea fmt", {usage = "[-l] [path ...]", about = "Formats Sea source."})
local list = cli:bool("l", false, "list the files whose formatting differs")
local paths = cli:parse()
```

`opts`: `usage` (what follows the name on the usage line), `about` (text
under it), `interspersed` (default `true`; `false` ends the flags at the
first positional argument, for subcommands), `exit_on_error` (default
`true`; `false` makes `parse` return an error value instead of exiting),
`long` (`"--"` (default) or `"-"`: how the usage writes names longer than
one letter).

---

## `test`

A test framework in pure Sea: groups, hooks, assertions with readable
differences, skipping, benchmarks, a pretty or TAP report, and an exit status
for CI. It is built into the `sea` binary as `require("sea/test")` (the same
file as `libs/test/test.sea`), and `sea test` runs the `*_test.sea` files of
a project with it (see "Command Line" in `reference.md`).

```sea
local t = require("sea/test")

t.describe("parser", function()
    local p = nil
    t.before_each(function() p = new_parser() end)
    t.it("reads numbers", function()
        t.eq(p:parse("42"), 42)
    end)
    t.it("rejects junk", function()
        t.is_error(p:parse("4x2"), "unexpected")
    end)
end)

t.main()   -- run, print the report, exit 1 if anything failed
           -- (optional under `sea test`, which runs what the file defined)
```

```
parser
  ✓ reads numbers
  ✗ rejects junk

1) parser > rejects junk
   expected an error value, got 4

1 passed, 1 failed (0.01 s)
```

**Defining tests:**

| Function | Description |
|----------|-------------|
| `t.describe(name, fn)` | A group (groups nest; full names are `group > sub > test`) |
| `t.it(name, fn)` | A test |
| `t.skip(name, fn)`, `t.describe_skip(name, fn)` | Defined but not run (reported as skipped); a skipped group runs none of its hooks. `local group = os.getenv("DB_URL") and t.describe or t.describe_skip` skips tests that need a server |
| `t.only(name, fn)` | When any test uses `only`, only those run |
| `t.todo(name)` | A placeholder, reported as todo |
| `t.before_each(fn)`, `t.after_each(fn)` | Around every test of the group and its subgroups (outer `before_each` first, inner `after_each` first) |
| `t.before_all(fn)`, `t.after_all(fn)` | Once around the group's tests (a failing `before_all` fails them) |
| `t.bench(name, fn)` | A benchmark: `fn(b)` runs the code `b.n` times; `b.n` grows until a run takes `benchtime`; `b:reset()` restarts the clock after setup. Run only when asked (`bench` option) |

```sea
t.bench("join", function(b)
    local parts = make_parts(1000)
    b:reset()
    for i = 1, b.n do string.join(parts, ",") end
end)
```

prints `join   20000   51234.5 ns/op` (name, `b.n`, time per run).

**Assertions** (a failing one stops its test; the others go on). Each takes
an optional last `msg` that prefixes the failure:

| Assertion | Passes when |
|-----------|-------------|
| `t.eq(got, want)` | Deep equality: lists element by element, tables, maps and structs by keys and values, `1 == 1.0`; the failure shows where they differ (`at items[2].name`) |
| `t.ne(got, other)` | Not deeply equal |
| `t.ok(v)`, `t.no(v)` | `v` is truthy / `nil` or `false` |
| `t.is_nil(v)`, `t.not_nil(v)` | |
| `t.near(got, want [, eps])` | Within `eps` (default 1e-9 relative) |
| `t.gt`, `t.ge`, `t.lt`, `t.le(got, bound)` | The comparison holds |
| `t.contains(hay, needle)` | A substring, a list element or a record value (deep equality) |
| `t.matches(s, pattern)` | `s` matches a regular expression (module `re`) |
| `t.is_error(v [, text])` | `v` is an error value, its message containing `text` |
| `t.no_error(v)` | `v` is not an error value (returns `v`) |
| `t.raises(fn [, text])` | `fn` panics (or hits a runtime error), the message containing `text`; returns the message |
| `t.fail([msg])` | Never |

A runtime error or a panic inside a test (or a hook) is reported as an
*error*, with its message (`file.sea:12: attempt to index a nil value`).

**Running:** `t.run([opts]) → result` prints the report and returns
`{passed, failed, skipped, todo, total, ok, failures}` (`failures`: `{name,
kind, message}`); `t.main([opts])` runs and exits with status 1 when
something failed. Options: `filter` (run the tests whose full name contains
it; default: the environment variable `SEA_TEST`), `format` (`"pretty"`,
`"tap"`, `"quiet"`), `timing` (show times; default `true`), `bench` (also
run the benchmarks whose full name contains it, `"."` for all; default
`SEA_BENCH`), `benchtime` (seconds per benchmark; default `SEA_BENCHTIME` or
1). The result also has `benches`: `{name, n, ns_per_op}`. Also `t.equal(a,
b)` (the deep equality as a bool), `t.show(v)` (a value as readable text),
`t.has_run()` (whether `run`/`main` ran) and `t.reset()` (forget the tests
defined so far).

---

## `stream`

Buffered readers, writers and scanners, like Go's `bufio`: read lines,
read up to a delimiter, exactly n bytes, peek; batch small writes; split an
input into lines, words or your own tokens. Pure Sea, built into the `sea`
binary as `require("sea/stream")` (the file is `libs/stream/stream.sea`).

```sea
local stream = require("sea/stream")

local fd = net.connect("example.com", 80, 5000)
local w = stream.writer(fd)
w:write("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n")
w:flush()
local r = stream.reader(fd, {timeout = 5000})
print(r:read_line())                       -- HTTP/1.0 200 OK
local head = r:read_until("\r\n\r\n")
local body = r:read_all()

local sc = stream.scanner(stream.open("access.log")?)
while sc:scan() do
    if string.contains(sc:text(), " 500 ") then print(sc:text()) end
end
if sc:err() then print(sc:err().message) end
```

**Sources and destinations.** A reader reads from a *source*: a socket (an
int fd from `net`), a process from `process.spawn` (its stdout), a file
(`stream.open`), standard input (`stream.stdin()`), a string
(`stream.string(s)`), a TLS connection (`ssl.stream(conn)`), or any table
with `read(self, max, timeout)` returning a string, `""` at the end, or an
error value. A writer writes to a *destination*: a socket, a process (its
stdin), a file, a function `fn(data)`, or a table with
`write(self, data, timeout)`. Sockets and processes wait through the
netpoller, so in a goroutine a read parks only that goroutine.

| Function | Description |
|----------|-------------|
| `stream.reader(src [, opts]) → reader` | `opts`: `size` (buffer, default `stream.SIZE` = 64 KB), `timeout` (ms for each read of the source) |
| `stream.writer(dst [, opts]) → writer` | `opts`: `size` (bytes kept before writing to `dst`), `timeout` |
| `stream.scanner(src [, opts]) → scanner` | Over a source or a reader. `opts`: `split` (below), `max` (longest token, default 1 MB), `size`, `timeout` |
| `stream.open(path [, mode [, opts]]) → reader\|writer\|error` | `"r"` (default) a reader; `"w"`, `"a"` a writer |
| `stream.stdin([opts])`, `stream.stdout([opts])`, `stream.stderr([opts])` | Standard input as a reader; standard output / error as writers |
| `stream.string(s) → reader` | A reader over a string |
| `stream.buffer() → writer` | A writer that keeps what is written: `buf:string()` |

**Reader** (the end of the input is `nil`; failures are error values — a
timeout is the error `"timeout"` and the reader stays usable):

| Method | Description |
|--------|-------------|
| `r:read_line([max]) → string\|nil\|error` | The next line without `\n` / `\r\n` (the last one may lack it) |
| `r:read_until(delim [, max]) → string\|nil\|error` | Up to and including `delim`; at the end, what is left without it. More than `max` bytes without `delim` is an error (the data stays) |
| `r:read([max]) → string\|nil\|error` | What is buffered (up to `max`), or one read of the source when the buffer is empty |
| `r:read_full(n) → string\|nil\|error` | Exactly `n` bytes; the error `unexpected end of input` when it ends first |
| `r:peek(n) → string\|error` | The next `n` bytes (fewer at the end) without consuming them |
| `r:skip(n) → int\|error` | Discard `n` bytes; returns how many |
| `r:read_all() → string\|error` | Everything up to the end |
| `r:buffered() → int` | Bytes read from the source and not consumed yet |
| `r:close()` | Close the source (socket, file, ...) |

**Writer:** `w:write(...) → true|error` (strings as they are, other values
through `tostring`; written to the destination when `size` bytes are
waiting), `w:flush() → true|error` (after an error the data stays
buffered), `w:buffered()`, `w:close()` (flush, then close the destination).
Call `flush` when done: nothing flushes by itself at exit.

**Scanner:** `while sc:scan() do ... sc:text() ... end`, then `sc:err()`
(`nil` when the input simply ended). `split` is `"lines"` (default; without
`\n` / `\r\n`), `"words"` (separated by whitespace), `"bytes"`, `"chars"`
(UTF-8 characters), or a function `split(data, at_eof) → advance, token`:
`advance` bytes are consumed (`0` asks for more data) and a non-nil `token`
is the next result. `sc:split(mode)` changes it.

```sea
local function commas(data, at_eof)
    local at = string.find(data, ",")
    if at ~= nil then return at, string.sub(data, 1, at - 1) end
    if at_eof and data ~= "" then return len(data), data end
    return 0, nil
end
local sc = stream.scanner(stream.string("a,b,c"), {split = commas})
```

---

## `log`

Leveled, structured logging in the style of Go's `log/slog`. Pure Sea, built
into the `sea` binary as `require("sea/log")` (`libs/log/log.sea`). Lines go
to stderr by default.

```sea
local log = require("sea/log")

log.info("listening", {port = 8080})
-- 2026-10-18 14:30:05 INFO  listening port=8080
log.error("cannot open db", {path = p, err = err})   -- an error value shows its message
log.set_level("debug")
log.set_format("json")
log.info("listening", {port = 8080})
-- {"time":"2026-10-18T14:30:05.120-03:00","level":"info","msg":"listening","port":8080}

local l = log.new({format = "json", output = "app.log", fields = {service = "api"}})
local rl = l:with({request = id})    -- adds fields, shares level, format and output
rl:info("done", {status = 200})
```

| Function | Description |
|----------|-------------|
| `log.debug/info/warn/error(msg [, fields])` | Log at that level with the default logger. `fields`: a table, struct or map (`{port = 8080}`) |
| `log.fatal(msg [, fields])` | Log at the error level, then `os.exit(1)` |
| `log.set_level(level)` | `"debug"`, `"info"` (default), `"warn"`, `"error"` or `"off"`: lines below it are dropped |
| `log.set_format(format)` | `"text"` (default) or `"json"` (one object per line: `time`, `level`, `msg`, then the fields) |
| `log.set_output(out)` | A path (opened for appending), `"stderr"` (default), `"stdout"`, a handle from `io.open`, or `function(line)` |
| `log.set_time(on)` | `false`: no time stamp (under systemd or in tests) |
| `log.enabled(level) → bool` | Whether a level is shown (to skip building costly fields) |
| `log.with(fields) → logger` | A logger adding `fields` to every line |
| `log.new(opts) → logger` | A logger of its own; `opts`: `level`, `format`, `output`, `fields`, `time` |
| `log.logger` | The default logger |

A logger has the same functions as methods: `l:info(msg, fields)`,
`l:set_level("debug")`, `l:with(fields)`, `l:enabled("debug")`, and
`l:log(level, msg, fields)`. Fields come out in the order of their literal
(a map's in key order), the logger's before the call's. In text format, a
value whose text has spaces, quotes or `=` is quoted (`key="a b"`).

## `dotenv`

Reads `.env` files into the environment (with `os.setenv`). Pure Sea, built
into the `sea` binary as `require("sea/dotenv")` (`libs/dotenv/dotenv.sea`).

```sea
local dotenv = require("sea/dotenv")
dotenv.load()?                          -- ./.env when there is one
local key = os.getenv("API_KEY")
```

| Function | Description |
|----------|-------------|
| `dotenv.load([path [, opts]]) → table\|error` | Set the file's variables and return them. Without `path` it reads `./.env`, and a missing file is fine (an empty table); a given `path` must exist. `opts.override = true` replaces variables already set (by default the environment wins, so real settings beat the file); `opts.search = true` looks in the parent directories too |
| `dotenv.values([path [, opts]]) → table\|error` | The same without touching the environment |
| `dotenv.parse(text [, env]) → table\|error` | Parse a string; `env` is a table to expand `$VAR` against instead of the environment |
| `dotenv.find([name]) → path\|nil` | The nearest file named `name` (default `.env`) in the current directory or a parent |
| `dotenv.stringify(table) → string` | A `.env` text, keys sorted, values quoted when needed (it parses back to the same table) |

**Format:**
- Blank lines and `# comments` are skipped.
- `KEY=value`: spaces around `=` and the value are trimmed, and an optional `export ` before the name is ignored.
- Names are letters, digits, `_`, `.` and `-`, not starting with a digit.
- Unquoted values: a `#` after a space starts a comment.
- `'single quotes'` and `` `backquotes` ``: the text as written.
- `"double quotes"`: the escapes `\n \t \r \" \\ \$`, and the value may span lines.
- Expansion: `$VAR`, `${VAR}` and `${VAR:-default}` expand in unquoted and double-quoted values, to earlier keys of the file first, then the environment.
- Errors name the line: `dotenv: line 3: unterminated "`.

## `template`

Mustache templates in pure Sea. Conformance: the whole official
[Mustache spec](https://github.com/mustache/spec) passes, its optional
parts included (194 of 194: interpolation, sections, inverted sections,
comments, partials, set delimiters, lambdas, dynamic names, inheritance).

```sea
local template = require("template")

print(template.render("Hello, {{name}}!", {name = "Sea"}))     -- Hello, Sea!

local page = template.compile(fs.read("page.mustache")?)?
local html = page:render(data, template.loader("templates"))?
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `template.compile` | `compile(src [, opts]) → template\|error` | Parse once; `tpl:render(data [, partials]) → string\|error` renders it (as often as needed), `tpl.source` is the text |
| `template.render` | `render(src, data [, partials [, opts]]) → string\|error` | Compile and render in one call |
| `template.loader` | `loader(dir [, ext]) → function` | Partials from files: `{{>header}}` reads `dir/header.mustache` (cached; names with `..` or a leading `/` are refused) |
| `template.escape` | `escape(s) → string` | The HTML escaping `{{x}}` applies (`& < > " '`) |

`opts`: `escape` (a function `s → s` used for `{{x}}`, or `false` for none;
default HTML), `delimiters` (`{"<%", "%>"}` to start with other tags),
`max_depth` (nested partials, default 100: deeper is an error, which catches
a partial that includes itself). A syntax error is an error value with the
line: `template: unclosed section 'items' at line 4`.

**Tags:**

| Tag | Meaning |
|-----|---------|
| `{{name}}` | The value, HTML-escaped (`nil` is empty; numbers, bools and bigints as `tostring` shows them) |
| `{{{name}}}`, `{{& name}}` | Not escaped |
| `{{a.b.c}}` | A dotted name: `a` is looked up through the context stack, then `b` and `c` inside it |
| `{{.}}` | The current item |
| `{{#name}}...{{/name}}` | A section: repeated for each item of a list (an array or a table `{a, b}`), rendered once with the value pushed on the context stack for any other truthy value; skipped for `nil`, `false` and empty lists |
| `{{^name}}...{{/name}}` | Rendered only when the value is `nil`, `false` or an empty list |
| `{{! comment }}` | Left out |
| `{{> name}}` | A partial: another template from `partials`, rendered in the current context (a standalone partial tag indents every line of the partial); `{{>*name}}` takes the partial's name from the value `name` |
| `{{=<% %>=}}` | Change the delimiters from here on |
| `{{< parent}}...{{/parent}}` | Inheritance: render the partial `parent` with its `{{$block}}`s replaced by the ones given inside this tag |
| `{{$ block}}default{{/block}}` | A block a child template may replace |

Tags alone on their line (sections, comments, partials, delimiters,
blocks) take the whole line with them, so templates can be laid out
naturally. `partials` is a table (or map, or struct) from names to template
sources (or compiled templates), or a function `name → source`; a missing
partial renders as nothing. Values are looked up in tables, maps and structs.

**Lambdas:** a function in the data is called when its tag is rendered:
`{{name}}` calls `f()` and renders the string it returns as a template (then
escapes it); `{{#name}}text{{/name}}` calls `f(text)` with the section's
unrendered text and renders the result in the current context.

```sea
local data = {}
data.bold = function(text) return "<b>" .. text .. "</b>" end
data.who = "world"
print(template.render("{{#bold}}hi {{who}}{{/bold}}", data))   -- <b>hi world</b>
```

---

## `image`

Grayscale images in pure Sea: PNG, BMP and PGM/PPM/PBM read and written,
JPEG read, ASCII art in and out. Used by the OCR and QR code libraries. PNG goes
through `libs/compress` (build it with `make -C libs/compress`); the other
formats need nothing.

```sea
local image = require("image")

local img = image.load("scan.png")?             -- PNG, BMP, PNM or JPEG, by content
local photo = image.load("foto.jpg", {max_side = 2000})?   -- a big JPEG decoded smaller
print(img.width, img.height, img:get(10, 20))   -- 0 (black) .. 255 (white)
img:set(0, 0, 0)
img:save("copy.bmp")?                           -- the format by extension
print(image.from_ascii("#.#\n.#.", {scale = 4}):to_ascii({threshold = 128}))
```

An image is a table `{width, height, data, binary}`: `data` is an
`int_array` of `width * height` gray levels, row by row
(`data[y * width + x]`, 0 black, 255 white); `binary` marks an image of only
black and white. Color files are read as their luminance (an alpha channel
is composed over white); 1-bit and 8-bit PNG (gray, RGB, palette, with
alpha), 1/8/24/32-bit BMP and P1–P6 PNM are read.

JPEG (`jpeg.sea`): baseline, extended sequential and progressive, 8-bit,
Huffman coded, any chroma subsampling, restart intervals; only the
luminance is decoded (the color components are read and dropped), and the
EXIF orientation of a phone's photo is applied (the turns: 3, 6, 8).
Arithmetic coding, lossless and 12-bit JPEG are refused. The inverse DCT
is libjpeg's accurate integer one. A big photo decodes at about 2.5 s per
MB of file in the interpreter (1.6 s with `--jit`), a small one (a
business card, 26 KB) in 0.05 s; `scale` (2, 4 or 8, or `max_side`
choosing it) decodes straight at that size and skips most of the inverse
DCT, though the coded data is still read in full.

| Function | Signature | Description |
|----------|-----------|-------------|
| `image.new` | `new(width, height [, fill]) → image` | A new image (`fill` default 255, white) |
| `image.load` | `load(path [, opts]) → image\|error` | Read a file (the format is detected from its bytes; `opts` go to `decode_jpeg`) |
| `image.decode` | `decode(bytes [, opts]) → image\|error` | The same, from a string |
| `image.decode_png`, `decode_bmp`, `decode_pnm` | `decode_png(bytes) → image\|error` | One format |
| `image.decode_jpeg` | `decode_jpeg(bytes [, opts]) → image\|error` | A JPEG's luminance. `opts`: `scale` (1, 2, 4, 8: that many times smaller), `max_side` (the smallest scale whose longest side fits), `orient` (`false` ignores the EXIF orientation) |
| `image.encode_png`, `encode_bmp`, `encode_pgm`, `encode_pbm` | `encode_png(img) → string` | Encode (PBM: pixels below 128 are black) |
| `image.save_png`, `save_bmp`, `save_pgm`, `save_pbm` | `save_png(img, path) → true\|error` | Encode and write |
| `image.from_ascii` | `from_ascii(art [, opts]) → image` | One pixel per character: `.` ` ` `0` white; `#` `@` `█` `1` `*` black; `%` `▓` dark, `+` `=` `▒` mid, `-` `:` `░` light gray; anything else black. `opts`: `scale` (pixels per character, default 1), `pad` (white border), `binary` (every character but `.` ` ` `0` black). Empty first and last lines are dropped |

| Method | Signature | Description |
|--------|-----------|-------------|
| `img:get` | `get(x, y) → int` | The gray level (255 outside the image) |
| `img:set` | `set(x, y, v)` | Set it (clamped to 0..255; outside the image: nothing) |
| `img:is_dark`, `img:is_ink` | `is_dark(x, y [, threshold]) → bool` | Darker than `threshold` (default 128) |
| `img:clone` | `clone() → image` | A copy |
| `img:to_ascii` | `to_ascii([opts]) → string` | One character per pixel: by gray level through `opts.ramp` (default `"@%#*+=-:. "`), or with any of `threshold` (default 128), `dark` (`"#"`), `light` (`"."`) as two characters |
| `img:save` | `save(path) → true\|error` | By extension: `.png`, `.bmp`, `.pgm`/`.pnm`, `.pbm` (anything else: PNG) |

---

## `truetype`

TrueType fonts in pure Sea: reads the outlines of a `.ttf` (tables head,
maxp, hhea, hmtx, loca, glyf and cmap formats 4 and 12; simple and compound
glyphs) and draws text anti-aliased (quadratic curves flattened, non-zero
winding fill, 4×4 samples a pixel) into `libs/image` grayscale images. No
hinting or kerning; CFF outlines (`.otf` with PostScript curves) are not
read. The OCR library renders its training glyphs with it.

```sea
local tt = require("truetype")

local font = tt.load("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")?
local img = font:render("Ação, 12!", 32, {pad = 4})  -- one line of text
img:save("text.png")?
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `truetype.load` | `load(path) → font\|error` | Read a font file |
| `truetype.parse` | `parse(bytes) → font\|error` | The same, from a string |

| Method | Signature | Description |
|--------|-----------|-------------|
| `font:glyph_index` | `glyph_index(codepoint) → int` | The glyph of a character (0: missing) |
| `font:advance` | `advance(gid) → int` | Its advance width in font units |
| `font:outline` | `outline(gid) → contours` | Its contours: arrays of `{x, y, on}` in font units, y up (`on`: on the curve, else a quadratic control point) |
| `font:scale` | `scale(size) → number` | Pixels per font unit for a size in pixels (the em) |
| `font:metrics` | `metrics(size) → {ascent, descent, line_gap, height}` | Line metrics in whole pixels (`height` = ascent + descent) |
| `font:text_width` | `text_width(text, size) → number` | The advance of a UTF-8 string in pixels |
| `font:draw` | `draw(img, text, x, baseline, size [, ink]) → number` | Draw a line into an image, its baseline at y = `baseline`, toward the gray `ink` (default 0); returns the x after it |
| `font:render` | `render(text, size [, opts]) → image` | A new image of one line: as tall as the font's line, the baseline at y = ascent. `opts`: `pad` (pixels left and right, default 0), `ink` (0), `background` (255) |

The font table also has `units_per_em`, `num_glyphs`, `ascender`,
`descender` and `line_gap` (font units).

---

## `tensor`

Numeric kernels on `float_array`: the heavy loops of a small transformer
(matrix-vector and batched matrix products and their backward passes,
RMSNorm, causal attention, softmax, Adam), so a model written in Sea trains at C speed while
the orchestration stays in Sea. On a small transformer written in Sea, swapping
these kernels in for the same functions written in Sea gave identical results,
58× faster.

**Require:** `require("tensor")` (relative to your file)
**Build:** `make -C libs/tensor` (compiled with `-O3 -march=native`)

Every function works in place on arrays addressed as `(array, offset)`
and returns `nil` unless noted. The arrays are `float_array`s (double) or
`float32_array`s (float), all of the same kind in one call (mixing them is a
panic): float32 takes half the memory and runs the matrix products about
twice as fast. Reductions (RMSNorm, softmax, the attention's dot products)
accumulate in double for both; `save` writes 8 or 4 bytes per element. Matrices are row-major: `w[i*nout + j]` links
input `i` to output `j`. Backward functions accumulate (`+=`) into their
gradient arguments; the caller zeroes them. A range outside an array, or an
argument of the wrong type, is a runtime error (a panic).

| Function | Effect |
|----------|--------|
| `matvec(x, xo, w, nin, nout, out, oo)` | `out[oo+j] = Σ_i x[xo+i]·w[i·nout+j]` |
| `matvec_back(dout, doo, x, xo, w, dw, nin, nout, dx, dxo)` | `dw += x ⊗ dout`, `dx += w · dout` |
| `matvec_cols(x, xo, w, nin, nout, cols, ncols, out, oo)` | only the outputs listed in `cols` (an `int_array`): `out[oo+k] = Σ_i x[xo+i]·w[i·nout+cols[k]]` |
| `matvec_cols_back(dout, doo, x, xo, w, dw, nin, nout, cols, ncols, dx, dxo)` | its backward |
| `matmul(x, xo, w, T, nin, nout, out, oo)` | `matvec` for `T` rows at once (`x` and `out` hold `T` rows of `nin` and `nout`): `out[oo+t·nout+j] = Σ_i x[xo+t·nin+i]·w[i·nout+j]`. Blocked for the cache: several times faster than `T` calls of `matvec` |
| `matmul_back(dout, doo, x, xo, w, dw, T, nin, nout, dx, dxo)` | its backward: `dw += xᵀ·dout`, `dx += dout·wᵀ`; `dw` or `dx` may be `nil` to skip it |
| `rmsnorm(x, xo, y, yo, n) → r` | `y = x·r` with `r = 1/√(mean(x²) + 1e-5)` |
| `rmsnorm_back(dy, dyo, x, xo, r, n, dx, dxo)` | `dx += r·dy − x·r³·(dy·x)/n` |
| `relu(x, xo, y, yo, n)` | `y = max(x, 0)` |
| `relu_back(d, doff, x, xo, n)` | `d = 0` where `x ≤ 0` |
| `softmax(p, off, n)` | in place, numerically stable |
| `attention(q, k, v, att, y, T, C, H, B)` | causal multi-head attention over `T` positions of width `C`, `H` heads, context `B` (`T ≤ B`); `att` (`H·B·B`) keeps the probabilities for the backward |
| `attention_back(q, k, v, att, dy, dq, dk, dv, T, C, H, B)` | accumulates into `dq`, `dk`, `dv` |
| `attention_step(q, qo, k, v, y, yo, T, C, H)` | the attention of one new position (the last of `T`) against a KV cache: `q[qo..qo+C)` is its query, `k` and `v` hold the `T` rows of keys and values (row `s` at `s·C`), and `y[yo..yo+C)` gets what row `T−1` of `attention` gives — for generating text without recomputing the context (no probabilities kept, no backward) |
| `gather_cols(src, nrows, ncols, cols, coff, nc, dst [, trans])` | the columns `cols[coff .. coff+nc)` (an `int_array`) of `src` (`nrows × ncols`) into `dst`: `dst[i·nc+k]`, or with `trans` one column per row, `dst[k·nrows+i]` — an embedding lookup is `gather_cols(E, d, V, ids, off, T, x, true)` |
| `scatter_cols(dst, nrows, ncols, cols, coff, nc, src [, trans])` | the reverse, accumulating (`+=`; a column listed twice gets both) |
| `zero(a [, off, n])` | zero a range (the whole array by default) |
| `add(dst, doff, src, soff, n)` | `dst[doff+i] += src[soff+i]` |
| `copy(dst, doff, src, soff, n)` | copy a range (overlap allowed) |
| `scale(a, off, n, k)` | `a[off+i] *= k` |
| `sumsq(a [, off, n]) → number` | `Σ a[off+i]²`, accumulated in double (the whole array by default): the squared norm, for gradient clipping |
| `adam(w, g, m, v, n, lr, beta1, beta2, c1, c2, eps)` | one Adam step; `c1 = 1 − beta1^t`, `c2 = 1 − beta2^t` |
| `save(path, a [, append]) → bytes` | write the array as raw elements, doubles or floats (native byte order); an error value on failure |
| `load(path, a, offset) → next offset` | fill the whole array from the file at a byte offset; an error value if the file is short |
| `threads([n]) → n` | how many threads `matmul` and `matmul_back` use (default: one per CPU, or `SEA_TENSOR_THREADS`); with `n`, sets it (`1`: only the calling thread) |

```sea
local tensor = require("tensor")
local x = float_array()
x[] = 1.0
x[] = 2.0
local w = float_array()
for i = 1, 6 do w[] = i end
local out = float_array()
for i = 1, 3 do out[] = 0.0 end
tensor.matvec(x, 0, w, 2, 3, out, 0)      -- out = 9, 12, 15
```

Calls from several goroutines on different arrays run in parallel.
Products of at least a million multiply-adds are also split across a pool of
threads (`tensor.threads`); while one call uses the pool, calls from other
goroutines run on their own thread, so the CPUs are not oversubscribed. The
split gives the same results, bit for bit, as one thread. Test:
`tests/unit/113_tensor.sea`.

---

## `ndarray`

N-dimensional arrays in the manner of NumPy: float64, float32, complex128, int64 and bool arrays of any number
of dimensions, views that share data (slices, transposes, reshapes), element-wise operations with
broadcasting, reductions along axes, matrix products, `einsum`, linear algebra, Fourier transforms, random numbers, and
`.npy` / `.npz` / text files. The loops run in C (`ndarray_native.so`); the API around them is Sea
(`ndarray.sea`). Sea has no operator overloading, so arithmetic is written with methods or
functions: NumPy's `(a + 1) * b` is `a:add(1):mul(b)`, and `a[1, 1:]` is `a:get("1, 1:")`.

**Require:** `require("ndarray")`
**Build:** `make -C libs/ndarray` (`-O3 -march=native`; `.npz` files also need `make -C libs/compress`)
**Test:** `make -C libs/ndarray test`

```sea
local np = require("ndarray")

local a = np.arange(6):reshape(2, 3)            -- int64 [[0 1 2] [3 4 5]]
print(a:repr())                                 -- array([[0, 1, 2],
                                                --        [3, 4, 5]])
print(a.shape, a.dtype, a.ndim, a.size)         -- [2, 3]  int64  2  6
local b = a:mul(2):add(np.array([10, 20, 30]))  -- broadcasting
print(b:sum({axis = 0}):tostring())             -- [26 50 74]
print(a:get("1, 1:"):tostring())                -- [4 5]           a[1, 1:]
a:set(":, 0", 9)                                -- a[:, 0] = 9 (in place)
print(a:get(a:gt(4)):tolist())                  -- [9, 9, 5]       a[a > 4]

local x = np.linalg.solve(np.array([ [3, 1], [1, 2] ]), np.array([9, 8]))   -- [2. 3.]
local rng = np.random.default_rng(42)
local m = rng:normal(0, 1, [1000, 3])
print(m:mean({axis = 0}):repr())
np.save("m.npy", m)?                            -- numpy.load reads it, and the other way round
```

**Arrays.** An array is a Sea table holding `data` (a `float_array` for float64, an `int_array`
for int64 and bool, for float32 an `int_array` with two 4-byte floats packed in each element, for
complex128 a `float_array` with each element's real and imaginary parts side by side),
`dtype` (`"float64"`, `"float32"`, `"complex128"`, `"int64"`, `"bool"`), `shape` and `strides` (arrays of ints;
strides in elements), `offset`, `ndim` and `size`, plus its methods. Every function of the
module below is also a method: `np.sum(a, 0)` is `a:sum(0)`. Where an array is expected, a Sea
number, bool, nested array, `float_array` or `int_array` works too (converted as by `np.array`). A
result with no dimensions (`a:sum()`, `a:get(1, 2)`, `np.dot` of two vectors) is a Sea scalar.
`print(a)` shows the table: use `a:tostring()` (NumPy's `str`) or `a:repr()`.

**Dtypes.** Integer literals make int64 arrays, any float makes float64, `true`/`false` make
bool; float32 is asked for (`np.zeros(n, "float32")`, `a:astype("float32")`, `rng:random(n,
"float32")`). Operations promote bool < int64 < float32 < float64, except that int64 with float32
gives float64; a Sea number or int does not widen an array (NumPy's rule for Python scalars:
`a32:mul(2.5)` stays float32, an int64 array times 2.5 is float64). `divide` and the math functions
give float (float32 stays float32); comparisons give bool; the sum of bools is int64. float32
arithmetic, `matmul` and `einsum` run in float32 (half the memory, up to twice as fast);
reductions accumulate in float64 and round to float32 at the end; sorting, `linalg` and the
statistics compute in float64 and return float32. Elements read back into Sea as numbers
(`np.array([0.1], "float32"):get(0)` is `0.10000000149011612`, as NumPy's `tolist`); printing shows
the shortest digits that tell float32 values apart (`0.1`).

**Complex numbers.** Sea has no complex type: a complex scalar is a table `{re, im}`, made by
`np.complex(re [, im])` (`np.j` is `{re = 0, im = 1}`). Complex128 arrays come from such values
(`np.array([np.complex(1, 2), 3])`), `astype("complex128")`, `np.zeros(n, "complex128")` or an
operation with a complex operand (`a:add(np.j)`), and give them back (`z:get(0)`, `z:tolist()`,
`z:sum()`). Complex is the widest dtype: anything with complex128 is complex128. Arithmetic,
`power`, `equal` / `not_equal`, the logical functions, `sqrt`, `exp`, `log` (and `log2`, `log10`,
`log1p`, `expm1`, `exp2`), the trigonometric and hyperbolic functions and their inverses,
`reciprocal`, `square`, `sign` (`z / |z|`), `isnan` / `isinf` / `isfinite`, `sum`, `prod`, `mean`,
`var` / `std` (of `|z - mean|`, float64), `cumsum`, `cumprod`, `matmul`, `einsum`, `where`,
indexing, joining and files work on them; `absolute` and `angle` give float64; `real(a)` and
`imag(a)` (also methods) are float64 **views** that write through (`z:imag():set(0, 9)`), and
`conjugate` (`conj`) the conjugate. Complex numbers have no order: `min`, `max`, `argmax`, `sort`,
`unique`, `searchsorted`, `floor_divide`, `mod`, `maximum`... panic with `complex numbers are not
ordered` or `not defined for complex numbers`; a cast to a real dtype keeps the real part.

Integer arithmetic wraps around at 64 bits,
and integer `floor_divide` / `mod` by zero give 0 (as NumPy does, without its warning). Values
are read back into Sea as ints while they fit 48 bits, as numbers beyond.

**Errors.** Misuse is a panic with NumPy's message (`operands could not be broadcast together
with shapes (2, 3) (4,)`, `index 5 is out of bounds for axis 0 with size 3`); `recover()` catches
it. A singular matrix (`solve`, `inv`), a matrix that is not positive definite (`cholesky`) and
a damaged or unsupported file are error values.

| Area | Functions (all `np.`; the ones that take an array are also methods) |
|------|-------------|
| Creation | `array(v [, dtype])`, `asarray`, `zeros(shape [, dtype])`, `ones`, `empty`, `full(shape, value [, dtype])`, `zeros_like` / `ones_like` / `full_like` / `empty_like`, `arange([start,] stop [, step] [, dtype])`, `linspace(start, stop [, num = 50 [, endpoint]])`, `logspace`, `eye(n [, m [, k [, dtype]]])`, `identity`, `diag(v [, k])` (`linspace` takes a dtype 5th), `tri`, `tril`, `triu`, `meshgrid(x, y, ... [, {indexing}])` (one array per input, as multiple values), `from_data(float_array [, shape [, dtype]])` (no copy: the array and the `float_array` share their elements) |
| Indexing | `a:get(index...)`: ints, slice strings (`"1:3"`, `"::-1"`, `":"`), `nil` (`:`), `"..."`, `np.newaxis`, several in one string (`"1, ::2"`); one index array (ints or 1-D bools) for an axis; a bool array of the same shape (a mask). Basic indices give a **view** (or a scalar), index arrays and masks a copy. `a:set(index..., value)` assigns in place with broadcasting and returns `a`. `take(a, indices [, axis])`, `item([i...])`, `nonzero`, `argwhere`, `flatnonzero`, `count_nonzero`, `where(cond, x, y)` |
| Shape | `reshape(a, shape)` or `a:reshape(2, -1)` (a view when it can), `ravel`, `flatten`, `a:T()`, `transpose([axes])`, `swapaxes`, `moveaxis`, `squeeze([axis])`, `expand_dims`, `broadcast_to` (a view with strides 0: do not write into it), `atleast_1d`, `atleast_2d`, `as_strided(a, shape, strides [, offset])` (a view with any geometry inside the data; strides in elements), `flip([axis])`, `fliplr`, `flipud`, `roll`, `repeat`, `tile`, `diagonal([k])` (a view), `copy`, `astype(dtype)`, `ascontiguousarray`, `fill(value)`, `tolist()` |
| Joining | `concatenate(arrays [, axis = 0])`, `stack(arrays [, axis])`, `vstack`, `hstack`, `column_stack`, `dstack`, `split(a, n_or_indices [, axis])`, `array_split` (arrays of views) |
| Element-wise | `add` (`a:add`), `subtract` (`sub`), `multiply` (`mul`), `divide` (`div`), `floor_divide` (`floordiv`), `mod` (`remainder`), `power` (`pow`), `maximum`, `minimum`, `fmax`, `fmin`, `arctan2`, `hypot`, `copysign`; `equal` (`eq`), `not_equal` (`ne`), `less` (`lt`), `less_equal` (`le`), `greater` (`gt`), `greater_equal` (`ge`); `logical_and` / `or` / `xor` / `not`; `bitwise_and` / `or` / `xor`, `left_shift`, `right_shift`, `invert`; `negative` (`neg`), `absolute` (`abs`), `sign`, `square`, `sqrt`, `cbrt`, `reciprocal`, `exp`, `exp2`, `expm1`, `log`, `log2`, `log10`, `log1p`, `sin` ... `arctanh`, `floor`, `ceil`, `trunc`, `rint`, `round(a [, decimals])` (halves to even), `clip(a, lo, hi)`, `isnan`, `isinf`, `isfinite`, `signbit`, `deg2rad`, `rad2deg`, `conjugate` (`conj`), `angle`, `real`, `imag`. The binary and unary functions take an `out` array last: `np.add(a, b, a)` |
| Reductions | `sum`, `prod`, `min` (`amin`), `max` (`amax`), `mean`, `var`, `std`, `any`, `all`, `argmin`, `argmax`, `nansum`, `nanmean`, `ptp`, `median`, `average(a [, axis [, weights]])`, `quantile(a, q [, axis [, keepdims]])`, `percentile`; the axis is the 2nd argument (an int, an array of ints or nil for all) and `keepdims` the 3rd, or an options table: `a:sum({axis = 0, keepdims = true})`, `a:var({ddof = 1})`. `cumsum([axis])`, `cumprod`, `diff(a [, n [, axis]])` |
| Sorting | `sort(a [, axis = -1])` (a copy; the method `a:sort()` sorts in place), `argsort` (stable), `unique(a [, {return_index, return_inverse, return_counts}])` (the extra arrays as multiple values), `searchsorted(a, v [, "left" \| "right"])`, `bincount`, `histogram(a [, bins [, range]])` (counts, edges) |
| Products | `matmul(a, b)` (with batches broadcast; a blocked kernel, threaded above a few million operations), `dot`, `vdot`, `inner`, `outer`, `cross`, `convolve(a, v [, mode])`, `trace`; `einsum(subscripts, a, b, ...)`: `"ij,jk->ik"`, `"ii"` (trace), `"ii->i"` (diagonal), `"ij->ji"`, `"bij,bjk->bik"`, `"...ij,...jk"`, implicit output (the labels used once, in alphabetical order); operands are contracted left to right, each step a (batched) `matmul` |
| Comparing | `isclose(a, b [, rtol [, atol]])`, `allclose`, `array_equal` |
| Formatting | `array_str(a)` / `a:tostring()`, `array_repr(a)` / `a:repr()`: NumPy's layout (aligned columns, the fewest digits up to 8 decimals, scientific notation for wide ranges, `...` above 1000 elements) |
| Files | `save(path, a)` / `load(path)` (`.npy` 1.0–3.0; it reads `<f4`, `<i4`, `u1`... and big-endian and Fortran-ordered files: `<f4` as float32, other floats as float64, `<c16` and `<c8` as complex128, integers as int64), `savez(path, arrays [, compressed])` / `savez_compressed` (`arrays` an array or a table of names) and `load` of a `.npz` (a table of names), `savetxt(path, a [, {fmt, delimiter, header}])`, `loadtxt(path [, {delimiter, skiprows, comments, usecols, dtype}])` |

**`np.linalg`** (float64, 2-D; `det`, `inv`, `solve` and `eig` also complex): `det`, `inv`, `solve(a, b)` (b 1-D or 2-D),
`eig(a)` (w, v: the eigenvalues and right eigenvectors of any square matrix, unit columns with their
largest component real; Householder reduction to Hessenberg form, then complex QR steps with
Wilkinson shifts; float64 when the matrix is real and every eigenvalue is real, complex128
otherwise, as NumPy), `eigvals`, `cholesky` (lower),
`qr(a [, "reduced" | "complete" | "r"])` (q, r: Householder), `eigh(a)` (w ascending, v: a
symmetric matrix's lower triangle, by Jacobi rotations), `eigvalsh`, `svd(a [, full_matrices =
true [, compute_uv = true]])` (u, s, vh: one-sided Jacobi), `norm(x [, ord [, axis [,
keepdims]]])` (vector p-norms, `inf`, `"fro"`, `"nuc"`, matrix 1 / 2 / inf), `matrix_rank`,
`pinv`, `lstsq(a, b [, rcond])` (x, residuals, rank, s), `cond`, `matrix_power(a, n)`,
`multi_dot`. `eig` takes about 50 ms for a 100×100 matrix and 0.4 s for 200×200.

**`np.fft`**: `fft(a [, n [, axis = -1 [, norm]]])` and `ifft` (complex128; `a` cut or padded with
zeros to `n` along the axis; `norm` `"backward"` (the default: `ifft` divides by n), `"ortho"` or
`"forward"`), `fft2` / `ifft2`, `fftn(a [, s [, axes [, norm]]])` / `ifftn`, `rfft` (the n // 2 + 1
non-negative frequencies of a real signal) / `irfft(a [, n])`, `rfft2` / `irfft2`, `rfftn` /
`irfftn`, `fftfreq(n [, d])`, `rfftfreq`, `fftshift([axes])`, `ifftshift`. Any length: a radix-4
Stockham transform for powers of two, a direct DFT below 64 points, Bluestein's algorithm (through
a power of two) for the others. A transform of 2^20 points takes about 80 ms, a batch of 1024
transforms of 1024 points about 27 ms (NumPy's pocketfft is a few times faster: no SIMD here).

**`np.random`**: `default_rng([seed])` returns a generator (xoshiro256**; without a seed, the
system's entropy) with `random([size [, dtype]])`, `uniform([low, high, size [, dtype]])`,
`normal([loc, scale, size [, dtype]])`, `standard_normal([size [, dtype]])` (dtype `"float32"` for
float32), `integers(low [, high] [, size])` (high exclusive, unbiased),
`exponential`, `poisson` (PTRS above λ = 30), `choice(a [, size [, replace [, p]]])`,
`shuffle(a)` (in place, first axis), `permutation(n | a)`. `size` is nil (one Sea number), an
int or a shape. The module functions `seed`, `rand(d0, d1...)`, `randn`, `randint`, `random`,
`uniform`, `normal`, `choice`, `shuffle`, `permutation` use one shared generator.

**Speed.** On the machine it was written on: `x:mul(2):add(x)` over 1M elements in about 10 ms
(float64) / 2 ms (float32), `x:sum()` in 0.6 / 0.4 ms; a 500×500 `matmul` in about 27 / 14 ms
(threaded); about 3 µs of overhead per call on small arrays (each new array copies the method
table into itself).
Constants: `np.pi`, `np.e`, `np.inf`, `np.nan`, `np.newaxis`, `np.j`.

---

## `pdf`

Reading PDF files in pure Sea: each page's text and its images (a scanned page is an image: a JPEG
one goes to `image.decode`, and an OCR reads it). And writing them: pages with text in the standard
fonts, lines and rectangles (`pdf.new`). Streams go through `libs/compress` (build it with
`make -C libs/compress`). Tests: `bin/sea test libs/pdf`.

```sea
local pdf = require("pdf")

local doc = pdf.load("nota.pdf")?             -- or pdf.open(bytes)
print(doc.page_count, doc.info.Title)
print(doc:text())                             -- every page, "\f" between pages
for _, img in doc:page_images(0) do
    if img.format == "jpeg" then local page = require("image").decode(img.data)? end
end
```

Objects are found by scanning the file (`n g obj`), so a damaged cross-reference table or incremental
updates do not matter; object streams (PDF 1.5) are opened. Filters: FlateDecode (with PNG
predictors), ASCIIHexDecode, ASCII85Decode, LZWDecode; DCTDecode stays a JPEG file. Text comes from the
text operators (`Tj`, `TJ`, `'`, `"`) with their positions (text matrix, CTM, form XObjects): lines
by the baseline, top to bottom, and a space where the gap is wider than 0.15 of the font size (the
fonts' widths measure the runs; a wide character spacing, as groff uses for spaces, is measured glyph
by glyph). Characters go through the font's ToUnicode CMap, else its encoding (WinAnsi with
`/Differences` glyph names). An encrypted PDF is refused.

| Function | Signature | Description |
|----------|-----------|-------------|
| `pdf.open` | `open(bytes) → doc\|error` | Parse a PDF |
| `pdf.load` | `load(path) → doc\|error` | The same, from a file |
| `doc.page_count`, `doc.info` | | The number of pages; the Info dictionary's strings (`Title`, `Author`, `Producer`...) |
| `doc:text` | `text() → string` | Every page's text, separated by `"\f"` |
| `doc:page_text` | `page_text(i) → string\|error` | One page's text (`i` from 0) |
| `doc:page_images` | `page_images(i) → array\|error` | The images the page draws: `{format = "jpeg", data, width, height}` (the JPEG file) or `{format = "gray", width, height, data}` (one byte a pixel, from 8-bit gray, RGB or CMYK) |
| `doc:object`, `doc:resolve`, `doc:get`, `doc:stream_data` | | The parsed objects, for what the rest does not cover |

**Writing** (`libs/pdf/writer.sea`): text in the standard fonts, which need nothing embedded —
`helvetica`, `helvetica-bold`, `helvetica-oblique`, `helvetica-boldoblique`, `courier`, `courier-bold` — written in WinAnsi (Windows-1252: Portuguese
and the other Western European languages; other characters become `?`), measured with the fonts'
standard widths so alignment matches what a reader shows; lines, rectangles (stroked, filled or both,
which also draws barcodes); content streams compressed. Colors are `{r, g, b}` from 0 to 1 or a gray.

```sea
local doc = pdf.new({title = "DANFE", author = "Loja"})
local p = doc:page({top_down = true})           -- A4; y measured from the top
p:text(40, 50, "Nota fiscal nº 123", {font = "helvetica-bold", size = 14})
p:text(555, 50, "R$ 1.234,56", {align = "right"})
p:line(40, 60, 555, 60, {width = 1})
p:rect(40, 70, 515, 40, {width = 0.5})
fs.write("nota.pdf", doc:bytes())
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `pdf.new` | `new([info]) → document` | A new document; `info`: `{title, author, producer}` |
| `pdf.A4` | | `{width = 595.28, height = 841.89}` (points) |
| `doc:page` | `page([opts]) → page` | A new page: `{width, height}` in points (A4 by default), `top_down` (y from the top edge; a rectangle's y is its top) |
| `page:text` | `text(x, y, s [, opts]) → nil` | A line of text, `y` its baseline; `opts`: `font`, `size` (10), `align` (`left`, `center`, `right` of x), `color` |
| `page:line` | `line(x1, y1, x2, y2 [, opts]) → nil` | `opts`: `width` (0.5), `color`, `dash` |
| `page:rect` | `rect(x, y, w, h [, opts]) → nil` | Stroked by default; `fill = true` (or `fill_color`) fills it, and `stroke = true` keeps the border too; `width`, `color` |
| `doc:width` | `width(s, font, size) → number` | The width of a line in points |
| `doc:wrap` | `wrap(s, font, size, max_width) → array` | The text in lines that fit (by words; `\n` breaks a line) |
| `doc:bytes` | `bytes() → string` | The PDF file |

---

## `embed`

Text embeddings in-process, in the spirit of FastEmbed: GGUF models run by
[llama.cpp](https://github.com/ggml-org/llama.cpp) on the CPU, a model by name, downloaded once,
normalized vectors out. Build: `make -C libs/embed`. The first build runs `fetch.sh`, which
downloads the llama.cpp source (release b11119, SHA-256 checked) and builds its shared libraries
into `libs/embed/vendor/` (needs cmake and a C++ compiler; tuned for the machine's CPU). To use an
installed copy: `make -C libs/embed LLAMA_INC=/usr/local/include LLAMA_LIB=/usr/local/lib`.
Tests: `make -C libs/embed test` (they need a model: `EMBED_TEST_MODEL`, or ollama's `all-minilm`;
skipped without one).

```sea
local embed = require("embed")

local m = embed.load("embeddinggemma")?            -- downloaded the first time (~330 MB)
local docs = m:documents(["Condições de parcelamento: 30/60/90 dias", "Contas bancárias e Pix"])?
local q = m:query("prazos de pagamento")?
print(embed.similarity(q, docs[0]), embed.similarity(q, docs[1]))
m:close()
```

A model reference is one of:
- a known name (`embed.models()`): `embeddinggemma` (multilingual, 768 dimensions), `qwen3-embedding`
  (multilingual, 1024), `bge-m3` (multilingual, 1024), `all-minilm` (English, 384, small and fast).
  The copy ollama already pulled is used when llama.cpp can load it (ollama converts some models,
  such as embeddinggemma, for its own engine), else the GGUF is downloaded from Hugging Face;
- `ollama:<name>[:<tag>]`: a model ollama pulled (`$OLLAMA_MODELS`, `/usr/share/ollama/.ollama/models`,
  `~/.ollama/models`);
- `hf:<owner>/<repo>/<file>.gguf`: downloaded from Hugging Face;
- a path to a `.gguf` file.

Downloads go to `$SEA_EMBED_CACHE` (default `~/.cache/sea/embed`). In a goroutine, loading and
embedding run on a worker thread and only that goroutine waits; a model computes one call at a time.
`SEA_EMBED_LOG=1` shows llama.cpp's own log.

| Function | Signature | Description |
|----------|-----------|-------------|
| `embed.load` | `load(ref [, opts]) → model\|error` | Load a model. `opts`: `threads` (default: the cores less one; all of them lose to the machine's other processes), `context` (tokens per text; longer texts are cut: default the model's, up to 2048), `pooling` (`"mean"`, `"cls"`, `"last"`; default the model's), `batch` (texts per pass, default 8), `query_prompt` / `document_prompt` (default: the known model's), `normalize` (default `true`), `progress` (`function(bytes)`, called while the model downloads) |
| `embed.find` | `find(ref) → array` | The files of a model already on this machine (ollama's copy, the download cache, the path), downloading nothing; `[]` when it would be downloaded. A known model may list an ollama copy llama.cpp cannot load (then `load` downloads Hugging Face's) |
| `embed.models` | `models() → array` | The known models: `{name, description, hf, ollama}` |
| `embed.similarity` | `similarity(a, b) → number` | Cosine similarity of two vectors |
| `embed.version` | `version() → string` | The llama.cpp release built in (`"b11119"`) |
| `model:embed` | `embed(texts) → array\|error` | One vector (array of numbers) per text, as given; a string is one text |
| `model:documents` | `documents(texts) → array\|error` | The same with the model's document prompt (embeddinggemma: `"title: none \| text: "`) |
| `model:query` | `query(text) → vector\|error` | One text with the query prompt (embeddinggemma: `"task: search result \| query: "`) |
| `model:info` | `info() → table\|error` | `{dimension, context, pooling, description, file}` |
| `model:close` | `close()` | Free the model (after the calls still using it); later calls are errors |

---

## `duckdb`

[DuckDB](https://duckdb.org), the embedded analytical SQL database: CSV,
Parquet and JSON files, window functions, `ATTACH`, all from SQL. Build:
`make -C libs/duckdb`. The first build runs `fetch.sh`, which downloads the
official `libduckdb` release (1.5.6, SHA-256 checked) into
`libs/duckdb/vendor/`. To use an installed copy instead:
`make -C libs/duckdb DUCKDB_INC=/usr/local/include DUCKDB_LIB=/usr/local/lib`.
Tests: `make -C libs/duckdb test`.

```sea
local duckdb = require("duckdb")

local db = duckdb.open("sales.duckdb")?           -- nil or ":memory:": in memory
db:exec("CREATE TABLE IF NOT EXISTS sales (day DATE, item VARCHAR, qty INTEGER, price DECIMAL(10,2))")?
local added = db:exec("INSERT INTO sales VALUES (?, ?, ?, ?)", "2026-10-01", "tea", 3, 4.5)?
for _, r in db:query("SELECT item, sum(qty * price) AS total FROM sales GROUP BY item ORDER BY total DESC")? do
    print(r.item, r.total)
end
print(db:value("SELECT count(*) FROM read_csv('orders.csv')"))
db:close()
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `duckdb.open` | `open([path [, opts]]) → db\|error` | Open (or create) a database file; no path or `":memory:"` is in memory |
| `duckdb.version` | `version() → string` | The DuckDB library version (`"v1.5.6"`) |
| `db:exec` | `exec(sql, params...) → int\|error` | Run SQL; returns the number of rows changed. Without parameters it may hold several statements separated by `;` |
| `db:query` | `query(sql, params...) → array\|error` | All rows, each a table keyed by column name |
| `db:row` | `row(sql, params...) → table\|nil\|error` | The first row |
| `db:value` | `value(sql, params...) → value\|nil\|error` | The first column of the first row |
| `db:columns` | `columns(sql, params...) → table\|error` | Column-wise: `{names, types, count, data = {column = values}}` (see below) |
| `db:each` | `each(sql, fn, params...) → int\|error` | Call `fn(row)` per row (`false` stops); returns the rows seen |
| `db:prepare` | `prepare(sql) → stmt\|error` | A prepared statement |
| `db:appender` | `appender(table [, schema]) → app\|error` | Fast bulk insert into a table |
| `db:transaction` | `transaction(fn) → value\|error` | `BEGIN`, `fn(db)`, then `COMMIT`; `ROLLBACK` if `fn` returns an error value or panics (the panic goes on) |
| `db:connect` | `connect() → conn\|error` | Another connection to the same database, with the same methods |
| `db:interrupt` | `interrupt()` | Stop the query running on this connection (from another goroutine) |
| `db:close` | `close() → bool` | Close; on the database it closes every connection, statement and appender. Closing twice is fine |

**Options for `open`** (table, map or struct): `read_only = true`, `threads`,
`memory_limit` (`"2GB"`) and any other [DuckDB setting](https://duckdb.org/docs/configuration/overview)
by name, with the value as a string, number or bool. An unknown setting or a
bad value is an error.

**Parameters** come after the SQL (`?` or `$1`, `$2`...), or as one positional
table `{a, b}`, or as a table of named values `{name = v}` for `$name`:

```sea
db:value("SELECT name FROM people WHERE id = ? AND age > ?", 7, 18)
db:query("SELECT * FROM people WHERE id = ? AND age > ?", {7, 18})
db:row("SELECT * FROM people WHERE age > $min AND city = $city", {min = 18, city = "Recife"})
```

`nil` is NULL, bools, ints, numbers and strings map to the obvious types
(DuckDB casts them to the parameter's type, so a string works for a `DATE`);
a `bigint` goes as `BIGINT`, `HUGEINT` or `BIGNUM`, the narrowest that holds it.
Arrays, `int_array`, `float_array` and positional tables become LISTs,
tables with string keys STRUCTs and maps `[=]` MAPs. To pass one list as the
only parameter, wrap it: `db:value("SELECT list_sum(?)", {values})`.

**Result values:**

| DuckDB | Sea |
|--------|-----|
| `NULL` | `nil` (row tables keep the key) |
| `BOOLEAN` | bool |
| integers (`TINYINT` … `UBIGINT`, `HUGEINT`, `UHUGEINT`) | exact: an int within Sea's ±2^47, a number up to ±2^53, a `bigint` beyond |
| `BIGNUM` | `bigint` |
| `FLOAT`, `DOUBLE`, `DECIMAL` | number |
| `VARCHAR`, `BLOB`, `ENUM`, `UUID`, `BIT` | string (a BLOB holds its bytes) |
| `DATE`, `TIME`, `TIMESTAMP` (all precisions) | ISO string: `"2024-02-29"`, `"13:45:07.25"`, `"2024-01-02 03:04:05.123456"`; `TIMESTAMPTZ` in UTC with `+00`; `infinity`; BC dates end in ` (BC)` |
| `INTERVAL` | `{months, days, micros}` |
| `LIST`, `ARRAY` | array |
| `STRUCT` | table |
| `MAP` | map `[=]` |
| `UNION` | the value of its member |
| `GEOMETRY` | a GeoJSON-shaped table: `{type = "Point", coordinates = [1, 2]}`, `LineString`, `Polygon`, the `Multi` kinds (Z and M ordinates included), `{type = "GeometryCollection", geometries = [...]}`; cast to `VARCHAR` for WKT or `BLOB` for WKB |
| `VARIANT` | the value it holds, converted as above (objects → tables, arrays → arrays; a MAP inside one arrives as an array of `{key, value}`) |

Dates and times come as strings because they are exact and go back into SQL
as parameters unchanged; `time.unix` builds a Time from `epoch(ts)` when you
need one. VARIANT values are read from DuckDB's internal layout, which the C
API does not describe; the library's test covers every type, so a DuckDB
version that changed it would fail there.

**`columns`** returns the result column by column: integer columns without
NULLs as `int_array`, floating and decimal ones as `float_array` (ready for
`libs/tensor`), the rest as arrays; `types` holds the SQL type of each column
(`"DECIMAL(10,2)"`, `"VARCHAR[]"`, `"STRUCT(a INTEGER)"`).

```sea
local c = db:columns("SELECT x, y FROM points")?
print(c.count, c.types[0], type(c.data.x))      -- 1000  DOUBLE  float_array
```

**Prepared statements** keep the parsed plan: `stmt:query(params...)`,
`stmt:exec`, `stmt:row`, `stmt:value`, `stmt:columns`, `stmt:each(fn, params...)`
and `stmt:close()`. `stmt.params` lists the parameter names (`"1"`, `"2"` for
positional ones). A wrong number of parameters is an error.

**Appenders** insert rows faster than `INSERT`: `app:append(row)` takes a
sequence in column order (`{1, "ana", "2026-10-01"}`) or a table by column
name (absent columns get their `DEFAULT`); `app:append_rows(rows)` takes many;
`app:flush()` writes what is buffered; `app:close()` flushes and frees.
Values are cast to the column types (strings to dates, ints to decimals...).
After an error, close the appender.

**Goroutines.** One connection runs one query at a time (others on it wait
their turn). In a goroutine a query runs on a worker thread (up to 8) and only
that goroutine waits, so the scheduler keeps running the others; on the main
thread the query runs in place. For queries in parallel, give each goroutine
its own `db:connect()`:

```sea
local results = chan(4)
for i = 1, 4 do
    go function()
        local c = db:connect()
        results <- c:value("SELECT count(*) FROM read_parquet(?)", "part" .. i .. ".parquet")
        c:close()
    end ()
end
```

All failures are error values whose message is DuckDB's
(`duckdb.query: Catalog Error: Table with name nope does not exist!`),
including a closed handle (`the connection or statement is closed`).

---

## `sqlite`

SQLite, the embedded transactional SQL database, compiled into the module
(nothing to install). `make -C libs/sqlite` downloads the SQLite 3.53.4
amalgamation into `libs/sqlite/vendor/` (`fetch.sh`, pinned SHA-256) and
builds `sqlite_native.so`; `make -C libs/sqlite test` runs its tests. Built
with FTS5, R*Tree, JSON and math functions; extension loading is off.

```sea
local sqlite = require("sqlite")

local db = sqlite.open("app.db", {wal = true})?
db:exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INT)")?
db:exec("INSERT INTO users (name, age) VALUES (?, ?)", "ana", 31)?
print(db:last_insert_id())
for _, u in db:query("SELECT * FROM users WHERE age >= :min", {min = 18})? do print(u.id, u.name) end
db:close()
```

| Function | Description |
|----------|-------------|
| `sqlite.open([path [, opts]]) → db\|error` | Open (or create) a database file; `nil` or `":memory:"` for an in-memory one. `opts`: `read_only`, `create` (default `true`), `timeout` (ms to wait for another connection's lock; 5000), `wal` (`PRAGMA journal_mode = WAL`: readers and a writer at once), `foreign_keys` (default `true`) |
| `sqlite.version() → string` | `"3.53.4"` |
| `sqlite.blob(bytes)` | A value that binds as a BLOB (strings bind as TEXT) |
| `db:exec(sql, params...) → int\|error` | Run a statement; the rows changed. Without parameters the SQL may hold several statements separated by `;` |
| `db:query(sql, params...) → array\|error` | The rows, tables keyed by column name |
| `db:row(sql, params...) → row\|nil\|error` | The first row |
| `db:value(sql, params...) → value\|nil\|error` | The first column of the first row |
| `db:each(sql, fn, params...) → int\|error` | Call `fn(row)` for each row as it is read (memory stays flat; `false` from `fn` stops); `fn` may use the same database |
| `db:prepare(sql) → stmt\|error` | A prepared statement: `stmt:query/exec/row/value(params...)`, `stmt:each(fn, params...)`, `stmt:close()`, `stmt.params` (parameter names, `"1"`, `"2"`... for `?`), `stmt.columns` |
| `db:transaction(fn) → value\|error` | `BEGIN`, `fn(db)`, then `COMMIT`; rolls back when `fn` returns an error value or panics (the panic goes on) |
| `db:last_insert_id() → int` | The rowid of the last `INSERT` on this connection |
| `db:backup(path) → true\|error` | A consistent copy of the database into a file (online backup) |
| `db:connect() → db\|error` | Another connection to the same file (for goroutines writing in parallel; open with `wal = true`) |
| `db:interrupt()` | Stop the statement running on the connection (from another goroutine) |
| `db:close()` | Close the connection and its statements; later calls are error values |

**Parameters**: values after the SQL for `?` / `?NNN`, one positional table
`{a, b}`, or a table of named values `{name = v}` for `:name`, `@name` or
`$name`. Sea values bind as: `nil` → NULL, `bool` → 0/1, `int` → INTEGER,
`number` → REAL, `string` → TEXT, `sqlite.blob(s)` → BLOB, `bigint` →
INTEGER (TEXT beyond 64 bits).

**Results**: INTEGER → `int` within ±2^47, `number` while exact (±2^53),
`bigint` beyond; REAL → `number`; TEXT → `string`; BLOB → `string` (bytes);
NULL → `nil`.

**Goroutines**: in a goroutine, `exec`/`query`/`row`/`value` run on a worker
thread (at most 8) and only that goroutine waits; on the main thread they run
in place. One connection runs one statement at a time; use `db:connect()` for
parallel work. Failures are error values: `sqlite.exec: UNIQUE constraint
failed: users.email`, `sqlite.query: expected 2 parameters, got 1`.

---

## `cockroach`

CockroachDB client in pure Sea: the PostgreSQL wire protocol (v3) over the
built-in `net` module, so it also talks to PostgreSQL. Nothing to build;
`libs/ssl` (`make -C libs/ssl`) is needed only for TLS. Every network wait
parks only the goroutine that waits.

```sea
local crdb = require("cockroach")

local db = crdb.open("postgresql://root@localhost:26257/defaultdb?sslmode=disable")?
db:exec("CREATE TABLE IF NOT EXISTS accounts (id INT PRIMARY KEY, owner STRING, balance DECIMAL)")?
db:upsert("accounts", [{id = 1, owner = "ana", balance = 100}, {id = 2, owner = "bo", balance = 50}])?
for _, a in db:query("SELECT * FROM accounts WHERE balance > $1", 10)? do print(a.owner, a.balance) end
db:transaction(function(tx)               -- retried on serialization conflicts (40001)
    local _ = tx:exec("UPDATE accounts SET balance = balance - $1 WHERE id = $2", 30, 1)?
    return tx:exec("UPDATE accounts SET balance = balance + $1 WHERE id = $2", 30, 2)
end)?
db:close()
```

| Function | Description |
|----------|-------------|
| `crdb.open([url] [, opts]) → db\|error` | A pool of connections (one is opened right away, so a bad address or password fails here). `url`: `postgresql://user:password@host:26257/db?sslmode=…&sslrootcert=…` (also `postgres://`, `cockroachdb://`); its query takes `sslmode`, `sslrootcert`, `sslcert`, `sslkey`, `application_name`, `connect_timeout` (s), `options`, and any other key as a session setting. `opts` override the URL (see below) |
| `crdb.connect([url] [, opts]) → conn\|error` | One connection without a pool: the same `run/query/exec/row/value/each`, `close()`, and `conn.status` (`"I"` idle, `"T"` in a transaction, `"E"` in a failed one) |
| `db:query(sql, params...) → array\|error` | The rows, tables keyed by column name (NULL columns are absent). Parameters are `$1`, `$2`, ... |
| `db:exec(sql, params...) → int\|error` | Rows affected. Without parameters the SQL may hold several statements separated by `;` |
| `db:row(sql, params...)`, `db:value(sql, params...)` | The first row / its first column, or `nil` |
| `db:each(sql, fn, params...) → int\|error` | `fn(row)` as the rows arrive; `false` ignores the rest |
| `db:run(sql, params, mode [, fn])` | The same with the parameters in an array; `mode` is `"rows"`, `"exec"`, `"row"`, `"value"` or `"each"` |
| `db:insert(table, rows [, opts]) → int\|rows\|error` | A multi-row `INSERT` (columns from the first row's keys, sent in batches). `opts`: `upsert` (`UPSERT`: insert or replace by primary key), `on_conflict` (`"DO NOTHING"`, `"(id) DO UPDATE SET …"`), `returning` (`"*"` or columns: the rows come back) |
| `db:upsert(table, rows [, opts])` | `db:insert` with `upsert = true` |
| `db:transaction(fn [, opts]) → value\|error` | `fn(tx)` in a transaction: commits when it returns, rolls back when it returns an error value or panics (the panic goes on). On SQLSTATE 40001 (CockroachDB's "restart transaction", from a statement or the commit) `fn` runs again in a new transaction, with a growing pause, up to `opts.max_retries` (10) times, so `fn` must have no effects outside the database. Also takes `db:begin`'s options |
| `db:begin([opts]) → tx\|error` | An explicit transaction holding one connection: `tx:query/exec/row/value/each/run`, `tx:commit()`, `tx:rollback()`. `opts`: `isolation` (`"SERIALIZABLE"`, `"READ COMMITTED"`), `priority` (`"LOW"`, `"NORMAL"`, `"HIGH"`), `read_only` |
| `db:ping() → true\|error`, `db:stats() → {max_conns, idle, in_use}`, `db:close()` | |
| `db.server_version`, `db.crdb_version` | What the server reported (`"13.0.0"`, `"CockroachDB CCL v24.2.0 …"`) |
| `crdb.sqlstate(err) → string\|nil` | The SQLSTATE of a server error (`"23505"` unique violation, `"40001"`, `"42P01"` unknown table) |
| `crdb.bytes(s)`, `crdb.json(v)` | Parameter wrappers: send `s` as `BYTES`; send an array as JSON instead of an SQL array |
| `crdb.quote_ident(name)` | `"name"` with quotes doubled; a dotted name part by part |
| `crdb.parse_url(url) → config\|error` | The URL as a table of options |

**`opts`**: `host` (`localhost`), `port` (26257), `user` (`root`),
`password`, `database` (`defaultdb`), `sslmode` (`disable`, `allow`,
`prefer` (default: TLS when the server offers it), `require` (TLS, no
certificate check), `verify-ca`, `verify-full` (also the host name)),
`sslrootcert` (the cluster's CA), `sslcert` + `sslkey` (client certificate
authentication), `application_name` (`sea`), `settings` (session variables
set at connect: `{timezone = "UTC"}`), `max_conns` (8), `max_idle_time` (ms
a connection may stay idle before it is replaced; 300000),
`connect_timeout` (ms; 10000), `timeout` (ms for each network wait of a
statement; none), `exact_numeric` (`DECIMAL` as strings instead of numbers).
Authentication: trust, password, MD5 and SCRAM-SHA-256 (CockroachDB's
default), or a client certificate.

**Parameters** go to the server separately from the SQL (extended query
protocol), as text: `nil` → NULL, `bool`, `int`, `number`, `bigint`,
`string`, a Time (RFC 3339), an array → an SQL array (`= ANY($1)`), a table
or map → JSON, `crdb.bytes(s)` → BYTES.

**Results**: `INT` → `int` (a `bigint` beyond ±2^47, as `unique_rowid()`
values usually are), `FLOAT` and `DECIMAL` → `number`, `BOOL` → `bool`,
`JSONB` → decoded, `BYTES` → a string of bytes, arrays → arrays (nested
too), everything else (`STRING`, `UUID`, `DATE`, `TIMESTAMP`, `INTERVAL`,
...) → the server's text.

**Errors** are values: `cockroach: relation "nope" does not exist (SQLSTATE
42P01)`, `cockroach: connect localhost:26257: connection refused`. Network
failures close that connection; the pool opens a new one on the next call.
`db:exec("BEGIN")` on the pool makes no sense (each call may get another
connection): use `db:transaction` or `db:begin`.

Examples: `libs/cockroach/examples/basics.sea` (tables, parameters, types,
bulk insert with `returning`, `ON CONFLICT`, errors) and `bank.sea`
(concurrent transfers that collide and are retried; the total stays right).
They read `COCKROACH_URL`.

Tests: `bin/sea test libs/cockroach`; with
`COCKROACH_URL=postgresql://root@localhost:26257/defaultdb?sslmode=disable`
they also run against a server (`cockroach start-single-node --insecure`).

---

## `smtp`

Sending e-mail in pure Sea: an SMTP client over the built-in `net` module, with
TLS through `libs/ssl` (`make -C libs/ssl`). Every wait parks only the calling
goroutine (netpoller).

```sea
local smtp = require("smtp")

local r = smtp.send("smtps://ana%40x.com:secret@smtp.x.com", {
    from = "Loja <no-reply@x.com>", to = ["ana@x.com"], cc = "bia@x.com", bcc = ["arquivo@x.com"],
    reply_to = "sac@x.com", subject = "Seu pedido nº 12", text = "Olá!", html = "<p>Olá!</p>",
    attachments = [{filename = "pedido.csv", content = csv_bytes, type = "text/csv"}],
})?
print(r.id, r.accepted)      -- the Message-ID, and the recipients the server took
```

URLs: `smtps://` (TLS from the start, port 465), `smtp://` (STARTTLS, port 587;
refused when the server does not offer it), `smtp://…?tls=none` (plain text,
for a local test server). `user:password` are URL-encoded; the client logs in
with AUTH PLAIN, or AUTH LOGIN when that is all the server offers.

| Function | Description |
|----------|-------------|
| `smtp.send(url, mail [, opts]) → result\|error` | Connect, send one message, quit. `result`: `{id, accepted = [address], rejected = [{address, error}]}`; refused recipients go in `rejected`, and only all of them refused is an error |
| `smtp.dial(url [, opts]) → client\|error` | Connected, TLS on and logged in, for several messages: `client:send(mail)` (same result), `client:close()` (QUIT) |
| `smtp.message(mail) → string` | The message's bytes (RFC 5322 + MIME): headers with accents as RFC 2047 words, text and HTML as `multipart/alternative`, attachments in base64 under `multipart/mixed`; an attachment with a `cid` is an image inline in the HTML (`<img src="cid:...">`, under `multipart/related`; without HTML it goes as a file); `bcc` stays out of the headers |
| `smtp.parse_url(url) → table\|error` | `{secure, plain, host, port, user, pass}` |
| `smtp.address(s) → string` | The bare address of `"Name <a@b.com>"` |

`mail`: `from`, `to` (a string or an array), `cc`, `bcc`, `reply_to`, `subject`,
`text`, `html`, `attachments` (`[{filename, content, type}]`), `headers`
(`{name = value}`), `id` (a Message-ID; one is made otherwise). `opts`:
`timeout` (ms per operation, default 20000), `helo` (default `localhost`) and
the `ssl.connect` options (`verify`, `ca`, ...).

---

## `clickhouse`

ClickHouse client in pure Sea, over the HTTP interface (port 8123, 8443 with
TLS) through `libs/http` (`make -C libs/http libs/ssl`). Each call is one
request on kept-alive connections, so a db can be shared by goroutines.

```sea
local clickhouse = require("clickhouse")

local db = clickhouse.open("http://default:secret@localhost:8123/default")?
db:exec("CREATE TABLE IF NOT EXISTS hits (at DateTime, url String, ms UInt32) ENGINE = MergeTree ORDER BY at")?
db:insert("hits", [{at = time.now(), url = "/", ms = 12}, {at = time.now(), url = "/doc", ms = 30}])?
for _, r in db:query("SELECT url, avg(ms) AS ms FROM hits WHERE ms > {min:UInt32} GROUP BY url", {min = 5})? do
    print(r.url, r.ms)
end
db:each("SELECT * FROM hits", function(row) print(row.url) end)?   -- streamed
```

| Function | Description |
|----------|-------------|
| `clickhouse.open([url] [, opts]) → db\|error` | `url`: `http(s)://user:password@host:port/database?setting=value` (or `clickhouse://…`, `secure=true` for HTTPS, `verify=false`). Runs `SELECT 1` to check the address and credentials unless `ping = false` |
| `db:query(sql [, params [, opts]]) → array\|error` | The rows, tables keyed by column name |
| `db:row(...)`, `db:value(...)` | The first row / its first column, or `nil` |
| `db:result(sql [, params [, opts]]) → table\|error` | `{columns = [{name, type}], rows, statistics = {elapsed, rows_read, bytes_read}, rows_before_limit}` |
| `db:each(sql, fn [, params [, opts]]) → int\|error` | Stream the result (`JSONEachRow`): `fn(row)` as rows arrive, memory stays flat; `false` stops reading. An exception raised by the server midway is returned as an error |
| `db:exec(sql [, params [, opts]]) → summary\|error` | Statements without a result (`CREATE`, `ALTER`, `INSERT … SELECT`, `OPTIMIZE`). The summary holds the server's counters as ints: `written_rows`, `written_bytes`, `read_rows`, `elapsed_ns`, ..., and `query_id` |
| `db:insert(table, rows [, opts]) → summary\|error` | Rows (tables keyed by column) sent as `JSONEachRow`; missing columns get their defaults, Times are sent as text. `opts`: `columns` (name them explicitly), `batch` (rows per request, 100000), `settings`, `query_id` |
| `db:raw(sql [, params [, opts]]) → string\|error` | The body as the server wrote it: add a `FORMAT` (`CSVWithNames`, `TSV`, `Parquet`, `Pretty`, ...) |
| `db:ping() → true\|error`, `db:version() → string`, `db:close()` | |
| `clickhouse.error_code(err) → int\|nil` | The server's error number (60 unknown table, 516 authentication failed, ...) |
| `clickhouse.literal(v)`, `clickhouse.quote_ident(name)` | A value as a ClickHouse literal (`'it\'s'`, `[1,2]`, `{'k':1}`, `NULL`); `` `name` `` quoted |
| `clickhouse.parse_url(url) → config\|error` | The URL as a table of options |

**`opts` of `open`**: `host` (`localhost`), `port` (8123; 8443 with
`secure`), `secure` (HTTPS), `verify` (`false` skips the certificate check),
`user` (`default`), `password`, `database`, `settings` (sent with every
query: `{max_execution_time = 30}`), `timeout` (ms per network wait;
300000), `compress` (gzip responses; `true`), `session_id` (a server session,
for temporary tables and `SET`), `ping`. **Per call** `opts`: `settings`,
`query_id`.

**Parameters** are ClickHouse's own query parameters: `{name:Type}` in the
SQL and a table `{name = value}`. The server parses each value by its type,
so values never become SQL text: strings, numbers, `bool`, `bigint`, `nil`
(for `Nullable`), Times (`DateTime`), arrays (`Array(T)`), tables and maps
(`Map(K, V)`).

**Results** come as JSON with 64-bit integers unquoted: `UInt64`/`Int64`
beyond ±2^47 are exact `bigint`s; `Decimal` and floats → `number`; `Bool` →
`bool`; `Array`/`Tuple` → arrays; `Map` → tables; `Nullable` NULL → absent;
dates, `DateTime`, `UUID`, `Enum` → strings.

**Errors** are values with the server's text: `clickhouse: Code: 60.
DB::Exception: Unknown table expression identifier 'nope' …
(UNKNOWN_TABLE)`. Queries run with `wait_end_of_query=1`, so a failure is
reported as an error even after rows were produced.

Examples: `libs/clickhouse/examples/basics.sea` (inserts, typed parameters,
`Map`/`Array`/`Nullable`, results with column types, CSV, errors) and
`analytics.sea` (a million rows made by the server, parallel reports,
quantiles, streaming with `each`, CSV export). They read `CLICKHOUSE_URL`.

Tests: `bin/sea test libs/clickhouse`; with
`CLICKHOUSE_URL=http://default:secret@localhost:8123/default` they also run
against a server (`docker run -p 8123:8123 -e CLICKHOUSE_PASSWORD=secret
clickhouse/clickhouse-server`).

---

## `xlsx`

Excel workbooks (`.xlsx`, Office Open XML) in pure Sea, on `libs/compress`
(the ZIP) and `libs/xml`: read every sheet into rows of values, and write
workbooks with several sheets.

```sea
local xlsx = require("xlsx")

local book = xlsx.read("pedidos.xlsx")?            -- or xlsx.decode(bytes)? (an upload)
for _, sheet in book.sheets do
    print(sheet.name, len(sheet.rows))             -- rows: arrays of values, from column A
end
xlsx.write("saida.xlsx", [
    {name = "Clientes", rows = [ ["Nome", "Limite", "Desde"], ["Ana", 1500, {date = "2026-10-31"}] ]},
    {name = "Itens", rows = [ ["Código", "Preço"], ["CAN-01", 2.5] ]},
])?
local bytes = xlsx.encode(sheets)?
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `xlsx.read` | `read(path) → {sheets = [{name, rows}]}\|error` | Every sheet, in the workbook's order |
| `xlsx.decode` | `decode(bytes) → {sheets}\|error` | The same, from the file's bytes |
| `xlsx.write` | `write(path, sheets) → true\|error` | A workbook of `sheets` (`[{name, rows}]`; a name: 1 to 31 characters, none of `\ / ? * [ ] :`, not repeated) |
| `xlsx.encode` | `encode(sheets) → string\|error` | The same, as bytes |
| `xlsx.serial_to_date` | `serial_to_date(n [, date1904]) → string` | An Excel date serial as `"YYYY-MM-DD"` (with `" HH:MM:SS"` when it has a time) |
| `xlsx.date_to_serial` | `date_to_serial(s) → number\|nil` | The other way |

A row has no holes: an empty cell is `""`, and the trailing empty cells (and
empty rows at the end) are left out. Values are strings, numbers (`int`
when whole), booleans and dates as text, recognized by the cell's number
format (the built-in date formats, or a custom one with `d`, `m`, `y` or
`h` outside quotes and brackets). A formula's cached value is read; formulas
are not evaluated. Written strings are inline (no shared-strings table);
`{date = "YYYY-MM-DD[ HH:MM[:SS]]"}` writes a date cell shown as dd/mm/yyyy.

## `parser`

The Sea parser is a built-in module now: `require("sea/parser")`, with no
build step. See [reference.md](reference.md#module-seaparser).
