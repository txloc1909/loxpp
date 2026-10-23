# An Int kind beside Number

**Status:** accepted. The owner approved it in #823 (2026-10-23). Tracking
issue: #824. It replaces `docs/records/integer-type-split-model.md`, the
decision of #792 that the owner dropped in #810 on 2026-10-23.

## Owner decisions

The owner decided these forks on 2026-10-23.

| Fork | Decision |
|---|---|
| Purpose | Low-level control. A program can express a bit-mask data structure with exact bits. Unchanged from #792. |
| Model | Number stays the one everyday kind, an IEEE 754 double. Int is a second kind, a signed 64-bit integer. The two kinds do not mix. No name means "Int or Number". |
| Literal | A decimal literal with the suffix `i` is an Int: `1i`, `255i`. A hex `0x…` or binary `0b…` literal is an Int with no suffix. A literal with no suffix and no `0x` or `0b` prefix stays a Number. |
| Mixing | An operator with one Int operand and one Number operand is a type fault. `1 == 1i` is `false` and `1 != 1i` is `true`, because `==` never faults across kinds. |
| Range | Int is a signed 64-bit integer on every implementation. The inline width of the native VM is implementation-defined. |
| Overflow | Every Int operation wraps in two's complement. |
| `/` and `%` | On two Ints, `/` is floor division and `%` is floor modulo, so `a == (a / b) * b + a % b`. A zero divisor is a runtime fault. |
| Conversions | `int(x)` converts an integral Number or a decimal String to an Int. A fractional Number or a hex String is a runtime fault. `float(x)` converts an Int or a String to a Number. |
| Indexing | A List or String index, a slice bound, and an enum field index accept an integral Number or an Int. |
| Helpers | A dedicated module `integer` for Int functions and constants. The name `int` is the conversion function, so the module cannot take it. |
| Printing | An Int prints as its decimal digits, the same text as the equal Number. `str(1i)` is `"1"`. |
| Bit operations | The operators `&`, `\|`, `^`, `~`, `<<`, `>>` and `>>>`, on Ints only. No operator protocol. Unchanged from #792. |
| Shifts | `>>` keeps the sign bit. `>>>` fills with zero bits. A shift count outside 0 to 63 is a runtime fault. Unchanged from #792. |
| UInt | No unsigned kind. One inline tag bit stays reserved for a later one. Unsigned views of an Int are functions in the Int module. |

## Why this model and not the split

The decision of #792 split Number into Int and Float with `1 == 1.0`. Under
that model every in-range integer has two representations. Equality, hashing,
the compiler's constant pool, the map keys and the literal patterns each need
a cross-kind rule, and every whole-number Float prints as `5.0`, which changes
about 1441 `CHECK` lines on four consumers. The review of #792 made this
objection: the change "splits Number, the most basic type". The alternative
that review offered was a library over Numbers, which cannot hold 64 bits. A
second kind that does not mix was not considered there.

Every dynamic language with two numeric kinds pays the cross-kind cost in one
of two ways. Python and Lua 5.3 keep one equality and one hash across kinds.
Ruby and Scheme keep two equalities. JavaScript BigInt takes a third way: a
BigInt is a separate kind, `1n + 1` is a type error, `BigInt(x)` and
`Number(x)` convert, the bit operators work on it, and `1n === 1` is false.
This proposal takes the BigInt way, because Lox++ already has the rule it
needs: no implicit coercion. `1 + "1"` is a type fault today
(`spec/03-types.md`), and `1 + 1i` is the same fault for the same reason.

What the model keeps untouched: every existing program, every `CHECK` line,
the Number print format, the index rules, `len`, `ord`, `math.floor`, the
jlox suite that runs through `bootstrap/lox_interpreter.lox`, and the four
backends' Number paths. The change is additive, with one lexical exception:
a decimal literal, whole or fractional, written directly before `in` or `if`
(constraint 4). A program that does not have that form has the same meaning
after the change as before it.

What the model costs: an Int literal needs its suffix, so `x & (x - 1i)` and
not `x & (x - 1)`, and a hash-mix loop over bytes writes `int(ord(c))`. The
four purposes of the decision, a flag set, a bitboard, a Bloom filter and a
hash mix, are specialist code, so this cost falls only on them. The
JavaScript precedent shows that programmers accept it.

## Why not UInt

An unsigned kind reuses the Int machinery, but it adds a second inline tag,
a second suffix, a second branch in every operator dispatch, a 3 by 3 operand
table, six conversions and a JVM wrapper over `Long` with the
`compareUnsigned` family. The estimate is half of the Int effort again. No
dynamic language has a separate unsigned kind. Java covers the same ground
with functions on its signed kind, and this proposal does the same: `>>>`
and the `u`-prefixed functions in the Int module. The one free inline tag bit
stays reserved, so a later UInt is an additive proposal with no layout change.
The FFI decision chooses its own carrier.

## Why an Int at all

This section is carried from #792. The main purpose is low-level control.
A program must be able to write a flag set, a bitboard, a Bloom filter, a
hash mix, or a packed field with exact bits. A signed 64-bit integer is the
familiar tool for this.

Today a Number is an IEEE 754 double (`spec/03-types.md`, "Number"). An
integer above 2^53 loses precision, and the language has no bit operation.
`examples/zlib.lox`, the CRC32 in
`examples/codecrafters/build-your-own-http-server/http.lox` and the idler
task in `benchmarks/core/richards.lox` do bit work with `math.floor`, `%`
and `*`, and each bit operation is a loop. A two-halves class is not a
number: it has no arithmetic and no numeric order. A library over 53-bit
Numbers cannot hold a 64-square bitboard.

This serves the *memory* control under "No high-level facility removes
low-level control" in `VISION.md`: the program "can hold a 64-bit pattern as
one numeric value with every bit exact and apply a bit operation to it in one
step". A listed control is a capability by definition, so the litmus test
does not apply. `VISION.md` needs no change for this model.

## Vision tensions

- **Wrap is silent.** "Fail loudly" is about undefined behavior. Wrap is
  defined behavior, and bit work needs it. A program that must detect
  overflow checks the range itself. The owner decided this in #792 and keeps
  it.
- **The suffix.** A second literal form adds surface. The alternative, a
  Number that becomes an Int by context, is implicit coercion, which the
  language refuses everywhere else.
- **Two integer-shaped values.** `1` and `1i` are different values that print
  the same. `type()` tells them apart, and `==` across them is `false`, as
  `1 == "1"` is. A program that needs to know the kind asks `type()`.
- **"Simple".** One kind, one suffix, seven operators, two conversions, one
  module. The operand table has one rule for every mixed cell: a type fault.

## Semantics (draft spec text)

### Lexical

- `DIGIT+` is a Number literal, and `DIGIT+ "." DIGIT+` is a Number literal,
  as today.
- `DIGIT+ "i"` is an Int literal. `DIGIT+ "." DIGIT+ "i"` is a compile-time
  error. The suffix is part of the token, so `1 i` is two tokens and a syntax
  error, as it is today.
- A decimal Int literal outside the int64 range is a compile-time error, with
  one exception, as in Java: a decimal Int literal whose value is
  9223372036854775808 is valid when it is the direct operand of a unary `-`.
  So `-9223372036854775808i` is the smallest Int, and
  `-(9223372036854775808i)` is an error.
- `0x` followed by hex digits, and `0b` followed by binary digits, are Int
  literals and take no suffix. Each gives a 64-bit pattern, as in Java:
  `0xFFFFFFFFFFFFFFFF` is `-1i` and `0x8000000000000000` is the smallest
  Int. A literal with more than 64 bits is a compile-time error. There are
  no digit separators.
- New tokens: `&`, `|`, `^`, `~`, `<<`, `>>`, `>>>`. The scanner does not use
  any of them today. The planned pipe operator `|>`
  (`docs/decisions/dynamic-functional-programming.md`) does not clash: the
  scanner reads the longest token.

### Precedence

The bit operators go between comparison and `+ -`, in the Python order. So
`x & m == 0i` means `(x & m) == 0i`, and the C trap does not occur. This is
the full table that replaces the one in `spec/02-syntax.md`, "Operator
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

### Operand kinds

| Operation | Int, Int | Number, Number | Int, Number or Number, Int |
|---|---|---|---|
| `a + b`, `a - b`, `a * b` | Int, wraps | Number, as today | Type fault |
| `a / b` | Int, floor division; `b == 0i` faults; the smallest Int divided by `-1i` wraps to itself | Number, as today | Type fault |
| `a % b` | Int, floor modulo; `b == 0i` faults; the smallest Int modulo `-1i` is `0i` | Number, as today | Type fault |
| `a < b`, `a <= b`, `a > b`, `a >= b` | Boolean | Boolean, as today | Type fault |
| `a == b`, `a != b` | By value | By value, as today | `false` and `true`, never a fault |
| `a & b`, `a \| b`, `a ^ b`, `a << b`, `a >> b`, `a >>> b` | Int | Type fault | Type fault |
| `-a` | Int, wraps; the negation of the smallest Int is itself | Number, as today | — |
| `~a` | Int | Type fault | — |
| `!a` | `false`: an Int is always truthy, `0i` included | As today | — |

The existing operator protocols (`__add__` and the others) do not change. An
Instance operand follows its protocol, as today. The bit operators have no
protocol: an Instance operand is a type fault. The spec's protocol table gets
a row that says so, so the check script counts it.

In C, `INT64_MIN / -1` and `INT64_MIN % -1` are undefined. Here both are
defined, and neither is a fault.

### Bit operators

- The operands must be Ints. A Number operand, an integral one included, is a
  type fault.
- `~x` inverts every bit. `a & b`, `a | b` and `a ^ b` work on all 64 bits.
- `x << n` shifts left and wraps. `x >> n` shifts right and copies the sign
  bit. `x >>> n` shifts right and fills with zero bits.
- The shift count `n` must be an Int. A count outside `0i` to `63i` is a
  runtime fault.

### Equality, ordering and hashing

- Two Ints are equal when they hold the same value. An Int and a Number are
  never equal: `1 == 1i` is `false`. An Int and a value of any other kind are
  never equal, as today.
- An Int hashes by its value. `1` and `1i` are two different map keys.
- An Int is a valid map key. No Int is NaN, so no Int is an invalid key.
- A literal pattern in `match` follows `==`: the pattern `1i` matches `1i`
  and not `1`.

### Printing

- An Int prints as its decimal digits, with a `-` sign when it is negative:
  `str(1i)` is `"1"` and `str(-1i)` is `"-1"`. This is the same text as the
  equal Number.
- The Number format does not change here. It changes in
  `number-print-format.md` (#834), which replaces `%g` with the shortest
  decimal representation and makes the parity sentence above true for every
  value, not only the small ones.

### Indexing

- A List or String index, a slice bound, and an enum field index accept an
  integral Number, as today, or an Int. The range checks are the same for
  both.
- The fault rows for a fractional or non-numeric index do not change.

### Conversions

- `int(x)`: an Int gives itself. An integral Number inside the int64 range
  gives the equal Int. A fractional Number, a NaN, an infinity, or a Number
  outside the range is a runtime fault, as JavaScript `BigInt(1.5)` throws.
  A program that wants a rounding writes `int(math.floor(x))` or
  `int(math.round(x))`, which name the rounding at the site. A String parses
  as a decimal integer with an optional sign, or faults. A hex String faults:
  `int()` parses decimal only, and a hex String goes through
  `integer.parse_unsigned` or a program's own loop.
- `float(x)`: a Number gives itself. An Int converts exactly up to 2^53 and
  rounds to the nearest double beyond. A String parses as a decimal number, or
  faults. The function gives a Number; its name pairs with `int`.

### The Int module

The module is `integer`.

| Name | Meaning |
|---|---|
| `integer.min_value`, `integer.max_value` | The smallest and the largest Int. |
| `integer.abs(x)` | The absolute value. `abs(min_value)` wraps to `min_value`. |
| `integer.min(a, b)`, `integer.max(a, b)` | The smaller and the larger Int. |
| `integer.udiv(a, b)`, `integer.urem(a, b)` | Division and remainder with both operands read as unsigned 64-bit values. `b == 0i` faults. |
| `integer.ult(a, b)` | `true` when `a` is below `b` with both read as unsigned. |
| `integer.ustr(x)` | The decimal digits of `x` read as unsigned: `ustr(-1i)` is `"18446744073709551615"`. |
| `integer.parse_unsigned(s)` | The Int whose bit pattern is the unsigned decimal `s`, or a fault when `s` does not parse or exceeds 2^64 − 1. |

Every argument must be an Int, or the call is a type fault. The `math`
module does not change: its functions take Numbers, as today.

Out of scope: popcount, count of leading or trailing zeros, and rotate. A
later proposal can add them to the module.

### Standard library

- `len`, `ord`, `chr`, `exit`, `sleep`, `read_bytes`, the port arguments and
  `math.floor`, `math.ceil`, `math.round` do not change. They take and give
  Numbers.
- `type()` returns `"Int"` for an Int. The `type()` table in
  `spec/05-stdlib.md` gets the row, and the sendable table follows it.

### Messages

An Int is always a message, as a Number is (`spec/03-types.md`, the message
rule).

## Native layout

A Value is 64 bits. Every double and every int64 cannot both fit inline, so
an Int needs a box for some values. This is carried from #792.

- The positive quiet-NaN space has 50 payload bits, and only the values 1 to
  4 are in use (`src/value.h`). An inline Int takes one tag bit of that space,
  bit 49, and a signed payload of 48 bits (−2^47 to 2^47 − 1). Bit 48 stays
  free and is reserved for a later UInt.
- An Int outside that range is an immutable heap object that holds one
  `int64_t`. The native VM must not box an Int that fits the inline range,
  so the cost rule stays "inside the range, no allocation; outside it, one
  allocation". Equality, hashing and printing use the value, never the
  address, so a program cannot see whether an Int is inline or boxed.
- The spec binds int64 only. The inline width is implementation-defined, so a
  change of the width, or a JIT that keeps Ints in registers, is not a
  language change.
- Doubles do not change. Number code keeps its speed and its layout.

**Cost of a box.** On a `release` build, one allocation of a small object
adds about 60 to 80 ns to a loop step, and about 100 ns when the collector
has 100,000 live objects to mark (measured in #792).

## Consumers

Each consumer and what it owes under the backend roles in `VISION.md`.

| Consumer | Obligation | What changes |
|---|---|---|
| Native VM | The full spec | `src/value.h` (the inline tag and the box), `src/value.cpp` (equality, `hashValue`, `ValueHash`, `stringify`), `src/container_objects.h` (`isMessage`), the GC mark and free switches, `src/chunk.h` (new opcodes), `src/vm.cpp` (fast paths), `src/runtime.cpp` (slow paths, the index checks), `src/scanner.cpp`, `src/parser.cpp`, `src/compiler.cpp` (the suffix, the new tokens, the precedence rows, the literal range checks), `src/stdlib/globals.cpp` (`int`, `float`), a new Int module file, `src/stdlib/reflect_api.cpp` (`type()`) |
| JVM backend | The spec, as the semantic oracle | `java.lang.Long` beside `Double` in `runtime/jvm/src/lox/LoxOps.java`; the natives in `LoxRuntime.java`; constant emission and the new opcodes in `src/backend/jvm_emitter.cpp`. `Long.equals(Double)` is already `false`, so the map keys need no rule |
| QBE backend | What its experiment needs | The inline tag check and the double fast paths in `src/backend/qbe_emitter.cpp`; the box through `src/backend/rt_capi.cpp`. The QBE corpus gates run the bootstrap interpreter compiled by QBE, so the backend must run every Int construct that the bootstrap uses |
| Bootstrap interpreter | The spec | `parseNumber`, `isLoxNum`, `reflectTypeName`, the scanner, the parser precedence, the new operators, the index checks and the module in `bootstrap/loxpp_interpreter.lox`. It can use host Ints after the native VM has them |
| jlox-mode interpreter | No change | `bootstrap/lox_interpreter.lox` reads plain Lox. `1i` is a syntax error there, as in jlox |
| Resolver | Accept the new expressions | `src/analyze.cpp` has no number logic |
| LSP | Correct documentation | `src/lsp/stdlib_docs.cpp`: the new functions and the module |
| tree-sitter grammar | Parse every program | `editors/tree-sitter-loxpp/grammar.js`: the suffix, the tokens, the precedence |
| Editor syntaxes | Highlight the new tokens | `editors/loxpp-vscode/syntaxes/lox.tmLanguage.json`, the `editors/loxpp.tmbundle` syntaxes, `editors/loxpp.nvim/syntax/lox.vim` |
| Fault table check | Cover the new rows | `tools/check_fault_table.py` |
| Spec | Define it | `spec/01-lexical.md`, `spec/02-syntax.md`, `spec/03-types.md` (the Int section, equality, hashing, the sendable table, the `str()` table), `spec/04-semantics.md` (the operand table, the precedence, the protocol row, indexing, the fault rows), `spec/05-stdlib.md` (`int`, `float`, the module, the `type()` row) |

## Countable sets

The spec will list these sets. Each gets a table and a script that checks
the table is complete, before backend work starts:

- the operand-kind table above, one row per operator and three columns;
- the precedence table;
- the operator protocol table, with a row for the bit operators that says
  "no protocol";
- the compile-time rejections, five rows in `spec/01-lexical.md`: a decimal
  Int literal above the int64 range, except under the unary-minus exception; a
  hex or binary literal above 64 bits; a fractional literal with the suffix,
  which is a `BADINT` token; and a name after a literal. The table of examples
  in the same file lists each form that follows from the three rules of the
  section, with its tokens and its result, and
  `tools/check_int_literal_rejections.py` requires a spec probe for each row of
  it;
- the runtime fault rows: a mixed-kind operand, a bit operator on a Number
  or an Instance, a shift count that is not an Int or is out of range, Int
  `/ 0i` and `% 0i`, `int()` on a fractional Number, a NaN, an infinity, a
  Number outside the int64 range, or a String that does not parse as decimal,
  `float()` on a String that does not parse, and each module function on a
  non-Int;
- the edge rows that do not fault: the smallest Int `/ -1i` and `% -1i`, the
  negation of the smallest Int, the largest Int `+ 1i`, `1 == 1i` is
  `false`, and `0i` is truthy.

## Constraints for the mission

These bug classes are known before any node starts. Each node's review
checks them.

1. **Undefined behavior under the test gate.** The `debug` preset builds
   with `-fsanitize=address,undefined`, and it is the ctest gate. Do every
   wrapping operation in `uint64_t` and cast back. Test for the smallest Int
   with `-1i` before the C++ `/` and `%`. Check the range before every
   double-to-int cast in `int()` and the index checks.
2. **A new object kind touches every switch over `ObjType`.** The GC mark
   and free, `isMessage`, `stringify`, equality, hashing, the type name, and
   the QBE runtime C API each gain a case. A missing case is silent until a
   boxed Int reaches it, so the native node's probes include a boxed Int at
   every one of these sites.
3. **The constant pool keys by bit pattern, not by `==`.** This is true
   today and must stay true. `1` and `1i` have different bits, so they are
   different constants.
4. **Four forms change meaning, as a consequence of the longest match.**
   `1in`, `1if`, `1.5in` and `1.5if` were valid before the suffix existed, and
   now the `i` is the suffix, so each is a syntax error; probes `239` to `242`
   of `test/translation-probes/` pin them. No program in this repository
   writes one of the four, so no program here changes meaning, and a program
   elsewhere puts a space before the keyword.
5. **One normative statement per lexical question.** Tokenization is the
   longest match over the lexical grammar. The range of a literal is by value,
   never by spelling. The unary-minus exception is by value and on the
   grammar. Every other sentence about a literal is an example in the table of
   examples of `spec/01-lexical.md`. A scanner consumer (the native scanner at
   N2, the bootstrap interpreter at N6, the tree-sitter grammar and the
   tooling parser at N7) implements the three rules, not the list of examples,
   and its review checks each row of the table against them.
6. **A check reads one object, never prose.** A check in a `tools/check_*.py`
   script names one object: a table row or cell, a `// COVERS:` tuple, or one
   named line of a probe. Its evidence comes from that object only. A check
   that searches a file or a section for a phrase is not a check of a
   countable set: another occurrence satisfies it, or an equal-meaning edit
   breaks it. No script checks that a sentence of prose is present. Every site
   that adds a problem has one mutation on record that makes it fire, and a
   node that adds a site adds its mutation to the record. The shared layer is
   the check scripts on `tools/spec_tables.py`.

## Program impact

No existing program in this repository changes. The mission adds programs
that use masks as probes of the purpose: a flag set, a bitboard, a hash mix.
During the mission these programs live under `test/translation-probes/` as
spec probes, so each consumer's expected-failure file covers them until its
node merges.
An example under `examples/` that uses Int lands at mission close, when every
corpus gate is green.

## Mission sketch

This is not a node list. The DAG is in the tracking issue, #824.

1. Spec, the shared probes with compile-time and run-time coverage, the check
   scripts, and the expected-failure file per consumer.
2. Native: the value layer and the literals, then the operators, then the
   conversions and the module.
3. JVM.
4. Bootstrap interpreter, after the JVM, because the JVM job runs the
   bootstrap on the JVM backend.
5. Editors and LSP, from the spec alone.
6. QBE, last.
7. The differential gate on both phases, the purpose examples under
   `examples/`, the "Shipped" section and the retrospective.
