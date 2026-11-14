# Sea for AI assistants

This guide is for language models (Claude, Gemini, GPT, Copilot, ...) that
have to read or write **Sea**, a small language that is not in their training
data. It is self-contained: read it all before writing Sea code. It is also a
good short introduction for people.

Sea looks like **Lua** and behaves like **Go**: Lua's syntax (`local`,
`function ... end`, `then`, `do`, `--` comments), Go's runtime model
(goroutines, channels, `select`, `defer`, `panic`/`recover`, errors as
values). Many details differ from both. Code written from Lua or Go habits
alone will not compile or will misbehave: the rules in section 1 are the
ones models get wrong most often.

Every code block in sections 2 to 6 runs as written, and the output shown in
its `-- →` comments is checked by the test suite (`tests/unit/132_ai_guide`).
Blocks starting with `-- sketch` are illustrations and are not run.

Authoritative references, when you need more detail:
[`docs/reference.md`](reference.md) (language, built-ins, standard library,
command line) and [`docs/libs.md`](libs.md) (external libraries).

---

## 0. Workflow: check your code with the tools

You can (and should) verify Sea code instead of guessing:

```text
sea check file.sea        # undefined names, unused variables, wrong arg counts, bad
                          # format strings, missing modules... without running
sea fmt file.sea          # rewrite in the canonical layout (-d shows a diff, -l lists)
sea file.sea args...      # run (also: sea run file.sea, or any file starting with #!/usr/bin/env sea)
sea -e 'print(1 + 1)'     # a one-liner; `sea -` runs a program from stdin
sea test                  # run the *_test.sea files (sea/test framework)
sea doc file.sea          # the documentation in the comments
sea doc string.split      # signature and description of a built-in (or: sea doc split)
echo 'print(x)' | sea check -   # check a program from stdin (-f json: machine-readable)
```

In this repository the binary is `bin/sea` (`make` builds it). `sea check`
knows the common mistakes of programmers coming from other languages and
says what to write instead (`undefined variable 'pairs' (iterate with
'for k, v in t do')`). Run it after every edit.

Runtime errors name the value that failed and, for names from other
languages, what to use instead: `attempt to call a nil value (field 'gsub' of
global 'string'): use string.replace(s, old, new) for plain text, or
re.gsub(pattern, s, repl)`. Read the parenthesis and the hint before
changing anything else.

---

## 1. The rules models get wrong

1. **Arrays are 0-indexed.** `a[0]` is the first element, `a[-1]` the last.
   Loop over indexes with `for i = 0, len(a) - 1 do`.
2. **Arrays are `[1, 2, 3]` (or `[]` empty), and you append with `a[] = v`.**
   There is no `array()`, `push()`, `table.insert`, `append` or `#`.
   **Nested arrays need a space after the first bracket**: `[ [1, 2], [3] ]`,
   because `[[` starts a long string (as in Lua). `sea fmt` writes it so.
3. **`{1, 2, 3}` is a positional *table*** (keys 0, 1, 2), not an array.
   It works with `len`, indexing and `for-in`, but use `[...]` for lists.
4. **`len(x)`, not `#x`.** `#` is not an operator. `len(nil)` is 0.
5. **`for k, v in t do`** iterates arrays (index, value), tables and maps
   (key, value), structs (field, value) and channels (value). There is no
   `pairs`, `ipairs`, `next` or `range`.
6. **One statement per line.** `local a = 1 local b = 2` and
   `x = 1 y = 2` are syntax errors. Separate statements on one line with
   `;`. A block whose body is a single statement may stay on one line:
   `if x then return end`, `function(a) return a * 2 end`.
7. **`{x = 1, y = 2}` is a sealed struct.** You can read and change `x`
   and `y`, but adding a field later (`p.z = 3`, or `function p:m()`) is a
   runtime error. To build an object field by field, start from an empty
   table: `local o = {}` then `o.x = 1`, `function o:m() ... end`. Or put
   every field, methods included, in the literal.
8. **Strings are 1-indexed** in `string.sub`, `string.find`, `string.byte`
   (as in Lua), while arrays and tables are 0-indexed. `s[1]` on a string
   is `nil`: use `string.sub(s, i, i)`.
9. **Strings are bytes.** `len("é")` is 2. The `utf8` module counts
   characters: `utf8.len`, `utf8.sub`, `utf8.upper`.
10. **`string.find` and `string.replace` are plain text, not patterns.**
    Regular expressions are the `re` module (RE2 syntax: `re.match`,
    `re.gsub`, `re.test`). There are no Lua patterns (`%d`, `gmatch`).
11. **Errors are values.** Functions return `error("msg")` instead of
    throwing; callers test `is_error(v)` and read `v.message`, or propagate
    with the postfix `?` operator: `local x = f()?` returns the error from
    the current function if `f()` returned one; `fs.write(p, s)?` alone on a
    line does the same and drops the value. There is no `pcall`,
    `try/catch` or `throw`.
12. **`panic`/`recover` work like Go.** `recover()` only works inside a
    deferred function. Runtime errors (indexing `nil`, calling `nil`,
    adding a field to a struct) are panics too.
13. **`int` and `number` are different types.** Integer literals are `int`
    (48-bit), literals with `.` or an exponent and the result of `/` are
    `number` (double). `1 == 1.0` is true, but they are different map keys.
    An integer literal wider than 48 bits is a compile error (write `1e15`
    or use `bigint`).
14. **`int` overflow wraps around** at 48 bits (±140 737 488 355 327) with
    no error and no promotion to float. Use `bigint` for larger integers.
15. **`//` and `%` floor** (toward minus infinity): `-7 // 2` is `-4`,
    `-7 % 2` is `1`. `/` always gives a `number`. Integer `// 0` and `% 0`
    panic; `1 / 0` is `inf`.
16. **A float with an integral value prints without `.0`**: `print(3.0)`
    shows `3`. Use `type(v)` to tell `int` from `number`.
17. **A function call in an expression list gives one value.**
    `print(f())`, `{f()}` and `g(f())` keep only the first result. Only
    `local a, b = f()` (and `a, b = f()`) receive several, and `return f()`
    passes all of f's results on (not `return obj:m()`: a method call keeps
    one; assign it to locals to get more).
    Varargs do pass through: `return g(...)`, `[a, ...]`, `{...}` and
    `local x, y = ...` take all of them.
18. **Not-equal is `~=`** (`!=` is accepted too). Logical operators are the
    words `and`, `or`, `not`. Only `nil` and `false` are falsy: `0` and `""`
    are true.
19. **Methods use `:`.** `obj:m(x)` passes `obj` as the first argument
    (`self`); `function obj:m(x)` declares `self`. Built-in objects follow
    the same rule: `mu:acquire()`, `wg:add(1)`, `rx:match(s)`, and strings:
    `s:upper()`, `s:split(",")`.
20. **Variables are global unless declared `local`.** Always write `local`.
    `const NAME = v` is a local that cannot be reassigned.
21. **Numeric `for` includes both ends**: `for i = 1, 3` runs 1, 2, 3;
    `for i = 0, n - 1` runs n times (none when n is 0). The step is the
    optional third value.
22. **Maps are `[=]`.** A map takes keys of any type and supports the
    two-value lookup `local v, ok = m[k]` (or `m.name`; `ok` is true when
    the key is there, even holding `nil`, as a JSON `null` decodes). Tables
    (`{}`) also take most keys; prefer maps for dictionaries with
    non-string keys.
23. **Order:** iterating a table or map follows hash order, not insertion
    order (a struct follows the order of its literal). Sort keys when the
    order matters: `sort.sort(keys)`.
24. **`require` paths are relative to the requiring file**:
    `require("./util")` loads `util.sea` next to the current file;
    a library goes by its name, `require("http")` (installed with `sea`, or
    `libs/` of the source tree a `bin/sea` runs from). Built-in modules
    (`string`, `math`, `fs`, `re`, `json` is *not* one) need no `require`.
25. **String interpolation uses backquotes**: `` `total: ${n} items` ``
    (any expression inside `${}`; not `"${n}"`, not `f"..."`).
    **No `goto`, `repeat ... until`, `switch`, classes, metatables,
    `setmetatable`, `__index`, `++` / `--` (but `+= -= *= /= //= %= ^= ..=` exist),
    ternary `?:`** (use `cond and a or b`, which fails when `a` is false
    or nil), or `print` with `%` formatting (use `printf` or
    `string.format`).
26. **Escapes:** `\n \t \r \\ \" \' \0 \xHH \u{HHHH}` and `\a \b \f \v`.
    Any other backslash sequence is a compile error: `"\d+"` must be
    `"\\d+"` (or the long string `[[\d+]]`), and a byte is `"\x01"`
    (Lua's decimal `"\1"` is an error).
27. **Reserved words cannot name variables or parameters**: besides Lua's
    (`and break do else elseif end false for function if in local nil not
    or return then true while`), Sea reserves `const continue del go defer
    panic select case default`, so `local case = 1` is a syntax error. They
    can name fields and methods: `t.default`, `{go = 1}`, `q:in(...)` and
    `function Query:or(...)` are fine.
28. **`string.join(list, sep)`** joins an array or a positional table;
    `table.concat` does not exist.
29. **Regexes in long strings:** `[[...]]` ends at the first `]]`, which a
    pattern like `[^\]]+\]` contains. Write such patterns as `[=[...]=]`
    (or a normal string with doubled backslashes: `"\\d+"`).
30. **`print(a, b)` separates with a tab** (Python uses a space). For exact
    output use a template: `` print(`${a} ${b}`) `` or `printf`.

---

## 2. Syntax in one screen

```sea
-- comment; --[[ long comment ]]
local n = 42                      -- int
local pi = 3.14                   -- number
local s = "text" .. " more"       -- .. concatenates (numbers are converted)
local msg = `n is ${n}, twice ${n * 2}`   -- template string: ${expr} inside backquotes
local count = 0
count += 2                        -- also -= *= /= //= %= ^= and ..= (append to a string)
local raw = [[no \n escapes here]]
local yes, no = true, nil

if n > 40 and s ~= "" then
    print("big")                  -- → big
elseif n == 0 then
    print("zero")
else
    print("small")
end

for i = 1, 3 do io.write(i, " ") end        -- numeric for, inclusive
print()                                     -- → 1 2 3

local i = 0
while i < 10 do
    i = i + 1
    if i % 2 == 0 then continue end         -- continue and break exist
    if i > 5 then break end
end
print(i)                                    -- → 7

local function add(a, b) return a + b end   -- one-statement body on one line
print(add(2, 3), type(add))                 -- → 5  function
print(yes, no, raw)                         -- → true  nil  no \n escapes here
print(msg)                                  -- → n is 42, twice 84
```

---

## 3. Values and types

`type(v)` returns one of: `nil`, `bool`, `int`, `number`, `string`,
`array`, `table`, `map`, `struct`, `function`, `channel`, `int_array`,
`float_array`, `float32_array`, `bigint`, `error`.

```sea
print(type(1), type(1.5), type("a"), type(true), type(nil))  -- → int  number  string  bool  nil
print(7 // 2, 7 / 2, -7 // 2, -7 % 2, 2 ^ 10)               -- → 3  3.5  -4  1  1024
print(1 == 1.0, 3.0, 0.1 + 0.2, 1 / 0)                      -- → true  3  0.3  inf
print(10 .. "px", "a" .. 1.5, tostring(nil))                -- → 10px  a1.5  nil
print(tonumber("42"), tonumber("0x10"), tonumber("4x"))      -- → 42  16  nil
print(140737488355327 + 1)                                   -- → -140737488355328
print(0 and "0 is true", "" and "empty is true")             -- → 0 is true  empty is true
print(nil or "default", false and 1 or 2)                    -- → default  2
```

Big integers are explicit:

```sea
local f = bigint.new(1)
for i = 2, 25 do f = bigint.mul(f, i) end
print(f, f > 1e20)                    -- → 15511210043330985984000000  true
```

---

## 4. Collections

```sea
-- array: 0-indexed, written [a, b, c], grows with a[] = v
local a = ["x", "y"]
a[] = "z"
print(a[0], a[-1], len(a))            -- → x  z  3
del a[1]                              -- remove and shift down
print(len(a), a[1], a[9])             -- → 2  z  nil
for idx, v in a do io.write(idx, "=", v, " ") end
print()                               -- → 0=x 1=z

-- map: any key type, two-value lookup
local m = [=]
m["apple"] = 3
m[42] = "answer"
local v, ok = m["pear"]
print(m["apple"], m[42], v, ok, len(m))   -- → 3  answer  nil  false  2
del m[42]

-- struct: the literal fixes the fields
local p = {x = 1, y = 2}
p.x = 10
print(p.x + p.y, p.z)                 -- → 12  nil

-- table: starts empty and grows freely
local t = {}
t.name = "sea"
t[7] = "seven"
print(t.name, t[7], t.missing)        -- → sea  seven  nil

-- positional table (keys 0, 1, 2)
local letters = {"a", "b", "c"}
print(letters[0], len(letters))       -- → a  3

-- nested data
local cfg = {}
cfg.servers = [{host = "a.example", port = 80}, {host = "b.example", port = 8080}]
local grid = [ [1, 2], [3, 4] ]       -- a space after "[": "[[" starts a long string
print(cfg.servers[1].port, grid[1][0])   -- → 8080  3
print(grid, {k = "v"})                -- → [ [1, 2], [3, 4] ]  {k = "v"}
-- print shows contents; dump(v) prints big structures indented

-- list helpers (map, filter, sum, max, keys, group_by, ...)
local nums = [3, 1, 4, 1, 5]
print(list.map(nums, function(x) return x * 10 end), list.sum(nums), list.max(nums))   -- → [30, 10, 40, 10, 50]  14  5
print(list.filter(nums, function(x) return x > 2 end), list.unique(nums))             -- → [3, 4, 5]  [3, 1, 4, 5]

-- sorting
local words = string.split("pear apple fig", " ")
sort.sort(words)
print(string.join(words, ","))        -- → apple,fig,pear
local keys = []
for k, _ in {b = 1, c = 2, a = 3} do keys[] = k end
sort.sort(keys)
print(string.join(keys, " "))         -- → a b c
```

`int_array(n)` and `float_array(n)` are typed numeric arrays (contiguous,
fast; same `[]=`, indexing and `len`; iterate them with a numeric `for`).
`n` is a capacity: the array starts empty, and writing past the end grows it
with zeros.
`float32_array(n)` is the same with 32-bit floats: half the memory, values
rounded to float when stored (`0.1` reads back as `0.10000000149012`).

---

## 5. Functions, methods and objects

```sea
local function divmod(a, b)
    return a // b, a % b              -- several results
end
local q, r = divmod(17, 5)
print(q, r)                           -- → 3  2
print(divmod(17, 5))                  -- → 3
-- (a call inside an argument list gives only its first result)

local function sum(...)
    local total = 0
    for _, x in {...} do total = total + x end
    return total
end
print(sum(1, 2, 3))                   -- → 6

local function counter()              -- closures capture variables
    local n = 0
    return function()
        n = n + 1
        return n
    end
end
local c = counter()
c()
print(c())                            -- → 2

-- an object: start from {} and add fields and methods
local function new_account(owner)
    local self = {}
    self.owner = owner
    self.balance = 0
    function self:deposit(v)
        self.balance = self.balance + v
        return self
    end
    return self
end
local acct = new_account("ana")
acct:deposit(50):deposit(25)
print(acct.owner, acct.balance)       -- → ana  75

-- or a struct literal that already holds its methods
local point = {x = 3, y = 4, norm = function(self) return math.sqrt(self.x ^ 2 + self.y ^ 2) end}
print(point:norm())                   -- → 5

-- shared methods: copy them into each object
local Stack = {}
function Stack.push(self, v) self.items[] = v end
function Stack.pop(self)
    local v = self.items[-1]
    del self.items[len(self.items) - 1]
    return v
end
local function new_stack()
    local s = {}
    s.items = []
    for name, f in Stack do s[name] = f end
    return s
end
local st = new_stack()
st:push(1)
st:push(2)
print(st:pop(), len(st.items))        -- → 2  1
```

A chain of calls may break before `:` or `.`: a line that starts with one
continues the expression above (`b:add(1)` then `    :add(2)` on the next
line); `sea fmt` keeps the breaks.

A keyword may name a field or a method (`q:in(...)`, `q:or(...)`, `t.select`,
`{end = 1}`, `function Query:and(...)`): after `.` or `:` and as a table key
before `=` it can only be a name. Variables and parameters still cannot be
keywords.

Loop variables are fresh in each iteration, so closures and goroutines
created in a loop keep that iteration's value (as in Go 1.22).

---

## 6. Errors, defer, panic

```sea
local function parse_age(s)
    local n = tonumber(s)
    if n == nil then return error("not a number: " .. s) end
    if n < 0 then return error("negative age") end
    return n
end

local function describe(s)
    local age = parse_age(s)?         -- an error returns from describe at once
    return "age " .. age
end

print(describe("42"))                 -- → age 42
local r = describe("abc")
print(is_error(r), r.message)         -- → true  not a number: abc

-- defer runs at function exit, last in first out; its arguments are evaluated now
local function work()
    local step = 1
    defer print("deferred, step was", step)
    defer function() print("closure sees", step) end
    step = 2
end
work()
-- → closure sees  2
-- → deferred, step was  1

-- panic and recover (recover also catches runtime errors)
local function safe_div(a, b)
    defer function()
        local e = recover()
        if e ~= nil then print("recovered:", string.contains(e.message, "division by zero")) end
    end
    return a // b
end
print(safe_div(7, 2))                 -- → 3
print(safe_div(1, 0))
-- → recovered:  true
-- → nil
```

When a deferred call recovers, the function returns `nil` (its zero
results, as in Go), even if the deferred code changed a local. Use error values for expected failures and panics for bugs.

---

## 7. Concurrency

Goroutines run on an M:N scheduler (all CPU cores). A channel carries any
value.

```sea
local jobs = chan(10)                 -- buffered channel (chan() is unbuffered)
local results = chan(10)

local function worker(id)
    for j in jobs do results <- j * j end   -- receives until jobs is closed
end
for w = 1, 3 do go worker(w) end
for j = 1, 5 do jobs <- j end
close(jobs)
local total = 0
for _ = 1, 5 do total = total + <-results end
print(total)                          -- → 55

-- select, with a default case for a non-blocking attempt
local ch = chan(1)
ch <- "ready"
select
    case v = <-ch then
        print("got", v)               -- → got  ready
    case default then
        print("nothing")
end

-- shared state: sync.mutex, sync.waitgroup
local wg = sync.waitgroup()
local mu = sync.mutex()
local count = 0
for i = 1, 100 do
    wg:add(1)
    go function()
        defer wg:done()
        mu:acquire()
        count = count + i
        mu:release()
    end()
end
wg:wait()
print(count)                          -- → 5050
```

- `go f(x)` or `go function() ... end()` starts a goroutine.
- `time.sleep(ns)` takes **nanoseconds**: `time.sleep(100 * time.Millisecond)`.
- `sync.once()` (`o:do_once(fn)`) runs a function once across goroutines.
- Waiting on a channel, a timer or a socket (`net`, `libs/http`) parks only
  the goroutine; file I/O blocks its thread.

---

## 8. Modules

A module is a file that returns a value, usually a table of functions:

```sea
-- sketch: geometry.sea
local M = {}

-- geometry.area(w, h) → the area of a w × h rectangle
function M.area(w, h)
    return w * h
end

local function helper() end           -- private: not exported

return M
```

```sea
-- sketch: main.sea, next to geometry.sea
local geometry = require("./geometry")
print(geometry.area(3, 4))
```

- Paths are relative to the requiring file; `.sea` is added (a native
  `.so` library is found the same way). Modules are loaded once and cached.
- Then the current directory, then `sea_modules/` (packages installed by
  `sea get`), then `SEA_PATH`, then the libraries installed by
  `make install` (`require("json")` works by name with an installed `sea`).
- Built-in modules need no `require` (they are globals):
  `math string utf8 re encoding hash io os fs path process sync time net gc
  sort list bigint json csv`. `require("sea/parser")`, `require("sea/test")`,
  `require("sea/check")`, `require("sea/stream")`, `require("sea/flags")`,
  `require("sea/log")` and `require("sea/dotenv")` are built in too.
- Doc comments: a comment block at the top of the file documents the
  module; `--` lines right above a function document it, by convention
  starting with the signature (`-- name(args) → result`). `sea doc` reads
  them.

---

## 9. Standard library cheat sheet

Signatures, one per line. `[x]` is optional; `→ v|error` returns an error
value on failure.

**Globals**: `print(...)` (containers print their contents: `[1, 2]`,
`{k = 1}`), `dump(v)` (indented, for inspecting data), `printf(fmt, ...)` (no newline),
`type(v)`, `tostring(v)`, `tonumber(s) → n|nil`, `len(v)`,
`error(msg)`, `is_error(v)`, `assert(v [, msg])`, `require(path)`,
`chan([cap])`, `close(ch)`, `panic(v)`, `recover()`, `int_array([cap])`,
`float_array([cap])`, `float32_array([cap])`, `isnan(x)`, `isinf(x)`.

**string** (1-based positions; also as methods `s:upper()`):
`len sub(s,i[,j]) upper lower reverse rep(s,n[,sep]) byte(s[,i]) char(...)
format(fmt,...) find(s,text[,init]) → pos|nil split(s,sep[,n]) → array splitlines(s)
format_number(n[,decimals[,sep[,point]]])
contains starts_with ends_with replace(s,old,new) trim
pad_left/pad_right/pad_center(s,w[,ch]) join(array[,sep]) normalize_ws`.
`string.format`: `%d %i %u %f %e %g %s %c %x %X %%` with width, precision
and flags.

**utf8**: `len valid invalid sanitize char(cp...) codepoint(s[,i])
codepoints chars sub(s,i[,j]) offset upper lower reverse width`.

**re** (RE2; `p` is a pattern string or `re.compile(p [, "ims"])`):
`test(p,s) → bool`, `match(p,s[,init]) → m|nil` (`m.text`, `m.start`,
`m.stop`, `m.groups[1]`, `m.named.name`), `captures(p,s[,init]) → [whole, g1, ...]|nil`
(strings only, 2× faster than `match`: use it to parse lines), `all(p,s[,n])`,
`findall(p,s[,n]) → strings`, `gsub(p,s,repl[,n])` (repl: string with
`$1`, a function of the match, or a table), `split(p,s[,n])`, `escape(s)`.
Double the backslashes in normal strings: `"\\d+"`, or use `[[\d+]]`.

**math**: `pi huge maxinteger mininteger abs ceil floor round trunc sqrt
exp log(x[,base]) log2 log10 pow fmod sin cos tan asin acos atan
max(...) min(...) random() random(n) random(lo,hi) randomseed([s])`.
`floor`/`ceil`/`round`/`trunc` return ints.

**io**: `read(["l"|"n"|"a"])` (stdin), `write(...)`, `write_err(...)`,
`open(path[,mode]) → h|nil`, `stdin() → h`, `close(h)`, `read_line(h)`,
`read_bytes(h,n)`, `read_all(h)`, `write_string(h,s)`, `write_line(h,s)`,
`flush(h)`, `scanner(path)` + `scan(sc)` + `scan_text(sc)` (line by line).

**fs** (errors are values: `fs.read(p)?`): `read(p) → s|error`,
`write(p,data[,{append,mode,atomic}])`, `append(p,data)`, `exists isfile
isdir`, `stat(p)` (`size`, `mode`, `mtime`, `type`), `list(dir) → names`,
`walk(dir[,{files,max_depth}]) → paths`, `mkdir(p[,{parents}])`,
`remove(p[,{recursive,force}])`, `rename copy symlink readlink realpath
chmod touch tempdir([prefix]) tempfile cwd chdir glob(pattern)`.

**path**: `join(a,b,...) normalize dirname basename(p[,suffix]) ext stem
parts isabs abs relative(base,target) sep`.

**os**: `args` (array: `os.args[1]` is the first argument, `os.args[0]` is
nil; count with `len(os.args) - 1`), `exit([code])`, `getenv(name)`,
`exec(cmd) → {code, output}` (through the shell), `urandom(n)` (secure
random bytes), `token([n])` (secure random hex), `uuid()` (UUID v4).

**process** (no shell when the command is an array):
`run(cmd[,opts]) → {ok, code, stdout, stderr, timed_out}|error`,
`spawn(cmd[,opts]) → proc` (`proc:write`, `close_stdin`, `read`,
`read_line`, `wait`, `kill`), `pid()`, `kill(pid[,sig])`. opts: `cwd`,
`env`, `stdin`, `stdout`, `stderr`, `timeout` (ms).

**time** (durations are int **nanoseconds**, at most ~39 hours in an int):
constants `Nanosecond Microsecond Millisecond Second Minute Hour Day`;
`now() → Time`, `date(y,m,d[,h,min,s][,zone])` (zone: `"local"`, `"UTC"`,
`"-03:00"` or an IANA name like `"Europe/Lisbon"`, with its summer time), `parse(s[,layout[,zone]]) →
Time|error` (ISO 8601 by default: `"2026-10-18"`, `"2026-10-18 14:30"`,
`"...T14:30:00Z"`), `unix(sec[,ns])`, `time() → unix seconds`, `sleep(ns)`,
`since(t) → ns`, `days(ns)`, `seconds(ns)`... A Time has methods
`t:year() t:month() t:day() t:hour() t:minute() t:second() t:weekday()
t:add_date(y,m,d) t:add_seconds(s) t:add(ns) t:sub(t2) → ns
t:before(t2) t:after(t2) t:utc() t:in_zone("+03:00" or "America/Sao_Paulo") t:iso()
t:format("02/01/2006 15:04")` (Go layouts; also for parse). `print(t)` shows
`time(2026-10-18T14:30:00-03:00)`.

**sync**: `mutex()` (`acquire release try_lock`), `waitgroup()` (`add(n)
done wait`), `once()` (`do_once(fn)`).

**net** (TCP; timeouts in ms; parks the goroutine): `listen(port[,host])`,
`accept(fd[,t])`, `connect(host,port[,t])`, `read(fd,max[,t]) → s` (`""` at
end), `write(fd,data[,t])`, `close(fd)`, `shutdown(fd)`, `local_port(fd)`,
`peer(fd)`.

**encoding**: `base64_encode(s[,{url,pad}]) base64_decode base32_encode
base32_decode hex_encode hex_decode url_encode url_decode html_escape`.

**hash**: `md5 sha1 sha224 sha256 sha384 sha512(s[,raw])` (hex, or raw
bytes), `hmac(alg,key,data[,raw])`, `new(alg)` (`h:update(s)`,
`h:digest()`), `crc32 adler32 fnv1a`, `equal(a,b)` (constant time).

**list** (global; new arrays, input unchanged; callbacks get `(v, i)`):
`map(a,fn) filter(a,fn) reduce(a,fn[,init]) find(a,fn) index(a,x)
contains(a,x) any(a,fn) all(a,fn) count(a,fn|x) sum(a[,fn]) min(a[,fn])
max(a[,fn]) mean(a) median(a) stdev(a) range(n) range(from,to[,step])` (to **inclusive**)
`unique(a) slice(a,from[,to])` (to **exclusive**) `concat(a,b,...) flatten
reversed copy chunk(a,n) zip(a,b,...) group_by(a,fn) count_by(a,fn)` (→ maps)
`keys(t) values(t) items(t)` (tables/maps sorted by key); counters:
`inc(m,k[,n])` (`m[k] = (m[k] or 0) + n`), `most_common(m[,n])` (→
`[[k, count], ...]`, largest first, ties by key).

**sort**: `sort(a[,less])` (stable, in place), `by(a,key[,reverse])` (key: function,
field name, or fields `["-total", "name"]`; a key function may return an array
`[-x.n, x.name]`, compared like a tuple),
`sorted(a[,less]) → copy`, `reverse(a)`, `is_sorted`, `search(a,x)`,
`asc`, `desc`.

**bigint**: `new(v[,base])`, `add sub mul div mod pow neg abs cmp
tostring(a[,base]) toint tonumber`. Comparisons, `==`, `..`, `tostring`
and `print` accept bigints; `+ - *` do not.

**sea/log** (`local log = require("sea/log")`; leveled logging like Go's
slog, to stderr): `log.info(msg[,fields])`, `log.debug`, `log.warn`,
`log.error`, `log.fatal` (exits 1) → `2026-10-18 14:30:05 INFO  listening
port=8080`; `log.set_level("debug")`, `log.set_format("json")`,
`log.set_output(path|"stdout"|handle|fn)`; `log.new({level, format, output,
fields, time = false})` → `l:info(...)`, `l:with(fields)` (a child logger).

**sea/stream** (`local stream = require("sea/stream")`; buffered I/O like
Go's bufio, over sockets, processes, files, stdin, strings):
`stream.reader(src[,{size,timeout}])` → `r:read_line()`, `r:read_until(delim)`,
`r:read_full(n)`, `r:read([max])`, `r:peek(n)`, `r:read_all()`;
`stream.writer(dst)` → `w:write(...)`, `w:flush()`;
`stream.scanner(src[,{split}])` → `while sc:scan() do sc:text() end`
(split `"lines"`, `"words"`, `"bytes"`, `"chars"` or a function);
`stream.open(path[,mode])`, `stream.stdin()`, `stream.stdout()`,
`stream.string(s)`, `stream.buffer()`. The end of input is `nil`.

**gc**: `collect()`, `stats()`. Memory is reference counted with a cycle
collector; you rarely need these.

---

## 10. External libraries (`libs/`)

Required by path (relative to the requiring file), or by name
(`require("yaml")`) when Sea was installed with `make install`. Native ones
need a build first: `make -C libs/<name>`. `json` and `csv` are built in.

| Library | Path from the repo root | What |
|---------|-------------------------|------|
| `json` | built in (global `json`) | `json.encode(v[,{indent,sort_keys}])`, `json.decode(s) → v\|error` |
| `csv` | built in (global `csv`) | `csv.decode(s[,{header,numbers}]) → rows\|error`, `csv.encode(rows)`, streaming a file row by row |
| `yaml`, `toml`, `xml` | `libs/<name>/<name>` | Parse and write those formats |
| `http` | `libs/http/http` | Client (`http.get(url)`, `http.post(url, body, type)`, `resp.status`, `resp.body`, `resp:json()`) and server (`http.serve(port, function(req) return http.json(v) end)`, `http.router()`) |
| `ssl`, `websocket`, `jwt` | `libs/<name>/<name>` | TLS, WebSockets, JSON Web Tokens |
| `sqlite`, `duckdb` | `libs/<name>/<name>` | `local db = sqlite.open("app.db")?`, `db:exec(sql, params...)`, `db:query(sql, ...) → rows`, `db:row`, `db:value`, `db:transaction(fn)` |
| `cockroach` | `libs/cockroach/cockroach` | CockroachDB/PostgreSQL (pure Sea): `crdb.open("postgresql://root@localhost:26257/defaultdb?sslmode=disable")?`, then as sqlite with `$1, $2` parameters; `db:insert/upsert(table, rows)`; `db:transaction(fn)` retries conflicts (40001) |
| `clickhouse` | `libs/clickhouse/clickhouse` | ClickHouse over HTTP: `clickhouse.open("http://default:@localhost:8123/default")?`, `db:query(sql, {name = v})` with `{name:Type}` in the SQL, `db:exec`, `db:insert(table, rows)`, `db:each(sql, fn)` (streamed) |
| `flags` | built in: `require("sea/flags")` | Command-line flags: `flags.string/int/number/bool/list(name, default, usage, {alias, choices})`, then `local files = flags.parse()`; `.value` holds each result |
| `template` | `libs/template/template` | Text templates |
| `console` | `libs/console/console` | Terminal colors, prompts, spinners |
| `compress` | `libs/compress/compress` | gzip / zlib, ZIP and tar archives (`zip_write`, `tar_extract`, ...) |
| `tensor` | `libs/tensor/tensor` | Numeric kernels on `float_array` (matvec, attention, Adam) |
| `ndarray` | `libs/ndarray/ndarray` | NumPy-like N-dimensional arrays: `local np = require("ndarray")`, `np.arange(6):reshape(2, 3)`, methods instead of operators (`a:add(b):mul(2)`, `a:get("1, 1:")`, `a:set(":, 0", 9)`), `a:sum({axis = 0})`, `np.linalg`, `np.random.default_rng(seed)`, `np.save`/`np.load` (.npy) |
| `test` | built in: `require("sea/test")` | The test framework (below) |

Full APIs: [`docs/libs.md`](libs.md).

---

## 11. Tests

Test files are named `*_test.sea`; `sea test` finds and runs them.

```sea
-- sketch: stack_test.sea
local t = require("sea/test")
local stack = require("./stack")

t.describe("stack", function()
    local s = nil
    t.before_each(function() s = stack.new() end)

    t.it("pops what was pushed", function()
        s:push(1)
        t.eq(s:pop(), 1)
    end)

    t.it("fails on an empty pop", function()
        t.is_error(s:pop(), "empty")
    end)
end)
```

Assertions: `t.eq(got, want)` (deep equality), `t.ne`, `t.ok(v)`,
`t.no(v)`, `t.is_nil`, `t.not_nil`, `t.near(a, b[, eps])`, `t.gt/ge/lt/le`,
`t.contains(hay, needle)`, `t.matches(s, regex)`, `t.is_error(v[, text])`,
`t.no_error(v) → v`, `t.raises(fn[, text])`, `t.fail(msg)`. Hooks:
`before_each`, `after_each`, `before_all`, `after_all`. Also `t.skip`,
`t.only`, `t.todo`, `t.bench`. Run with `sea test -v`, `sea test -run name`,
`sea test -cover`. A complete example is `examples/users/users_test.sea`.

---

## 12. A complete program

```sea
-- sketch: wc.sea — count lines, words and bytes: sea wc.sea FILE...
local function count(file)
    local data = fs.read(file)?
    local words = len(re.findall("\\S+", data))
    local lines = len(re.findall("\n", data))
    return {lines = lines, words = words, bytes = len(data)}
end

if len(os.args) < 2 then
    io.write_err("usage: sea wc.sea FILE...\n")
    os.exit(2)
end

local status = 0
for i = 1, len(os.args) - 1 do
    local file = os.args[i]
    local c = count(file)
    if is_error(c) then
        io.write_err(c.message, "\n")
        status = 1
    else
        printf("%7d %7d %7d %s\n", c.lines, c.words, c.bytes, file)
    end
end
os.exit(status)
```

More programs: [`examples/`](../examples/) (each directory has a runnable
`.sea` file; `examples/users` is an application with a test suite).

---

## 13. Translating from other languages

| Lua / Python / Go / JS | Sea |
|------------------------|-----|
| `t = {1, 2, 3}` (Lua list), `[1, 2, 3]`, `[]int{1, 2, 3}` | `local t = [1, 2, 3]` (indexes from 0) |
| `[[1, 2], [3, 4]]` | `[ [1, 2], [3, 4] ]` (`[[` would start a long string) |
| `table.insert(t, v)`, `t.append(v)`, `append(t, v)`, `t.push(v)` | `t[] = v` |
| `table.remove(t, i)`, `del t[i]` | `del t[i]` (0-based) |
| `#t`, `len(t)`, `t.length` | `len(t)` |
| `t[1]` as the first element | `t[0]` |
| `for i, v in ipairs(t)`, `for i, v := range t` | `for i, v in t do` (i from 0) |
| `for k, v in pairs(t)`, `for k, v in d.items()` | `for k, v in t do` |
| `d = {}` (Python dict), `map[K]V{}` | `local d = [=]` |
| `if k in d`, `v, ok := m[k]` | `local v, ok = d[k]` |
| `local s = a .. b` | the same |
| f-strings `f"{k}={v}"`, template literals `` `${k}` `` | `` `${k}=${v}` `` (backquotes), or `string.format("%s=%d", k, v)` / `printf` |
| `s:gsub(pattern, r)`, `re.sub` | `re.gsub(pattern, s, r)` (subject second) |
| `s:find("x", 1, true)` | `string.find(s, "x")` |
| `[x * 2 for x in a]`, `a.map(f)`, `sum(a)`, `max(a, key=f)`, `Counter(a)` | `list.map(a, function(x) return x * 2 end)`, `list.sum(a)`, `list.max(a, f)`, `list.count_by(a, f)` |
| `c[k] += 1` (defaultdict/Counter), `c.most_common(5)` | `list.inc(c, k)`, `list.most_common(c, 5)` |
| `sorted(a, key=lambda x: (-x.n, x.name))` | `sort.by(a, ["-n", "name"])` (in place) or `sort.by(a, function(x) return [-x.n, x.name] end)` |
| `d.keys()`, `d.items()`, `a[1:3]`, `range(n)` | `list.keys(d)`, `list.items(d)`, `list.slice(a, 1, 3)`, `list.range(n)` |
| `pcall(f)`, `try/except`, `try/catch` | error values + `?`, or `defer` + `recover()` |
| `error("x")` (Lua throws) | `panic("x")`; `error("x")` only *creates* an error value |
| `return nil, "message"` | `return error("message")` |
| `if err != nil { return err }` | `local v = f()?` |
| `x ~= nil and x or y` | the same (`cond and a or b`) |
| `x += 1`, `x++` | `x += 1` (there is no `++`) |
| `!=`, `&&`, `\|\|`, `!` | `~=` (or `!=`), `and`, `or`, `not` |
| `null`, `None`, `undefined` | `nil` |
| `class Foo`, `setmetatable` | a function returning a table with fields and methods (section 5) |
| `go f()` | the same |
| `ch <- v`, `v := <-ch` | `ch <- v`, `local v = <-ch` |
| `time.Sleep(100 * time.Millisecond)` | `time.sleep(100 * time.Millisecond)` |
| `datetime.strptime(s, "%d/%m/%Y")`, `datetime.fromisoformat(s)` | `time.parse(s, "02/01/2006")`, `time.parse(s)` |
| `d + timedelta(days=30)`, `t.AddDate(0, 1, 0)` | `t:add_date(0, 0, 30)`, `t:add_date(0, 1, 0)` |
| `sleep(0.1)` (seconds) | `time.sleep(100 * time.Millisecond)` (nanoseconds) |
| `os.exit(1)`, `sys.exit(1)` | `os.exit(1)` |
| `uuid.uuid4()`, `secrets.token_hex(16)`, `os.urandom(n)` | `os.uuid()`, `os.token(16)`, `os.urandom(n)` |
| `io.read("*l")` | `io.read("l")` |
| `import json`, `import csv` | nothing: `json` and `csv` are built in (`json.decode(s)`) |
