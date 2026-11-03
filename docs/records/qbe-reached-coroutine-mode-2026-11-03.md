# The QBE coroutine mode follows the reached instructions, 2026-11-03

The measurement for #1084: an unreached `yield` must cost a QBE program
nothing. Before this change `qbe::needsCoroutineMode` scanned the whole
decoded tree, so a `yield` in dead code switched the mode on for every
function, and coroutine mode turns register promotion off
(`promoteRegisters && !coroutineMode`). The mode now reads the reached set the
shared analysis computes (`FunctionStackAnalysis::reached`).

## Setup

| Item | Value |
|---|---|
| Commit | the merge commit of #1084, release preset, `loxpp` and `loxrt` targets |
| Toolchain | `loxpp-dev-qbe`: Ubuntu clang 18.1.3, QBE 1.3; `tools/check_qbe_toolchain.sh` passed |
| Machine | 12th Gen Intel(R) Core(TM) i5-1235U, 10 cores, 12 threads; Linux 7.2.11 (Arch host, Ubuntu container) |
| Power | AC on, governor `powersave`, energy preference `balance_performance` |
| Pinning | every run on core 2, a P-core, with `taskset` |
| Protocol | each program compiled with `loxpp --target qbe -o`; 5 warm-up runs discarded, 15 measured, median; three interleaved rounds |

## The programs

`benchmarks/coroutine_mode_floor.lox` calls `step` 200000 times from a loop
inside `run`, with the counter and the limit in local slots.

`benchmarks/coroutine_mode_dead_yield.lox` is the floor plus one `yield 0`
after `step`'s last `return`. No path reaches it, so no frame can ever
suspend there.

`benchmarks/coroutine_mode_tax.lox` is the floor plus a `yield` in a separate
function that nothing calls. The shared analysis does reach that function's
own entry, and calls are dynamic, so the mode still covers it: this is the
residual rule, and this program still measures its cost.

The "before" column is the same source compiled by the same tree with only the
reached test removed from `anyReachedInstruction` (`src/backend/qbe_emitter.cpp`),
which restores the whole-tree scan.

## Results, ns per call

| Program | QBE before | QBE after | Mode |
|---|---|---|---|
| `coroutine_mode_dead_yield` | 22.10 / 21.80 / 22.88 | **17.15 / 17.05 / 17.29** | off |
| `coroutine_mode_floor` | 16.94 / 17.58 / 17.28 | 16.94 / 17.58 / 17.28 | off |
| `coroutine_mode_tax` | 19.25 / 19.63 / 19.75 | 19.25 / 19.63 / 19.75 | on |

The three figures per cell are the three interleaved rounds, in round order.

The dead-yield program falls from about 22.0 ns to about 17.2 ns and lands on
the floor, within the run-to-run spread of the floor itself. It also falls
below the tax program, because under the whole-tree rule the `yield` sat in
`step` itself, the frame the loop calls 200000 times, and that frame paid a
resume-state dispatch on every call. The residual rule keeps the cost on the
tax program: 19.25 to 19.75 ns, about 2.4 ns per call over the floor.

The SSA agrees with the timing. For `coroutine_mode_dead_yield`,
`build/qbe_emit_program` emits no `rt_resume_state` call and one `=l phi`
after the change, and three `rt_resume_state` calls and no phi before it.

## What the run-time check shows

`test/translation-probes/367_dead_code_after_suspend_points.lox` puts a
`yield`, a `task.receive()`, a `task.spawn`, a `resume`, a `defer` and a
`match` in dead code after a `return`, a `throw`, a `break` and a `continue`,
in a function and at the top level. Its output is byte-identical to native
after the change, and the program now compiles in normal mode: no
`rt_resume_state` in its SSA, three promotion phis. It also now runs through
`rt_call` rather than `rt_run_tasks`, because no reached instruction names
`task`; that is the same entry native gives a program that never spawns a
task, and the pair `qbe::needsCoroutineMode` / `qbe::treeUsesTasks` moved
together so the scheduler entry and the mode never disagree.
