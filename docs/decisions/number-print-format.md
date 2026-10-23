# Number canonical string form

**Status:** accepted. The owner decided it in #834 on 2026-10-24. It amends
`integer-kind.md`, "Printing".

## The decision

The canonical string form of a Number — what `print` and `str()` produce — is
the shortest decimal that round-trips to the same IEEE 754 double, presented as
ECMAScript §7.1.12.1 `Number::toString`, radix 10, with Lox++'s existing
lower-case names for the special values.

The spec has said "shortest decimal representation" since the type table was
written, but every consumer prints C `printf("%g")` — six significant digits,
scientific notation outside `[1e-5, 1e6)`. That misprints values, not just
styles them: `fib(35)` is `9227465` but prints `9.22746e+06` (that is
`9227460`); `123456789` prints `1.23457e+08` (`123457000`). The Int decision
promises an Int prints "the same text as the equal Number"; under `%g` that is
false above six digits. This decision makes the consumers match the spec, not
the reverse.

## The rule

Let `x` be a Number.

| Value | String form |
|---|---|
| `0.0`, `-0.0` | `0` |
| `inf`, `-inf` | `inf`, `-inf` |
| `nan` (negative payload) | `nan`, `-nan` |
| finite, integral, `1 ≤ |x| < 10²¹` | the bare digits, no `.` and no exponent |
| finite, non-integral, `10⁻⁶ ≤ |x| < 10²¹` | the shortest digits with a decimal point |
| finite, `0 < |x| < 10⁻⁶` or `|x| ≥ 10²¹` | scientific `d[.ddd]e±N`, lower-case `e` |

"Shortest digits" means the unique shortest decimal string that, read back as a
double, yields exactly `x` (round-to-nearest, ties-to-even). Every distinct
double has a distinct shortest form, so the rule is deterministic and needs no
C library to state it.

### The forks

| Fork | Decision |
|---|---|
| Reference | ECMAScript `Number::toString`, radix 10 — not Java `Double.toString` (which appends `.0` and would break the Int parity) and not Python `repr` (different thresholds) |
| Negative zero | `-0.0` prints `0`, matching the spec's existing `0` example and IEEE `+0 == -0`. Today `%g` prints `-0`. |
| Special values | Keep the existing `inf` / `-inf` / `nan` / `-nan`. The reference algorithm governs only finite values. |
| Notation threshold | Fixed for `10⁻⁶ ≤ |x| < 10²¹`; scientific outside. Both thresholds sit away from the int64 range, so every integral Int value prints the same bare digits as its equal Number. |

## The edge table (countable set)

The spec lists this set, and a check script proves the table complete before any
backend work. Each row has one `// COVERS:` probe.

| Row | Value | String |
|---|---|---|
| positive zero | `0.0` | `0` |
| negative zero | `-0.0` | `0` |
| small integer | `42.0` | `42` |
| negative integer | `-3.0` | `-3` |
| fraction | `3.14` | `3.14` |
| half | `0.5` | `0.5` |
| imprecise sum | `0.1 + 0.2` | `0.30000000000000004` |
| seven digits | `1000000.0` | `1000000` |
| nine digits | `123456789.0` | `123456789` |
| fixed upper bound | `1e20` | `100000000000000000000` |
| scientific lower bound | `1e21` | `1e+21` |
| fixed lower bound | `1e-6` | `0.000001` |
| scientific upper bound | `1e-7` | `1e-7` |
| positive infinity | `math.inf` | `inf` |
| negative infinity | `-math.inf` | `-inf` |
| not a number | `math.nan` | `nan` |

The two adjacent pairs — `1e20`/`1e21` and `1e-6`/`1e-7` — are the off-by-one
probes.

## Consumers

Each consumer and what it owes under the backend roles in `VISION.md`. A
language change updates the bootstrap interpreter in the same PR, but only as
much as CI requires.

| Consumer | Obligation | What changes |
|---|---|---|
| Spec `spec/03-types.md` | Define it | The two Number rows of the canonical-string table, plus the edge table |
| Native VM | The full spec | `src/value.cpp` `stringify`: `snprintf("%g")` becomes the shortest form; add explicit `inf`/`nan` handling, which `std::to_chars` does not format |
| JVM backend | The spec, as the oracle | `LoxOps.formatNumber` (`runtime/jvm/src/lox/LoxOps.java`): delete the `%g` emulation, emit the shortest form; update `StringifyTest.java` |
| QBE backend | Follows native | No separate formatter — it inherits `stringify` through the shared runtime C API. Verified by the gate. |
| Bootstrap interpreter | The spec | Inherits the host `str()` (`bootstrap/loxpp_interpreter.lox`, `stringify`); no change expected |
| jlox-mode interpreter | No change | Inherits the host `str()` (`bootstrap/lox_interpreter.lox`) |
| Resolver, LSP, tree-sitter, editors | No change | No number formatting |
| JVM constant emission | Out of scope | `formatDoubleBitsLiteral` (`src/backend/jvm_emitter.cpp`) writes `ldc2_w` operands, not the canonical form |
| Probes and check script | Cover the set | One probe file and `tools/check_number_format.py`, following the `check_int_*.py` pattern |
| Corpus | The examples | The `CHECK` lines that print a Number with more than six significant digits or a magnitude at least `10⁶` |
| `integer-kind.md` | Amend | "The Number format does not change" — see below |

## What this amends in `integer-kind.md`

The Int decision's Printing section ends with "The Number format does not
change." That scope claim stays true of the Int mission itself — the mission
does not change Number's format. But the format *does* change in this issue,
which is what makes the Int decision's parity promise ("the same text as the
equal Number") hold for every value instead of only the small ones.

## Bug classes

These are known before any node starts.

1. **Locale.** `%g` under a non-C locale can emit a comma decimal separator.
   The shortest form is ASCII on every backend. Native `std::to_chars` is
   locale-independent; the JVM side must not use `String.format` without
   `Locale.ROOT`.
2. **Threshold off-by-one.** The `1e-6`/`1e-7` and `1e20`/`1e21` boundaries are
   inclusive on the fixed side. A `<=`/`<` slip diverges. The four boundary
   probes catch it.
3. **Integer detection.** An integral double prints bare (no `.0`); a
   non-integral prints a `.`. A value like `1e20` is integral and prints bare;
   `1.5` prints `1.5`.
4. **Negative zero.** `-0.0` prints `0` on every backend. A backend that prints
   `-0` or `-0.0` diverges.

## Differential gate

Run-time only. This change has no compile-time or rejection surface, so the
existing compile-corpus sweeps suffice and no phase-1 gate is added. The
run-time gate compares native, JVM, bootstrap, and QBE on the number-format
probe, and `tools/diff_runtimes.py` on the affected examples. Each backend node
checks itself against native at its own review.

## Program impact

Only programs that print a Number with more than six significant digits, or an
integral value of magnitude at least `10⁶`, change. That is a handful of
`CHECK` lines across `examples/` — `memo_fib.lox`, `higher_order.lox`,
`linear_regression.lox`, `stats.lox`, `shapes.lox`, `class_dispatch.lox`,
`polar.lox`. The node enumerates the exact set by running
`tools/check_examples.py` and pastes the corrected digits. `examples/bigint.lox`
is unaffected: its `bigFromNumber` reads digits with `math.floor`, not `str()`.

## The Int parity probe

This decision pins the format that makes the Int decision's parity clause true.
The cross-kind probe — `print 1000000; print 1000000i;`, both
`// CHECK: 1000000` — lands with the Int mission, because Int does not exist on
`main` yet. It is a follow-up of this decision, tracked in #824.
