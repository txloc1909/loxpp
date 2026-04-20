# Profiler + Concurrency Design Notes

## What the profiler is currently coupled to

**1. Single call stack (`m_profilerScopes` parallel to `m_frames[]`)**
The `std::optional<ProfileFunctionScope>` array is indexed by frame depth,
which implicitly assumes one call stack. This is the tightest coupling.

**2. Clock: `CLOCK_PROCESS_CPUTIME_ID`**
This measures process-wide CPU time. With multiple threads running Lox code
simultaneously, times from different threads accumulate into the same clock —
you can't attribute time to a specific thread of execution.

**3. GC stats via shared `MemoryManager`**
`ProfileGcScope` reads `bytesAllocated` from `MemoryManager`, which will become
a shared resource under any concurrent model with a shared heap. The
`ProfilerData*` pointer stored in `MemoryManager` would have a single owner in
a multi-VM world.

**4. `ProfilerData` owned by `VM`**
One struct per VM instance. Whether this is a problem depends entirely on your
concurrency model.

---

## Impact by concurrency model

### Actor model / isolated VMs (no shared heap)
Each actor gets its own `VM` → its own `ProfilerData`. **Least disruptive.**
The parallel `m_frames[]`/`m_profilerScopes[]` design holds naturally
per-actor.

Problems that remain:
- You'd get N separate profiler reports, one per actor — no merged view
- `CLOCK_PROCESS_CPUTIME_ID` still accumulates across actors if they run on OS
  threads; switch to `CLOCK_THREAD_CPUTIME_ID`

### Green threads / fibers sharing one VM
Each fiber has its own call stack but they multiplex on one VM. This **breaks
the current design** directly — `m_profilerScopes[]` is indexed by frame depth
globally, not per-fiber stack. `m_frames[]` would need to become per-fiber too,
and `m_profilerScopes[]` would follow.

This is the model where the most refactoring is needed, but it's still local to
`vm.h`/`vm.cpp`.

### Shared heap, OS threads
`MemoryManager` becomes contended. The GC profiler (`ProfileGcScope` +
`bytesAllocated`) needs synchronization. `setProfilerData()` becomes racy
unless you move to per-thread `ProfilerData` with a merge step at report time.

---

## Does the profiler restrict your design space?

**Not meaningfully, because of the `#ifdef` guards.** You can freely experiment
with concurrency models with `LOXPP_PROFILE=OFF` — the profiler exerts zero
influence on runtime data structures in that build. The guard is the key design
decision that keeps this contained.

What the profiler's existence *does* do is **force you to make the concurrency
model explicit earlier than you might otherwise have to.** Specifically, before
re-enabling profiling you'll need to answer:

1. **Stack ownership**: Is a call stack per-VM, per-fiber, or per-OS-thread?
   The `m_profilerScopes[]` array follows whatever owns `m_frames[]`.
2. **Heap ownership**: Shared GC or isolated? Determines whether
   `ProfileGcScope` needs locks or per-thread accounting.
3. **What does "time" mean?** Wall clock vs CPU time per thread vs CPU time per
   logical task — this choice differs across concurrency models.

These are questions you'd have to answer anyway to design concurrency
correctly; the profiler just makes the stakes visible. The biggest concrete
changes you'd likely need when revisiting the profiler for a concurrent
runtime:

- `CLOCK_PROCESS_CPUTIME_ID` → `CLOCK_THREAD_CPUTIME_ID` (or `CLOCK_MONOTONIC`
  for wall time)
- `m_profilerScopes[]` moved to wherever `m_frames[]` lives
- GC stats: either per-GC-owner with a merge, or an atomic counter

None of this requires redesigning the RAII scope guard approach — that
structure survives any concurrency model.
