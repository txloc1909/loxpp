# More than one garbage collector in the native runtime

**Status:** accepted. 2026-11-17, owner review of #1257 (part of #1165). Mission: #1277, which closes when #1308 merges.

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
   objects again and the foreign-object order rule. The handle objects (4.5)
   and the unreached foreign objects are left to the first steps of the
   sweep, which run them in parts before any span is swept. The roots are
   not scanned again (4.3), and the weak String table is not scanned here
   (4.7).
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
few. The heap keeps them in a separate list. The first sweep steps free the
unmarked ones, in parts of at most the step units, before any span is swept:
a sweep clears the marks that the list reads. The span sweep does not free
them. The handle closes at the first sweep step after the finish pause, not
in the finish pause. This is soon enough: the step follows at the next
pacing call or at the idle burst, and the finish pause no longer follows the
size of the list (#1299). A handle that the program makes while the stage
runs is black, and it goes to a separate list that the stage does not read.
Before a
worker goes idle (before `epoll_wait` or the inbox wait), the collector uses
the idle time for steps of the cycle, for at most 0.5 ms each time. It does
not run the rest of the cycle at once: a wait can be one millisecond long,
and a request can arrive at any time (review of #1298).

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

**Decided on 2026-11-26 (#1285): the default is `incremental`.** All four
criteria held on `main` at 7b50fc53, with the size-class heap of phase 8.
The choice follows the criteria of this section. It also follows an
instruction of the owner in the agent session of 2026-11-26, which the
mission orchestrator relayed to the implementer: the choice can follow from
the data of the earlier nodes, after N5, N6 and N8 merged. That instruction
is not in a GitHub comment by the owner; the owner's approval of the pull
request that sets this default is the owner's record of the choice. The
evidence, with each run and each command, is in the [comment on
#1285](https://github.com/txloc1909/loxpp/issues/1285#issuecomment-7193262258):

1. Large-heap programs and the Redis load, 3 runs each, 15 runs: the largest
   step 8374 units (limit 10420), p99.99 of the steps 115 to 257 us, MMU(10
   ms) 56.3 to 61.3%. Each run meets each limit.
2. `benchmarks/`, 18 programs, interleaved: geometric mean 1.005 of the time
   of `stw` (range 0.983 to 1.062). Peak heap over live heap: 1.81 to 2.03 on
   the large-heap programs, 2.20 on `churn_peak.lox`.
3. The whole ctest passes with each collector, the examples pass with
   `incremental` under stress GC, and the output of the collectors is the
   same on the 132 examples.
4. The measurements use the heap of phase 8.

`stw` stays in the binary, and `LOXPP_GC=stw` chooses it. The default is
`kDefaultCollector` in `src/collector.h`, and the native VM and the QBE
runtime both read it. `incremental` runs the large-heap programs 1.09 to
1.49 times longer in wall time than `stw`, because of the pacing (section
14). No criterion measures this. A program that does not need short pauses
can choose `stw`.

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
   the criteria in section 10.1. Decided on 2026-11-26: `incremental`
   (section 10.1, #1285).

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

## 14. What N5 chose (#1282)

`IncrementalCollector` (`src/incremental_collector.{h,cpp}`) is the
`incremental` collector. `LOXPP_GC=incremental` chooses it. At N5, `stw` stayed
the default; N7 made `incremental` the default (section 10.1).

- **One white color.** The sweep does not use a second color. Each span has an
  epoch (`Span::sweepEpoch`). An object that the heap makes during the sweep
  is marked when its span is not swept, and is left white when its span is
  swept. This is the rule of 4.4 without a bit in the header.
- **String read barrier.** During a mark, a lookup that finds a String marks
  it. During a sweep, it marks the String only when the span of the String is
  not swept. A String of another heap is marked in the mark, and while the
  foreign stage of the sweep (the Weak String table item) has not ended. `PollState::internBarrier` is true in the mark and the sweep.
  `PollState::marking` is true in the mark only, so the store hooks cost
  nothing in the sweep.
- **Weak String table.** The sweep removes a String from the table when it
  frees it. The foreign stage of the first sweep steps removes each foreign
  String that it releases, entry by entry, before it gives back the hold. No
  step scans the table.
- **Gray stack and handle list.** Linked blocks of 2048 bytes from the buffer
  classes of the heap (`src/seg_stack.h`). No step copies an array.
- **Chunks.** The tracer cuts a List, a Map, an Instance, a Class, a Coroutine
  stack and a Task mailbox into parts of 64 units. A Map or a Table that
  rehashes between two parts is traced again from bucket 0.
- **Pacing.** A step stops at 8192 units or 100 us. The program runs until
  the collector has 25% of the time since the cycle began. A cycle starts at
  1.5 times the live heap that the last cycle found (at least 1 MiB), where
  the live heap is the heap at the start of that cycle less the bytes that
  its sweep freed. The heap after a cycle is not used: it holds the objects
  that the program made during the cycle (they are black), and a pacing that
  counted them as live let the peak grow to 2.4 to 3.5 times the live heap
  on a long churn (review of #1298). When the heap passes 1.3 times its size
  at the start of the cycle, the share rises to 45%. It stays under one
  half, so that MMU(10 ms) stays above 50%.
- **Finish pause.** It reads the held objects, the inbox targets and the drain
  batch again under their locks, traces what is left, runs the verifier, sets
  the handle stage and the foreign stage, and starts the sweep. The stages
  run in the first sweep steps: `runReleaseStages` frees the unmarked handle
  objects, then drops the unreached foreign entries (`ForeignSet::scanStep`),
  each in parts of at most the step units. The count of a foreign object is raised when the mark
  reaches it (`markForeign`), not in the finish pause: the program can give
  back the count of an envelope in flight during a mark of many steps, and
  the owner would free the object. This was a defect of the first version of
  N5 (review of #1298, found with `foreign_string.lox`).
- **Idle and abort.** `Collector::onIdle` runs steps of the cycle for at most
  0.5 ms before a worker sleeps. `collectNow` (and so `retire` and a dead heap) cuts a cycle
  in progress short and runs one full cycle.
- **Verifier.** A separate mark from the roots, with a hash set, at the end of
  each mark. It runs in debug builds, or with `LOXPP_GC_VERIFY=1`.
- **Stress.** `LOXPP_STRESS_GC=1` runs one step of 16 units at each
  allocation.

### Measurement

`release` build, no verifier, 3 runs of each program (median, or worst of 3).
Programs of `benchmarks/gc_large_heap/`. Largest `units` of one step,
99.99th percentile of the step time, MMU(10 ms) (from `tools/gc_report.py`),
and the peak heap over the live heap. The peak is the largest `bytes_peak` of
a cycle record (the pacing looks every 16 KiB). The live heap is the largest
`bytes_after` of a `stw` threshold collection of the same program.

| Program | largest units | p99.99 | MMU(10 ms), worst | peak / live, median, worst |
|---|---|---|---|---|
| big_map | 8370 | 233 us | 56.5% | 1.81, 1.82 |
| list_write | 8202 | 214 us | 57.5% | 2.01, 2.01 |
| live_then_churn | 8224 | 208 us | 60.9% | 2.02, 2.09 |
| long_chain | 8203 | 116 us | 60.5% | 1.97, 1.97 |
| live_then_churn, churn 24000000 | 8224 | 187 us | 60.6% | 2.16, 2.17 |

`stw` has one pause of 88 to 214 ms on the same programs and MMU(10 ms) of 0%.
`test/gc_incremental/latency/idle_wait.lox` (a live heap of 1.5 million
objects and 400 short waits) has MMU(10 ms) of 62 to 70%; with the cycle run
to its end at each idle it was 0.3%. The `benchmarks/` suite, 18 programs, has
a geometric mean of 0.996 of the time of `stw` (range 0.966 to 1.059). The
programs of `benchmarks/gc_large_heap/` themselves run longer than with `stw`
(wall time, one run: big_map 4.40 s against 3.51 s, list_write 2.43 s against
1.64 s, live_then_churn 1.94 s against 1.74 s, long_chain 2.10 s against
1.83 s), because the cycles start from the live heap and run at a share of 25
to 45% of the time.

Known gaps, and where they are filed:

- The finish pause reads the held objects, the inbox targets, the drain batch
  and what these make gray. #1351. The handle list, the foreign set, the start
  pause and `markForeign` no longer follow the size of the foreign set:
  resolved in #1299 (PR #1350).
- `MemoryManager::releaseHold` runs `delete this` when the owner heap is dead
  and the hold is its last: one sweep step (or one `stw` collection) can
  destroy a whole other heap. #1352.
- A cycle with cause `exit` is one step of the whole heap. It runs after the
  program ends, and `tools/gc_report.py` shows it on its own line. #1300.
- Function, Closure, Enum, DeferredCall and Bound* objects are not cut into
  parts. The language bounds the size of each. #1301.
- The start pause reads all roots at once: it grows with the number of tasks
  and of globals. This breaks goal 1 above the sizes of "The limit of the start pause"
  below. #1302.
- A worker that waits for a long time on one wait runs 0.5 ms of its cycle
  and sleeps. #1303.
- The allocator stalls of section 13 (item 4) stay. Goal 1 does not count
  them (#1294, #1295, #1296).

### The limit of the start pause (#1302)

The start pause reads the roots of the program in one step. Its size is the
number of roots, one unit for each. We chose to state this limit and not to
read the roots in parts, for these reasons:

| Root kind | Barrier today | Bound |
|---|---|---|
| Value stack, frames, open upvalues | None, by design (4.3): each instruction writes a slot | `FRAMES_MAX` frames: 1000 frames are 10114 units, 66 us |
| Globals (`m_globals` and the global slots of compiled code) | None | The globals of the program text: 32000 globals are 64111 units, 1.25 ms |
| Run queue, receiving set, waiting map | None: `std` containers, about 25 mutation sites in `src/runtime.cpp` | The number of tasks of the program, with no bound |

Reading in parts needs, for each kind, a barrier at each write path (the
interpreter, the global slots of compiled code and QBE, each site of the
three task containers) and a cursor that survives insert and erase. The value
stack cannot get a barrier at a cost that 4.3 accepts, so it stays in the
start pause in any design. A missed site is a use-after-free that shows only
on a large heap (6.1, "a missed barrier"). We do not take that risk for a
case that the language does not need often. If a program needs it, a new
decision can replace this limit with the parts and the barriers above.

**Goal 1 does not hold for a program with more than about 55000 waiting tasks
or about 25000 globals.** The start step grows with the roots, and the p99.99
of the steps is then above 1 ms. This is a defect against goal 1 that we
accept for now, and not a case that goal 1 leaves out. The contract gap that
goes with it is #1368.

The cost model: the start step is the roots, about 18 ns for each task that
waits (release build; 100000 tasks in `task.receive` are 100113 units, 1.8 to
1.95 ms in 5 runs) and about 39 ns for each global. The step passes 1 ms at
about 55000 waiting tasks, or about 25000 globals. The probe has about 1200
steps, so its p99.99 is its largest step, and a cycle that is not often does
not lower it. Measured on the probe with more tasks (release build):

| Waiting tasks | Start step units | p99.99 of the steps | MMU(10 ms) |
|---|---|---|---|
| 100000 | 100113 | 1777 us | 62.5% |
| 200000 | 200113 | 4308 us | 56.9% |
| 300000 | 300113 | 5853 us | 41.5% |

The start step and the p99.99 limit are broken above about 55000 tasks. The
MMU(10 ms) limit of 50% is broken between 200000 and 300000 tasks.
`test/gc_incremental/latency/many_tasks.lox` and
`check_many_tasks` in `tools/check_gc_incremental.py` assert the limit: the
start step has between one and 1.5 units for each task, and at most 10 ms.

Named bug classes that the review of #1298 found, as constraints for all
later work:

- **A structure that the tracer cuts by index needs a rule for a removal that
  moves the elements that stay.** The barrier of a pop sees the element that
  leaves, not the ones that move. The Task mailbox counts the messages that
  left the front (`ObjTask::poppedCount`), and the tracer moves its index by
  the change. A List removal (`eraseAt`) and a mailbox erase shade each moved
  element. Every other structure that the tracer cuts (Map, Instance, Class,
  Coroutine stack) was checked: a Map cut by bucket restarts when it
  rehashes, and a deleted entry is a tombstone, so nothing moves.
- **A count that a mark reaches must be raised at the reach, not at the end of
  the mark.** See the finish pause above.
- **A pacing that counts the objects of the cycle as live.** See Pacing.

## 15. Shipped

The mission #1277 closes when #1308 merges. The default collector is
`incremental`. `stw` stays in the binary, and `LOXPP_GC=stw` chooses it. The
mission changed no language rule and no spec text. This section changes no
accepted text above it.

### 15.1 The nodes

| Node | Issue | PR | Merge commit | What it delivered | Evidence |
|---|---|---|---|---|---|
| N1 | #1278 | #1288 (the result, in section 13). The prototype branch is never merged. | `9deb416f` | The prototype, the choice of the barrier kind (Yuasa), and the new form of goal 1. | Section 13. |
| N2 | #1279 | #1289 | `cb3678d9` | Trace format 3, the MMU report in `tools/gc_report.py`, the large-heap programs in `benchmarks/gc_large_heap/`. | `docs/records/gc-stw-baseline-2026-11-21.md`. |
| N3 | #1280 | #1290 | `e6d2d330` | The collector interface. The old collector is `stw` behind it. | `tools/check_gc_stw_counts.py` and the pinned counts. The review compared 48 collections of 4 programs with the baseline of N2: all equal. |
| N4 | #1281 | #1291 | `8ac21c37` | The write-barrier layer: private references behind mutators, the table, and `tools/check_gc_barrier_table.py`. | The review measured the cost of the barrier on `benchmarks/` at 1.0001 of the base (limit 1.02). |
| N8 | #1284 | #1293 | `ef3888cc` | The size-class heap, mark bitmaps and the heap's own allocator. Stall records. Large blocks go back to the operating system in slices. | The review measured the geometric mean of `benchmarks/` at 0.971 of the base. A program that drops a 60 M-element List three times has a peak RSS of 725 MB (ratio 1.89 over the live heap). |
| N5 | #1282 | #1298 | `4016b571` | The `incremental` collector (section 14). | Section 14, "Measurement". The review found 4 blocking defects and each fix has a check that fails without it. |
| N6 | #1283 | #1304 | `7b50fc53` | CI runs each suite with each collector. The gate `tools/check_gc_pause.py`, the check `tools/check_gc_collector_runs.py` and the comparison `tools/diff_gc_collectors.py`. | The review ran `ctest` with each collector: 2392 of 2392. The gate holds a self-test with a case for each of its limits. |
| N7 | #1285 | #1306 | `11bef6b2` | `incremental` is the default. | The comment https://github.com/txloc1909/loxpp/issues/1285#issuecomment-7193262258 (issue #1285). All four criteria of section 10.1 held. |

The evidence of N7 for the goals: 15 of 15 runs of the large-heap programs
and of the Redis load (524286 streams) have a largest step of 8201 to 8374
units, a p99.99 of 115 to 257 us and an MMU(10 ms) of 56.3 to 61.3%. The
geometric mean of `benchmarks/` is 1.005 of `stw` (limit 1.10). The peak is
at most 2.20 times the live heap (limit 2.5).

### 15.2 The follow-ups

Each follow-up is one open issue. This list is the state of the issues at the
close, and the issues answer "is it done".

| Issue | What it is | Where the decision speaks of it |
|---|---|---|
| #1292 | The barrier check misses a function that returns a pointer to a pointer to a slot. | Section 6, item 1. |
| #1294 | The `std` containers that objects own are not timed. | Section 14, allocator stalls. |
| #1295 | The gray stack of `stw` is one array. | Section 14, allocator stalls. |
| #1296 | The `std` containers of `MemoryManager` are not timed. | Section 14, allocator stalls. |
| #1297 | The large-block release work of one step is not bounded as a sum. | Sections 4.2 and 6.1, item 5. |
| #1299 | The finish pause, the start pause and `markForeign` follow the foreign set and the handle list. | Section 14, known gaps. |
| #1300 | The `exit` cycle is one step of the whole heap. | Section 14, known gaps. |
| #1301 | Function, Closure, Enum, DeferredCall and Bound objects are not cut into parts. | Section 14, known gaps. |
| #1302 | The start pause grows with the number of tasks and globals. | Section 14, known gaps. |
| #1303 | A worker that waits for a long time keeps a cycle in the middle of its mark. | Section 14, known gaps. |
| #1305 | The test of the `pi` watchdog is flaky on CI. | None. It is a harness test. |
| #1307 | The peak-ratio check of `churn_peak` can fail under host load. | Sections 10.1 and 14. |

The problem #1165 stays open. The mission removed the pause that grows with
the heap for the programs in the gate. The gaps above are the cases where a
step still grows, and the owner decides when #1165 closes.

The wall time of the large-heap programs is 1.09 to 1.49 times the time with
`stw`, as the #1285 comment shows. This is a cost of the pacing, it is no
defect, and no issue exists for it. A program that needs no short pauses
chooses `LOXPP_GC=stw`.

The retrospective is `docs/records/garbage-collectors-retro.md`. It
proposes four rules for `AGENTS.md` and the playbook (#1309, #1310, #1311,
#1312) and one proposal for a generational mode (#1313). Each is an open
issue and binds nothing until a PR lands it.
