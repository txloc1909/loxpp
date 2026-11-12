# More than one garbage collector in the native runtime

**Status:** proposal. It waits for the owner's approval of the goals, the
collector set and the barrier design. When approved, it moves to
`docs/decisions/` and a mission tracking issue holds its nodes.

Related: #1165 (the pause grows with the live heap), #1134 (the cost of a
large String). Facts about the current heap: `docs/reference/gc-runtime-contract.md`.
Other runtimes: `docs/reference/gc-survey.md`.

## 1. The problem

The native VM has one collector: a stop-the-world mark-sweep, like clox. The
pause has two parts:

- the mark and the trace visit each live object, so they grow with the live
  heap;
- the sweep visits each object and calls `delete` on each dead one, so it
  grows with the whole heap.

So no limit on the pause exists. A larger heap gives a longer pause, and the
program cannot choose when it comes.

Evidence:

- The Redis example (#1165): 9.6 million objects, 5.77 million live. One
  collection stopped the worker for 0.79 s. All clients of the server waited.
- The S6 probe of the same example stalled on one collection of about one
  million list cells. `Cons8` (#1247) made the program use 8 times fewer
  objects. That fixed the probe, but it is a change to one program, not to
  the VM.
- A test on main at 36284862 (release build) kept 3 million live instances,
  then made 6 million short-lived ones:

  | objects before → after | trace | sweep | pause |
  |---|---|---|---|
  | 2797220 → 2797220 | 66 ms | 22 ms | 88 ms |
  | 5594276 → 3000290 | 42 ms | 63 ms | 105 ms |
  | 6000416 → 3000290 | 41 ms | 108 ms | 149 ms |

  The trace costs about 14 ns for each live object. The sweep costs about
  8 ns for each survivor and about 30 ns for each freed object. Most of these
  costs are cache misses: each object is a separate `new`, and its mark bit
  is in its header.

`VISION.md`, "Performance", says that the native VM is the runtime users run,
and that it must be fast for a dynamic language. A server that stops all its
clients for most of a second does not meet that goal.

## 2. Goals and non-goals

### Goals

1. **A limit on latency that does not depend on the heap size.** With the
   incremental collector of this proposal, on the CI host:
   - each pause is at most **1 ms**, at any heap size, for a program whose
     stack and globals are inside the default limits;
   - **MMU(10 ms) ≥ 50%**: in every 10 ms window, the program runs for at
     least 5 ms.

   These numbers are a first choice, not a measured result. Phase 1 below
   checks them on a prototype, and the decision states the final numbers.
2. **More than one collector in one runtime**, behind one interface. The
   current mark-sweep stays as the reference collector.
3. **A limited cost:**
   - the incremental collector is at most **10%** slower than the current
     collector on the geometric mean of `benchmarks/`;
   - with the current collector, the new barrier code costs at most **2%**;
   - the peak heap is at most **2.5×** the live heap (now it is about 2×).
4. **No change to the language.** The spec makes the timing of garbage
   collection implementation-defined (`spec/README.md`, "Conformance",
   item 4). Every program gives the same output with each collector.
5. **A runtime that stays simple to read and to check** (`VISION.md`, native
   VM role).

### Non-goals

- A concurrent collector (one that runs on another thread).
- A moving or compacting collector. It breaks the QBE register promotion
  (`docs/reference/gc-runtime-contract.md`, section 4) and every raw `Obj*` that C++
  code keeps.
- Collection of more than one worker heap at a time. Each worker collects
  its own heap, as now.
- Program control of the collector (a standard-library API). That is a
  language change and needs its own proposal.

## 3. The collectors

| Name | What it is | When |
|---|---|---|
| `stw` | The current stop-the-world mark-sweep. It is the reference: the simplest to read, and the one that the other collectors must match. | stays |
| `incremental` | Tri-color incremental mark and incremental sweep, non-moving, on the worker's own thread. It meets goal 1. | this proposal |
| `generational` | A non-moving generational mode on top of `incremental` (Lua 5.4 and Ruby have one). It makes most collections cheaper on a large heap that does not change much, like the Redis example. Its full collection uses the incremental mark, so goal 1 still holds. | later, a separate decision |

All three share the object model, the roots, the barrier hooks and the
trace format. The `stw` collector ignores the barrier.

## 4. The design of `incremental`

### 4.1 The cycle

1. **Start (a short pause):** scan the roots and make them gray. The roots
   are the value stack, the frames, the open upvalues, the globals, the temp
   roots, the held objects and the inbox targets.
2. **Mark (in steps):** each allocation does a step of mark work, in
   proportion to the bytes it allocates (the pacing). A step takes gray
   objects and traces them.
3. **Finish (a short pause):** the work that must be atomic: the roots again
   if the barrier needs it (4.3), the held objects again, the weak String
   table, the foreign-object order rule, and the objects that own OS handles
   (4.5).
4. **Sweep (in steps):** each allocation sweeps a part of the heap.
5. **Idle:** no collector work until the heap grows past the threshold.

### 4.2 Each step has a limit

A step must have a limit, or goal 1 fails. These objects can be very large:

- a List with millions of elements;
- a Map or a Table with millions of entries;
- the globals table, and the value stack.

So the tracer can stop inside one object and keep its position (an index
into the elements or the buckets). A large object is traced in parts, over
more than one step. The barrier (4.3) keeps the part that is already traced
correct.

### 4.3 The write barrier

A barrier runs at each store of a reference into a heap object. Two kinds
are possible. The prototype in phase 1 chooses between them:

- **Incremental update (Dijkstra), forward:** when the collector is in the
  mark phase and the program stores a white object into a black one, the
  barrier makes the white object gray. The finish pause scans the roots
  again. That scan has a limit, because the stack and the globals have
  limits.
- **Snapshot at the beginning (Yuasa):** before a store overwrites a
  reference, the barrier makes the old target gray. The finish pause does
  not scan the roots again. More dead objects live to the next cycle.

**Not accepted:** the backward barrier for containers (Lua's choice for
tables, where a black table that gets a store becomes gray again). The
finish pause must trace each object that became gray again. A program that
writes into a List of millions of elements during the mark makes that pause
grow with the List, and goal 1 fails.

The fast path of either barrier is one test of a byte that the heap keeps
("is a mark in progress?"). The `stw` collector never sets it, so the test
always goes the same way.

### 4.4 Colors and new objects

- The header byte `marked` becomes a color byte (white A, white B, gray,
  black). It fits in the same byte; `sizeof(Obj)` does not change.
- An object made during the mark is black. Yuasa needs this. With Dijkstra
  it is the simple choice, and its cost is only that some dead objects live
  one cycle more.
- Two white colors, as in Lua: an object made during the sweep gets the
  white of the next cycle, so the sweep does not free it.

### 4.5 Objects that own OS handles

An unreachable `File`, `Socket`, `Server` or `Process` must close its
handle soon, even when the program stops allocating. These objects are
few. The heap keeps them in a separate list, and the finish pause frees the
unmarked ones at once. The incremental sweep does not free them. The sweep
also finishes at once before a worker goes idle (before `epoll_wait` or the
inbox wait), so a quiet program does not keep garbage.

### 4.6 Workers and foreign objects

- Messages are immutable, so no other thread stores into an object of this
  heap. The barrier runs only on the owner thread.
- A worker can raise a hold on an object of this heap during the mark. The
  finish pause reads the hold counts again, under the hold lock, as roots.
- The order rule stays: raise the counts of newly reached foreign objects
  before any release.
- `retire` (the exit collection of a worker) and a dead heap use one full
  collection. They have no program to wait for.

### 4.7 The weak String table

The finish pause removes each white String from the intern table, as
`removeWhiteStrings` does now. A String made during the mark is black, so a
new String is never removed in the same cycle.

### 4.8 Pacing

Two values, as in Lua and Go:

- **when** a cycle starts: when the heap reaches a factor of the live heap
  after the last cycle (now the factor is 2);
- **how much** work each allocated byte pays for, so that the mark and the
  sweep end before the heap grows too far.

The pacing must also keep goal 3 (the peak heap). Phase 1 measures the
values.

## 5. The collector interface

The runtime gives the collector:

- the roots (the current `markRoots` callbacks);
- the trace of each object type (the current `traceObject`);
- the size of each object (`objAllocatedSize`);
- the allocation path (`create<T>`, `rawAlloc`).

The collector gives the runtime:

- an allocation call, which is also the safe point where steps run;
- a barrier call for each store kind (a store into a field, an element, an
  entry, an upvalue, a coroutine snapshot);
- a call that makes a full collection now (for `retire`, the idle point,
  and tests);
- the trace records.

The runtime code no longer touches `marked` or `allObjects`. Only the
collector does.

### 5.1 When the collector is chosen

This question is open. The owner decides it in the decision document. The
interface must work with each answer.

| Choice | Cost | Use |
|---|---|---|
| Build time (a CMake option) | None at run time; the barrier can compile to nothing for `stw`. Each collector needs its own CI build. | The smallest and fastest binary. |
| Init time (an environment variable or a flag, read once when the heap starts) | One test of a heap byte on each barrier and allocation. All collectors are in the binary. | Choose the collector for a program, for example a server. CI runs the same binary with each collector. |
| Run time (a switch while the program runs, as in Lua 5.4) | The same as init time, plus a switch that finishes the current cycle first. It needs a standard-library API, which is a language change. | Change the collector for one phase of a program. |
| One choice for each worker | Each heap is already separate. It works with init time or run time. | A worker that serves clients can use `incremental`, and a batch worker can use `stw`. |

## 6. The barrier sites and the completeness check

The barrier is only as good as its coverage. A store with no barrier makes
the collector free a live object: a use-after-free that may show only on a
large heap.

1. **Make the compiler find the sites.** The mutable references of each
   object type (the table in `docs/reference/gc-runtime-contract.md`, section 7)
   become private. Code changes them only through member functions that run
   the barrier. A direct store then does not compile.
2. **A table and a script.** A table lists each mutator and the barrier it
   runs. A script checks that each mutable reference has a mutator in the
   table, and that no code outside the allowed files reads the raw
   container for write (AGENTS.md, "Plans for a language change", rule 2).
3. **Types without a barrier yet (Ruby's method).** A type that is not
   converted yet is marked as unprotected. The finish pause traces all
   unprotected objects again. That is correct, but it does not meet goal 1
   for a heap with many such objects. So the native library can move to the
   barrier one type at a time, and each step is correct.
4. **QBE.** Compiled code stores only into the value stack. Every heap store
   is an `rt_*` call into the runtime, so the runtime barrier covers QBE. A
   future JIT or AOT compiler that writes into heap objects in its own code
   must emit the barrier fast path.

## 7. How we show that it is correct

- **A verifier (debug builds).** At the end of each mark, a separate full
  mark from the roots, with a side table, checks that no reachable object
  is white. A missed barrier then fails at once and names the object. It
  does not depend on a later crash.
- **Stress modes.** `LOXPP_STRESS_GC=1` with `incremental` runs a small step
  at each allocation, so the program and the collector mix as often as
  possible.
- **ASan** in the debug build, as now.
- **The same tests with each collector.** The full `ctest`, the example
  corpus (under stress GC too), the QBE job and the 4 Redis shards run with
  each collector.
- **Mutations.** Remove one barrier, one finish-pause step, or the limit of
  a step, and see a test fail. A barrier mutation must fail the verifier.

## 8. Measurement and gates

- The trace format goes to v3. It must describe a cycle with steps: a record
  for the start, for each step (or a summary of the steps), for the finish,
  and for the end of the sweep. `gc_report.py` shows the maximum pause, the
  pause percentiles and the MMU for each collector. Each tool that reads the
  trace changes in the same pull request (`docs/reference/gc-runtime-contract.md`,
  section 8).
- The programs:
  - `benchmarks/`, for the speed (goal 3);
  - a large-heap test (the program of section 1, made part of
    `benchmarks/`);
  - the Redis example with the #1165 load (524286 streams).
- A CI job checks the maximum pause and the MMU of `incremental` on the
  large-heap test. The CI host is slower than a local machine and it
  varies, so the job checks a count where it can (for example, the largest
  number of objects traced in one step), and a time only with a wide
  margin.

## 9. Effect on each consumer

This is not a language change, but it touches each user of the heap:

| Consumer | Effect |
|---|---|
| Native VM | The interface, the barrier sites, the new collector. |
| QBE backend | No change to generated code; the runtime barrier covers it (section 6). The QBE CI job runs with each collector. |
| JVM backend | None. It uses the JVM's collector. |
| Bootstrap interpreter, resolver, LSP, tree-sitter, editors | None. |
| Native library (`src/stdlib/`) | Each store goes through a mutator (section 6). |
| Trace tools and the Redis probe S10 | Trace format v3. |
| A future JIT or AOT compiler | It must emit the barrier fast path. |

## 10. Phases

A sketch of the order. The decision's tracking issue holds the real nodes.

1. **Prototype and measure.** A throwaway branch with an incremental mark on
   the current heap, for the large-heap test only. It checks the numbers of
   goal 1 and chooses the barrier kind (4.3). No merge.
2. **Measurement tools.** Trace v3, MMU in `gc_report.py`, the large-heap
   benchmark.
3. **The interface.** Move the current collector behind the interface as
   `stw`. No change in behavior, and the same trace values.
4. **The barrier layer.** The mutators, the table and the script. The
   barrier does nothing yet with `stw`, and goal 3 (2%) is measured here.
5. **The `incremental` collector.** The cycle, the step limits, the pacing,
   the handle list, the verifier.
6. **The gates.** CI runs every suite with each collector.
7. **The default.** The owner decides which collector is the default.
8. **Later, separate decisions:** a heap layout with size classes and mark
   bitmaps (it makes each collector faster), and the `generational` mode.

## 11. Risks

- **A missed barrier.** The largest risk. Sections 6 and 7 reduce it: the
  compiler finds direct stores, the verifier finds a missed barrier at
  once, and unprotected types stay correct.
- **The cost on the dispatch loop.** Each store into a field or an element
  gets one more test. Goal 3 limits it, and phase 4 measures it before the
  new collector exists.
- **Fragmentation.** A non-moving heap with one `new` for each object can
  fragment. The size-class layout in phase 8 is the answer if the
  measurements show a problem.
- **The pacing.** If the pacing is wrong, the heap grows too far before the
  mark ends, or the steps take too much of the program's time. Phase 1 and
  the MMU gate find this.
- **The finish pause.** It scans the roots (with Dijkstra), the held
  objects and the handle list. A program with a very large globals table
  or a very deep stack makes it longer. The limits of the stack and the
  number of globals bound it, and the gate measures it.

## 12. Open questions

1. When is the collector chosen: build time, init time, run time, or for
   each worker (section 5.1)?
2. Which barrier kind: incremental update or snapshot at the beginning
   (section 4.3)? The phase-1 prototype gives the data.
3. Are the numbers of goals 1 and 3 right, after phase 1?
4. Is the heap layout (size classes and mark bitmaps) part of this mission,
   or a separate one later?
5. Which collector is the default at the end (phase 7)?
