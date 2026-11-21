# What the native runtime needs from a collector

This document states facts about the native heap as it is. Other code relies
on these facts, so any collector that replaces or joins the current one must
keep them, or change the code that relies on them in the same pull request.
`docs/decisions/garbage-collectors.md` uses this list.

Keep this document current: a pull request that changes one of these facts
changes this file.

## 1. The heap and its objects

- The heap is `MemoryManager` (`src/memory_manager.h`). There is one heap
  for each worker. Only the thread of that worker allocates in it and
  collects it.
- The heap has one collector, an object of a class that derives from
  `Collector` (`src/collector.h`). `LOXPP_GC` names it when the heap starts.
  The only name is `stw`, the default: `StwCollector`
  (`src/stw_collector.{h,cpp}`). An unknown name ends the process with
  status 64. `MemoryManager::collector()` gives it. `MemoryManager` keeps
  its own API (`create<T>`, `makeString`, `pushTempRoot`, `markObject`,
  `collectGarbage`, `collectionCount`) and forwards to the collector.
- `MemoryManager::create<T>` makes each object with one `new`, and gives the
  pointer to the collector (`Collector::onCreate`). `StwCollector` keeps it
  in `allObjects`, a plain `std::vector<Obj*>` that only the collector
  touches. Its sweep calls `delete` on each dead object.
- The header (`struct Obj`, `src/object.h`) has a vtable pointer, `type`
  (1 byte), `marked` (1 byte) and `heapId` (2 bytes). Four bytes of padding
  follow. `marked` is private. Only `StwCollector` is its friend, so a use
  anywhere else does not compile. `heapId` is set once and never changes, so
  another thread can read it at any time.
- `Collector` has the hooks of the write barrier (`barrierField`,
  `barrierElement`, `barrierEntry`, `barrierUpvalue`, `barrierSnapshot`)
  and the read barrier of the intern table (`internHit`). They are empty for
  `stw`, which never sets `PollState::marking`. Only `internHit` has callers
  today (the lookups of the intern table). The runtime does not call the
  store hooks yet.
- There are 23 object types (`enum class ObjType`). These own OS handles
  and close them in their destructor: `ObjFile`, `ObjSocket`, `ObjServer`,
  `ObjProcess`. So the time of the sweep is the time when an unreachable
  handle closes.
- `create<T>` rejects a pointer that the NaN tag cannot hold
  (`pointerFitsInValue`). An allocator that gets memory in another way must
  keep this check.

## 2. When a collection can run

- A collection runs only inside an allocation: `create<T>`, or `rawAlloc`
  (through `VmAllocator`, for the buffers of Strings, Lists, Maps, Tables
  and coroutine snapshots). No collection runs at any other point.
- So every allocation is a safe point, and C++ code may hold a raw `Obj*`
  in a local only while it does not allocate, or while the object is
  rooted.
- A container buffer grows through `VmAllocator::allocate`, so a collection
  can run in the middle of a `std::vector` or a hash map growth. At that
  moment the old buffer is still complete and the container is consistent:
  the new buffer is allocated before any element moves.
- `LOXPP_STRESS_GC=1` runs a full collection at each allocation.
- The threshold: `PollState::pollAt = max(2 × bytesAllocated after the
  sweep, 1 MB)`. `PollState` is in the heap (`MemoryManager::m_poll`), and
  the collector writes it. `MemoryManager::shouldPoll` compares
  `bytesAllocated` with it on each allocation, and calls
  `Collector::poll` when the heap is above it (or when stress is on).
- Byte accounting: `bytesAllocated` counts `sizeof(T)` for each object
  plus each buffer that `VmAllocator` allocates. `objAllocatedSize`
  (`src/object_trace.h`) gives the size that the sweep subtracts.

## 3. Roots

- The runtime marks each root through `MemoryManager::markObject` and
  `markValue`, which call the collector. `MemoryManager::markAllRoots` calls
  every source below. The references of each object type are in
  `traceObjectEdges` (`src/object_trace.h`), a template over the marker, so
  every collector uses the same code.
- `Runtime::markRoots` (`src/runtime.cpp`): the value stack from `stack` to
  `stackTop`, the closure and the deferred calls of each frame, the open
  upvalues, the globals table, the built-in classes, the current coroutine,
  the yielded value and the main task.
- `Compiler::markRoots` while a compiler runs.
- Temporary roots: `pushTempRoot`/`popTempRoot`, a stack of `Obj*`. About
  85 call sites in the runtime, the compiler and the native library use it.
- Held objects: an object of this heap that another worker holds
  (`m_holders`, read under `m_holdMutex` in one step at the start of a
  collection).
- The targets of envelopes in the inbox, and the batch that the drain took
  out and has not delivered yet (`m_draining`).
- A coroutine that is not running keeps its stack slice, frames, handlers,
  deferred calls and open upvalues in `ObjCoroutine` vectors. They are heap
  data, not roots: the coroutine object is traced.

## 4. QBE-compiled code

- `libloxrt` bundles `loxpp_core`, so a `--target qbe` program uses this
  same heap and collector.
- Compiled code stores Values only into the runtime value stack, which is a
  root. Each store into a heap object (a field, an index, an upvalue, a
  global) is a call into the runtime (`rt_op_set_property`,
  `rt_op_set_index`, `rt_set_upvalue`, `rt_op_set_global_at`, and the other
  `rt_*` functions in `src/backend/rt_capi.h`).
- Register promotion (`src/backend/qbe_promotion.h`) keeps some locals in
  registers. It writes them back to their stack slots before each call that
  can allocate. So the collector sees them only at a safe point. A
  collector that moves objects would break this: a promoted register would
  keep the old address.

## 5. Weak references

- The String intern table (`m_strings`) holds its Strings weakly.
  `StwCollector::removeWhiteStrings` removes each unmarked String after the
  trace and before the sweep. Each lookup that finds a String calls
  `Collector::internHit` (`makeString`, `findString`, `receiveMessage`).
- Every String is interned, so String equality is pointer equality. A
  collector must not free an interned String that is still reachable, and
  must remove each dead String from the table before it is freed.

## 6. Workers and foreign objects

From `docs/decisions/concurrency-model-decision.md`, "Lifetime across
workers":

- A message object stays in the heap that made it. A worker that receives
  it reads it in place. A message is immutable, so no other thread writes
  into an object of this heap.
- Another thread reads the fields of a held object while the owner can
  collect. The owner does not free it, because a held object is a root.
- `m_foreign` is the set of objects of other heaps that this heap holds,
  and their side marks. The owner's collector marks them there, never in
  their header.
- Order rule: a collection raises the counts of newly reached foreign
  objects (`adoptReachedForeign`) before it releases any count
  (`releaseUnreachedForeign`).
- `retire` runs one last collection whose only roots are the held objects
  (trace cause `exit`). A dead heap stays until its last hold is released,
  and the last `releaseHold`, on any thread, frees it.

## 7. Stores of a reference into a heap object

A store of a Value or an `Obj*` into a heap object happens through these
paths. An incremental or generational collector needs a barrier on each of
them. A first grep finds about 40 direct sites. The true list is a task of
the proposal, not of this file.

| Object | Mutable references |
|---|---|
| `ObjInstance` | `fields` (a `Table`) |
| `ObjClass` | `methods`, the superclass link at class creation |
| `ObjList` | `elements` (a `VmVector<Value>`) |
| `ObjMap` | `map` (a `CoreHashMap` of key and value) |
| `ObjUpvalue` | the closed value, and `location` |
| `ObjClosure` | its upvalue array, filled after creation |
| `ObjCoroutine` | the snapshot vectors, written on each suspend |
| `ObjTask`, `ObjIterator`, `ObjError`, `ObjDeferredCall` | fields that are set after creation (to be listed) |

These types do not change after creation: `ObjString`, `ObjInt`, `ObjEnum`,
`ObjBoundMethod`, `ObjBoundNative`, `ObjNative`, `ObjEnumCtor`,
`ObjFunction` (after the compiler is done with it).

## 8. Observation

- `LOXPP_GC_TRACE=<file>` writes one record for each collection (format
  v3, described in `tools/gc_report.py`). The stop-the-world collector writes
  `gc` records only. The format also has `cycle`, `step` and `stall` records,
  for an incremental collector and for the heap allocator. Nothing writes them
  yet. These read the trace: `tools/gc_report.py` (and
  `tools/jvm_gc_log.py` through `summarise_gcs`), `tools/check_gc_trace.py`,
  `tools/check_gc_trace_v3.py`, `tools/check_gc_min_heap.py`,
  `tools/check_gc_sweep_linear.py`, `benchmarks/gc_compare.py`, and the Redis
  example's `tests/run_stages.py` (probe S10 reads `objs_before` of `gc`
  records).
- `benchmarks/gc_large_heap/` holds the programs that keep a heap of millions
  of objects. `docs/records/gc-stw-baseline-2026-11-21.md` gives the object
  counts of each collection of these programs with this collector.
- `collectionCount()` is read by tests.
- The spec makes the timing of garbage collection implementation-defined
  (`spec/README.md`, "Conformance", item 4). No program can see when an
  object is freed, except through the time when an OS handle closes.
