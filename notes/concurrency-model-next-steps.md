# Concurrency Model: Next Steps

## Why this note

`expressiveness-roadmap.md` flags true parallelism (item 7) as the only
un-bootstrappable capability gap, and says its concurrency *model* must be
decided before building toward it — late choice means redoing the GC, FFI
thread-safety, and the profiler. This note pins down the sequence of actions
that follows from weighing Go's concurrency model (goroutines + CSP channels +
work-stealing scheduler) as one candidate among others for that decision.

## Summary of the trade-off

**For adopting Go's model specifically:** it's the best-documented point in the
design space already surveyed in `concurrency_in_bytecode_vms.md`; CSP channels
extend the share-nothing/copy-by-value stance the roadmap already committed to;
goroutines reuse the suspend/resume machinery item 5 (coroutines) requires
anyway; M:N scheduling suits lightweight script-like tasks better than 1:1 OS
threads; channels are safe-by-default for a dynamically-typed audience unlikely
to hand-roll correct locking.

**Against committing to it now:** the native interpreter is unoptimized and
~6x slower than the JVM backend (`benchmark_report_2026-06-08.md`) — the wrong
end of the pipeline to optimize first; the GC (`vm.h`/`vm_allocator.h`, one
`MemoryManager` per `VM`, not thread-safe) is the real blocker and needs a
deliberate redesign, not an incremental patch; the roadmap explicitly calls
for comparing CSP against actor/isolated-heap (BEAM) alternatives before
picking, and BEAM's share-nothing-per-process model may fit the existing
copy-by-value stance better than Go's shared-heap CSP; Go's model is a package
deal (growable stacks, async preemption, work-stealing queues, netpoller) with
none of the prerequisite infrastructure present; and three backends (native,
JVM, CLR) risk diverging on concurrency semantics if the native VM copies Go
specifically while JVM/CLR keep their own host primitives.

**Net recommendation:** don't commit to Go's model yet. Build item 5
(coroutines/generators) first as the model-agnostic stepping stone both CSP
and actor-style scheduling need, then make the model decision with real data
instead of upfront guesswork.

## Actionable items

1. **Write a decision doc before touching code.** Add
   `notes/concurrency-model-decision.md` scoring CSP (Go) vs. actor/isolated-heap
   (BEAM) vs. OS-threads (JVM) against Lox++'s actual constraints: the
   share-nothing/copy-by-value stance already adopted, the single
   non-thread-safe `MemoryManager` per `VM`, and the three-backend reality.
   This satisfies the `AGENTS.md` planning policy — plan and get approval
   before any language change.

2. **Scope and build item 5 (coroutines/generators) first, independent of the
   model decision.** Stackful suspend/resume + `yield` for the native VM,
   useful standalone (lazy sequences, custom iterables) even if full
   concurrency never lands.

3. **Update `spec/` for coroutines in the same PR**, per the source-of-truth
   rule — observable suspend/resume/yield semantics only, no opcodes or
   bytecode references.

4. **Fix or explicitly defer the profiler coupling** flagged in
   `profiler-concurrency-notes.md` while coroutines land: decide now whether
   `m_frames[]`/`m_profilerScopes[]` become per-fiber, or track a follow-up —
   don't let them stay silently globally-indexed once multiple stacks exist.

5. **Re-run the JVM/CLR emission-contract check for coroutines.** Confirm
   whether `jvm_emitter.cpp`/`clr_emitter.cpp` need new opcodes or lowering for
   suspend points, keeping all three backends semantically consistent.

6. **Revisit the decision doc from item 1 only after coroutines ship and are
   stable**, using real data — how the suspend/resume machinery behaves, what
   it cost — to decide whether the natural next step is a scheduler (Go-style)
   or isolated per-task heaps (actor-style).

7. **Do not start GC redesign, scheduler work, or channel primitives before
   item 6 produces an explicit go/no-go.** Picking the model early and
   building on it before evidence exists is exactly the ordering the roadmap
   warns against.

8. **Track the whole sequence as a mission**, per `multi-agent-playbook.md`
   conventions, rather than a single PR — it spans multiple decision points
   and at least one full feature before the concurrency model is finalized.
