# Coroutine cost on the QBE backend, 2026-10-17

The QBE half of the measurements issue #590 asks for. The native half is
`coroutine-cost-2026-10-17.md`; this record uses the same programs compiled
with `--target qbe`, plus one pair that isolates the whole-program
coroutine-mode rule. It gives the numbers and what they bound for a growable
stack under compiled code. It does not decide.

## Setup

| Item | Value |
|---|---|
| Commit | `4a29789`, release preset, `loxpp` and `loxrt` targets |
| Toolchain | `loxpp-dev-qbe`: Ubuntu clang 18.1.3, QBE 1.3; `tools/check_qbe_toolchain.sh` passed |
| Machine | Intel Core 7 240H, 10 cores, 16 threads; Linux 7.2.5, Fedora 44 |
| Power | AC on, governor `powersave`, energy preference `balance_performance` |
| Pinning | every run on core 2, a P-core, with `taskset` |
| Protocol | each program compiled once in the image; on the host 5 warm-up runs discarded, 10 measured, median; `benchmarks/coroutine_cost_qbe.sh` |

Native numbers quoted below are from the native record, same machine, same
day.

## Per-yield cost in coroutine mode

| Program | QBE, run 1 | run 2 | Native |
|---|---|---|---|
| `coroutine_yield.lox`: one resume and one yield, one frame live | 179 | 186 | 151 |
| `coroutine_call_baseline.lox`: the same loop with a plain call | 148 | 150 | 90 |

The round trip costs about 30 ns more on QBE than on the native VM. The
baseline costs about 60 ns more, for a reason that has nothing to do with
coroutines: the loop sits at top level, so `i`, `REPS`, and `step` are
globals, and compiled code reaches a global through a runtime call on every
access, where the interpreter does one hash lookup inline. That makes this
pair a poor floor on QBE. The difference inside QBE, about 31 ns, is not the
cost of the suspend machinery alone; the global-access gap hides part of it.
The pair below is the one to read.

## The coroutine-mode tax, with local state

`coroutine_mode_floor.lox` runs the same call loop inside a function, so the
counter and the limit are local slots. `coroutine_mode_tax.lox` is the same
program plus a function that contains a `yield` and is never called. On QBE
that one `YIELD` compiles the whole program in coroutine mode: every function
is a resume-block state machine and register promotion is off.

| Program | QBE, ns per call | Native, ns per call |
|---|---|---|
| floor: no `yield` in the program | 38.5 (37.4 to 44.4) | 65 |
| tax: an unreached `yield` in the program | 44.5 (43.3 to 52.1) | 65 |

The mode costs about 6 ns per call, 16 percent, on a call-bound loop with
promotable locals. On the native VM the unreached `yield` changes nothing.
With local state QBE is 1.7 times faster than the interpreter on this loop;
with globals it is 1.6 times slower. The second fact is filed as #592.

## Cost against suspension depth, coroutine mode

| Live frames at the yield | QBE, ns per round trip | Native |
|---|---|---|
| 1 (plus the two frames that reach it) | 212 | 167 |
| 10 | 326 | 310 |
| 100 | 1423 | 1340 |

The slope is 12.2 ns per frame, the same as native's 12 ns. The snapshot copy
is runtime code shared by both backends (`Runtime::suspendCurrentCoroutine`,
`Runtime::resumeCoroutine`), so compiled code pays the same copy. The
constant is about 45 ns higher on QBE, which is the state-machine re-entry
of the frames that reach the yield.

## Memory per suspended coroutine

| Suspended at depth | N | QBE, bytes per coroutine | Native |
|---|---|---|---|
| 1 | 50,000 | 505 | 503 |
| 10 | 50,000 | 1,361 | 1,356 |

Identical within noise. The compiled executable links the same runtime and
the same `ObjCoroutine`, and a compiled frame's snapshot carries one extra
`int`, its resume point, per frame.

## What this bounds for a growable stack

The open question from the native record: a growable or segmented stack per
task removes the 12 ns per frame copy, but every grow moves the array, and
compiled code holds `base` in a register across a function and writes slots
at `base + 8 * height` (hazards Q1 and Q3 in the QBE decision). What would a
reload of `base` at every safe point cost?

Coroutine mode already pays a cost of that shape. Each resume block
re-enters a function at a saved point and re-derives its frame from the
runtime, and the measured price of that machinery is 6 ns per call on a
call-bound loop. A reload of `base` at each safe point is a subset of what a
resume block does, so 6 ns per call is an upper bound on the per-call price
of a movable stack under compiled code, on this loop. It is a bound from the
same mechanism class, not a measurement of a growable stack; none exists.

The comparison for #590 is then:

- Copy-on-suspend, as built: about 60 ns per switch, plus 12 ns per live
  frame, on both backends; 500 bytes per idle task.
- A movable per-task stack: no per-frame copy at a switch; at most about 6
  ns on every call, paid whether or not the program ever switches; memory per
  task set by the initial stack size, not by the live slice.

The break-even is about five live frames per switch. A model that switches
at shallow points keeps copy-on-suspend and pays nothing on ordinary calls. A
model that switches deep, or often at depth, is better served by a movable
stack, and then every program pays up to 6 ns per call for it. That last
point is a `VISION.md` control: a construct must not add work at a site that
does not name it.

## Not measured

- A growable stack. Nothing exists to measure; the bound above comes from
  the coroutine-mode state machine.
- The whole-program rule's cost on a corpus. The 6 ns is one call-bound
  loop; `benchmarks/run.py` on the full corpus with and without an unreached
  `yield` would give the corpus-wide figure.
- Register promotion in coroutine mode. The mode turns it off; the 6 ns
  includes that loss.
