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
  one stack, one scheduler, and one inbound queue. Workers share no mutable
  state. A value crosses between workers only if it is a message, a
  scalar, a string, or an enum value of messages, and then by reference.
- A **task** is a coroutine on its worker's stack: copy-on-suspend, as built
  (#523). Tasks on one worker share that worker's heap and switch only at
  explicit points: `yield`, a receive on an empty mailbox, and the blocking
  calls the stdlib names. A send never suspends. There is no preemption.
- The **primitive** is spawn and send. `spawn` starts a task on the current
  worker or on a new worker; `send` appends a value to a task's mailbox and
  returns; `receive` takes the next one or suspends. Channels, actors, futures, and pools are
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

**A send is constant time.** A message is a scalar, a string, or an enum
value whose fields are all messages. It crosses by reference. Nothing is
copied at a send. This is the *cost* control of `VISION.md` applied to the
primitive: a send names one value and does one bounded thing with it.

The alternative, deep copy on send, was measured
(`docs/records/message-copy-cost-2026-10-17.md`): about 100 ns per object
and 20 to 30 ns per map entry, rising by half past the cache, and 45 ns
per live object at every collection the receiver runs while the message
lives. That cost is linear in the reachable graph of the message, which the
sender can predict only if it knows what it sends, and the receiver pays
again at sites that never named the message. Copy is therefore not the
primitive.

**Why enum values, and no new primitive.** The language already has an
immutable data shape. An enum value's fields are read by position or by
pattern, and an assignment to one is a runtime error; a string is
immutable; a scalar is a value. That is the shape the FP direction points
at, sum types by convention, and at the worker boundary it becomes
immutable by construction. No `freeze` builtin, no frozen flag on lists and
maps, and no check on their mutating operations are needed.

**The sendable bit.** Whether a value is a message is known at
construction. A scalar and a string are messages. An enum value's bit is
the conjunction of its fields' bits, computed when the constructor runs,
from bits that are already known: constant per construction. A list, a
map, an instance, a function, a closure, a class, a coroutine, and a handle
are never messages. The check at a send reads one bit. A send of a value
whose bit is clear raises `SendError` at the send site. There is no
implicit copy and no implicit freeze.

**Lists and maps.** A plain list or map is mutable, so it is not a message.
A program that must send one converts it first, and the conversion is the
linear work, named where it is written: a record with known fields becomes
an enum value, `Record(id, name, x, y)`; a map with unknown keys becomes a
persistent map, a tree of enum nodes with structural sharing, written as a
library in the FP direction, which carries any keys and is a message because
every node is an enum value. A deep copy of lists and maps is writable in
Lox++ with recursion, lists, and maps, so by the capability test it is a
library too; a native `copy` may join the stdlib later if a measurement
says the library is too slow, and it changes no rule.

**Lifetime across workers.** A message object lives in the heap that
created it. When a send crosses to another worker, the sender increments a
holder count on the object, under the owner heap's hold lock (constraint
"The holder counts of one heap" below), on behalf of the receiving worker,
before the message enters that worker's inbound queue; the receiver
registers the object in its *foreign set* when it drains the queue. The
count moves at the send and not at the receive because the message sits in
the inbound queue between the two, and the owner's collector must not free
it there. Each send raises the count by one, so while two envelopes with
the same object are in a queue, one worker contributes two. The drain
releases a duplicate, so after a drain each worker contributes at most one
to a count. A count changes only at these events, never on the
interpreter's hot path: a send across workers; a spawn, which raises one
count on the new task handle for the spawning worker and one on the start
message; a drain; the new worker's receipt of its start message; a
collection; a worker exit; and the free of a dead heap. The creating
worker's collector treats an object with a count above zero as a root. The
receiver's collector marks
foreign objects in a side table of its own, never in the object header,
because the header's mark bit belongs to the owner's collector; a foreign
object the receiver's mark phase did not reach leaves its foreign set and
the count drops. A message has no pointer to mutable state and no cycle
can cross a worker boundary, so the count has no cycle problem.

**The owner traces from a held object.** A message may hold a message that a
third worker owns: A sends an enum value E that A owns, whose field is F,
which C owns and A received earlier. B then reaches F through E, but F's
count was raised only for A. The count is therefore per root, and the
owner's collector traces from every held root: A treats E as a root while B
holds it, reaches F, and keeps F in its foreign set, so C keeps F for as
long as B holds E. The chain holds by induction across any number of
workers, and the send stays constant because no graph is walked at a send.

**Retention across heaps is acyclic.** A message is immutable and is built
after its fields, so a reference across heaps always points at an older
object, and the hold graph across heaps has no cycle. A dead worker's heap
releases every foreign hold except those reachable from its own held
messages, so two dead heaps can never hold each other forever. This is the
reason immutability was worth its cost: it makes the lifetime of a shared
object end, in finite time, without a cycle collector across heaps.

**A task handle is an identity.** A request carries the sender's handle, so
a handle is a message. A task object owns a mailbox and a coroutine
snapshot, which are mutable state of another thread, so a handle that
crosses workers is an identity, the worker and the task number, and
tracing stops at it. A send to a handle routes through the target worker's
inbound queue. A send to a dead task, or to a task of a dead worker, raises
`DeadTaskError` at the send site: a lost message is a fault, not a silence.

**Strings are interned on receive.** Inside one heap every string is
interned, so `==` on two strings of that heap is a pointer compare. A
received string is therefore not held as a second object with the same
characters. The receiver probes its own intern table with the hash the
object already carries: if the characters are present it keeps its local
object and shares nothing; if not, it inserts the received object into its
table as the interned one and counts it as foreign. Intern on receive
changes only the received value itself. A string in a field of a received
enum value stays the object of its own heap, because a walk of the fields
would make a send linear. So `==` on two strings of two different heaps
compares their characters, with the stored hash first, and `==` on any
other pair of objects stays a pointer compare. The constraint "Strings of
two heaps" below states this as a rule for every backend. *Constraint:* the
probe must be sublinear in the table size. The table is open addressing
with a bounded load factor, so the probe is expected constant; the hash is
not keyed, so hostile input can lengthen a probe chain, and a keyed hash
is the fix if that ever matters. A string of any length costs one probe
to receive, not a copy.

**What this costs the rest of the language.** One bit per enum value, set
at construction. One error kind, `SendError`. A foreign set and a side
mark table per worker. Nothing runs user code or adds work proportional to
anything at a site that does not name it.

### Mailboxes

**Each task owns one mailbox, and it is unbounded.** A mailbox is a
first-in first-out queue of messages. `send(task, msg)` appends to it and
returns; it never suspends and never fails for lack of room. `receive()`
takes the head of the current task's mailbox or suspends the task until a
message arrives. The length of a mailbox is observable, so a program can
see a queue grow.

The bounded form was weighed and rejected for the primitive. A send that
suspends when a queue is full stops a task at a site that asked for no
limit, which fails the *cost* control, and it puts the classic deadlock,
two tasks each waiting to send into the other's full queue, into the
primitive itself. With an unbounded mailbox the only wait a task can enter
is its own `receive`.

**Each worker owns one inbound queue.** It is the path a message takes from
another thread: the sender takes the queue's lock, appends an envelope of
target task and message, releases, and signals; the receiving worker drains
the queue between tasks, in one swap under the lock, delivers each envelope
to its task's mailbox, and wakes the tasks that were parked on `receive`. A
send inside a worker goes straight to the target's mailbox with no lock. A
mutex and a condition variable per inbound queue is the first form; a
lock-free queue is an optimisation with a measurement, later.

**The credit channel is required.** An unbounded mailbox moves the memory
bound from the runtime to the program, and the *memory* control of
`VISION.md` says the program must be able to bound what it uses. So the
language ships the bound as a library, in Lox++, over the primitive: a
channel with a capacity N, where the consumer grants N credits to the
producer, the producer sends only while it holds a credit, and the consumer
returns a credit after each message it takes. Under this protocol a
producer with no credit parks on its own `receive`, never on anyone's
queue, and the memory of the edge is bounded by N. The library is part of
the mission, as its own node, and it is the recommended form for every
edge where a producer can outrun its consumer. Until the import system
lands, it ships as an example program the way `examples/option_result.lox`
does; after it, as a stdlib module.

**Deadlock is a diagnosed fault.** When every worker waits on its
condition variable with at least one task parked on `receive` and nothing
inbound anywhere, the program can never progress. The last worker to go
idle detects this with one atomic count and ends the program with
`DeadlockError`, naming the parked tasks. It costs nothing on the hot path.

### Scheduler

Per worker, one run queue, first in first out, no priorities. A task runs
until it suspends. A blocking call blocks its worker, every task on it, and
nothing else; the stdlib says which calls block. Asynchronous I/O inside a
worker is the first follow-up on this seam, not part of this decision.

**The suspend points are the default set, and a fair mode is reserved.** The
spec's table of suspend points, `yield`, `receive` on an empty mailbox, and
the blocking calls, is the set a program pays for by default. A task that
computes without reaching one starves every other task on its worker, and
a program cannot add a switch point inside a library it calls; the lever at
launch is placement, a worker of its own. An opt-in fair mode per worker,
which forces a switch at a loop back-edge or a call after a budget of
work, is reserved: it pays only where it is enabled, it changes the meaning
of no program that does not enable it, and QBE's coroutine mode already
makes every function re-enterable. The spec must word the table so that
such a mode adds points under a flag without a change of rule. The mode
itself is a later decision with a measurement.

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

### What the model requires of an allocator or a collector

A new allocator or collector is a separate decision. This section records
what this model fixes for it and what it leaves free, so that the
experiment starts from the constraints and not from a surprise.

| The model requires | Why | What it rules out |
|---|---|---|
| One heap per worker, collected by its owner's thread, with no other thread mutating it | Workers share no mutable state | Nothing; this is the freedom below |
| A held object, count above zero, never moves and is never freed | Another thread reads its fields at any time, including during the owner's collection | A copying or compacting collector that moves every object. A held object is pinned, or lives in a non-moving space |
| Nothing a foreign reader reads is written by the collector | A reader sees fields, never the mark bit | A mark bit in the object header that a reader could see. A side bitmap removes the constraint |
| A held object is a root, and the collector traces from it | A held message may hold a third worker's message | A collector that treats a root as a leaf |
| A heap can outlive its thread as a dead heap and be freed as a whole | A worker may exit while another holds its messages | An allocator that cannot release a whole heap at once, or that must be freed by its creating thread |
| Every change of a holder count, and the owner collector's read of all its counts in one step, happen under one hold lock of the owner heap; a collection raises each new foreign object before it releases any count | The count moves at a send, and a worker can reach an object only through another held one (constraint "The holder counts of one heap" below) | A lone atomic count per object that the owner reads one at a time |

| The model leaves free | What it means for an experiment |
|---|---|
| No concurrent collector and no write barrier for concurrency | The mutator of a heap is stopped while the heap is collected. A pause stops one worker, not the program. A barrier is needed only if the collector wants one, for a generational design |
| A parallel collector | Several threads may collect one heap while its worker is stopped, and a worker may stop at a safe point for another worker's collector. The model forbids a second mutator, not a second collector |
| The message bit is known before allocation | A constructor evaluates its fields first, so the allocation site knows whether the object is a message. A segregated non-moving space for messages and a moving space for everything else needs no extra pass |
| A dead heap holds exactly the held messages | After the exit collection, the retained memory is bounded by what is held, whatever the allocator |

A mark-region collector with side mark bitmaps fits every row: non-moving
by default, pinning built into its evacuation, regions that free as a
whole, and a bump allocator inside a region. That is a candidate, not a
decision. `benchmarks/copy_bench.cpp` is the harness to extend to object
headers once allocation can take an allocator, and the GC trace gives the
pause and the live set per collection.

### Constraints from named bug classes

Each constraint below names a structural bug class. It binds all remaining
work of the mission and every later change to the layer it names.

**The holder counts of one heap.** *Bug class:* with one lone atomic count
per object, the owner's collector reads the counts one at a time. A worker
that reached G only through a field of a held E raises G, then releases E.
The owner can read G before the raise and E after the release, see no hold
on either, and free G while the worker uses it. *Constraint:* every change
of a count of a heap, from any thread, and the owner's read of all the
counts of that heap, happen under one hold lock of that heap, and the owner
reads all the counts in one step. Every collection, the exit collection
included, raises a count on each foreign object that it reached and that
is not in its foreign set yet, before it releases any count. Then the owner
never sees the release of E without the raise of G. *Shared layer:* the
hold table of the native `MemoryManager` (`hold`, `releaseHold`, and the
held roots of a collection). Every path that changes a count goes through
it: the send, the spawn (the hold on the new task handle and on the start
message), the drain, the new worker's receipt of its start message, the
collection, the worker exit, and the free of a dead heap. The QBE backend
links this runtime. The JVM and the bootstrap have one collector for all
workers and keep no count.

**Strings of two heaps.** *Bug class:* intern on receive changes only the
received value. A string in a field of a received enum value is not in the
receiver's intern table, so a pointer `==` says that two strings with the
same characters are not equal, as values and as map keys. *Constraint:*
`==` on two strings is the equality of their characters on every backend.
A backend may compare by pointer only for two strings that one intern table
holds. Every hash that a map uses for a string key is a hash of its
characters, so a string of another heap finds the same entry. A table that
compares its string keys by pointer takes only keys of its own intern table
and interns a key from a program value first. *Shared layer:* the String
equality of `Value` (`operator==` in `src/value.cpp`) and the hashes beside
it. On native, the map of the language uses `operator==` and the stored
hash. The tables of globals, fields, and methods take only interned keys:
the compiler's constants, and the field names of `reflect`, which it
interns first. `ValueHash` hashes by pointer and is correct only for the
values of one heap, which is all that the compiler's constant pool holds.
The JVM and the bootstrap compare strings by their characters already.

**The QBE coroutine-mode rule.** *Bug class:* "the program contains
`yield`" is a syntactic selector. A task suspends also in a
`task.receive()` on an empty mailbox, which is a call of a native, so a
program with tasks and no `yield` was compiled without resume points.
*Constraint:* the rule is the property "a compiled frame can be live under a
suspension": a `YIELD` anywhere, or any name of the global `task`, through
which alone a program reaches the task natives. A program in coroutine mode
runs its main task on the scheduler, as native does. *Shared layer:*
`qbe::needsCoroutineMode` (`src/backend/qbe_emitter.h`), the one test that
the front end and `tools/qbe_emit_program.cpp` call. A new suspend point in
the spec's table must be added to it.

### What this unblocks and in which order

1. The spec: workers, tasks, spawn, send, receive, the unbounded mailbox
   and its observable length, `SendError`, `DeadlockError`, `DeadTaskError`,
   the task handle as an identity, the sendable
   set as a table with a check script, the suspend points as a table, and
   the list of blocking calls.
2. The native runtime: per-worker `Runtime` and `MemoryManager`, the
   sendable bit on enum values, the foreign set and side mark table, intern
   on receive, the mailbox, the inbound queue, the run queue, the deadlock
   detector, `CLOCK_THREAD_CPUTIME_ID`.
3. The credit channel library in Lox++, with the rule that blocking waits
   form a directed acyclic graph of ranks.
4. The bootstrap, then the JVM as oracle, then QBE parity, each diffed
   against native at its own review.
5. After the mission, in this order: a per-worker netpoller for
   asynchronous I/O first, because a blocking call blocks its worker and a
   server with a thousand connections otherwise needs a thousand workers,
   so the mission alone does not close #519; then #519; then GC item 11
   per worker heap; then FFI in the full package, where a blocking foreign
   call blocks its worker and foreign code never holds another worker's
   object.

This decision does not open the mission. The mission's tracking issue and
node issues follow it, per `AGENTS.md`, "Missions".

## Non-goals

- No GC redesign. One collector per worker is the collector that exists.
- No preemption, no work stealing, no growable stack, no shared mutable
  heap, no implicit copy, no freeze primitive, no bounded mailbox in the
  primitive. Each is a new decision if
  ever wanted, and each must beat the numbers above.
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
