# An Int type and bit operators

**Status:** superseded by `docs/proposals/integer-kind.md` (2026-10-23).
The owner approved this decision in #792 (2026-10-22) and dropped it in
#810 on 2026-10-23, before any node started. The reason is in the
proposal that replaces it, "Why this model and not the split". This
file binds nothing. It is kept as the record of the split model and of
the forks the owner decided under it.

## Owner decisions

The owner decided these forks.

| Fork | Decision |
|---|---|
| Purpose | Low-level control. A program can express a bit-mask data structure with exact bits. |
| Model | Two separate kinds, Int and Float. `type(1)` is `"Int"` and `type(1.0)` is `"Float"`. The name `"Number"` goes away, and no name means "Int or Float". |
| Range | Int is a signed 64-bit integer on every implementation. The inline width of the native VM is implementation-defined; the spec binds int64 only. |
| Overflow | Every Int operation wraps in two's complement, as in Go and Lua 5.3. |
| `/` | Always gives a Float. `7 / 2` is `3.5`, and `4 / 2` is `2.0`. |
| Floor division | A standard library function, `math.idiv(a, b)`. `//` stays the comment syntax. |
| Equality and order | `1 == 1.0` is `true`, and `m[1]` and `m[1.0]` are the same map entry. An Int and a Float compare by their exact mathematical values. |
| Printing | A Float always shows a `.` or an exponent (`1.0`). An Int shows its digits (`1`). The exact Float format is one table in the spec. |
| Literals | A decimal literal with no `.` is an Int. Hex `0x…` and binary `0b…` literals are Ints and give a 64-bit pattern, as in Java. No digit separators. |
| Bit operations | The operators `&`, `\|`, `^`, `~`, `<<`, `>>` and `>>>`. |
| Shifts | `>>` keeps the sign bit. `>>>` fills with zero bits. A shift count outside 0 to 63 is a runtime fault. |
| Bit operands | Ints only. Any other operand, an Instance included, is a type fault. No operator protocol (dunder) exists for the bit operators. |
| Indexing | An index and a slice bound must be an Int. A Float index is a type fault. |
| `math.floor`, `math.ceil`, `math.round` | Return an Int. A NaN, an infinity, or a value outside the int64 range is a runtime fault. |
| Conversions | `int(x)` and `float(x)` exist. `int` truncates toward zero and faults on a value that has no Int. |

## Why

The main purpose is low-level control. A program must be able to write a flag
set, a bitboard, a Bloom filter, a hash mix, or a packed field with exact bits.
A signed 64-bit integer is the familiar and basic tool for this.

Today a Number is an IEEE 754 double (`spec/03-types.md`, "Number"). An integer
above 2^53 loses precision, and the language has no bit operation.

The proposal serves the *memory* control under "No high-level facility removes
low-level control" in `VISION.md`: the program "can hold a 64-bit pattern as
one numeric value with every bit exact and apply a bit operation to it in one
step". Under "What counts as a language gap", a listed control is a
capability by definition, so the litmus test does not apply to this control.

No value does this today:

| Value | 64 exact bits | Number | Bit operation in one step |
|---|---|---|---|
| Number | No, only up to 2^53 | Yes | No |
| An 8-byte String | Yes | No | No |
| Two 32-bit halves, as a List or a class | Yes | No | No |

The two-halves form is in use in the repository: `examples/zlib.lox`, the
CRC32 in `examples/codecrafters/build-your-own-http-server/http.lox`, and the
idler task in `benchmarks/core/richards.lox` do bit work with `math.floor`,
`%` and `*`. A class with `__eq__` and `__hash__` makes two halves one value
and a map key. That emulation is not a number: it has no arithmetic and no
numeric order, and each bit operation is a loop. It is also slow.

### Performance

`VISION.md` puts performance on its own axis, so these numbers do not make the
case. They show the cost of the emulation that programs use today. Each row is
2000 rounds of xorshift64 (six bit operations a round) on a native `release`
build:

| Form | µs per round |
|---|---|
| Two 32-bit halves, one bit at a time (`floor`, `%`, `*`) | 118 |
| Two 32-bit halves, an 8-bit xor table | 22 |
| Plain Number arithmetic, three multiply-add-modulo steps | 0.45 |

Inline caches and a JIT do not remove this cost, because it is in the
algorithm: each bit operation is a loop. With the Int type, each bit
operation is one opcode. "Native layout" below gives the cost when the result
needs a box.

### FFI

The FFI (roadmap item 6) will pass `int64_t` and `uint64_t` values to C. A
Number cannot carry them exactly above 2^53. The FFI decision chooses its own
carrier. LuaJIT, for example, keeps a boxed 64-bit value in its FFI and no
integer kind in the core language. If this proposal is accepted first, the FFI
can use Int.

## Vision tensions

- **Wrap is silent.** "Fail loudly" is about undefined behavior. Wrap is
  defined behavior, and bit work needs it. But an arithmetic overflow gives no
  error. A program that must detect overflow checks the range itself. The owner
  decided this.
- **Shift counts fault.** In C, a shift count outside the word size is
  undefined. Here it is a runtime fault, so "Fail loudly" still applies.
- **"Simple".** Two numeric kinds and seven operators add surface. One
  mixed-kind table holds all the arithmetic rules, so a reader finds them in
  one place. The bit operators have no operator protocol, which keeps the
  protocol table and every backend smaller.
- **Cost of large Ints in the native VM.** An int64 does not fit inline in a
  NaN-boxed Value, so an operation whose result has high bits allocates (see
  "Native layout"). The *cost* control asks for a cost that the programmer
  can predict. The rule is simple: a result inside the inline range costs no
  allocation, and a result outside it costs one. This is the same kind of
  rule as for `+` on two Strings, which always allocates.

## Native layout

How the native VM holds an int64.

A Value is 64 bits. Every double and every int64 cannot both fit inline in 64
bits, so one of the two kinds needs a box for some values.

**A small Int inline, and a box for the rest.**

- The positive quiet-NaN space has 50 payload bits, and only the values 1 to 4
  are in use (`src/value.h`). An inline Int takes one tag bit of that space and
  a signed payload of 48 bits (−2^47 to 2^47 − 1). The tag is a high payload
  bit, so the tags 1 to 4 stay different from the inline Ints 0 to 4. One
  payload bit stays free.
- An Int outside that range is a heap object that holds one `int64_t`. It
  is immutable. The native VM must not box an Int that fits the inline range,
  so the cost rule stays "inside the range, no allocation". Equality, hashing and printing use the value, never the
  address, so a program cannot see if an Int is inline or boxed.
- The spec binds int64 only. The inline width is implementation-defined, so a
  change of the width, or a JIT that keeps Ints in registers, is not a
  language change. The JVM backend uses `long` with no such split.
- Doubles do not change. Float code keeps its speed.

**Cost of a box.** On a `release` build, one allocation of a small object adds
about 60 to 80 ns to a loop step, and about 100 ns when the collector has
100,000 live objects to mark. A round of xorshift64 on 64-bit values can box
each of its six results, so the worst case adds about 0.4 µs. The fastest
emulation takes 22 µs a round.

**Alternatives and why they are not chosen:**

| Alternative | Reason |
|---|---|
| A 128-bit Value (tag plus payload) | Every stack slot, List element and map entry doubles in size. Every consumer that knows the NaN-box layout changes, QBE too. |
| Inline 63-bit Ints, boxed Floats | Float code allocates. It still does not hold 64 bits. |
| Every Int boxed | Indices and loop counters allocate. |

A later JIT can keep Ints in registers with no box. This proposal does not
depend on it.

## Semantics (draft spec text)

### Lexical

- `DIGIT+` is an Int literal. `DIGIT+ "." DIGIT+` is a Float literal.
- A decimal Int literal outside the int64 range is a compile-time error, with
  one exception, as in Java: the literal `9223372036854775808` is valid when
  it is the direct operand of a unary `-`. So `-9223372036854775808` is the
  smallest Int, and `- (9223372036854775808)` is an error. The standard
  library also gives `math.int_min` and `math.int_max`.
- `0x` followed by hex digits, and `0b` followed by binary digits, are Int
  literals. Each gives a 64-bit pattern, as in Java: `0xFFFFFFFFFFFFFFFF` is
  `-1` and `0x8000000000000000` is `math.int_min`. A literal with more than
  64 bits is a compile-time error. There are no digit separators.
- New tokens: `&`, `|`, `^`, `~`, `<<`, `>>`, `>>>`. The scanner does not use
  any of them today (`src/scanner.cpp`: `<` and `>` take only `=` after them).
  The planned pipe operator `|>` (`docs/decisions/dynamic-functional-programming.md`)
  does not clash: the scanner reads the longest token.

### Precedence

The bit operators go between comparison and `+ -`, in the Python order. So
`x & m == 0` means `(x & m) == 0`, and the C trap does not occur. This is the
full table that replaces the one in `spec/02-syntax.md`, "Operator
Precedence"; the expression grammar changes to match.

| Precedence | Operators | Associativity |
|---|---|---|
| 1 (lowest) | `=` (assignment) | Right |
| 2 | `or` | Left |
| 3 | `and` | Left |
| 4 | `==` `!=` | Left |
| 5 | `<` `<=` `>` `>=` `in` | Left |
| 6 | `\|` | Left |
| 7 | `^` | Left |
| 8 | `&` | Left |
| 9 | `<<` `>>` `>>>` | Left |
| 10 | `+` `-` | Left |
| 11 | `*` `/` `%` | Left |
| 12 | `!` `-` `~` (unary) | Right (prefix) |
| 13 (highest) | `()` (function call), `.` (property access), `[]` (index) | Left |

### Arithmetic

| Operation | Int, Int | Any Float operand |
|---|---|---|
| `a + b`, `a - b`, `a * b` | Int, wraps | Float |
| `a / b` | Float | Float |
| `a % b` | Int, floor semantics; `b == 0` faults; `math.int_min % -1` is `0` | Float, floor semantics (as today) |
| `-a` | Int, wraps (`-math.int_min` is `math.int_min`) | Float |
| `math.idiv(a, b)` | Int, floor; `b == 0` faults; `math.idiv(math.int_min, -1)` wraps to `math.int_min` and does not fault | Float, `floor(a / b)` |

In C, `INT_MIN % -1` and `INT_MIN / -1` are undefined. Here both are defined,
and neither is a fault.

The existing operator protocols (`__add__` and the others) do not change.

### Bit operators

- The operands must be Ints. Any other operand, also an integral Float and
  also an Instance, is a type fault.
- `~x` inverts every bit. `a & b`, `a | b` and `a ^ b` work on all 64 bits.
- `x << n` shifts left and wraps. `x >> n` shifts right and copies the sign
  bit. `x >>> n` shifts right and fills with zero bits.
- A shift count `n` outside 0 to 63 is a runtime fault.
- The bit operators have no operator protocol. The purpose of the operators is
  a control over machine words. A wider pattern (a 128-bit bitboard, a Bloom
  filter, a BigInt) is a class over Ints with methods, and the spelling of a
  library is not a language gap. The spec's protocol table gets a row that
  says so, so the check script counts it. A protocol can come later as its
  own proposal, with a program that needs it as the probe; the addition is
  additive, because no program that runs today changes its meaning.

### Equality, ordering and hashing

- An Int and a Float compare by their exact mathematical value. An
  implementation must not convert the Int to a double first:
  `9007199254740993 == 9007199254740992.0` is `false`.
- `<`, `<=`, `>` and `>=` between an Int and a Float use the same rule: the
  exact mathematical values, with no conversion of the Int to a double first.
  So `9007199254740993 > 9007199254740992.0` is `true`. A NaN operand gives
  `false` for all four, as it does today.
- An integral Float in the int64 range hashes as the equal Int, so `m[1]` and
  `m[1.0]` are one entry. `-0.0` and `0` are equal and hash the same.
- NaN stays an invalid map key.

### Printing

- An Int prints as its decimal digits, with a `-` sign when it is negative.
- A Float prints with a `.` or an exponent: `1.0`, `3.5`, `1e+20`. The exact
  format is one table in the spec, and that table is a deliverable of the
  first mission node, because every `CHECK` line that shows a Float depends
  on it. The native `stringify` (`src/value.cpp`) and the JVM `formatNumber`
  (`runtime/jvm/src/lox/LoxOps.java`) both follow it.

### Indexing

- An index and a slice bound must be an Int.
- A Float index, `xs[2.0]` included, is a type fault. The error message names
  `math.idiv`, because `xs[n / 2]` is the case a user meets first. The
  `IndexNotIntegerError` row of the fault table becomes this type fault.

### Standard library

- `len`, `ord`, `math.floor`, `math.ceil` and `math.round` return an Int.
  For `floor`, `ceil` and `round`, a NaN, an infinity, or a result outside the
  int64 range is a runtime fault. A program tests the range first when it
  needs to.
- `chr`, `exit`, the port arguments, `sleep` and `read_bytes(n)` take an Int.
- `type()` returns `"Int"` or `"Float"`.
- `int(x)`: an Int gives itself. A Float truncates toward zero; a NaN, an
  infinity, or a value outside the int64 range is a runtime fault. A String
  parses as a decimal integer, with an optional sign, or faults.
- `float(x)`: a Float gives itself. An Int converts exactly up to 2^53 and
  rounds to the nearest double beyond. A String parses as a decimal number, or
  faults.
- `math.idiv(a, b)`, `math.int_min` and `math.int_max` are added to `math`.
- Out of scope: helpers such as popcount, count of leading or trailing zeros,
  and rotate. A later proposal can add them.

### Messages

An Int and a Float are always messages, as a Number is today
(`spec/03-types.md`, the message rule).

## Consumers

Each consumer and what it owes under the backend roles in `VISION.md`.

| Consumer | Obligation | What changes |
|---|---|---|
| Native VM | The full spec | `src/value.h` (the inline Int tag and the boxed Int, see "Native layout"), `src/value.cpp` (equality, `hashValue`, `ValueHash`, `stringify`), `src/chunk.h` (new opcodes), `src/vm.cpp` (fast paths), `src/runtime.cpp` (slow paths, `checkSequenceIndex`, `opModulo`), `src/scanner.cpp`, `src/parser.cpp`, `src/compiler.cpp` (`number()`, `binary()`, the stack-height table), `src/math.cpp`, `src/stdlib/globals.cpp`, `src/stdlib/reflect_api.cpp`, and the os, net, process and task natives |
| JVM backend | The spec, as the semantic oracle | `java.lang.Long` beside `Double` in `runtime/jvm/src/lox/LoxOps.java`, and new emission in `src/backend/jvm_emitter.cpp`. Java `long` already wraps and has `>>>` |
| QBE backend | What its experiment needs | It writes NaN-box bit patterns into its output (`src/backend/qbe_emitter.cpp`, `src/backend/rt_capi.cpp`). The new representation must not break it. Expected-failure entries are permitted |
| Bootstrap interpreter | The spec | `parseNumber`, `isLoxNum`, `reflectTypeName`, the index checks, the scanner, the parser precedence, and the new operators in `bootstrap/loxpp_interpreter.lox`. It can use host Ints and host bit operators after the native VM has them |
| Resolver | Accept the new expressions | `src/analyze.cpp` has no number logic |
| LSP | Correct documentation | The 29 `Number` mentions in `src/lsp/stdlib_docs.cpp` become `Int` or `Float` |
| tree-sitter grammar | Parse every program | `editors/tree-sitter-loxpp/grammar.js`: tokens, precedence, Int and Float literals |
| Editor syntaxes | Highlight the new tokens | `editors/loxpp-vscode/syntaxes/lox.tmLanguage.json`, the `editors/loxpp.tmbundle` syntaxes, `editors/loxpp.nvim/syntax/lox.vim` |
| Fault table check | Cover the new rows | `tools/check_fault_table.py` |
| Spec | Define it | `spec/01-lexical.md`, `spec/02-syntax.md`, `spec/03-types.md`, `spec/04-semantics.md` (operators, precedence, the protocol table, indexing, the fault table), `spec/05-stdlib.md` |

## Countable sets

The spec will list these sets. Each gets a table and a script that checks the
table is complete, before backend work starts:

- the mixed-kind operator table (each operator against each pair of kinds);
- the precedence table;
- the operator protocol table, with a row for the bit operators that says
  "no protocol";
- the compile-time rejections: a decimal Int literal outside the int64 range
  (except the unary-minus case), and a hex or binary literal of more than 64
  bits;
- the new runtime fault rows: a bit operator on a non-Int, a shift count out
  of range, Int `% 0`, `math.idiv` by zero, a Float index, `floor`, `ceil` or
  `round` on a NaN, an infinity, or a value outside the int64 range, `int()`
  on the same, and `int()` or `float()` on a String that does not parse;
- the edge rows that do not fault, so the check script sees them:
  `math.int_min % -1` is `0`, `math.idiv(math.int_min, -1)` is
  `math.int_min`, `-math.int_min` is `math.int_min`, and `math.int_max + 1`
  is `math.int_min`.

## Program impact

The repository has about 500 `.lox` programs and 1441 `CHECK` lines.

- A whole-number Float prints `5.0`, not `5`. Each `CHECK` line that shows a
  result of `/` or of Float math changes.
- 38 `floor(a / b)` patterns keep their meaning, because `floor` returns an
  Int. A program that indexes with `a / b` directly changes to `math.idiv`.
- Two `CHECK` lines in `%g` exponent form (`3.6288e+06`, `9.22746e+06`) change
  when the value is an Int.
- Four programs test `type(x) == "Number"` (`examples/bigint.lox`,
  `examples/json.lox`, the SQLite example, and one translation probe) and
  change to the two names. The bootstrap interpreter's `reflectTypeName`
  changes with them.

The mission updates every affected program in the same PRs, with no
deprecation period (`AGENTS.md`). It also adds programs that use masks (a flag
set, a bitboard) as probes of the purpose.

## Mission sketch

This is not a node list. The DAG is in the tracking issue, #810.

1. Spec, the Float print format table, shared probes, and expected-failure
   files. The probes cover compile-time behavior (the literal rejections and
   the unary-minus exception) and run-time behavior, as `AGENTS.md` requires
   of the differential gate.
2. Native: the value layer, then arithmetic and bit operators, then the
   standard library and printing.
3. JVM.
4. Bootstrap interpreter.
5. Editors and LSP.
6. The update of every affected program.
7. QBE, last.
