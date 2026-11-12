# What the native runtime needs from a collector

This document states facts about the native heap as it is (main at
36284862). Other code relies on these facts, so any collector that replaces
or joins the current one must keep them, or change the code that relies on
them in the same pull request. `docs/proposals/garbage-collectors.md` uses
this list.

Keep this document current: a pull request that changes one of these facts
changes this file.

## 1. The heap and its objects

- The heap is `MemoryManager` (`src/memory_manager.h`). There is one heap
  for each worker. Only the thread of that worker allocates in it and
  collects it.
- `MemoryManager::create<T>` makes each object with one `new`, and keeps
  the pointer in `allObjects`, a plain `std::vector<Obj*>`. `sweep` calls
  `delete` on each dead object.
- The header (`struct Obj`, `src/object.h`) has a vtable pointer, `type`
  (1 byte), `marked` (1 byte) and `heapId` (2 bytes). Four bytes of padding
  follow. `marked` belongs to the collector of the heap that owns the
  object. `heapId` is set once and never changes, so another thread can
  read it at any time.
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
- The threshold: `m_nextGC = max(2 × bytesAllocated after the sweep,
  1 MB)`.
- Byte accounting: `bytesAllocated` counts `sizeof(T)` for each object
  plus each buffer that `VmAllocator` allocates. `objAllocatedSize` gives
  the size that `sweep` subtracts.

## 3. Roots

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
  `removeWhiteStrings` removes each unmarked String after the trace and
  before the sweep.
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
  v2, described in `tools/gc_report.py`). These read it:
  `tools/gc_report.py`, `tools/check_gc_trace.py`,
  `tools/check_gc_min_heap.py`, `benchmarks/gc_compare.py`, and the Redis
  example's `tests/run_stages.py` (probe S10 reads `objs_before`).
- `collectionCount()` is read by tests.
- The spec makes the timing of garbage collection implementation-defined
  (`spec/README.md`, "Conformance", item 4). No program can see when an
  object is freed, except through the time when an OS handle closes.
