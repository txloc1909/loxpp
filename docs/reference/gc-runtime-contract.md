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
  The names are `stw`, the default: `StwCollector`
  (`src/stw_collector.{h,cpp}`), and `incremental`: `IncrementalCollector`
  (`src/incremental_collector.{h,cpp}`). An unknown name ends the process
  with status 64. `MemoryManager::collector()` gives it. `MemoryManager` keeps
  its own API (`create<T>`, `makeString`, `pushTempRoot`, `markObject`,
  `collectGarbage`, `collectionCount`) and forwards to the collector.
- The memory of a heap is `SizeClassHeap` (`src/size_class_heap.{h,cpp}`),
  the member `MemoryManager::m_store`. It cuts chunks of 1 MiB into spans of
  64 KiB. A span holds slots of one size class (24 classes, 16 to 2048
  bytes), and the span of a pointer is `ptr & ~0xFFFF`. Objects and buffers
  use separate spans. A block over 2048 bytes is a large block with its own
  allocation, in a class of the form m * 2^k (m = 4 to 7).
- `MemoryManager::create<T>` takes a slot with `allocObject`, runs the
  constructor, and then calls `commitObject`. Only a committed object is in
  the allocation bitmap of its span, so a collection that runs inside the
  constructor does not see it. Then it calls `Collector::onCreate`, which is
  empty for `stw`. There is no list of all objects: the collector finds
  the objects through the spans (`firstObjectSpan`, `sweepSpan`,
  `forEachObject`). A sweep that goes through the spans in more than one
  step calls `beginSweep` first. Each span keeps the epoch of the last sweep
  that visited it (`Span::sweepEpoch`), and a span that the heap takes during
  a sweep counts as swept, so `isUnswept(obj)` tells a collector whether the
  sweep has to reach the span of `obj`. `sizeof(T)` must be at most 2048 and the alignment at
  most 16 (a `static_assert` in `create<T>`).
- The mark bit of an object is a bit in the mark bitmap of its span
  (`SizeClassHeap::tryMark`, `isMarked`). Only a collector reads or writes
  it, for the objects of its own heap. The sweep frees `alloc & ~mark` of a
  span, word by word, runs the destructor, and puts the slot on the free
  list of the span. An object never moves.
- The header (`struct Obj`, `src/object.h`) has a vtable pointer, `type`
  (1 byte) and `heapId` (2 bytes). Five bytes of padding follow. `heapId`
  is set once and never changes, so another thread can read it at any time.
- The allocator limits its own stalls. A small allocation or free of the
  memory of the heap (an object slot, or a `VmAllocator` buffer of at most
  2048 bytes) never calls the C allocator or the kernel. An empty span goes
  to a pool that every class takes from. A span or a chunk is not returned
  to the operating system before the heap ends. A freed large block waits in
  a cache for its class. The cache holds at most the larger of the bytes of
  the large blocks in use and 4 MiB. A block over that limit, and a cached
  block when the large blocks in use fall under the cache, waits in a queue.
  One slice of at most 1 MiB goes back to the kernel (`munmap`) for each
  allocation of a large block, each free of a large block, and each span of a
  sweep (`SizeClassHeap::releaseStep`). `largestReleaseBytes()` gives the
  largest slice that one call gave back. The release rate is tied to the map
  rate: before the heap maps N new bytes (a large block that the cache does
  not hold, or a new chunk), it gives back N bytes from the queue, in slices
  of at most 1 MiB, and it moves cached blocks to the queue if the queue is
  empty. So the queue never grows while the program grows, and during a
  make-and-drop pattern the footprint stays at most 2.5 times the largest
  live heap (`AGrowingBufferThatIsMadeAndDroppedStaysInGoal3`). No operation walks or copies an
  array whose size follows the heap size: free lists are intrusive, the
  lists of spans are linked through the span headers, and a bitmap has a
  fixed size for each span.
- The calls that the heap itself makes to the C allocator or the kernel are
  these, and the heap times each one: a new chunk (`operator new`, 1 MiB and
  64 KiB), the map of a large block (`mmap`), and a slice of the release of a
  large block (`munmap`). The heap writes a `stall` record to the GC trace
  (section 8) for each one longer than 100 us (`LOXPP_GC_STALL_LIMIT_US`
  changes the limit in microseconds, and 0 records every call). A test
  (`EveryCallToTheCAllocatorOfTheHeapIsTimed`) counts these calls and the
  records.
- These calls to the C allocator are **not** timed, and no `stall` record
  covers them. The C allocator serves them through `std::` containers that
  an object or the collector owns. They are known gaps, each with an issue:
  the containers of the objects (`ObjClosure::m_upvalues`, the `Chunk` of an
  `ObjFunction`, the `std::string` and `std::shared_ptr` members of the
  container objects, the profiler members), #1294; the gray stack of
  the stop-the-world collector, #1295 (the incremental collector does not
  have this gap: its gray stack and its handle list are linked blocks of 2048
  bytes from the buffer classes of the heap, `src/seg_stack.h`, so no push or
  pop calls the C allocator); the containers of `MemoryManager` (`m_foreign`,
  `m_reachedForeign`, `m_tempRoots`, `m_draining`, `m_holders`), #1296. The
  sweep runs the destructors of the objects, so the `free` calls of the first
  class run inside the sweep. The test
  `TheUntimedCallsAreTheOnesTheContractNames` pins the first class.
- A freed slot, and a cached large block, are poisoned for AddressSanitizer
  (`ASAN_POISON_MEMORY_REGION`), and a slot that the heap hands out again is
  unpoisoned. A debug build then reports a read of a freed object or buffer
  as `use-after-poison`.
- `Collector` has the hooks of the write barrier (`barrierField`,
  `barrierElement`, `barrierEntry`, `barrierUpvalue`, `barrierSnapshot`)
  and the read barrier of the intern table (`internHit`). They are empty for
  `stw`, which never sets `PollState::marking` or `PollState::internBarrier`.
  The incremental collector sets `marking` during a mark (the five store hooks
  test it) and `internBarrier` during a mark and a sweep (`internHit` tests
  it). The lookups of the intern table call `internHit`. Each store of a reference into a heap object calls a
  store hook, through a mutator of the object type (section 7).
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
- `LOXPP_STRESS_GC=1` runs a full collection at each allocation with `stw`.
  With `incremental` it runs one step of 16 units at each allocation, and a
  new cycle starts when the last one ends.
- `Collector::onIdle` is called by `Runtime::waitForWork` before a worker with
  an empty inbox may sleep. `incremental` finishes its cycle there, so a quiet
  worker keeps no garbage and no OS handle. `stw` does nothing.
- The threshold: `PollState::pollAt = max(2 × bytesAllocated after the
  sweep, 1 MB)`. `PollState` is in the heap (`MemoryManager::m_poll`), and
  the collector writes it. `MemoryManager::shouldPoll` compares
  `bytesAllocated` with it on each allocation, and calls
  `Collector::poll` when the heap is above it (or when stress is on).
- Byte accounting: `bytesAllocated` counts `sizeof(T)` for each object
  plus the requested bytes of each buffer that `VmAllocator` allocates. It
  does not count the rounding of a size class. `objAllocatedSize`
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
  trace and before the sweep. The incremental collector does not scan the
  table: its sweep removes a String when it frees it, and its finish pause
  removes the foreign Strings that it releases
  (`releaseUnreachedForeign(true)`). Each lookup that finds a String calls
  `Collector::internHit` (`makeString`, `findString`, `receiveMessage`). The
  incremental collector marks that String during a mark, and during a sweep
  when the sweep has not reached its span.
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

A store of a reference into a heap object goes through a mutator, a member
function of the object type. The mutator runs the write barrier hook of the
collector (`src/collector.h`, `src/barrier.h`). The members that hold a
reference are private, so a direct store does not compile. A reader gets a
const accessor. The hooks do nothing for `stw`.

The rules of a mutator:

- It calls the hook before it overwrites or removes a reference. The hook gets
  the old value and the new value. A mutator reads the old value only while
  `Collector::marking()` is true.
- It reads the old value after any growth of a buffer. A growth calls the
  allocator, and an allocation can collect and start a mark. A store that can
  grow calls `ensureRoom()` first, so the hook and the store make no
  allocation. A push has no old value, so its hook runs after the growth.
- The hooks have one old value and one new value. A store that removes an
  entry of a Table or a Map, or that replaces its key, calls the hook twice:
  first for the key as a `Value`, then for the value.
- A type whose object holds no allocator (Closure, Upvalue, Iterator, Socket,
  Server) takes a `Collector&` in its mutator. The other types read the
  collector from the allocator of their container (`VmAllocBase::barrierCollector`).

`tools/check_gc_barrier_table.py` checks this table against the headers (see
its docstring for the ten checks). The ctest test `GcBarrierTable` runs it.
The status of a row:

- `protected`: the member is private. Each named mutator exists and reaches
  each named hook.
- `immutable`: the member is const.
- `exempt`: the note gives the reason that the member needs no mutator.
- `unprotected`: the type has no barrier yet. `barrier::isUnprotectedType`
  lists it, and the collector traces each object of the type again in the
  finish pause. No type has this status now.

| Object | Member | Mutators | Barrier | Status | Note |
|---|---|---|---|---|---|
| `ObjString` | - | - | - | immutable | The characters and the hash are set by the constructor. |
| `ObjInt` | - | - | - | immutable | The value is set by the constructor. |
| `ObjNative` | - | - | - | immutable | A function pointer and an arity. |
| `ObjFunction` | `name` | the compiler | - | exempt | The compiler writes the name and appends constants while the function is not yet reachable from the program. An insert overwrites no reference, and a snapshot barrier needs none. |
| `ObjFunction` | `chunk` | the compiler | - | exempt | The compiler writes the name and appends constants while the function is not yet reachable from the program. An insert overwrites no reference, and a snapshot barrier needs none. |
| `ObjUpvalue` | `next` | - | - | exempt | The list of open upvalues. Open upvalues are roots, so the collector does not trace this link. |
| `ObjUpvalue` | `m_location` | `setLocation` | - | exempt | Moves the cell between the stack, the closed value and a snapshot. The value does not change, and the old and new cell are both kept alive. |
| `ObjUpvalue` | `m_closed` | `set` | `barrierUpvalue`, `barrierSnapshot` | protected | A store through a closed upvalue is an Upvalue store. A store into a cell of a snapshot goes to the owner as a Snapshot store. A store to an open cell of the value stack needs none. |
| `ObjUpvalue` | `m_owner` | `setOwner`, `close` | `barrierUpvalue` | protected | - |
| `ObjClosure` | `function` | - | - | immutable | - |
| `ObjClosure` | `m_upvalues` | `setUpvalue` | `barrierElement` | protected | - |
| `ObjClass` | `name` | - | - | immutable | - |
| `ObjClass` | `m_superclass` | `setSuperclass` | `barrierField` | protected | - |
| `ObjClass` | `m_methods` | `setMethod`, `inheritMethods` | `barrierField` | protected | A Table store runs the hook for the value. A removed key goes in its own call. |
| `ObjInstance` | `klass` | - | - | immutable | - |
| `ObjInstance` | `m_fields` | `setField` | `barrierField` | protected | As ObjClass::m_methods. |
| `ObjBoundMethod` | `receiver` | - | - | immutable | Both are const members. |
| `ObjBoundMethod` | `method` | - | - | immutable | Both are const members. |
| `ObjBoundNative` | `receiver` | - | - | immutable | Both are const members. |
| `ObjBoundNative` | `native` | - | - | immutable | Both are const members. |
| `ObjList` | `m_elements` | `push`, `pop`, `set`, `resize`, `eraseAt` | `barrierElement` | protected | A shift of the elements runs the hook once for each slot that it overwrites. |
| `ObjIterator` | `collection` | - | - | immutable | - |
| `ObjIterator` | `m_current` | `setCurrent`, `takeCurrent` | `barrierField` | protected | - |
| `ObjMap` | `klass` | - | - | immutable | - |
| `ObjMap` | `m_map` | `mapSet`, `mapDel`, `mapSetHashed`, `mapDelHashed` | `barrierEntry` | protected | A removed entry, and an overwritten key, run the hook twice: first for the key as a Value, then for the value. `mapSet` takes keys that are identical when equal (interned Strings), so it reports the old value only; `mapSetHashed` also reports an old key that is equal but is another object. |
| `ObjEnumCtor` | `ctorName` | - | - | immutable | - |
| `ObjEnumCtor` | `enumName` | - | - | immutable | - |
| `ObjEnum` | `ctor` | - | - | immutable | - |
| `ObjEnum` | `m_fields` | `resize`, `set` | `barrierElement` | protected | The constructor of the value fills the fields once. A new slot is Nil. |
| `ObjError` | `klass` | - | - | immutable | - |
| `ObjError` | `message` | - | - | immutable | - |
| `ObjError` | `kind` | - | - | immutable | - |
| `ObjDeferredCall` | `callable` | - | - | immutable | - |
| `ObjDeferredCall` | `m_args` | `addArg` | `barrierElement` | protected | - |
| `ObjFile` | `klass` | - | - | immutable | - |
| `ObjSocket` | `klass` | - | - | immutable | - |
| `ObjSocket` | `m_readHolder` | `setReadHolder` | `barrierField` | protected | - |
| `ObjSocket` | `m_writeHolder` | `setWriteHolder` | `barrierField` | protected | - |
| `ObjServer` | `klass` | - | - | immutable | - |
| `ObjServer` | `m_acceptHolder` | `setAcceptHolder` | `barrierField` | protected | - |
| `ObjProcess` | `klass` | - | - | immutable | - |
| `ObjCoroutine` | `klass` | - | - | immutable | - |
| `ObjCoroutine` | `callee` | - | - | immutable | - |
| `ObjCoroutine` | `m_stack` | `captureStack`, `clearSnapshot` | `barrierSnapshot` | protected | A resume clears the whole snapshot, so each old reference goes to the hook. |
| `ObjCoroutine` | `m_frames` | `pushFrame`, `clearSnapshot` | `barrierSnapshot` | protected | - |
| `ObjCoroutine` | `m_defers` | `pushDefers`, `clearSnapshot` | `barrierSnapshot` | protected | - |
| `ObjCoroutine` | `m_openUpvalues` | `pushOpenUpvalue`, `clearSnapshot` | `barrierSnapshot` | protected | - |
| `ObjCoroutine` | `m_resultOverrides` | `pushResult`, `clearSnapshot` | `barrierSnapshot` | protected | - |
| `ObjCoroutine` | `m_waitingInner` | `setWaiting` | `barrierSnapshot` | protected | - |
| `ObjCoroutine` | `m_waitingIterator` | `setWaiting` | `barrierSnapshot` | protected | - |
| `ObjTask` | `m_co` | `setCo` | `barrierField` | protected | - |
| `ObjTask` | `m_mailbox` | `pushMessage`, `popMessage`, `eraseMessage`, `clearMailbox` | `barrierElement` | protected | - |
| `ObjTask` | `m_pendingValue` | `setPendingValue` | `barrierField` | protected | - |
| `ObjTask` | `m_startArg` | `setStartArg` | `barrierField` | protected | - |
| `ObjTask` | `m_selectWant` | `setSelectWant` | `barrierField` | protected | - |
| `ObjTask` | `m_io` | `setWait`, `clearWait` | `barrierField` | protected | The record names a Socket, a Server or a String. setWait runs the hook for the old target and the old text. |
| `ObjTask` | - | `ioProgress` | - | exempt | Gives the call record to the step functions of a network call that waits many times. They write only the progress (bytes, offset, connect state). The check script lets only src/stdlib/net_api.cpp call it. A write of target or text through it is not found by a script: this is a gap, and the review of net_api.cpp covers it. |


## 8. Observation

- `LOXPP_GC_TRACE=<file>` writes one record for each collection (format
  v3, described in `tools/gc_report.py`). The stop-the-world collector writes
  `gc` records and `stall` records. The incremental collector writes `cycle`
  and `step` records instead of `gc` records (one `step` for each pause, of
  kind `start`, `mark`, `finish`, `sweep` or `end`, with the `units` of the
  step, and one `cycle` at the end of the cycle). A cycle with cause `exit` is
  the collection that a worker runs as it ends: its steps are not pauses of
  the program, and `tools/gc_report.py` gives them their own line. The heap writes a `stall` record
  (`stall kind=<new|free> t0= t1=`) for each call into the C allocator (a new
  chunk or a large block) that takes longer than 100 us. These read the
  trace: `tools/gc_report.py` (and
  `tools/jvm_gc_log.py` through `summarise_gcs`), `tools/check_gc_trace.py`,
  `tools/check_gc_trace_v3.py`, `tools/check_gc_incremental.py`,
  `tools/check_gc_stall.py`,
  `tools/check_gc_min_heap.py`,
  `tools/check_gc_sweep_linear.py`, `benchmarks/gc_compare.py`, and the Redis
  example's `tests/run_stages.py` (probe S10 reads `objs_before` of `gc` and `cycle`
  records).
- `benchmarks/gc_large_heap/` holds the programs that keep a heap of millions
  of objects. `docs/records/gc-stw-baseline-2026-11-21.md` gives the object
  counts of each collection of these programs with this collector.
- `collectionCount()` is read by tests.
- The spec makes the timing of garbage collection implementation-defined
  (`spec/README.md`, "Conformance", item 4). No program can see when an
  object is freed, except through the time when an OS handle closes.
