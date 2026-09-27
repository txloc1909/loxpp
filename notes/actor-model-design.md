# Actor Model: Design Record

**Status: design record. Assumes the actor/isolated-heap model is adopted.**
The model is not committed yet; `concurrency-model-next-steps.md` item 6 owns
the go/no-go. This note records the *open problems* the actor commitment leaves
unsolved, framed as the questions item 6 must answer. It is a companion to
`concurrency-model-decision.md`, which scores the model and does the choosing.

## Why this note

`concurrency-model-decision.md` scores the actor model highest against Lox++'s
constraints as they exist today. Committing to it does not close the work: the
model choice fixes only the *unit of isolation*. Every problem that crosses that
boundary — message copy, scheduling, host-thread mapping, FFI — is orthogonal
new work, and the differential-testing requirement (C5) forces most of it to be
specified and implemented more than once. This note lists those problems so
item 6 has a complete question list, not a re-derived one.

## 1. Unit of isolation (the assumption)

The actor model's unit is one task owning one `VM` and one `MemoryManager`
(`src/memory_manager.h:18-94`). `MemoryManager` is reused as-is, with no
concurrent collector (C1, C8 in the decision doc). Each task therefore owns one
`ProfilerData`. This is the assumption every problem below is stated against.

## 2. Message copy / value story (C7)

**Item 6 must answer:** how a message payload crosses from one task's heap to
another. `Value` is a NaN-boxed `Obj*` (`src/value.h:40-93`); `ObjList` and
`ObjMap` hold heap storage on the VM allocator
(`src/container_objects.h:11-16,112-128`). No deep-copy, serialization, or
immutable-value facility exists. Open sub-questions:

- Deep copy, immutability, or a serialization format over maps and primitives?
- Cyclic containers: how does a copy handle a list or map that refers to
  itself?
- Identity vs. value: are two copies of the same object `==`? Are they the
  same map key?
- How is the resulting semantics written in `spec/` in implementation-neutral
  terms, so native/QBE, the JVM, and the bootstrap all match?

The roadmap's "share-nothing on maps + primitives" stance
(`expressiveness-roadmap.md:37-40`) describes *payloads*, not a copy facility.
The facility is still to be built.

## 3. Scheduler

**Item 6 must answer:** the task scheduler's shape. The actor model needs task
queues, message delivery, and a task switch. It is simpler than Go's
work-stealing package, but it is still real work, and it is written twice: once
for the native/QBE runtime and once in `LoxRuntime.java`.

## 4. Cross-implementation parity (C5)

**Item 6 must answer:** which observable actor semantics are defined in `spec/`,
and which exceptions are recorded explicitly before implementation. The JVM
backend must implement the scheduler and message copy independently; the
bootstrap has no host-thread or host-scheduler access of its own
(`concurrency-model-decision.md:75-82`). If one target exposes a primitive the
others cannot, `tools/diff_runtimes.py` fails. Item 6 must name every
implementation-gated exception in `spec/`, not discover it in CI.

## 5. Coroutine cost (measured, not predicted)

**Item 6 must answer:** the measured suspend/resume cost of item 5. Actor tasks
need suspension at the same `FrameSync` seam (`src/vm.cpp:497-538`) as every
other model. Per-yield overhead, stack-slice size, and what growable/segmented
stacks add — all from the item 5 build — are the evidence item 6 exists to
gather (`concurrency-model-decision.md:154-158,211-213`). Committing to the
model does not supply this data.

## 6. Stack/heap/thread mapping

**Item 6 must answer:** how tasks map to OS threads, and what "time" means.
Actor says one `VM` per task, but not whether tasks run 1:1 or M:N on threads.
That decision drives:

- `CLOCK_PROCESS_CPUTIME_ID` → `CLOCK_THREAD_CPUTIME_ID` (or a wall clock)
- N separate profiler reports with no merged view
  (`profiler-concurrency-notes.md:33-36`)
- whether a blocking FFI call stalls one task or the whole scheduler

## 7. FFI thread-safety

**Item 6 must answer:** how FFI fits the isolation model. FFI'd code escapes the
GC, memory safety, and the concurrency model by design, so it must be designed
*after* item 7's model is chosen (`expressiveness-roadmap.md:118-119`). The
actor commitment does not answer how a task calling a blocking C function
interacts with its own scheduler and heap.

## 8. QBE note

A suspend point on QBE must be a status-return seam at frame granularity, not a
mid-frame save: QBE compiles each Lox call to a real C frame and emits no
unwinding tables (`qbe-backend.md:50-68,184-186`). QBE reuses the native
runtime, so it cannot diverge on scheduler or message-copy semantics; it proves
codegen only. This is why C9 penalizes mid-frame suspension and reinforces the
actor lead.

## References

- `notes/concurrency-model-decision.md` — the scoring that produces this
  note's assumption; C1–C9 are defined there.
- `notes/concurrency-model-next-steps.md` — item 6, the go/no-go gate this
  note feeds.
- `notes/profiler-concurrency-notes.md` — profiler impact by model.
- `notes/expressiveness-roadmap.md` — item 5 (coroutines), item 7
  (parallelism), the share-nothing stance, and FFI ordering.
- `notes/qbe-backend.md` — the QBE code shape behind section 8.
