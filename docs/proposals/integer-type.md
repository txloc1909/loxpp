# An Int type and bit operators

**Status:** proposal. Waits for the owner's approval. On acceptance it moves to
`docs/decisions/` and a mission tracking issue opens.

## Owner decisions

The owner decided these forks on 2026-10-21.

| Fork | Decision |
|---|---|
| Purpose | Low-level control. A program can express a bit-mask data structure with exact bits. |
| Model | Two separate kinds, Int and Float. `type(1)` and `type(1.0)` are different. |
| Range | Int is a signed 64-bit integer on every implementation. |
| Overflow | Every Int operation wraps in two's complement, as in Go and Lua 5.3. |
| `/` | Always gives a Float. `7 / 2` is `3.5`, and `4 / 2` is `2.0`. |
| Floor division | A standard library function `idiv(a, b)`. `//` stays the comment syntax. |
| Equality | `1 == 1.0` is `true`. `m[1]` and `m[1.0]` are the same map entry. |
| Printing | A Float always shows a `.` or an exponent (`1.0`). An Int shows its digits (`1`). |
| Bit operations | The operators `&`, `\|`, `^`, `~`, `<<`, `>>` and `>>>`. |
| Shifts | `>>` keeps the sign bit. `>>>` fills with zero bits. A shift count outside 0 to 63 is a runtime fault. |

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

The review of this proposal (PR #792) gave a pure Lox++ library for the
two-halves form that agrees with a Python oracle. A class with `__eq__` and
`__hash__` makes it one value and a map key. That emulation is not a number:
it has no arithmetic and no numeric order, and each bit operation is a loop.
It is also slow (see "Performance").

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
  one place.
- **Cost of large Ints in the native VM.** An int64 does not fit inline in a
  NaN-boxed Value, so an operation whose result has high bits allocates (see
  "Native layout"). The *cost* control asks for a cost that the programmer
  can predict. The rule is simple: a result inside the inline range costs no
  allocation, and a result outside it costs one. This is the same kind of
  rule as for `+` on two Strings, which always allocates.

## Native layout

This section answers the question of how the native VM holds an int64.

A Value is 64 bits. Every double and every int64 cannot both fit inline in 64
bits, so one of the two kinds needs a box for some values.

**Recommendation: a small Int inline, and a box for the rest.**

- The positive quiet-NaN space has 50 payload bits, and only the values 1 to 4
  are in use (`src/value.h`). An inline Int takes one tag bit of that space and
  a signed payload of 48 bits (−2^47 to 2^47 − 1). The tag is a high payload
  bit, so the tags 1 to 4 stay different from the inline Ints 0 to 4. One
  payload bit stays free.
- An Int outside that range is a heap object that holds one `int64_t`. It
  is immutable. Equality, hashing and printing use the value, never the
  address, so a program cannot see if an Int is inline or boxed.
- The spec binds int64 only. The inline width is implementation-defined, and
  the JVM backend uses `long` with no such split.
- Doubles do not change. Float code keeps its speed.

**Cost of a box.** On the same `release` build, one allocation of a small
object adds about 60 ns to a loop step (205 ns becomes 265 ns, three runs).
A round of xorshift64 on 64-bit values can box each of its six results, so the
worst case adds about 0.4 µs. The fastest emulation takes 22 µs a round.

**Alternatives and why they are not recommended:**

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
- Open: hexadecimal `0x…` and binary `0b…` Int literals. Recommendation: add
  them, because a programmer writes masks in hex and binary. A hex or binary
  literal gives a 64-bit pattern, so `0xFFFFFFFFFFFFFFFF` is `-1` and
  `0x8000000000000000` is `math.int_min`, as in Java. A literal with more than
  64 bits is a compile-time error.
- New tokens: `&`, `|`, `^`, `~`, `<<`, `>>`, `>>>`. The scanner does not use
  any of them today (`src/scanner.cpp`: `<` and `>` take only `=` after them).
  The planned pipe operator `|>` (`docs/decisions/dynamic-functional-programming.md`)
  does not clash: the scanner reads the longest token.

### Precedence

The bit operators go between comparison and `+ -`, in the Python order. So
`x & m == 0` means `(x & m) == 0`, and the C trap does not occur.

| Precedence | Operators | Associativity |
|---|---|---|
| 5 | `<` `<=` `>` `>=` `in` | Left |
| 6 | `\|` | Left |
| 7 | `^` | Left |
| 8 | `&` | Left |
| 9 | `<<` `>>` `>>>` | Left |
| 10 | `+` `-` | Left |
| 11 | `*` `/` `%` | Left |
| 12 | `!` `-` `~` (unary) | Right (prefix) |

The rows above 5 do not change. `spec/02-syntax.md`, "Operator Precedence" and
the expression grammar, change to match.

### Arithmetic

| Operation | Int, Int | Any Float operand |
|---|---|---|
| `a + b`, `a - b`, `a * b` | Int, wraps | Float |
| `a / b` | Float | Float |
| `a % b` | Int, floor semantics; `b == 0` faults; `math.int_min % -1` is `0` | Float, floor semantics (as today) |
| `-a` | Int, wraps (`-math.int_min` is `math.int_min`) | Float |
| `idiv(a, b)` | Int, floor; `b == 0` faults; `idiv(math.int_min, -1)` wraps to `math.int_min` and does not fault | Float, `floor(a / b)` |

In C, `INT_MIN % -1` and `INT_MIN / -1` are undefined. Here both are defined,
and neither is a fault.

The existing operator dunders (`__add__` and the others) do not change.

### Bit operators

- The operands must be Ints. A Float operand, also an integral one, is a
  runtime fault.
- `~x` inverts every bit. `a & b`, `a | b` and `a ^ b` work on all 64 bits.
- `x << n` shifts left and wraps. `x >> n` shifts right and copies the sign
  bit. `x >>> n` shifts right and fills with zero bits.
- A shift count `n` outside 0 to 63 is a runtime fault.
- Open: dunders for the new operators (`__and__`, `__or__`, `__xor__`,
  `__lshift__`, `__rshift__`, `__urshift__`, `__invert__`, and the reflected
  forms). Recommendation: add them, to agree with the `Protocol` enum in
  `src/runtime.h`.

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
  format is one table in the spec. The native `stringify` (`src/value.cpp`)
  and the JVM `formatNumber` (`runtime/jvm/src/lox/LoxOps.java`) both follow it.

### Indexing

- An index and a slice bound must be an Int.
- Open: is `xs[2.0]` a fault? Recommendation: yes, because a Float index is
  almost always an error. The `IndexNotIntegerError` row then changes to a
  type fault.

### Standard library

- `len`, `ord`, `math.floor`, `math.ceil` and `math.round` return an Int.
- `chr`, `exit`, the port arguments, `sleep` and `read_bytes(n)` take an Int.
- `type()` returns a new name for each kind (see "Open questions").
- Open: the conversions `int(x)` and `float(x)`. Recommendation: add them.
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
| LSP | Correct documentation | `-> Number` signatures in `src/lsp/stdlib_docs.cpp` |
| tree-sitter grammar | Parse every program | `editors/tree-sitter-loxpp/grammar.js`: tokens, precedence, Int and Float literals |
| Editor syntaxes | Highlight the new tokens | `editors/loxpp-vscode/syntaxes/lox.tmLanguage.json`, the `editors/loxpp.tmbundle` syntaxes, `editors/loxpp.nvim/syntax/lox.vim` |
| Fault table check | Cover the new rows | `tools/check_fault_table.py` |
| Spec | Define it | `spec/01-lexical.md`, `spec/02-syntax.md`, `spec/03-types.md`, `spec/04-semantics.md` (operators, precedence, indexing, the fault table), `spec/05-stdlib.md` |

## Countable sets

The spec will list these sets. Each gets a table and a script that checks the
table is complete, before backend work starts:

- the mixed-kind operator table (each operator against each pair of kinds);
- the precedence table;
- the new fault rows: a bit operator on a Float, a shift count out of range,
  Int `% 0`, `idiv` by zero, a Float index (if the owner accepts that);
- the edge rows that do not fault, so the check script sees them:
  `math.int_min % -1` is `0`, `idiv(math.int_min, -1)` is `math.int_min`,
  `-math.int_min` is `math.int_min`, and `math.int_max + 1` is
  `math.int_min`.

## Program impact

The repository has about 500 `.lox` programs and 1441 `CHECK` lines.

- A whole-number Float prints `5.0`, not `5`. Each `CHECK` line that shows a
  result of `/` or of Float math changes.
- 38 `floor(a / b)` patterns change if `floor` returns an Int.
- Two `CHECK` lines in `%g` exponent form (`3.6288e+06`, `9.22746e+06`) change
  when the value is an Int.

The mission updates every affected program in the same PRs, with no
deprecation period (`AGENTS.md`). It also adds programs that use masks (a flag
set, a bitboard) as probes of the purpose.

## Open questions

1. `type()` names. Recommendation: `"Int"` and `"Float"`, to agree with the
   capitalized names today (`"Number"`, `"String"`).
2. Hex and binary literals. Recommendation: yes.
3. `xs[2.0]`. Recommendation: a fault.
4. `math.floor(1e300)`: a Float result, or a fault?
5. `int(x)` and `float(x)`, and where `idiv` lives (globals or `math`).
6. Dunders for the new operators. Recommendation: yes.

The question of the native Value layout has an answer in "Native layout".

## Mission sketch

This is not a node list. The DAG goes in the tracking issue after acceptance.

1. Spec, shared probes, and expected-failure files.
2. Native: the value layer, then arithmetic and bit operators, then the
   standard library and printing.
3. JVM.
4. Bootstrap interpreter.
5. Editors and LSP.
6. The update of every affected program.
7. QBE, last.
