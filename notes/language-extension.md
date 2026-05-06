# Lox++ Language Extension Design Recommendations

## Context

Munificent's original Lox was deliberately *minimal* — a single pedagogical
path through language-implementation space, not a general-purpose language. The
"design breaks" note warns against spending your "novelty budget" carelessly:
new operators are hard to read when users lack precedence intuition, and
overloading *familiar* operators differs from inventing new ones.

Lox++ has already extended Lox significantly (match expressions, enums, break/continue,
inheritance, List, Map, File I/O, and more). The answers below weigh each addition against that novelty
budget.

## Status Against PRs

| PR | Feature | Status |
|---|---|---|
| #41 | switch/case/default | **Superseded** — replaced by `match` expression (PR #61) |
| #42 | `List` type + `[...]` literal + `list[i]` subscript | **Merged** |
| #47 | `math` global object | **Merged** |
| #49 | Sequence protocol: `len()`, `str[i]`, List/String indexing | **Merged** |
| #51 | File I/O (`open`, `ObjFile`) | **Merged** |
| #54 | Map type with `{}` literal + `[]` key access | **Merged** |
| #55 | Iterator protocol (`for (var x in seq)`) | **Merged** |
| #56 | `List.remove(value)` method | **Merged** |
| #61 | `match` statement — Phase 1 (literal, wildcard, or-arm, guard) | **Merged** |
| #62 | `var {x, y}` destructuring — Phase 1.5 | **Merged** |
| #64 | `enum` declaration + constructor pattern matching — Phase 2 | **Merged** |
| #66 | Class patterns via instanceof dispatch — Phase 2.5 | **Merged** |
| #67 | `match` as expression — Phase 3.2 | **Merged** |
| #69 | `seq[start:end]` slice syntax | **Merged** |

---

## Topic 1: Numbers and Math

### Keep a single Number type — no separate integer

Do not add a separate integer type. IEEE 754 doubles represent all integers
exactly up to 2^53, which covers every practical Lox++ use case. Two numeric
types require coercion rules, division behavior differences (`3/2 == 1` vs
`1.5`), and overflow semantics — complexity that far exceeds the benefit.
Munificent explicitly excluded "type systems" from scope; a second numeric type
*is* a mini type system.

The only real cost is that array/string indexing with a float index is
conceptually odd. PR #42 resolves this correctly: require integer-valued
Numbers as indices (runtime error on fractional index). This is consistent with
Lox's "fail loudly" design.

### Add `%` (modulo)

**User-facing API:**
```lox
print 10 % 3;       // 1
print 7.5 % 2.5;    // 0  (float mod, same as Python)
print -7 % 3;       // 2  (floor division semantics, same as Python/Lua)

// Common use cases that become clean:
fun isEven(n) { return n % 2 == 0; }
fun fizzbuzz(n) {
    if (n % 15 == 0) print "FizzBuzz";
    else if (n % 3 == 0) print "Fizz";
    else if (n % 5 == 0) print "Buzz";
    else print str(n);
}
```

**Justification:** The concern "but integer type..." is a distraction. Python,
Lua, and JS all define `%` for floats. `%` is a *familiar* operator — users
already know its precedence is the same as `*` and `/`. The design breaks note
explicitly distinguishes overloading existing operators (safe) from creating
new ones (costly). `%` is firmly in the safe category. Place it in the `factor`
grammar rule alongside `*` and `/`.

### Add `math` global object

**User-facing API:**
```lox
// math is always available as a pre-loaded global instance
print math.sqrt(9);     // 3
print math.floor(3.7);  // 3
print math.ceil(3.2);   // 4
print math.abs(-5);     // 5
print math.pow(2, 10);  // 1024
print math.pi;          // 3.141592653589793
print math.e;           // 2.718281828459045

// Example: Newton's method for sqrt
fun mysqrt(n) {
    var x = n / 2;
    var i = 0;
    while (i < 20) {
        x = (x + n / x) / 2;
        i = i + 1;
    }
    return x;
}
print math.abs(mysqrt(2) - math.sqrt(2)) < 0.0001;  // true
```

**Justification:** At VM startup, create a single `ObjInstance` named `math`
and populate its field table with native function objects (`sqrt`, `floor`,
etc.) plus constant fields (`pi`, `e`). No Lox class definition needed — just a
pre-populated instance. This mirrors Lua's `math` table exactly and avoids
global namespace pollution. Minimum set: `floor`, `ceil`, `abs`, `sqrt`, `pow`.
Trig can come later.

---

## Topic 2: Data Structures and Operations

### Add a native `List` type

A Lox-level linked list class is a useful exercise but cannot replace a native
List: `get(i)` is O(n), string indexing requires O(1) access, and a `foreach`
loop needs a real iterable. A native List is the foundation for both.

**Status:** Fully implemented. PR #42 added creation and indexing; follow-up PRs added `append`, `pop`, `len` (#49), and `remove` (#56).

**Two differences from the original plan, both resolved correctly:**

1. **Literal syntax over constructor.** The original plan proposed `List()` / `List("a",
   "b")`. The implementation uses `[...]` literals — matches Python/JS/Lua,
   familiar to all users, uses zero novelty budget.

2. **Non-integer index → runtime error, not silent floor.** The original plan said
   "silently `floor()` the index." The implementation makes fractional index a runtime
   error ("`list[1.5]` → error, `list[1.0]` → ok"). Consistent with Lox's "fail loudly" design.

**User-facing API (updated to match PR #42 + planned methods):**
```lox
// Construction — literal syntax (PR #42)
var empty = [];
var nums = [1, 2, 3];
var mixed = [1, "hello", true, nil];

// Subscript get/set (PR #42)
print nums[0];              // 1
nums[0] = 99;
print nums[0];              // 99

// Non-integer index is a runtime error (PR #42 decision)
nums[1.5];                  // runtime error
nums[1.0];                  // ok — same as nums[1]

// Methods — still needed, not in PR #42
nums.append(4);             // push to end
print nums.pop();           // removes and returns last element
print nums.len();           // length

// Iteration (once foreach is added)
for (var x in nums) { print x; }
```

**Justification for `[...]` literal:** Subscript syntax `list[i]` adds `"["
expression "]"` as a suffix in the `call` grammar rule (same level as
`.field`). `[]` for indexing and `[...]` for list literals are universally
understood — zero novelty budget spent. PR #42 adds `BUILD_LIST`, `GET_INDEX`,
`SET_INDEX` opcodes.

### Can instances replace Map/Dict? Partially

Lox++ instances are already backed by a hash table. For **string-keyed,
identifier-shaped keys**, instances already work as maps:

```lox
// Instances-as-map already works for fixed string keys
class Config {}
var cfg = Config();
cfg.host = "localhost";
cfg.port = 8080;
print cfg.host;   // localhost

// But this does NOT work — keys must be identifiers, not expressions:
var key = "host";
print cfg.key;    // runtime error: field 'key' not found, not cfg["host"]
```

**Gaps that instances can't fill:**
1. Dynamic string keys: `obj[variable_key]` — impossible with identifier-only
   field access
2. Non-string keys: `obj[42]`, `obj[someInstance]` — impossible
3. Field enumeration: no way to iterate over an instance's fields from Lox
4. Existence check: no `has(key)` — field access on a missing key is a runtime
   error

**Recommendation: defer a dedicated Map type until after List**

```lox
// Future Map API (for reference, not implementing now):
var m = Map();
m.set("key", 42);
m.set(99, "number key");
print m.get("key");         // 42
print m.has(99);            // true
m.delete("key");
print m.len();              // 1

// With subscript syntax (once [] is added):
m["key"] = 42;
print m["key"];             // 42
```

**Why Python dict, not Lua table:** Lua's unified table works beautifully in a
prototype-based language. Lox++ is class-based — unifying classes and maps
retroactively would break method dispatch and inheritance semantics. Keep them
separate.

### foreach / iteration protocol

**Implemented:** List, String, and Map, via dedicated VM opcodes.

**User-facing API:**
```lox
for (var x in [1, 2, 3]) { print x; }   // 1 2 3
for (var ch in "hello") { print ch; }    // h e l l o
for (var k in map) { print k; }          // iterates over Map keys
```

Iterating over any other value is a runtime error.

**Implementation:** `for (var x in expr)` compiles to three new opcodes:

- `GET_ITER` — pops a List or String, pushes an `ObjIterator{collection, cursor=0}`
  in-place (GC-safe: replaces the stack slot rather than pop+alloc+push)
- `ITER_HAS_NEXT` — pops an iterator copy, pushes `true` if `cursor < length`
- `ITER_NEXT` — pops an iterator copy, pushes the element at `cursor` and advances it

The compiler emits two hidden locals (`(iter)` + item variable) instead of the
previous three (`(seq)` + `(idx)` + item). This eliminates the `len()` call and
index arithmetic on every iteration.

**Deferred — user-defined iterables:** User classes can opt in by implementing
`iter()` returning an object with `hasNext()` / `next()`. The planned desugaring:
```lox
// for (var x in expr) where expr is a user-defined iterable would desugar to:
var _it = expr.iter();
while (_it.hasNext()) { var x = _it.next(); ... }
```
Explicit `hasNext()` / `next()` (vs Python's `StopIteration` exception) avoids
needing exceptions in the language. This is essentially Java's
`Iterable`/`Iterator` — familiar, teachable, no new syntax. Not yet implemented.

---

## Topic 3: I/O and Side Effects

### File I/O: `open()` returns a file object with native methods

**User-facing API:**
```lox
// Reading
var f = open("data.txt", "r");
var line = f.readline();       // reads one line, strips trailing \n; nil on EOF
while (line != nil) {
    print line;
    line = f.readline();
}
f.close();

// Reading all at once
var f = open("data.txt", "r");
var content = f.read();        // entire file as a String
f.close();

// Writing
var f = open("out.txt", "w");
f.write("Hello, file!\n");
f.close();

// Appending
var f = open("log.txt", "a");
f.write("new entry\n");
f.close();
```

**Justification:** `open(path, mode)` is a global native function returning an
`ObjFile` — a new native object type (like `ObjString`). Its methods (`read`,
`readline`, `write`, `close`) are native functions dispatched via the VM's
method lookup. No new syntax needed; `f.readline()` looks like any other method
call. Modes: `"r"` (read), `"w"` (write/truncate), `"a"` (append). Mode `"r"`
by default or required — keep it simple.

**Missing implementation pieces:** `ObjFile` VM type, `open()` global native,
method dispatch for non-`ObjInstance` types (currently only `ObjInstance`
supports field/method lookup — `ObjFile` needs its own dispatch path in
`callValue` and `getProperty`).

### String as sequence: `len()` and `str[i]`

**User-facing API:**
```lox
var s = "hello";
print len(s);       // 5  — global native, works on String and List
print s[0];         // "h"  — single-character String
print s[4];         // "o"

// Strings are immutable — s[0] = "H" is a runtime error

// Iterate characters once foreach is added
for (var ch in s) {
    print ch;
}

// Common pattern: split on a delimiter (user-implemented using indexing)
fun split(s, delim) {
    var result = [];
    var current = "";
    var i = 0;
    while (i < len(s)) {
        if (s[i] == delim) {
            result.append(current);
            current = "";
        } else {
            current = current + s[i];
        }
        i = i + 1;
    }
    result.append(current);
    return result;
}
```

**Justification:** `len(x)` as a global native (not a method) is simpler to
implement on native types — no method dispatch overhead, and Python users find
it familiar. Extend `OP_GET_INDEX` dispatch to handle `ObjString`: index into
the byte array, return a single-char `ObjString`. Strings remain immutable, so
`OP_SET_INDEX` on a String is a runtime error. Note: this is ASCII character
indexing — each index corresponds to exactly one character, since the language
restricts strings to ASCII.

---

## Resolved Design Questions

### Q1: Global function vs method for builtins (`len(list)` vs `list.len()`)

**Answer: cross-type operations → global native; type-specific mutation →
native methods on the type.**

The VM's `OP_GET_PROPERTY` / `OP_INVOKE` (vm.cpp:351, 389) are
`ObjInstance`-only — they hardcheck `isInstance()`. Supporting `list.len()`
requires either (a) extending those paths with `if (isList(...))` special
cases, or (b) giving `ObjList` a shared `ObjClass*`. Neither is free.

Split by operation character:
- **`len(x)` → global native.** Works on List, String, (Map later) — a
  cross-type operation. Zero dispatch infrastructure cost. Consistent with
  `str()`, `clock()`, `input()` (vm.cpp:548–551). Add one type-switch inside the
  native.
- **`list.append(x)`, `list.pop()` → native methods.** Mutation operations
  tightly bound to a single type. Semantically, methods are correct.
  Implementation: give `ObjList` a shared `ObjClass*` (see Q2) — then dispatch
  reuses the existing VM path entirely, no new special-cases in `OP_INVOKE`.
- **`math.sqrt`, `math.floor` → field values, not methods.** These are just
  `NativeFunction` objects stored in the `math` instance's field table.
  `math.sqrt(2)` = get field → call value. `OP_INVOKE` already handles this
  two-stage path (vm.cpp:399–414: field-first, calls `callNative()` if field is
  native). No binding needed since math functions have no `this`.

### Q2: `ObjStdlib` (no class) vs `ObjInstance` (synthetic class) for stdlib objects

**Answer: no new `ObjStdlib` type. Use `ObjInstance` with a synthetic hidden
class for namespace objects; use a shared `ObjClass*` on each native type for
method-bearing types.**

The two use cases are distinct:

**Namespace objects (e.g. `math`):**
- Create a hidden `ObjClass` named `"math"` (or `"<math>"`) at VM startup with
  no methods.
- Create one `ObjInstance` of that class; populate its `fields` table with
  `NativeFunction` values (`sqrt`, `floor`, etc.) and `Number` constants (`pi`,
  `e`).
- Register the instance as the global `"math"`.
- Field access via `OP_GET_PROPERTY` (vm.cpp:351) already works — it checks
  `instance->fields` first.
- `OP_INVOKE` (vm.cpp:399–414) already handles calling a native found in fields.
- `str(math)` → `"math instance"` — acceptable.
- **Zero new ObjType.** All existing GC tracing, field lookup, stringify paths
  work unchanged.

**Native types with methods (`ObjList`, future `ObjFile`):**
- At startup, create one shared `ObjClass` (e.g. `listClass`) populated with
  native methods (`append`, `pop`).
- Add a `ObjClass* klass` field to `ObjList` (and future `ObjFile`), pointing
  to the shared class.
- Method dispatch via `OP_INVOKE` then uses the existing
  `instance->klass->methods.get(name)` path — **no new VM special-cases needed**.
- This requires a small change to PR #42's `ObjList` struct (add `klass` field).

**Why not a dedicated `ObjStdlib`?** Adding a new `ObjType` means: new enum
entry, new GC trace case, new `IS_STDLIB()` macro, new stringify branch, new
dispatch cases in `OP_GET_PROPERTY`/`OP_INVOKE`. That's ~8 code sites for a
type whose only difference from `ObjInstance` is "no class." The synthetic
class costs 4 lines at VM startup and zero ongoing maintenance.

### Open (still to decide)

- **Map key constraints:** restrict to String + Number (simple hashing) or
  arbitrary values (needs `hash()` protocol on instances)? String + Number covers
  95% of use cases.
- **String `substring(start, end)` method:** once `len()` and `str[i]` exist,
  users will immediately want this. Should it be a native method on String
  (requires `ObjString` to carry a class pointer, same as `ObjList` above) or a
  global `substr(s, start, end)` native?

---

## Suggested Implementation Order (by dependency)

| # | Feature | Status |
|---|---|---|
| 1 | `%` operator | **Done** |
| 2 | `math` global object | **Done** (#47) |
| 3 | `List` type with `[...]` literal + `list[i]` subscript | **Done** (#42) |
| 4 | `List` methods: `append`, `pop`, `len`, `remove` | **Done** (#42, #49, #56) |
| 5 | `len()` global native (for both List and String) | **Done** (#49) |
| 6 | String indexing `str[i]` (reuses `GET_INDEX` from #42) | **Done** (#49) |
| 7 | `seq[start:end]` slice syntax | **Done** (#69) |
| 8 | `for (var x in seq)` with iterator protocol | **Done** (#55) |
| 9 | File I/O (`open`, `ObjFile`) | **Done** (#51) |
| 10 | Map/Dict type | **Done** (#54) |
| 11 | `match` expression (phases 1–3.2) | **Done** (#61, #62, #64, #66, #67) |

---

## Programs Unlocked by Milestone

Each entry lists programs that *first become cleanly expressible* at that
milestone. Programs from earlier milestones remain available.

### Baseline (already possible today)

Features in hand: closures, classes + inheritance, `input()`, `str()`,
`clock()`, `match`, break/continue.

| Program | What it exercises |
|---|---|
| Fibonacci (recursive + iterative, timed with `clock()`) | Recursion, closures, benchmark harness |
| GCD / LCM via Euclidean algorithm | Iteration, arithmetic |
| Interactive quiz / trivia game | `input()`, classes for questions + scoring |
| Number guessing game | `input()`, loops, runtime |
| Class hierarchy demo (Shape → Circle/Rectangle, polymorphic `area()`) | Inheritance, `super`, dynamic dispatch |
| Towers of Hanoi | Recursion |

---

### Milestone 1 — `%` operator

| Program | Key use of `%` |
|---|---|
| FizzBuzz | `n % 15`, `n % 3`, `n % 5` |
| Leap-year checker | `y % 4 == 0 and y % 100 != 0 or y % 400 == 0` |
| Collatz sequence printer (`3n+1`) | `n % 2` to branch even/odd |
| Number → binary string converter | Repeated `n % 2`, build string with `+` |
| Luhn algorithm (credit card check digit) | Alternating digit sum with `% 10` |
| Digital root | `(n - 1) % 9 + 1` |
| Circular counter / clock arithmetic | `(val + step) % max` |

---

### Milestone 2 — `math` global object

Adds `math.sqrt`, `math.floor`, `math.ceil`, `math.abs`, `math.pow`, `math.pi`.

| Program | Key use of `math` |
|---|---|
| Quadratic formula solver | `math.sqrt(discriminant)`, handle negative → "no real roots" |
| 2D distance / vector magnitude | `math.sqrt(dx*dx + dy*dy)` |
| Circle & sphere calculator | `math.pi * r * r`, `(4/3) * math.pi * r * r * r` |
| Compound interest (`A = P(1 + r/n)^nt`) | `math.pow(base, exp)` |
| Integer square root / perfect-square check | `math.floor(math.sqrt(n))` |
| Standard deviation of hardcoded data | `math.sqrt(variance)` |
| Numerical root finder (Newton's method, bisection) | Compare user impl vs `math.sqrt` |

---

### Milestone 3 — `List` literal + `list[i]` subscript (PR #42)

Without `append`/`pop`/`len`, lists are fixed-size. Useful as static lookup
tables and multi-return packing.

| Program | What fixed-size lists enable |
|---|---|
| Day / month name lookup | `var days = ["Mon","Tue",...]; print days[d]` |
| Matrix multiply (2×2 or 3×3, hardcoded size) | `var m = [[1,2],[3,4]]`, index as `m[r][c]` |
| Swap two elements in-place | Temp variable + two index assignments |
| Binary search on a sorted literal | Index arithmetic, no append needed |
| Multiple return values packed in a list | `return [quot, rem]`, caller uses `[0]`, `[1]` |
| Table of precomputed values (e.g. sin table) | `var sins = [0, 0.5, 0.866, ...]` |

---

### Milestone 4+5 — `append`, `pop`, `len` + global `len()`

This is the first point where fully dynamic programs are possible. The largest
jump in expressiveness.

| Program | What dynamic lists enable |
|---|---|
| **Bubble / insertion / selection sort** | Mutate list in-place, `len()` for bounds |
| **Sieve of Eratosthenes** | Build a boolean list, mark composites |
| **Merge sort** (recursive) | Split at midpoint, merge into new lists |
| Stack class (`push` wraps `append`, `pop` wraps `pop`) | Encapsulate list behind an OOP interface |
| Queue class (ring-buffer or two-stack trick) | Explore trade-offs of different implementations |
| Collect all `input()` lines into a list, then process | Read until EOF (`nil`), then sort/search |
| Flatten nested list | Recursive descent on `list[i]` |
| Running histogram of numbers | `counts.append(0)` per bucket, increment |
| Higher-order `filter(pred, list)` | Build result list with `append` |

---

### Milestone 5+6 — `len()` + `str[i]` string indexing

| Program | What character-level access enables |
|---|---|
| Palindrome checker | Compare `s[i]` and `s[len(s)-1-i]` |
| String reversal | Build reversed string char by char |
| Caesar cipher / ROT13 | Shift each character's ASCII value |
| Anagram detector | Build sorted character frequency list |
| `split(s, delim)` → list of tokens | Walk `s[i]`, collect into list |
| Simple tokenizer for a calculator | Identify digits, operators, parens by char |
| Validate numeric input from `input()` | Check each `s[i]` is a digit or `.` |
| Run-length encoding | Count consecutive equal chars |

---

### Milestone 7 — `for (var x in seq)` / iterator protocol

`for-in` doesn't unlock new programs — it makes every existing program shorter
and enables user-defined iterable types.

| Program | What it showcases |
|---|---|
| `Range(start, end)` iterable class | Iterator protocol — `iter()` / `hasNext()` / `next()` |
| `Enumerate(list)` — yields `[i, val]` pairs | Composable iterators |
| `Zip(listA, listB)` — yields `[a, b]` pairs | Multiple iterables |
| `Map(fn, list)` / `Filter(pred, list)` as lazy iterators | Higher-order iteration without building intermediate lists |
| Rewrite sort / search programs using `for (var x in list)` | Demonstrate clarity improvement |

---

### Milestone 8 — File I/O

| Program | What it does |
|---|---|
| `wc` clone — count lines, words, chars | `readline()` loop, split on spaces |
| Grep-lite — print lines containing a substring | `readline()` + string search via `str[i]` |
| CSV reader → list of lists | `readline()` + `split(",")` |
| Config file parser (`key=value` lines) | `split("=")`, store in instance fields |
| Simple data pipeline | Read file → transform → write to new file |
| Append-only log writer | `open("log.txt", "a")` + `write()` in a loop |
| Line sorter | Read all lines with `readline()`, sort list, write back |

---

### Milestone 9 — Map/Dict type

| Program | What maps enable |
|---|---|
| **Word frequency counter** | `map.get(word)` + increment, then print sorted |
| **Memoized Fibonacci** | `cache.set(n, result)` — first real DP program |
| Phone book / contact manager | String → instance lookup, interactive CRUD via `input()` |
| Graph as adjacency map (`Map` of `List`) | BFS and DFS on user-defined graphs |
| Simple symbol table | Foundation for a mini-interpreter in Lox++ |
| Anagram grouping | `Map` keyed by sorted characters, `List` as value |
| **Huffman encoder** (capstone) | Character frequency map → priority queue → tree → encode |

---

### Milestone 10 — `seq[start:end]` slice syntax

| Program | What slicing enables |
|---|---|
| String trimming / windowing | `s[1:len(s)-1]` to strip first/last char |
| Subarray extraction | Pass `list[lo:hi]` to a sort helper instead of index arithmetic |
| Sliding-window algorithms | Iterate overlapping `list[i:i+k]` slices |
| Merge sort (cleaner split) | `list[0:mid]` and `list[mid:len(list)]` |
| Rotate a list | `list[k:] + list[:k]` (once `+` is defined for lists) |

---

### Milestone 11 — `match` expression, `enum`, destructuring

| Program | What pattern matching enables |
|---|---|
| Expression evaluator AST | `match node { Add(l, r) => ..., Num(n) => n }` |
| Option / Result type | `enum Option { Some(val) None }` with exhaustive match |
| Shape dispatch without `if`-chains | Class patterns: `match shape { Circle(r) => ... Rectangle(w, h) => ... }` |
| JSON-like value printer | Recursive match on value type |
| State machine | `match state { Idle => ... Running(job) => ... }` |
| Destructuring swap | `var {a, b} = expr` for packing/unpacking |
