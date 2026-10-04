# Coroutines / generators: design record

## Why

`notes/expressiveness-roadmap.md` item 5: single-core suspension (coroutines
/ generators). The primitive is stackful suspend + `yield`. Lazy and infinite
sequences, async I/O, cooperative scheduling, and suspendable custom
iterators all build on it. This note records the design decisions behind the
spec node (#524) and the nodes that follow.

## Model

Asymmetric coroutines, Lua-style. `yield` returns to the resumer; `resume`
re-enters. Symmetric transfer is out of scope.

- `yield expr` is an expression. It suspends the current coroutine, delivers
  `expr` to the resumer, and evaluates to the value the next `resume` sends.
- Single value each way. Lox++ has no multiple return values, so the
  yield/resume channel carries one value. The coroutine function's own
  parameters are filled by the first `resume`'s arguments, exactly like an
  ordinary call.
- Errors use the existing channel. `resume` on a dead coroutine, `resume` on
  a running or normal coroutine, and `yield` outside a coroutine are
  catchable runtime errors (`DeadCoroutineError`, `RunningCoroutineError`,
  `YieldOutsideCoroutineError`). A throw inside a coroutine propagates to the
  resumer, where it is catchable like any other throw.

## Surface

Stdlib-only. No new function kind, so `CALL` stays uniform.

- `coroutine.create(fn)` -> a suspended `Coroutine`.
- `co.resume(...)` -> the value `co` next yields or returns.
- `co.status()` -> `"suspended"` | `"running"` | `"normal"` | `"dead"`.

`for-in` accepts a `Coroutine` (resumes it once per iteration), and `__iter__`
may return one. This is how lazy/infinite sequences and suspendable custom
iterators fall out with no extra syntax.

## States

Four states, like Lua: `suspended`, `running`, `normal`, `dead`. `normal` is
the coroutine that resumed another and is waiting for it to yield or return.
`status()` reports all four.

## Native VM

New `ObjCoroutine` holding a frozen interpreter snapshot, copy-on-suspend:
stack slice, frame slice, handler records, defer entries, open upvalues,
result-check slots, and profiler scopes. `YIELD` flushes the register-cached
`ip` at the `FrameSync` seam and exits the nested run with the yielded value;
`resume` re-enters through the `reentrantCall`/`runNestedLoop` machinery
(`expressiveness-roadmap.md` item 1). `markRoots` walks every suspended
coroutine.

The declared invariant (read before any pass is touched): a suspended
coroutine owns exactly the interpreter state whose frames lie inside its
slice; nothing above or below. The `slots` and upvalue `location` pointers
are rebased on suspend and resume.

## Profiler

Per-coroutine `ProfilerData` and `m_profilerScopes`, under `#ifdef
LOXPP_PROFILE`. The root coroutine keeps the inline storage. Zero cost when
profiling is off. `CLOCK_PROCESS_CPUTIME_ID` stays correct because suspension
is cooperative on one OS thread.

## Backends

- QBE: functions containing `YIELD` fall back to the interpreter loop (the
  existing `invokeClosure` path). Compiling those functions to native code —
  removing the fallback — is tracked separately (#535).
- JVM: each coroutine is a JDK 21 virtual thread; `yield`/`resume` use an
  `Exchanger` handoff. No stack copying (shared-heap object model).
- Bootstrap: one host coroutine per Lox++ coroutine, like its existing I/O
  delegation.

## Fault table

Three new catchable rows, listed in `spec/04-semantics.md`, wired into
`tools/check_fault_table.py` (skipped on every consumer until the backend
nodes land) and `tools/check_bootstrap_error_kinds.py`
(`EXPECTED_SPEC_TABLE_KIND_COUNT` 20 -> 23).

## References

- `notes/expressiveness-roadmap.md` item 5.
- `notes/concurrency-model-decision.md` C1–C9.
- `notes/concurrency-model-next-steps.md` items 2–5.
- `notes/non-local-control-flow-retro.md` — the five process rules this
  mission follows.
- Tracking issue #523; node issue #524.
