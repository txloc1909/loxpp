# More than one garbage collector in the native runtime

**Status:** accepted. 2026-11-17, owner review of #1257 (part of #1165). Mission: #1277.

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

1. **A limit on latency that does not depend on the heap size.** Changed
   on 2026-11-21, after the prototype (section 13). With the incremental
   collector, at any heap size, for a program whose stack and globals are
   inside the default limits:
   - **No step of the collector grows with the heap.** One step does at
     most a fixed number of work units: objects traced, slots swept,
     elements or entries visited. A gate checks the largest count of one
     step. This is the main check, because a count does not depend on the
     host.
   - **Time of a step:** the 99.99th percentile of the collector steps is at
     most **1 ms** on the CI host. The target length of a step is 100 µs.
     The time gate has a wide margin, because hosts stall (section 13).
   - **MMU(10 ms) ≥ 50%**, computed from the collector steps: in every
     10 ms window, the program runs for at least 5 ms.
   - **Not part of this goal:** the time that the C allocator (`malloc` and
     `free`) spends inside one call. The gates report it on its own, as the
     allocator stall, and phase 8 limits it (section 13, item 4).

   Goal 3 below keeps its numbers. The prototype did not check them.
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
2. **Mark (in steps):** an allocation runs a step of mark work when the
   pacing says so (4.8). A step takes gray objects and traces them.
3. **Finish (a short pause):** the work that must be atomic: the held
   objects again, the foreign-object order rule, and the objects that own
   OS handles (4.5). The roots are not scanned again (4.3), and the weak
   String table is not scanned here (4.7).
4. **Sweep (in steps):** an allocation sweeps a part of the heap when the
   pacing says so. The sweep also removes each freed String from the
   intern table (4.7).
5. **Idle:** no collector work until the heap grows past the threshold.

### 4.2 Each step has a limit

A step must have a limit, or goal 1 fails. These objects can be very large:

- a List with millions of elements;
- a Map or a Table with millions of entries;
- the globals table, and the value stack.

So the tracer can stop inside one object and keep its position (an index
into the elements or the buckets). A large object is traced in parts, over
more than one step. The barrier (4.3) keeps the part that is already traced
correct. If a Map rehashes while it is traced in parts, its trace starts
again from the first bucket.

The same rule holds for each array that the collector or the heap owns: no
step may copy, compact or free such an array in a way that depends on the
heap size. A `std::vector` that doubles copies the whole array in one step.
So the gray stack and the list of all objects are segmented (a `std::deque`
in the prototype). The sweep does not compact the list of all objects: a
freed slot joins a chain that goes through the slots, and an allocation
takes the first slot of the chain.

### 4.3 The write barrier

**Decided on 2026-11-21: snapshot at the beginning (Yuasa).** A barrier
runs at each store of a reference into a heap object, in the mark phase
only:

- Before a store overwrites a reference in a heap object, the barrier makes
  the old target gray. The collector then keeps each object that was
  reachable when the mark started.
- The roots are scanned when the mark starts (4.1). A store into the value
  stack or into the globals needs no barrier, because the old value was
  reached at the start. The finish pause does not scan the roots again, so
  it does not depend on the depth of the stack.
- An object made during the mark is black (4.4).
- The cost is that more dead objects live to the next cycle.

**Not taken: incremental update (Dijkstra).** The barrier makes the new
target gray. It needs the roots to be scanned again in the finish pause. A
new object must also be gray, not black, because a constructor can store a
white object into it with no barrier. So the collector traces each new
object. In the prototype this was 17% slower than `stw` against 10% for
Yuasa. Its peak heap was lower (18% above `stw` against 27%), and the
difference between the two kinds was small against the noise of the host.

**Not accepted:** the backward barrier for containers (Lua's choice for
tables, where a black table that gets a store becomes gray again). The
finish pause must trace each object that became gray again. A program that
writes into a List of millions of elements during the mark makes that pause
grow with the List, and goal 1 fails.

The fast path of the barrier is one test of a byte that the heap keeps
("is a mark in progress?"). The `stw` collector never sets it, so the test
always goes the same way. A store that replaces a field of an Instance or an
entry of a Map must read the old value first, so that the barrier can mark
it. That read happens only during the mark.

### 4.4 Colors and new objects

- The header byte `marked` stays one byte, and `sizeof(Obj)` does not
  change. A set mark means gray or black. The gray stack holds the gray
  objects.
- An object made during the mark is black. Yuasa needs this.
- An object made during the sweep must survive this cycle and must be white
  at the start of the next one. The prototype sets the mark of an object in
  a slot that the sweep has not reached yet, and leaves it clear in a slot
  that the sweep has passed. Two white colors, as in Lua, are the other way.
  N5 chooses.

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

The finish pause must not remove the white Strings from the intern table.
That scan visits the whole table, and the prototype took 87 to 93 ms with
2 million Strings (section 13). Two rules replace it:

- **The sweep removes a String from the table when it frees it.** The cost
  of one String is one table deletion, and it does not depend on the size of
  the table.
- **A read barrier on the lookup.** When `makeString` or `findString`
  returns a String that is already in the table, the collector must keep
  that String. During the mark, the lookup makes it gray. During the sweep,
  the lookup must stop the sweep from freeing a String that it has not
  reached yet. N5 chooses how the barrier tells a String that the sweep has
  passed from one that it has not (for example, a bit in the header
  padding). Without this barrier, a dead String that the program finds in
  the table again is freed under the program.

The flag `PollState::marking` (`src/collector.h`) gates the write barriers and
this read barrier, and a mark sets it. The read barrier must also run during
the sweep, so the incremental collector sets the flag for the sweep too, or
adds a second flag.

A String made during the mark is black, so a new String is never removed in
the same cycle. This table is the only weak structure that the contract
lists (`docs/reference/gc-runtime-contract.md`, section 5). N5 searches the
runtime for any other weak holder before it ends.

### 4.8 Pacing

The pacing is by time, not by the bytes that the program allocates:

- **when** a cycle starts: when the heap reaches a factor of the live heap
  after the last cycle (now the factor is 2);
- **how long** a step runs: at most about 100 µs of work. After a step, the
  program runs until the collector has used its target share of the time (35%
  in the prototype);
- **an emergency rule:** if the heap grows past 1.5 times its size at the
  start of the cycle, the gap between the steps drops to zero, so the cycle
  ends.

The pacing by bytes is not accepted. The prototype paid for one large buffer
growth (a List that doubles) with a step of 10 to 16 ms, and MMU(10 ms) stayed
near 0%. Phase 5 tunes the length of a step and the target share, so that
goal 1 (including MMU(10 ms) at least 50%) and the peak heap of goal 3 both
hold. The prototype did not reach 50% on most programs with a 35% share
(section 13).

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

**Decided: init time.** The runtime reads the collector name once, when the
heap starts (an environment variable or a flag). All collectors are in the
binary. The cost is one test of a heap byte on each barrier and allocation.
The collector does not change while the program runs, so no standard-library
API is needed and no language change follows. The other choices stay in the
table as the options that were not taken.

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
   The table is section 7 of `docs/reference/gc-runtime-contract.md`, and
   `tools/check_gc_barrier_table.py` checks it. Three rules came with it.
   A mutator reads the old value after any growth of a buffer, because an
   allocation in the growth can start a mark (the bug class of 6.1, item 1).
   A removed or replaced key goes to the hook as a `Value` in its own call,
   so the hooks keep one old value and one new value. A type whose object
   holds no allocator takes the collector as an argument of its mutator.
3. **Types without a barrier yet (Ruby's method).** A type that is not
   converted yet is marked as unprotected. The finish pause traces all
   unprotected objects again. That is correct, but it does not meet goal 1
   for a heap with many such objects. So the native library can move to the
   barrier one type at a time, and each step is correct.
4. **QBE.** Compiled code stores only into the value stack. Every heap store
   is an `rt_*` call into the runtime, so the runtime barrier covers QBE. A
   future JIT or AOT compiler that writes into heap objects in its own code
   must emit the barrier fast path.

### 6.1 Named bug classes

Each class below came from the prototype. Each one is a constraint for all
remaining work. Each names the shared layer where it belongs, and the work
must check every other place that uses the same data.

1. **A missed barrier** (the heap stores of section 6). The layer: the
   mutators of each object type. A barrier that no probe forces is
   unproven. A program that stores only new objects needs no barrier, and
   it proves nothing. A probe must move old objects between large
   containers, in both directions at the same time, so that a missed barrier
   leaves a reachable white object.
2. **A weak reference that is read without a barrier** (4.7). The layer: the
   intern table, `makeString` and `findString`. Check every other weak
   holder in the runtime.
3. **A step that grows with the heap because of an array that the heap
   owns** (4.2). The layer: the data of the collector (the gray stack, the
   list of all objects, the free slots). Check each container that the
   collector uses, also the ones that it grows only in the sweep.
4. **A stall in the C allocator** (section 13, item 4). The layer: the heap
   layout of phase 8. A call of `malloc` or `free` can take longer than the
   step budget. No collector step can bound it.
5. **A call of the C allocator on a heap path that is not timed or not
   bounded** (the review of N8, PR #1293). The layer: the allocator of the
   size-class heap (N8). Three forms came up: a large block that goes back to
   the kernel in one call (its cost follows the block, and one block can be
   most of the heap); a claim of "no call" that the code did not keep,
   because the `std::` containers that objects and the collector own call
   `malloc` and `free` themselves; and a freed slot that no tool could
   report any more. The constraint: each call of the C allocator or the
   kernel on a heap path is timed, is bounded by a fixed size, and has a
   test that does not depend on time (a count, such as
   `largestReleaseBytes()`). A call that is not covered is listed in
   `docs/reference/gc-runtime-contract.md`, section 1, with an issue (#1294,
   #1295, #1296). N5 and N6 must check every other pass that uses the same
   data: the gray stack, the pause log, the gates, and every container that
   the sweep or a step frees.

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
  The prototype showed this for all barriers together. Each barrier needs
  its own probe and its own mutation (section 6.1, item 1).

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
- A CI job checks goal 1 for `incremental` on the large-heap test. The
  main check is a count: the largest number of work units in one step. The
  time checks (the 99.99th percentile of the steps, and MMU(10 ms)) have a
  wide margin. The CI host is slower than a local machine and it varies.
  The development VM of the prototype stalled a process for more than 1 ms
  about once every 0.3 s. The job reports the allocator stall on its own,
  and MMU with and without it.

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
The order of work is 1, 2, 3, 4, 8, 5, 6, 7: the heap layout (phase 8) comes
before the new collector (phase 5), because of the allocator stalls in
section 13. The owner decided this on 2026-11-21.

1. **Prototype and measure.** A throwaway branch with an incremental mark on
   the current heap, for the large-heap test only. It chose the barrier kind
   (4.3) and showed what goal 1 needs (section 13). No merge. Done.
2. **Measurement tools.** Trace v3, MMU in `gc_report.py`, the large-heap
   benchmark.
3. **The interface.** Move the current collector behind the interface as
   `stw`. No change in behavior, and the same trace values.
4. **The barrier layer.** The mutators, the table and the script. The
   barrier does nothing yet with `stw`, and goal 3 (2%) is measured here.
5. **The `incremental` collector.** The cycle, the step limits, the pacing,
   the handle list, the verifier.
6. **The gates.** CI runs every suite with each collector.
7. **The default.** The owner decides which collector is the default, by
   the criteria in section 10.1. This phase comes after phases 6 and 8.
8. **The heap layout.** Size classes and mark bitmaps (they make each
   collector faster). The layout also gives the heap its own allocator, so
   that one call cannot return memory to the operating system or rebuild
   free lists in one long step. This runs before phase 5. It works with
   `stw` only, and the collector interface of phase 3 and the barrier layer
   of phase 4 are already in place.
9. **Later, a separate decision:** the `generational` mode.

### 10.1 Criteria for the default collector

The choice is made at init time (section 5.1), so the default is one value
that the owner can change later. It is not a language change.

`incremental` becomes the default only if all of these hold. The owner
decides, and the evidence below must exist first:

1. **Goal 1 holds.** On the large-heap test and on the Redis example with
   the #1165 load (524286 streams), the CI gate of phase 6 shows the step
   counts, the 99.99th percentile of the steps, and MMU(10 ms) at least
   50%.
2. **Goal 3 holds on the finished collector.** The geometric mean of
   `benchmarks/` is at most 10% slower than `stw`, and the peak heap is at
   most 2.5× the live heap. Phase 4 measured only the barrier cost on `stw`,
   so this is the first full measurement.
3. **Phase 6 is green.** Every suite passes with each collector, including
   the stress mode and the verifier of section 7. A difference in output
   between the collectors blocks the default.
4. **The measurements use the final heap layout.** Phase 8 changes the cost
   of the trace and the sweep. It ends before phase 5, so this holds by the
   order of the phases.

If a criterion fails, `stw` stays the default. `incremental` stays in the
binary and a program can choose it. The owner can choose it as the default
later, when the failed criterion holds. The `generational` mode, when it
exists, needs a new decision and does not block this choice.

## 11. Risks

- **A missed barrier.** The largest risk. Sections 6 and 7 reduce it: the
  compiler finds direct stores, the verifier finds a missed barrier at
  once, and unprotected types stay correct.
- **The cost on the dispatch loop.** Each store into a field or an element
  gets one more test. Goal 3 limits it, and phase 4 measures it before the
  new collector exists.
- **Fragmentation.** A non-moving heap with one `new` for each object can
  fragment. The size-class layout in phase 8 is the answer, and phase 8
  measures it.
- **The pacing.** If the pacing is wrong, the heap grows too far before the
  mark ends, or the steps take too much of the program's time. The
  emergency rule of 4.8 and the MMU gate find this.
- **The finish pause.** It reads the held objects and frees the handle
  list. The prototype did not test a deep stack, a very large globals
  table, coroutines or a second worker, so the gate must measure them.
  The weak String table is no longer part of the finish pause (4.7).
- **The allocator.** One `malloc` or `free` stalled the prototype for 2 to
  7 ms, and one `free` for up to 143 ms (section 13). Phase 8 is the answer.
  Goal 1 does not count these stalls, and the gate reports them.
- **A host that stalls.** A time gate on a shared host gives false results.
  The gate checks counts first (section 8).

## 12. Decisions of the owner and what stays open

Decided on 2026-11-17 (review of #1257):

1. **When the collector is chosen:** at init time (section 5.1).
2. **The heap layout** (size classes and mark bitmaps) is part of this
   mission (phase 8).

Decided on 2026-11-21 (after the prototype of #1278):

3. **The barrier kind:** snapshot at the beginning, Yuasa (section 4.3).
4. **Goal 1 changes** to the form in section 2: counts of work for each
   step, the 99.99th percentile of the steps, and the allocator stall
   reported on its own.
5. **MMU(10 ms) at least 50% stays the goal.** N5 tunes the pacing (4.8) as
   it sees fit.
6. **Phase 8 runs before phase 5** (section 10).

Settled by a later step of this mission. The owner does not decide these
now:

7. **The numbers of goal 3:** phases 4 and 5 measure them on `benchmarks/`.
   The prototype did not.
8. **Which collector is the default:** the owner decides at phase 7, by
   the criteria in section 10.1.

## 13. Evidence from the prototype (#1278)

The prototype is the branch `feat/gc-n1-prototype`. It is never merged. It has
an incremental mark, a chunked sweep, both barrier kinds, a verifier, and
five programs in `benchmarks/gc_large_heap/`. The runs used a `release`
build on one VM with 4 cores. Each number is the median of 3 runs. Raw rows:
`benchmarks/gc_large_heap/n1-tools/matrix.out` on that branch.

| | `stw` | Dijkstra | Yuasa |
|---|---|---|---|
| Longest single pause | 380 to 600 ms | 0.4 to 1.9 ms, 87 ms on `big_map` | 1.8 to 5.3 ms, 57 and 90 ms on two programs |
| Step, 99.99th percentile | 380 to 600 ms | 0.4 to 1.5 ms, 68 ms on `big_map` | 1.7 to 5.6 ms, 70 ms on `big_map` |
| MMU(10 ms), collector steps | 0% | 0 to 61% | 0 to 23% |
| Run time, geometric mean, against `stw` | 1.00 | 1.17 | 1.10 |
| Peak heap, geometric mean, against `stw` | 1.00 | 1.18 | 1.27 |

What the data showed:

1. **The barriers are needed.** With the barriers, each program gave the
   right output and the verifier passed. With all barriers off, the
   verifier failed in both modes on `list_move`. A program that stores only
   new objects did not need a barrier. The probe covers List stores only.
2. **The weak String table** took 87 to 93 ms in the finish pause with 2
   million Strings (4.7).
3. **Arrays that the heap owns** gave steps of 1 to 16 ms (the gray stack),
   20 to 45 ms (the list of all objects), and up to 176 ms (compaction after
   the sweep) (4.2).
4. **The allocator** stalled the program. One `new` took 2 to 7 ms. One
   `free` took 27 to 143 ms inside a sweep step that had processed 16 slots.
   A larger trim threshold made the stalls smaller (5 to 24 ms) but did not
   remove them. The cause in glibc is not found. No collector can bound
   these stalls, so goal 1 does not count them and phase 8 limits them.
5. **Pacing by bytes** gave a step of 10 to 16 ms for one buffer growth.
   Time-paced steps (100 µs, 35% share) fixed this, and MMU(10 ms) reached 50%
   or more on 2 of the 10 runs-by-program cases only.
6. **The host stalls.** A tight loop saw 9 to 26 stalls longer than 1 ms in
   8 s, and one of 9.4 ms.

What the prototype did not check: the `benchmarks/` suite (goal 3), a deep
stack, a very large globals table, coroutines, a second worker, and the
probes for the barriers of Instance fields, Map entries and upvalues.
