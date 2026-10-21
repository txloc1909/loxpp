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
above 2^53 loses precision, and the language has no bit operation. A program
cannot get exact 64-bit patterns from doubles by composition, so this is a
capability gap, not tedium.

The proposal serves these goals in `VISION.md`:

- **"No high-level facility removes low-level control"**, the *memory*
  control: a program "can reach native code and raw bytes through an explicit
  interface". Exact 64-bit patterns are the base for bytes, masks and packed
  data.
- **"What counts as a language gap"**: bit-exact int64 work cannot be built
  from the primitives that the language has.
- The *cost* control: each bit operator is one opcode with a cost that the
  programmer can predict. It is not a native call.
- **"Performance"**: the native VM gets an Int fast path. Indices and loop
  counters do not go through a double.

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
- **Cost of large Ints in the native VM.** A NaN-boxed Value has about 50 free
  payload bits (`src/value.h`), so an int64 does not fit inline. If the native
  VM boxes the Ints that do not fit, an operation on a mask with high bits
  can allocate. Bit masks often use the high bits. The decision stage must
  choose the native layout with this cost in view (see "Open questions").

## Semantics (draft spec text)

### Lexical

- `DIGIT+` is an Int literal. `DIGIT+ "." DIGIT+` is a Float literal.
- A decimal Int literal outside the int64 range is a compile-time error.
- Open: hexadecimal `0x…` and binary `0b…` Int literals. Recommendation: add
  them, because a programmer writes masks in hex and binary.
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
| `a % b` | Int, floor semantics; `b == 0` faults | Float, floor semantics (as today) |
| `-a` | Int, wraps (`-INT_MIN` is `INT_MIN`) | Float |
| `idiv(a, b)` | Int, floor; wraps (`idiv(INT_MIN, -1)`); `b == 0` faults | Float, `floor(a / b)` |

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
| Native VM | The full spec | `src/value.h` (an Int representation), `src/value.cpp` (equality, `hashValue`, `ValueHash`, `stringify`), `src/chunk.h` (new opcodes), `src/vm.cpp` (fast paths), `src/runtime.cpp` (slow paths, `checkSequenceIndex`, `opModulo`), `src/scanner.cpp`, `src/parser.cpp`, `src/compiler.cpp` (`number()`, `binary()`, the stack-height table), `src/math.cpp`, `src/stdlib/globals.cpp`, `src/stdlib/reflect_api.cpp`, and the os, net, process and task natives |
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
  Int `% 0`, `idiv` by zero, a Float index (if the owner accepts that).

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
7. The native Value layout for the full int64 range: an inline small Int plus
   a heap box, or a different scheme. The choice sets the cost of masks with
   high bits.

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
