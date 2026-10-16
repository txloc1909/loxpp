# Coroutine suspend and resume cost, 2026-10-17

The native measurements that issue #590, the concurrency go/no-go, asks for:
the per-yield cost, how it grows with the suspended slice, and the memory a
suspended coroutine holds. This record gives the numbers and what they say
about stack ownership. It does not decide. The QBE part of #590 is not here.

## Setup

| Item | Value |
|---|---|
| Commit | `c22adad`, release preset, `loxpp` target only |
| Compiler | Ubuntu clang 18.1.3 in `loxpp-dev-env`, `-O3 -DNDEBUG` |
| Machine | Intel Core 7 240H, 10 cores, 16 threads; Linux 7.2.5, Fedora 44 |
| Power | AC on, governor `powersave`, energy preference `balance_performance` |
| Pinning | every run on core 2, a P-core, with `taskset` |
| Protocol | 5 warm-up runs discarded, 10 measured, median reported; `benchmarks/coroutine_cost.sh` |

The seed figure in the design-tensions decision, 112 to 144 ns per round trip,
came from a different machine. It is not comparable with these numbers. Every
later measurement for #590 should run `benchmarks/coroutine_cost.sh` on one
machine and compare within that run.

## Per-yield cost

| Program | ns per step, run 1 | run 2 |
|---|---|---|
| `coroutine_yield.lox`: one resume and one yield, one frame live | 150.9 | 151.7 |
| `coroutine_call_baseline.lox`: one call and return of the same loop shape | 89.2 | 90.9 |
| Difference: what the suspend and resume machinery adds | 61.7 | 60.8 |

Spread inside one set of ten runs: 142 to 158 ns for the coroutine, 87 to 99
for the baseline.

The baseline has the same interpreted work as the coroutine loop: the outer
loop compares and increments, the callee compares and adds once. So the
difference, about 61 ns, is the cost of one native `resume` call, one copy of
the live slice into the snapshot, and one copy back, with one frame live.

## Cost against suspension depth

`coroutine_yield_depth.lox` runs the yielding loop DEPTH frames deep. Seven
measured runs per depth, after three warm-ups.

| Live frames at the yield | ns per round trip | Spread |
|---|---|---|
| 1 (plus the two frames that reach it) | 167 | 156 to 177 |
| 10 | 310 | 303 to 314 |
| 100 | 1340 | 1330 to 1347 |

The growth is linear: about 12 ns per live frame per round trip. That is the
price of copy-on-suspend. A yield copies every frame between the coroutine's
base and the top into the snapshot, and the resume copies it back
(`Runtime::suspendCurrentCoroutine`, `Runtime::resumeCoroutine`). A coroutine
that yields from a shallow loop pays about 60 ns. A coroutine that yields
from inside a deep call tree pays 12 ns more per frame, both ways.

## Memory per suspended coroutine

`coroutine_footprint.lox` creates N coroutines, resumes each once so that it
sits suspended DEPTH frames deep, and keeps them all alive. The number is the
max resident set of the process, minus the run with N = 0, divided by N.

| Suspended at depth | N | Bytes per coroutine |
|---|---|---|
| 1 | 10,000 | 458 |
| 1 | 50,000 | 503 |
| 10 | 10,000 | 1,369 |
| 10 | 50,000 | 1,356 |

So about 500 bytes for an idle coroutine with one frame, and about 95 bytes
per extra live frame. The structure sizes explain it: `ObjCoroutine` is 288
bytes and owns nine `std::vector`s, each a separate heap block; a
`CoroutineFrameSnapshot` is 24 bytes, a `Value` 8 bytes, and each frame adds
its slots, its frame record, and one defer-list vector. 100,000 idle
coroutines cost about 50 MB.

For comparison, one VM owns a fixed `stack[]` of 131,584 bytes and a fixed
`m_frames[]` of 33,280 bytes: about 165 KB. A task that owns a VM at today's
sizes costs 330 times an idle copy-on-suspend coroutine.

## The depth check today

Every call checks `m_frameCount` against `FRAMES_MAX`, and every push of the
operand stack checks against a constant ceiling: `neededTop <= stack +
hardCeiling` in `Runtime::checkStackOverflow`, one compare against a constant.
A resume checks the whole slice against the same two ceilings before it
copies anything, so a coroutine suspended near the limit and resumed from a
deep stack raises `StackOverflowError` instead of writing past the arrays.
Compiled QBE code writes its frame's slots at `base + 8 * height` and runs
the same check at entry (hazards Q1 and Q3 in the QBE decision).

What the alternatives change in that check:

- **A stack per task, fixed size.** The ceiling becomes a field of the
  current stack instead of a constant: one load more per check, no copy at
  yield or resume, and the 12 ns per frame disappears. The cost moves to
  memory: 165 KB per task at today's sizes, unless the arrays shrink. A small
  per-task stack brings back a per-task overflow limit that a deep call tree
  hits sooner than today.
- **A growable or segmented stack per task.** The check keeps its shape, but
  a grow either moves the array, which invalidates every `Value*` and
  `slots` pointer held in `CallFrame`, open upvalues, and handler records, or
  chains a new segment, which makes `slots + n` arithmetic cross a segment
  boundary. The snapshot already stores every such pointer as an offset from
  the base so that a resume can rebase it; a growable stack would make that
  rebasing the normal path of every grow, not only of a resume. Compiled QBE
  code holds `base` in a register across a function, so a move under it
  needs a reload at every safe point, the same set of points where S8 spills
  promoted locals.

## What the numbers say for #590

- Suspension itself is cheap where it is used: about 61 ns on top of a 90 ns
  call, with one frame live. The "pay only where you use it" control in
  `VISION.md` holds today.
- The cost that scales is the copy. Deep yields pay 12 ns per frame per
  round trip. A scheduler that switches tasks at arbitrary depth pays that on
  every switch; a generator that yields from a loop does not.
- Memory favours copy-on-suspend by a large factor for idle tasks: 500 bytes
  against 165 KB. A VM-per-task model needs smaller or growable stacks before
  it can hold tens of thousands of tasks.
- The two numbers to weigh against each other at the decision are the 12 ns
  per frame of copying against the pointer-rebasing cost of a growable stack.
  The second is not measured. It is the first measurement the QBE block of
  #590 should make, because compiled code is where a moving stack costs most.

## Not measured

- QBE coroutine mode. The whole-program `yield` rule makes it a separate
  build and a separate question.
- The cost of a `yield` that crosses a `try` handler or a `defer` list. Both
  are copied in the snapshot and should add a constant per record.
- Any model other than copy-on-suspend. Nothing else exists to measure.
