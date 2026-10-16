# JUMP_TABLE benchmark — `bench_jump_table.lox`

## Setup

- Program: `examples/bench_jump_table.lox`
- Workload: 5 000 000 iterations of a dense 5-arm enum match (`Op` enum)
- Build: `cmake --preset release` (O3, LTO)
- Hardware: same machine, serial runs, 5 warm-up + 5 measured

## Results (wall-clock, `time`)

| Run | Before (sequential) | After (JUMP_TABLE) |
|-----|--------------------|--------------------|
| 1   | 1.024 s            | 0.910 s            |
| 2   | 1.021 s            | 0.894 s            |
| 3   | 1.014 s            | 0.890 s            |
| 4   | 1.001 s            | 0.883 s            |
| 5   | 0.978 s            | 0.875 s            |
| avg | **1.008 s**        | **0.890 s**        |

**Speedup: ~12 %** on a 5-arm, all-constructor, no-guard enum match.

## When JUMP_TABLE fires

The optimisation applies when the compiler's pre-scan of the match body finds:

- Every arm is a single enum constructor pattern (no `@`-binding, no `or`, no
  literals, no list/class patterns).
- No arm has an `if` guard.
- All constructors belong to the same enum.
- The tag values form a **dense** integer range (no gaps).

Falls back to the sequential GET\_TAG / CONSTANT / EQUAL / JUMP\_IF\_FALSE chain
otherwise (guards, or-patterns, sparse ranges, catch-alls, etc.).

## Why the speedup is moderate

The match is inside a function call (`apply()`), so the hot loop pays call/return
overhead and argument shuffling on every iteration.  The bytecode executed per
iteration is roughly:

- `CALL`, argument setup  
- `JUMP_TABLE` dispatch (1 opcode instead of ~6 per sequential check)  
- arm body (arithmetic on Numbers)  
- `RETURN`

Eliminating 4–5 opcodes per dispatch (GET\_TAG, CONSTANT, EQUAL,
JUMP\_IF\_FALSE, POP) from a 5-arm match accounts for the measured ~12 %
improvement.  A match-heavy program with minimal surrounding work would show
a larger gain.
