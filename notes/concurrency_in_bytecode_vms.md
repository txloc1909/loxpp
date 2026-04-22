# Concurrency in Bytecode VMs: Design Space & Literature

---

## 1. Concurrency Model (the biggest decision)

### OS Threads (1:1)
Each VM "thread" maps to a real OS thread. Simple, leverages multicore. The hard part is your object model — you'll need a thread-safe GC and careful locking. *Example: JVM.*

### Green Threads / Fibers (M:N)
A scheduler multiplexes many lightweight tasks onto fewer OS threads. You control the scheduler, so you can make it work-stealing, cooperative, or preemptive. *Examples: Erlang BEAM, Go runtime, early Java.*

### Actor Model
Concurrent units share no memory; they communicate via message-passing. Each actor has a mailbox. Your VM enforces isolation. *Examples: BEAM (Erlang/Elixir), Pony.*

### Coroutines / Async-Await
Stackful or stackless coroutines with explicit yield points. Cooperative, so no data races — but also no parallelism unless you layer it on top. *Examples: Lua coroutines, Python asyncio, wren.*

### Communicating Sequential Processes (CSP)
Like actors but communication is synchronous over channels rather than buffered mailboxes. *Examples: Go goroutines, Newsqueak.*

---

## 2. Memory & Isolation Model

This is tightly coupled to your concurrency model:

| Model | Memory Sharing | Typical Safety Mechanism |
|---|---|---|
| OS Threads | Shared heap | Locks, atomics, STM |
| Actors | No sharing | Message copying or immutability |
| CSP | Shared but structured | Channel ownership / linear types |
| Coroutines | Shared, single-threaded | No races (cooperative) |

The BEAM achieves isolation by **copying all messages** and using per-process heaps, which makes GC trivial (collect one process at a time) but adds copying cost. The JVM went the other way — shared heap, stop-the-world then concurrent GC.

A middle path: **immutable-by-default values** (like Clojure's persistent data structures) let you share without copying.

---

## 3. Garbage Collector Design

Concurrency breaks naive GC. Your main options:

- **Stop-the-world** — simplest, but pauses all threads. Fine for latency-insensitive workloads.
- **Per-thread/per-actor heaps** — collect each heap independently. Requires isolation or careful inter-heap references (remembered sets, write barriers).
- **Concurrent/incremental GC** — tricolor marking (Dijkstra/Yuasa write barriers). Hard to get right. See the *Garbage Collection Handbook*.
- **Reference counting + cycle detection** — CPython's model. Works with the GIL; harder without it.

---

## 4. Synchronization Primitives to Expose

What does *userspace* see? Your choices for the VM's instruction set / standard library:

- **Mutexes / condition variables** — universal but error-prone
- **Channels** — safe, composable (Go, Rust `std::sync::mpsc`)
- **Software Transactional Memory (STM)** — optimistic concurrency, automatic rollback. Great for complex state; overhead is real. (*Haskell STM is the canonical example*)
- **Atomic operations** — low-level but necessary for lock-free structures
- **Promises / Futures** — for async result propagation

---

## 5. Instruction Set Concerns

When you add concurrency, your bytecode itself may need:

- **Atomic read/write ops** — or memory model annotations (Java's `volatile`)
- **Yield / suspend instructions** — for cooperative scheduling
- **Thread-local storage access** — fast-path for per-thread data
- **Memory fence/barrier instructions** — if targeting weak-memory hardware (ARM)
- **Safepoints** — places the GC can stop a thread safely; often baked into loop back-edges and call sites

---

## 6. Scheduler Design

If you implement green threads/actors, you own the scheduler:

- **Work-stealing deque** (Cilk, Go, Tokio) — idle threads steal from busy ones; great cache locality
- **Cooperative vs. preemptive** — cooperative is simpler but one bad actor stalls everyone; Go moved to asynchronous preemption (Go 1.14)
- **Priority / fairness** — BEAM uses reductions (instruction counts) to time-slice actors fairly

---

## Key Literature

### Foundational Papers
- *Communicating Sequential Processes* — C.A.R. Hoare (1978). The CSP model.
- *Actors: A Model of Concurrent Computation* — Hewitt et al. (1973).
- *Making the Future Safe for the Past* — Baker & Hewitt. Actor GC.
- *Multiprocessing Compactifying Garbage Collection* — the original parallel GC paper lineage.

### VM-Specific
- *The Java Memory Model* — Manson, Pugh, Adve (POPL 2005). Canonical reference for shared-heap VMs.
- *Erlang's BEAM* — *Concurrent Programming in Erlang* (Armstrong et al.) + *BEAM Book* (open source, github.com/happi/theBeamBook)
- *Go's scheduler* — Dmitry Vyukov's *Go preemptive scheduler design doc* (2013) and the work-stealing scheduler paper by Blumofe & Leiserson (*Scheduling Multithreaded Computations by Work Stealing*, JACM 1999).
- *Lua 5.x coroutines* — the implementation notes in the Lua source and *The Implementation of Lua 5.0* (Ierusalimschy et al.)

### GC
- *The Garbage Collection Handbook* (Jones, Hosking, Moss) — the definitive reference, with dedicated chapters on concurrent and parallel GC.
- *A Unified Theory of Garbage Collection* — Bacon et al. (OOPSLA 2004).

### STM
- *Composable Memory Transactions* — Harris et al. (PPOPP 2005). Haskell STM.
- *Software Transactional Memory* — Shavit & Touitou (1995).

### Modern Practice
- *The Art of Multiprocessor Programming* — Herlihy & Shavit. Lock-free data structures, memory models.
- MicroPython, Wren, and Gravity VM source code are all small, readable bytecode VMs worth studying.

---

## Recommended Starting Points by Goal

| Goal | Start here |
|---|---|
| Maximize parallelism, shared heap | JVM memory model paper + GC Handbook |
| Fault tolerance, actor isolation | BEAM Book |
| Simple, safe concurrency | Go scheduler docs + CSP paper |
| Minimize complexity | Lua coroutines (stackful, cooperative) |
| Cutting-edge safety | Pony language (reference capabilities) |
