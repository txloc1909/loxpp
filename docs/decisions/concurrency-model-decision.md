# Concurrency Model Decision

**Status:** accepted. The decision was taken on 2026-10-17 (#590) on the
measurements in `docs/records/coroutine-cost-2026-10-17.md`,
`docs/records/coroutine-cost-qbe-2026-10-17.md`, and
`docs/records/message-copy-cost-2026-10-17.md`. The sections up to "Reading
the matrix" are the analysis that led to it; "Decision" states it.

## Why this doc

`docs/records/concurrency-model-next-steps.md` item 1 asks for a decision record that scores
the three candidate models — CSP (Go), actor/isolated-heap (BEAM), and OS
threads (JVM) — against Lox++'s actual constraints, before any code is written.
The point is to make the model choice deliberate, per the `AGENTS.md` planning
policy and `docs/decisions/expressiveness-roadmap.md`'s "build item 7 last, choose its model
first."

Two questions are separated here, because they have different answers:

- **Which model?** Scored below.
- **When to commit?** After coroutines ship and their cost is measured. That
  was item 6 of `docs/records/concurrency-model-next-steps.md`, and this
  document now records its outcome.

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
`MemoryManager` (`src/memory_manager.h`) owns `allObjects`, the interned
string `Table m_strings`, a single `m_grayStack`, `m_tempRoots`, a bump counter
`bytesAllocated`, and a `m_markRoots` callback. It is explicitly non-copyable and
non-movable because `VmAllocator` stores a raw `this` pointer
(`src/vm_allocator.h`). Any shared-heap model must build a concurrent or
thread-safe collector; a per-task-heap model can reuse this class once per task.

**C2 — `Runtime` state is sized for exactly one call stack.**
`m_frames[]`, `stack[]`, `m_handlerStack`, `m_deferLists`, and the profiler
arrays are all single-stack, fixed-capacity members of `Runtime`
(`src/runtime.h`; the interpreter loop in `src/vm.cpp` reaches them through
`m_rt`). Multiple lightweight tasks sharing one VM require each
array to become per-task.

**C3 — Re-entry into the interpreter is bounded and nested only.**
A native can call back into the interpreter and get a synchronous result:
`Runtime::reentrantCall`, `invokeCallableFromNative`, and
`invokeMethodFromNative` (`src/runtime.h`) push the callee and run a nested
`run(stopAtFrameCount)` to completion (#496; `callMethod` in
`src/stdlib/reflect_api.cpp` uses it). Defer thunks and coroutine resume use
the same seam. The re-entry is strictly LIFO on the one host stack: the nested
run must finish before its native caller continues, and a `yield` across a
native callback is a runtime error (`YieldAcrossNativeError`,
`spec/04-semantics.md`; enforced by `m_reentrantRunDepth`). So a model that
re-enters the VM from a native gets a working path, but one that needs to
interleave or switch away from a native-called frame still needs new
machinery.

**C4 — `FrameSync` is the existing suspend/resume seam.**
The register-cached `ip` is flushed into `frame->ip` on guard construction and
reloaded from the top of `m_frames` on destruction
(`FrameSync` in `src/vm.cpp`). `docs/records/benchmark_report_2026-08-26.md:391` records that
`yield` must "flush, save the frame/stack slice, and reload on resume" at this
same seam. Whatever the model, stackful suspension reuses this.

**C5 — The native VM decides; the other backends conform by role.**
The roles are in `VISION.md` ("The backends"). The native VM is the product
and the reference. The JVM is a semantic oracle and is never shipped. QBE is an
experiment. The bootstrap interpreter is a capability test that must match
native. The facts below describe the implementations; the roles decide what
each one owes a concurrency model.
The language has four implementations today: the native C++ VM (`src/vm.cpp`),
the QBE backend (`src/backend/qbe_emitter.cpp`, which reuses the native
runtime), the JVM backend (`src/backend/jvm_emitter.cpp` + `runtime/jvm/`),
and the self-hosted tree-walking interpreter
(`bootstrap/loxpp_interpreter.lox`). The CLR backend was deleted at the QBE
parity gate (#489, #491).
**QBE is not an independent implementation:** it reuses the native runtime
(`docs/decisions/qbe-backend.md:34-44`), so native-vs-QBE output checks code generation,
not runtime semantics; only the JVM checks runtime behavior independently. The
three independent surfaces for any language feature are therefore native/QBE,
the JVM, and the bootstrap. `tools/diff_runtimes.py` compares them, so any
observable semantic must match across all of them or be explicitly out of
scope. The bootstrap runs on top of the VM and has no host-thread or
host-scheduler access of its own.

**C6 — The profiler is coupled to the single stack.**
`m_profilerScopes[]` is parallel to `m_frames[]` (both in `src/runtime.h`), and the
clock is process-wide `CLOCK_PROCESS_CPUTIME_ID` (`src/profiler.h:66-70`). A
fiber/task model forces these per-task; an isolated-task model gives each task
its own `ProfilerData` (`docs/reference/profiler-concurrency-notes.md:26-43`). The
profiler is `#ifdef`-guarded, so it does not constrain data structures with
`LOXPP_PROFILE=OFF`, but it forces the stack-ownership question early.

**C7 — Values are references, not deep copies.**
`Value` is a NaN-boxed `Obj*` (`src/value.h`); `ObjList`/`ObjMap` hold
heap storage on the VM allocator (`src/container_objects.h`).
The roadmap's "share-nothing on maps + primitives" stance
(`docs/decisions/expressiveness-roadmap.md:37-40`) describes message *payloads*, but no deep
copy, serialization, or immutable-value facility exists yet. Every
message-passing model needs one; a shared-memory model needs it for nothing but
still needs a memory model.

**C8 — No concurrent GC groundwork exists.**
Collection is stop-the-world mark-sweep over `allObjects`
(`src/memory_manager.h`). `docs/records/benchmark_report_2026-08-26.md:396`
records that GC item 11 (generational/incremental) must wait for this model
go/no-go because the model sets the collector's shape.

**C9 — QBE compiles each Lox call to a real C frame.**
The QBE backend keeps clox's fused stack: a call passes a pointer to the callee's
stack window, and each Lox call uses a real C frame, with overflow checked
against `kFramesMax` on the C stack (`docs/decisions/qbe-backend.md:50-68,245-251`).
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

**Backend roles change how criteria 2 and 5 read.** The table scored parity
as if every backend were a product. Item 6 must re-score these criteria under
the roles:

- The native VM decides. A model is judged first by its cost and fit there.
- Host-thread support on the JVM is not an advantage, because the JVM is not
  shipped. The JVM implements the model only if it is to act as an oracle for
  concurrency semantics.
- The bootstrap interpreter must conform, so a model that it cannot express
  still loses. A scheduler written in Lox++ on top of coroutines is not a cost
  against a model: it is the capability test the bootstrap exists for.
- QBE's real C frames (C9) constrain only the QBE experiment and a future JIT,
  not the shipped product.

The OS-threads model is the worst fit on the criteria Lox++ cares about — it
contradicts the share-nothing stance, requires the hardest GC work, exposes the
error-prone primitives the language has so far avoided, and is unimplementable
in the bootstrap. Its one advantage, native host-thread support on the JVM,
would make that backend observably stronger than native/QBE and the bootstrap,
which `tools/diff_runtimes.py` exists to prevent.

## Per-backend mapping

The CLR column is omitted: the backend was deleted at QBE parity gate S7
(`docs/decisions/qbe-backend.md:292`). QBE shares the native runtime, so the two are
listed separately only where their code shapes differ.

| Model | Native C++ VM | QBE backend | JVM backend | Bootstrap interpreter |
|---|---|---|---|---|
| CSP (Go) | New scheduler, growable stacks, channels in the runtime | Inherits native's runtime, but a suspend point must survive real C frames (C9) | Goroutines → host threads/virtual threads; channels in `LoxRuntime.java` | Needs a scheduler written in Lox++ itself, or excluded |
| Actor / isolated heap | One `VM` per task; scheduler copies messages | Same runtime as native; no QBE-specific work beyond the shared runtime | One runtime instance per task; copy via shared serialization | Feasible: tasks as interpreter instances in one Lox++ process |
| OS threads | pthreads per task; thread-safe GC required | Same runtime as native; QBE frames complicate root scanning across task boundaries | `Thread` + shared heap; concurrent GC in the runtime | No host threads exist to expose |

The differential-testing requirement (C5) is the sharpest edge, within each
backend's role: if a backend that must conform cannot express a primitive,
semantics diverge and `tools/diff_runtimes.py` fails. Native and QBE share one runtime, so they
cannot diverge on runtime semantics — but the JVM and bootstrap can. Any
concurrency surface must therefore be specified in `spec/` in
implementation-neutral observable terms, with any exception recorded
explicitly rather than discovered in CI.

## Decision (2026-10-17)

**Go.** The model is **workers and tasks**: isolation at the OS-thread
boundary, cheap cooperative tasks inside it.

- A **worker** is one OS thread that owns one `Runtime`, one `MemoryManager`,
  one stack, one scheduler, and one mailbox. Workers share no mutable
  state. A value crosses between workers only if it is a scalar, a string,
  or frozen, and then by reference.
- A **task** is a coroutine on its worker's stack: copy-on-suspend, as built
  (#523). Tasks on one worker share that worker's heap and switch only at
  explicit points: `yield`, a send that blocks, a receive on an empty
  mailbox, and the blocking calls the stdlib names. There is no preemption.
- The **primitive** is spawn and send. `spawn` starts a task on the current
  worker or on a new worker; `send` delivers a value to a mailbox; `receive`
  takes the next one or suspends. Channels, actors, futures, and pools are
  libraries over these three.

This is actor-style isolation at worker granularity and CSP-style cheap tasks
inside a worker. It is the only candidate that reuses every mechanism that
exists and builds nothing the measurements argue against.

### Why this and not the two pure models

The measurements settled the stack axis and the vision settled the heap axis.

- **Not a VM per task.** An idle copy-on-suspend coroutine holds about 500
  bytes; a `Runtime` with today's fixed arrays holds 165 KB. A model whose
  unit of isolation is a whole VM costs 330 times more per idle task, or a
  redesign of the stack arrays first. The actor column of the scoring assumed
  that unit. Its isolation survives here, moved to the worker.
- **Not a shared heap across threads.** A Go-style shared heap needs a
  concurrent collector (C1, C8), the single largest piece of work in the
  space, and it gives up the control `VISION.md` names under *mutation*: a
  value another thread can change between two reads is a state change the
  program did not write. One heap per worker keeps `MemoryManager` as it is
  and keeps every write local.
- **Not a growable stack, not preemption.** Copy-on-suspend costs about 60 ns
  per switch plus 12 ns per live frame, on both backends. A movable stack
  removes the per-frame cost and charges every call up to 6 ns, switch or
  no switch. Break-even is about five live frames per switch. A model that
  switches only at explicit points stays under that line in ordinary code
  and charges nothing at a site that does not name a switch, which is the
  *cost* control in `VISION.md`. Preemption would switch at arbitrary depth
  and lose on both counts.

### Stack and heap ownership

| Question | Answer |
|---|---|
| Stack | One fixed stack per worker. A task owns a slice of it while running and a copied snapshot while suspended. |
| Heap | One `MemoryManager` per worker, collected stop-the-world by that worker alone. Workers never see each other's objects. |
| Threads | One OS thread per worker, 1:1. Tasks do not migrate between workers. The program decides how many workers exist. |
| Time | `CLOCK_THREAD_CPUTIME_ID` per worker. The profiler keeps one `ProfilerData` per worker and reports per worker; a merged report is a tool over the per-worker ones. |

### The message and value story (C7)

**A send is constant time.** A message is a scalar, a string, or a frozen
value, and it crosses by reference. Nothing is copied at a send. This is
the *cost* control of `VISION.md` applied to the primitive: a send names
one value and does one bounded thing with it.

The alternative, deep copy on send, was measured
(`docs/records/message-copy-cost-2026-10-17.md`): about 100 ns per object
and 20 to 30 ns per map entry, rising by half past the cache, and 45 ns
per live object at every collection the receiver runs while the message
lives. That cost is linear in the reachable graph of the message, which the
sender can predict only if it knows what it sends, and the receiver pays
again at sites that never named the message. Copy is therefore not the
primitive. It is an operation the program calls by name.

**Frozen values.** `freeze(v)` makes the whole graph reachable from `v`
immutable, in place, and returns it. It is linear in the graph, once, at a
site that names it. A frozen list, map, or enum value never changes again;
a mutating operation on one raises `FrozenError`. A scalar and a string are
frozen by nature. `freeze` on a value that holds a function, a closure, a
class, an instance, a coroutine, or a handle raises `FreezeError`: no code
and no captured state becomes shareable.

**The sendable set.** A send accepts a scalar, a string, or a frozen value.
Anything else raises `SendError` at the send site. There is no implicit
copy and no implicit freeze. A program that wants to send mutable data
writes `freeze` or `copy` and pays where it writes it. The same rule holds
between two tasks on one worker, so a program's meaning never depends on
where its tasks run, and no copy is paid there either.

**Mutation after receive.** A receiver that wants to change a message
writes `copy(v)`, which returns a fresh mutable deep copy on its own heap.
That copy is the measured cost above, named at the `copy` site. Cloning a
map's bucket array instead of re-inserting its entries brings a record
from about 190 ns to an estimated 50 ns; it is implementation work, not a
rule.

**Lifetime across workers.** A frozen object or a string lives in the heap
that created it. When another worker receives it, the receiver registers
it in its *foreign set* and increments a holder count on the object,
atomically. Each worker contributes at most one to a count, so the count is
bounded by the worker count and changes only at a receive and at a
collection, never on the interpreter's hot path. The creating worker's
collector treats an object with a count above zero as a root. The
receiver's collector marks foreign objects in a side table of its own,
never in the object header, because the header's mark bit belongs to the
owner's collector; a foreign object the receiver's mark phase did not reach
leaves its foreign set and the count drops. A frozen graph has no outgoing
pointers to mutable state and no cycle can cross a worker boundary, so the
count has no cycle problem.

**Strings are interned on receive.** `==` on two strings is a pointer
compare, and that is correct only because every string in a heap is
interned. A received string is therefore not held as a second object with
the same characters. The receiver probes its own intern table with the
hash the object already carries: if the characters are present it keeps
its local object and shares nothing; if not, it inserts the received object
into its table as the interned one and counts it as foreign. The invariant
holds inside each worker and `==` stays a pointer compare. *Constraint:* the
probe must be sublinear in the table size. The table is open addressing
with a bounded load factor, so the probe is expected constant; the hash is
not keyed, so hostile input can lengthen a probe chain, and a keyed hash
is the fix if that ever matters. A string of any length costs one probe
to receive, not a copy.

**What this costs the rest of the language.** Every mutating operation on
a list, a map, or an enum value checks one flag in the object header, a
constant that joins the type check already there. `freeze` adds one
builtin and one error kind; `copy` adds one builtin. The collector gains a
foreign set and a side mark table per worker. None of these runs user
code or adds work proportional to anything at a site that does not name
it.

### Scheduler

Per worker, one run queue, first in first out, no priorities. A task runs
until it suspends. A blocking call blocks its worker, every task on it, and
nothing else; the stdlib says which calls block. Asynchronous I/O inside a
worker is a later feature on this seam, not part of this decision.

### Per backend, by role

| Backend | What it owes | Feasibility |
|---|---|---|
| Native VM | The whole model. | Workers are `Runtime` instances on threads; tasks are the coroutines of #523; copy and mailbox are runtime code. No concurrent GC. |
| QBE | Code generation only; it links the native runtime. | A program with tasks contains `yield`, so it compiles in coroutine mode and pays about 6 ns per call. Accepted for an experiment. The whole-program rule stays a stopgap (design-tensions T1). |
| JVM | The model as a semantic oracle: observable output must match. | One runtime instance per worker on one Java thread, which means the static runtime state becomes per instance; tasks stay virtual threads; copy by the same rules. Host threads give it nothing extra, because it is not shipped. |
| Bootstrap interpreter | Runs the language and matches native on observable output. | Tasks are host coroutines, as today. A worker is a host worker that runs a second interpreter over the same source, or, as the implementation-gated exception named in `spec/`, a coroutine on one thread. Parallel speed-up is not observable output, so the second form conforms. |

### Re-score of criteria 2 and 5 under the backend roles

| # | Criterion | Workers and tasks |
|---|---|---|
| 2 | Independent-surface parity | 4. Copy and mailbox are implementation-neutral spec surface. The JVM owes an oracle only. The bootstrap expresses tasks today and expresses workers with one named exception. |
| 5 | Reuse of coroutine suspend and resume | 5. A task is the coroutine as built; nothing new on the suspend seam on any backend. |

The weighted total under the roles is 455 of 500: criterion 1 keeps the
actor column's 5, criteria 3, 6, 7, and 8 keep the actor column's 5, and
criterion 4 scores 4, because the scheduler is a run queue and a mailbox per
worker with no work stealing, no growable stacks, and no preemption.

### What this unblocks and in which order

1. The spec: workers, tasks, spawn, send, receive, `freeze`, `copy`,
   `SendError`, `FreezeError`, `FrozenError`, the sendable set as a table
   with a check script, and the list of blocking calls.
2. The native runtime: per-worker `Runtime` and `MemoryManager`, the frozen
   flag and its checks, the foreign set and side mark table, intern on
   receive, the mailbox, the run queue, `CLOCK_THREAD_CPUTIME_ID`.
3. The bootstrap, then the JVM as oracle, then QBE parity, each diffed
   against native at its own review.
4. After the mission: GC item 11 per worker heap; FFI in the full package,
   where a blocking foreign call blocks its worker and foreign code never
   holds another worker's object; a per-worker netpoller for asynchronous
   I/O; #519.

This decision does not open the mission. The mission's tracking issue and
node issues follow it, per `AGENTS.md`, "Missions".

## Non-goals

- No GC redesign. One collector per worker is the collector that exists.
- No preemption, no work stealing, no growable stack, no shared mutable
  heap, no implicit copy. Each is a new decision if ever wanted, and each
  must beat the numbers above.
- No FFI thread-safety work here. FFI is designed on this model, after it.
- No `spec/` change in this document.

## References

- `docs/records/concurrency-model-next-steps.md` — the sequence this doc
  belonged to; this decision completed it.
- `docs/records/actor-model-design.md` — the open problems of a VM-per-task
  actor model; this decision answers each at worker granularity.
- `docs/reference/concurrency_in_bytecode_vms.md` — design space, literature, and GC
  options per model.
- `docs/reference/profiler-concurrency-notes.md` — profiler impact by model.
- `docs/decisions/expressiveness-roadmap.md` — item 5 (coroutines) and item 7
  (parallelism), and the share-nothing messaging stance.
- `docs/records/benchmark_report_2026-08-26.md` §5 item 11 and "Dependencies on the
  expressiveness roadmap" — why GC waited for this decision.
- `docs/records/coroutine-cost-2026-10-17.md`,
  `docs/records/coroutine-cost-qbe-2026-10-17.md`, and
  `docs/records/message-copy-cost-2026-10-17.md` — the measurements.
- `docs/decisions/qbe-backend.md` — the QBE backend that replaced CLR at parity gate
  S7, its reuse of the native runtime, and its real-C-frame code shape.
- `spec/README.md` — the implementation-independent contract every target must
  satisfy.
