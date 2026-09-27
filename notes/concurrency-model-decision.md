# Concurrency Model Decision

**Status: provisional scoring only. This doc does not pick the shipping
model.** The final go/no-go is `concurrency-model-next-steps.md` item 6, taken
after coroutines ship and their real suspend/resume cost is known. This doc is
the artifact item 6 revises.

## Why this doc

`concurrency-model-next-steps.md` item 1 asks for a decision record that scores
the three candidate models — CSP (Go), actor/isolated-heap (BEAM), and OS
threads (JVM) — against Lox++'s actual constraints, before any code is written.
The point is to make the model choice deliberate, per the `AGENTS.md` planning
policy and `expressiveness-roadmap.md`'s "build item 7 last, choose its model
first."

Two questions are separated here, because they have different answers:

- **Which model?** Scored below.
- **When to commit?** Not now. The ordering in `concurrency-model-next-steps.md`
  stands: build item 5 (coroutines/generators) first, then decide at item 6 with
  runtime evidence.

## What "deciding" means for Lox++

The model choice is a *package deal* in every candidate. Go is not just
channels: it is growable stacks, async preemption, work-stealing deques, and a
netpoller. BEAM is per-process heaps, reduction-counting fairness, and
copy-on-send. The JVM is a full shared-memory model, locks, and a concurrent
collector. Choosing the label without the package underestimates the work;
choosing the package without every target in mind — native, the QBE backend
that replaces CLR, the JVM, and the bootstrap — risks divergence.

## The constraints any model must survive

These are the facts that make Lox++'s decision space narrower than a generic
language's. Each is verifiable in the tree.

**C1 — One non-thread-safe `MemoryManager` per VM.**
`MemoryManager` (`src/memory_manager.h:18-94`) owns `allObjects`, the interned
string `Table m_strings`, a single `m_grayStack`, `m_tempRoots`, a bump counter
`bytesAllocated`, and a `m_markRoots` callback. It is explicitly non-copyable and
non-movable because `VmAllocator` stores a raw `this` pointer
(`src/vm_allocator.h:26-46`). Any shared-heap model must build a concurrent or
thread-safe collector; a per-task-heap model can reuse this class once per task.

**C2 — `VM` state is sized for exactly one call stack.**
`m_frames[]`, `stack[]`, `m_handlerStack`, `m_deferLists`, and the profiler
arrays are all single-stack, fixed-capacity members of `VM`
(`src/vm.h:215-284`). Multiple lightweight tasks sharing one VM require each
array to become per-task.

**C3 — `run()` has no bounded re-entrant call path.**
`VM::run()` returns only at frame count 0 or on error; the `stopAtFrameCount`
parameter is a narrow re-entrancy seam added only so `defer` thunks can run to
completion (`src/vm.h:79-86`, `src/stdlib/reflect_api.cpp:168-175`). There is no
general "call back into the interpreter and get a synchronous return value"
path. Adding concurrency that re-enters the VM needs this path built first.

**C4 — `FrameSync` is the existing suspend/resume seam.**
The register-cached `ip` is flushed into `frame->ip` on guard construction and
reloaded from the top of `m_frames` on destruction
(`src/vm.cpp:497-538`). `notes/benchmark_report_2026-08-26.md:391` records that
`yield` must "flush, save the frame/stack slice, and reload on resume" at this
same seam. Whatever the model, stackful suspension reuses this.

**C5 — Four targets, three independent surfaces, and the CLR slot is being
replaced.**
The language has four implementations today: the native C++ VM (`src/vm.cpp`),
the JVM backend (`src/backend/jvm_emitter.cpp` + `runtime/jvm/`), the CLR
backend (`src/backend/clr_emitter.cpp` + `runtime/clr/`), and the self-hosted
tree-walking interpreter (`bootstrap/loxpp_interpreter.lox`). The CLR backend
is scheduled for deletion once the QBE backend passes its parity gate
(`notes/qbe-backend.md:18-22,292`), leaving native, QBE, JVM, and bootstrap.
**QBE is not an independent implementation:** it reuses the native runtime
(`notes/qbe-backend.md:34-44`), so native-vs-QBE output checks code generation,
not runtime semantics; only the JVM checks runtime behavior independently. The
three independent surfaces for any language feature are therefore native/QBE,
the JVM, and the bootstrap. `tools/diff_runtimes.py` compares them, so any
observable semantic must match across all of them or be explicitly out of
scope. The bootstrap runs on top of the VM and has no host-thread or
host-scheduler access of its own.

**C6 — The profiler is coupled to the single stack.**
`m_profilerScopes[]` is parallel to `m_frames[]` (`src/vm.h:277-284`), and the
clock is process-wide `CLOCK_PROCESS_CPUTIME_ID` (`src/profiler.h:66-70`). A
fiber/task model forces these per-task; an isolated-task model gives each task
its own `ProfilerData` (`notes/profiler-concurrency-notes.md:26-43`). The
profiler is `#ifdef`-guarded, so it does not constrain data structures with
`LOXPP_PROFILE=OFF`, but it forces the stack-ownership question early.

**C7 — Values are references, not deep copies.**
`Value` is a NaN-boxed `Obj*` (`src/value.h:40-93`); `ObjList`/`ObjMap` hold
heap storage on the VM allocator (`src/container_objects.h:11-16,112-128`).
The roadmap's "share-nothing on maps + primitives" stance
(`expressiveness-roadmap.md:37-40`) describes message *payloads*, but no deep
copy, serialization, or immutable-value facility exists yet. Every
message-passing model needs one; a shared-memory model needs it for nothing but
still needs a memory model.

**C8 — No concurrent GC groundwork exists.**
Collection is stop-the-world mark-sweep over `allObjects`
(`src/memory_manager.h:70-74`). `notes/benchmark_report_2026-08-26.md:396`
records that GC item 11 (generational/incremental) must wait for this model
go/no-go because the model sets the collector's shape.

**C9 — QBE compiles each Lox call to a real C frame.**
The QBE backend keeps clox's fused stack: a call passes a pointer to the callee's
stack window, and each Lox call uses a real C frame, with overflow checked
against `kFramesMax` on the C stack (`notes/qbe-backend.md:50-68,245-251`).
This is natural for a model whose unit is a whole task-owned stack, but hostile
to mid-frame stackful suspension: `yield` and goroutine switches would need
either a separate C stack per task or suspend points that unwind out of every
compiled frame. Any suspend/GC-root design must be proven on QBE, not only in
the interpreter loop.

## Scoring

Each criterion is scored 1 (worst fit for Lox++ / highest cost) to 5 (best fit /
lowest cost), then weighted. Weights reflect how much each criterion blocks or
reshapes work that already exists or is already planned.

| # | Criterion (weight) | CSP — Go | Actor/isolated heap — BEAM | OS threads — JVM |
|---|---|---|---|---|
| 1 | GC redesign cost (20) | 2 — shared heap needs a concurrent collector; C1, C8 | 5 — per-task heap reuses `MemoryManager` as-is; C1 | 2 — shared heap, contended `bytesAllocated`/gray stack; C1, C8 |
| 2 | Independent-surface parity (native/QBE, JVM, bootstrap) (20) | 2 — goroutine runtime must be defined in `spec/` and implemented across native/QBE, JVM, and bootstrap; bootstrap has no scheduler; C5 | 4 — task = own VM; message copy is an implementation-neutral spec surface; still needs a scheduler in native/QBE and bootstrap; C5 | 3 — the JVM hosts threads natively; bootstrap cannot expose them at all; C5 |
| 3 | Fit with the share-nothing messaging stance (15) | 3 — CSP is shared-but-structured; safe only if send copies; C7 | 5 — isolation is the model; matches the stance directly | 1 — shared heap plus locks is the opposite of the adopted stance |
| 4 | Runtime/scheduler infrastructure required (15) | 1 — work-stealing, growable stacks, async preemption, netpoller | 3 — scheduler needed, but per-task stacks and copy-on-send are simpler than Go's package | 4 — OS schedules; low custom infrastructure, high per-task cost |
| 5 | Reuse of coroutine suspend/resume (10) | 4 — goroutines are stackful coroutines at the same `FrameSync` seam, but QBE's real C frames make mid-frame suspension costly; C4, C9 | 3 — isolated tasks can be separate VMs, but lightweight actors want the same machinery | 1 — blocking OS threads share nothing with the `yield` seam |
| 6 | User safety for a dynamic audience (10) | 5 — channels are safe-by-default | 5 — no shared state to race on | 2 — locks/atomics are error-prone |
| 7 | Memory-model complexity (5) | 2 — shared-heap memory model | 5 — no shared memory model needed | 1 — weakest definition in the space |
| 8 | Profiler rework (5) | 2 — fibers sharing one VM break `m_profilerScopes[]`; C6 | 5 — per-task `ProfilerData`, least disruptive; C6 | 2 — contended GC stats, per-thread merge; C6 |
| | **Weighted total (of 500)** | **250** | **430** | **220** |

## Reading the matrix

The actor model scores highest **against the constraints as they exist today**,
for one structural reason: it is the only candidate whose unit of isolation
matches the current unit of ownership. A task that owns one `VM` and one
`MemoryManager` needs no concurrent collector, no per-fiber frame array, and no
shared profiler — constraints C1, C2, C6, and C8 all fall out instead of being
solved. Its costs are the ones already named in `concurrency-model-next-steps.md`:
message copying (C7) and a scheduler.

The QBE plan reinforces that lead rather than weakening it. QBE inherits the
native runtime, so a native-runtime-centric model reaches QBE for free, and the
parity burden drops to the JVM and bootstrap. The one candidate penalized by
QBE is Go's stackful goroutines: mid-frame suspension is awkward when every Lox
call is a real C frame (C9), whereas an actor task owns a whole stack and
switches at task granularity.

Two cautions against over-reading that result:

1. **The scores are static, not measured.** Every cell is a prediction from
   source structure. The whole point of item 6 is to replace prediction with
   the measured cost of item 5's suspension. A model can score well on paper
   and still lose on throughput, latency, or implementation cost.
2. **The lead is partly an artifact of good fit with today's design.** "Reuses
   the current architecture" is not the same as "best final architecture."
   Go's model scores poorly mostly because none of its prerequisites exist yet,
   not because channels are the wrong primitive for the language.

The OS-threads model is the worst fit on the criteria Lox++ cares about — it
contradicts the share-nothing stance, requires the hardest GC work, exposes the
error-prone primitives the language has so far avoided, and is unimplementable
in the bootstrap. Its one advantage, native host-thread support on the JVM,
would make that backend observably stronger than native/QBE and the bootstrap,
which `tools/diff_runtimes.py` exists to prevent.

## Per-backend mapping

The CLR column is omitted: it is retired at QBE parity gate S7
(`notes/qbe-backend.md:292`). QBE shares the native runtime, so the two are
listed separately only where their code shapes differ.

| Model | Native C++ VM | QBE backend | JVM backend | Bootstrap interpreter |
|---|---|---|---|---|
| CSP (Go) | New scheduler, growable stacks, channels in the runtime | Inherits native's runtime, but a suspend point must survive real C frames (C9) | Goroutines → host threads/virtual threads; channels in `LoxRuntime.java` | Needs a scheduler written in Lox++ itself, or excluded |
| Actor / isolated heap | One `VM` per task; scheduler copies messages | Same runtime as native; no QBE-specific work beyond the shared runtime | One runtime instance per task; copy via shared serialization | Feasible: tasks as interpreter instances in one Lox++ process |
| OS threads | pthreads per task; thread-safe GC required | Same runtime as native; QBE frames complicate root scanning across task boundaries | `Thread` + shared heap; concurrent GC in the runtime | No host threads exist to expose |

The differential-testing requirement (C5) is the sharpest edge: if one target
exposes a primitive the others cannot, semantics diverge and
`tools/diff_runtimes.py` fails. Native and QBE share one runtime, so they
cannot diverge on runtime semantics — but the JVM and bootstrap can. Any
concurrency surface must therefore be specified in `spec/` in
implementation-neutral observable terms, with any exception recorded
explicitly rather than discovered in CI.

## Provisional recommendation

1. **Build item 5 (coroutines/generators) first**, as `concurrency-model-next-steps.md`
   already orders. It is the model-agnostic stepping stone: CSP goroutines,
   actor tasks, and async I/O all need suspend/resume at the `FrameSync` seam
   (C4), and it is useful standalone (lazy sequences, custom iterables) even if
   item 7 never ships. Item 5's plan must now include QBE: a suspend point has
   to be expressible in QBE-compiled code, not only in the interpreter loop
   (C9).
2. **Do not commit to a model in this doc.** The static scores favor actor, but
   the source note's arguments against committing now — no measured suspension
   cost, no GC groundwork, and cross-implementation divergence risk — all still
   hold.
3. **Revisit at item 6** with real data from item 5, then choose between a
   Go-style scheduler and isolated per-task heaps.
4. **Keep GC item 11, scheduler work, channels, and FFI thread-safety blocked**
   until item 6 returns an explicit go/no-go (`concurrency-model-next-steps.md`
   items 6-7; `benchmark_report_2026-08-26.md:396`).

## What item 6 must produce

- The measured cost of item 5's suspend/resume: per-yield overhead, stack-slice
  size, and what growable/segmented stacks add to the single `CALL`-site depth
  check (`benchmark_report_2026-08-26.md:397`).
- A decision on stack ownership (per-VM, per-task, or per-OS-thread) and heap
  ownership (shared or isolated), as `profiler-concurrency-notes.md:63-70`
  requires.
- A cross-implementation feasibility check — native/QBE, JVM, and bootstrap —
  with any implementation-gated exception named in `spec/` before
  implementation. For QBE specifically: how a suspend point and GC root
  accounting survive its one-real-C-frame-per-call code shape (C9).
- A concrete message/value story for C7: deep copy, immutability, or a
  serialization format over maps and primitives.
- An explicit go/no-go, per `concurrency-model-next-steps.md` item 7.

## Non-goals

- No GC redesign. The collector shape follows the model decision.
- No scheduler, channel API, or thread primitive.
- No FFI thread-safety work; `expressiveness-roadmap.md:118-119` requires FFI to
  be designed after the model.
- No `spec/` change. This doc records reasoning only.

## References

- `notes/concurrency-model-next-steps.md` — the sequence this doc belongs to;
  item 6 revisits it.
- `notes/actor-model-design.md` — the open problems that remain once the actor
  model is committed, framed as the questions item 6 must answer.
- `notes/concurrency_in_bytecode_vms.md` — design space, literature, and GC
  options per model.
- `notes/profiler-concurrency-notes.md` — profiler impact by model.
- `notes/expressiveness-roadmap.md` — item 5 (coroutines) and item 7
  (parallelism), and the share-nothing messaging stance.
- `notes/benchmark_report_2026-08-26.md` §5 item 11 and "Dependencies on the
  expressiveness roadmap" — why GC waits for this decision.
- `notes/qbe-backend.md` — the QBE backend that replaces CLR after parity gate
  S7, its reuse of the native runtime, and its real-C-frame code shape.
- `spec/README.md` — the implementation-independent contract every target must
  satisfy.
