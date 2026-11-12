# Garbage collectors in other language runtimes

A short survey for `docs/proposals/garbage-collectors.md`. For each runtime:
what its collector does, and what we can take from it. The facts are from
the public documentation and source of each runtime. Check a fact in its
source before a design depends on it.

## Terms

- **Stop the world (STW):** the program stops for the whole collection.
- **Incremental:** the collector does its work in small steps between the
  steps of the program, on the same thread.
- **Concurrent:** the collector runs on another thread at the same time as
  the program.
- **Tri-color marking:** white (not seen), gray (seen, its references not
  traced yet), black (seen and traced). A collection ends when no gray
  object is left; then each white object is dead.
- **Write barrier:** code that runs at each store of a reference into a heap
  object, so that an incremental or generational collector stays correct
  while the program changes the heap.
  - *Incremental update* (Dijkstra, or Steele's backward form): when a black
    object gets a reference to a white object, mark the white object gray
    (forward), or make the black object gray again (backward). The roots
    must be scanned again at the end of the mark.
  - *Snapshot at the beginning* (Yuasa): before a reference is overwritten,
    mark the old target gray. The collector keeps every object that was
    reachable at the start. The roots need no second scan, but more dead
    objects survive to the next cycle.
- **Generational:** young objects are collected often and alone. An old
  object that points to a young one must be remembered, through the same
  kind of barrier.
- **MMU (minimum mutator utilization):** for a window of a given length, the
  smallest share of that window that the program had, over all windows.
  It shows when many short pauses come close together, which a maximum
  pause hides.

## Lua 5.1 to 5.4

- 5.1: incremental tri-color mark and sweep, non-moving, one thread.
- Barriers: a forward barrier for most objects, and a backward barrier for
  tables (a black table that gets a store becomes gray again, so a table
  that gets many stores costs one re-trace, not one barrier for each
  store).
- Coroutine (thread) objects are not made black during the mark. They are
  traced again in the atomic step at the end, together with the roots.
- Two white colors: objects made during the sweep get the new white, so the
  sweep does not free them.
- The pacing has two numbers: `pause` (when the next cycle starts, as a
  percentage of the live heap) and `stepmul` (how much work each step does
  for each allocated byte).
- 5.4 has two modes, incremental and generational, and a program can switch
  between them at run time (`collectgarbage("incremental")`,
  `collectgarbage("generational")`). The generational mode does not move
  objects; it uses ages and the same barriers.
- **What we take:** a simple VM close to ours uses incremental marking with
  a mixed barrier and gets a pause limit with no second thread. Two
  collectors can share one heap layout and one barrier, and can be switched
  at run time.

## Ruby (CRuby)

- 1.9.3: lazy sweep.
- 2.1 (RGenGC): generational, non-moving. An object whose C code has no
  barrier is "write-barrier unprotected": it never becomes old, and it is
  scanned at each minor collection.
- 2.2 (RincGC): incremental marking. Unprotected objects are scanned again
  at the end of the mark.
- 2.7: compaction on request (`GC.compact`).
- **What we take:** a way to add barriers one type at a time. A type with no
  barrier yet is not wrong, only slower. Our native library can change in
  steps.

## Go

- A concurrent tri-color mark and sweep, non-moving, with short STW phases
  at the start and the end of the mark.
- Since Go 1.8, a hybrid write barrier (a Yuasa deletion barrier plus a
  Dijkstra insertion barrier), so the goroutine stacks are not scanned
  again at the end.
- Memory comes from size classes in spans (from tcmalloc). Mark bits are in
  bitmaps for each span.
- A pacer chooses when a cycle starts, from the `GOGC` target.
- **What we take:** size classes with mark bitmaps, a pacer, and a barrier
  that removes the second stack scan. The concurrent part needs much more
  synchronization than a single-threaded VM wants.

## OCaml

- OCaml 4: a copying minor heap and an incremental mark-and-sweep major
  heap. Each mutable store goes through `caml_modify` (the barrier).
- OCaml 5 (multicore): a minor heap for each domain, and a major heap that
  is marked and swept concurrently, with size-class pools.
- **What we take:** a generational design where each mutable store already
  goes through one function. A copying minor heap needs exact roots, and it
  moves objects; see the QBE note in `gc-runtime-contract.md`.

## Immix (MMTk)

- A mark-region heap: 32 KB blocks of 128-byte lines. The allocator bumps a
  pointer through the free lines. Objects move only when a block is too
  fragmented (opportunistic defragmentation).
- MMTk is a toolkit of collectors (mark-sweep, Immix, generational
  versions) behind one interface. Several runtimes use it.
- **What we take:** a heap layout that allocates fast and sweeps by lines,
  not by objects. MMTk shows that one runtime can have several collectors
  behind one interface: the runtime gives the roots, the object scan and
  the barrier hooks; the collector gives the rest.

## CPython

- Reference counting, plus a cycle collector with generations.
- CPython 3.14 made the cycle collector incremental.
- **What we take:** little. Reference counting changes every store and
  every load, and it does not fit a NaN-boxed tracing VM.

## Wren and clox

- A plain STW mark-sweep with one list of all objects, as we have now.
- **What we take:** this is the reference that the other collectors must
  match in behavior. It stays simple to read and to check.

## V8 (Orinoco)

- A generational heap with a parallel scavenger for young objects, and
  concurrent and incremental marking for old objects.
- **What we take:** the direction a large production VM takes. It is much
  more complex than our vision allows for now.
