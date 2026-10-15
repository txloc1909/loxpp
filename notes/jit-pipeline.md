# JIT pipeline: design proposal (v0)

**Status: proposal, not approved. If built, it is an opt-in tier that needs the
`--target qbe` toolchain on the user's machine. It ships only in the full
package (`distribution.md`, "Two packages"), never in the simple one.** No issue or Project board exists yet. This
note is a design sketch to reach alignment on approach before any code lands,
per `AGENTS.md`'s planning policy. When approved, each stage below becomes one
GitHub issue.

## Why

`qbe-backend.md` records the AOT `--target qbe` backend (implemented under
mission #462). Its prototype evidence shows a toy emitter beating native by
about 3.6x on `fib(32)`, mostly by removing the interpreter's dispatch loop. A JIT is attractive because it could apply that
same win to hot functions without a separate compile-and-link step the user
has to invoke — and, longer term, it is the natural place to add speculative
type specialization that neither the AOT backend nor the interpreter can do.

This note scopes a deliberately small v0: reuse the AOT emitter unchanged,
add just enough machinery to trigger it lazily at runtime, and get the
dispatch-removal win on hot functions with the smallest possible new design
surface. It explicitly does not attempt type specialization, inline caches,
or on-stack replacement — see "Explicitly punted" below.

## Constraint from the concurrency model: no new thread touches the heap

`notes/concurrency-model-next-steps.md` (item 7) says not to start GC
redesign or scheduler work before the concurrency model decision (item 6)
produces a go/no-go. The GC (`memory_manager.h`) is one `MemoryManager`
per `VM`, not thread-safe.

A conventional tiered JIT compiles hot functions on a background compiler
thread. That thread would read bytecode and constants concurrently with the
mutator and would need to coordinate with a GC that is not built for it —
which is exactly the scheduler/GC work the concurrency note says to defer,
and it would quietly commit the project to a threading answer before item 6
decides one.

**v0 sidesteps this by compiling in a subprocess, not a thread — and
blocking on it synchronously rather than polling.** A forked child process
shares no memory with the parent VM: it cannot race the `MemoryManager`, and
it needs no lock. The parent's only coordination with the child is a
`waitpid` and an exit-code check, not shared state or a background thread —
the one interpreter thread simply pauses on a blocking call, the same as any
other syscall-bound wait. This keeps the JIT compatible with whatever the
concurrency model decision eventually picks, instead of anticipating it.

## Pipeline

### 1. Trigger

Add a call counter to `ObjFunction` (or `CallFrame`). Each call increments
it; crossing a threshold triggers a synchronous compile (§2) keyed by the
function's `chunk_decoder` id — see "Threshold: back-of-envelope" below for where the
number comes from.

Whole-function JIT, not tracing. A trace JIT (recording, guards, trace
stitching, side exits) is a materially bigger and riskier project than this
note's scope.

### Threshold: back-of-envelope

Because the compile blocks the one interpreter thread synchronously (§2), a
wrong threshold has a direct, visible cost: too low, and the program pays a
real 10-50 ms stall for a function that wasn't going to be called enough
afterward to earn it back. So the question is "how many more calls does a
function need ahead of it to make one blocking pause worth paying" — not a
CPU-efficiency question, a wall-clock one.

**Per-call savings.** `qbe-backend.md`'s prototype numbers for `fib(32)`:
native 0.62s, QBE 0.17s. Naive recursive `fib(32)` makes `2*Fib(33) - 1 =
7,049,155` calls. That's ~88 ns/call native, ~24 ns/call compiled — savings
of about **64 ns/call**. (This is the *dispatch-removal* number only — v0
has no type specialization, so don't expect more from real code than from
this microbenchmark.)

**Compile cost.** Not measured yet, so state the assumption plainly: QBE's
own pass is sub-millisecond for one small function — `qbe-backend.md`'s
"Why" section already notes QBE is fast and takes plain-text IL. The dominant cost is
`cc -shared`: process spawn plus invoking the system assembler and dynamic
linker to produce a loadable `.so`. Tiny-input, no-preprocessing invocations
like this typically run **10-50 ms** wall-clock on ordinary hardware — this
is the number to replace with a real measurement once v0 exists, not a
constant to trust blindly.

**Break-even.** `compile cost / per-call savings`:

| compile cost | break-even calls |
|---|---|
| 10 ms | 10,000,000 ns / 64 ns ≈ **156,000** |
| 50 ms | 50,000,000 ns / 64 ns ≈ **780,000** |

So the break-even point is on the order of **10⁵-10⁶ calls**, not the low
thousands. That's the point worth naming explicitly: production method JITs
(HotSpot's C1 tier, V8's Ignition→Turbofan) trigger in the 1,500-10,000
invocation range, but their compiles are in-process — microseconds to a few
milliseconds. This design's per-compile cost is dominated by `fork`+`exec`+
linker overhead, roughly 100-1000x more expensive per compile than an
in-process JIT, which is exactly why its threshold has to sit 100-1000x
higher. Copying a number tuned for a different architecture (e.g. picking
1,000 or 10,000 by analogy to HotSpot) would compile far more functions
than could ever earn back the cost.

**v0 starting point: 200,000 calls** — near the low (cheaper-compile) end of
the range, since undershooting means paying visible stalls on functions that
weren't hot enough to earn them back, while overshooting delays a real win
on a function that was already hot. Treat it as a placeholder to replace
with a measured `fork`+`cc -shared` cost the moment v0 has a working
subprocess step; this math is a starting point, not a substitute for
measuring.

This also narrows, without eliminating, the "many functions cross the
threshold near-simultaneously at startup" risk — now sharper than a pure
resource-contention concern, since the single interpreter thread would pay
each function's stall serially, back to back. At 200,000+ calls the
functions that reach it are naturally staggered by how a real program calls
them, unlike a 1,000-call threshold that many merely-warm functions would
cross together within the first few loop iterations of a program.

### 2. Compile synchronously, in a subprocess

The trigger check (§1) happens at function entry, before running the body.
Crossing the threshold blocks that one call — still on the sole interpreter
thread, no new thread — to compile before proceeding:

1. Serialize the target function's chunk to a temp file, keyed by its
   `chunk_decoder` id (the same stable id (`"0.2.1"`) the AOT startup path
   uses) — see "Cache key" below for why id alone does not fully key the
   cache.
2. Fork a child and `waitpid` on it. The child runs the existing AOT
   pipeline unchanged: decode -> `cfg` / `abstract_stack` / `handler_depth`
   -> emit `.ssa` -> `qbe` -> `.s` -> `cc -shared` -> a small `.so`. No new
   emitter code.
3. On success (child exits 0): `dlopen` the `.so`, `dlsym` the entry point,
   patch the call site (a function-pointer slot on `ObjFunction`), and jump
   straight into compiled code for *this* call too — not just future ones.
   Every call after this one hits the patched call site directly; the
   counter-and-threshold check is bypassed entirely from here on, not merely
   skipped.
4. On failure (toolchain missing, an unimplemented opcode, a compiler bug):
   mark the function "compile failed, don't retry" and fall back to
   interpreting this call and all future ones. Without this sticky flag, a
   function whose compile deterministically fails would refork and refail
   on every subsequent call once it re-crosses the check.

A synchronous compile needs no per-call poll and has no window where more
calls run interpreted before the swap lands. A function is either still
counting, compiled, or marked failed.

This costs one `fork`+`waitpid` per hot function — a single bounded pause
(the 10-50 ms estimated above), not a repeating poll. It is the same
"compile once, cache, attach by id" shape as the AOT backend's own
"Startup" section, just moved from process-start to per-function-at-runtime,
and paid once as a blocking call.

### Cache key: id alone is not enough

`chunk_decoder.cpp`'s `id` (`decodeFunctionNode`, `chunk_decoder.cpp:190`) is
assigned purely by constant-pool position — `"0"` for the root, `"<parent>.n"`
for the n-th function constant found in the parent's chunk. It encodes tree
shape, not bytecode content: editing a function's body without moving it in
declaration order leaves its id unchanged while the bytes underneath it
change. A `.so` cache keyed on id alone would, after any edit, silently keep
serving a stale compiled function for the new body — a correctness bug, not
a missed optimization.

The AOT backend's own "Startup" section already reached this conclusion for
a same-run check: it pairs id with "arity and a hash of its chunk bytes" so
compiled code and the freshly rebuilt `ObjFunction` fail fast on disagreement.
The v0 JIT cache needs the same pairing, for the same reason, plus one thing
the same-run check doesn't: since the `.so` is meant to survive across
process runs, the key must also pin the *toolchain* that produced it.

The project's `LOXPP_VERSION` (generated into `loxpp_version.h` from
`git describe --match "v*"`, `CMakeLists.txt:15-37`) is close but not
enough on its own. It is computed at configure time only, so a rebuild
without a reconfigure keeps the old stamp; and when no `v*` tag is
reachable it falls back to the fixed CMake project version. Either case
can serve a `.so` built by a loxpp whose bytecode format, opcode set, or
emitter has since changed. The toolchain component of the key must be a
per-build stamp — for example a hash of the running `loxpp` binary, or a
build id generated on every build.

**Cache key: `(id, arity, hash(chunk bytes), build stamp)`.** Any mismatch
is a cache miss, not an error — fall back to recompiling, the same as a cold
cache.

### 3. Runtime and GC surface stay as designed for the AOT backend

- Same fused-stack layout as `qbe-backend.md`'s central design choice:
  compiled and interpreted code share one value stack, so a function can
  flip from interpreted to compiled between calls with no stack translation.
- `markRoots()` still scans `stack..stackTop` only; a compiled frame is
  GC-visible exactly like an interpreted one.
- Because the swap happens synchronously, before the triggering call's body
  runs, and compilation itself is out-of-process, there is no
  concurrent-mutation-during-compile hazard and no partially-compiled state
  to design around in v0.

### 4. Coroutines: a compiled frame cannot suspend

Coroutines shipped (mission #523). In the native VM a coroutine owns the
interpreter frames inside its stack slice and `yield` snapshots that slice
(`coroutines.md`, "Native VM"). A JIT-compiled function is a real C frame,
which cannot be frozen mid-call. Selecting "functions that contain `YIELD`"
is not a sound way to keep them interpreted: a caller with no `YIELD` of
its own still suspends when its callee yields, and dynamic dispatch makes
the set of possible callers the whole program (`coroutines-retro.md`).

The AOT backend solves this with a whole-program switch: a program that
contains any `YIELD` compiles in coroutine mode (`treeContainsYield` in
`qbe_frontend.cpp`). v0 would use the same whole-program test and **disable the
JIT for any program that contains `YIELD`**. That rule is a stopgap. The
concurrency go/no-go (item 6) decides how a compiled frame can suspend, and
the JIT must adopt that answer, not freeze this rule. The chunk tree is complete
before the program runs, so the test is one pass at startup.

### 5. Backend choice: QBE, not LLVM

v0 has no type feedback and no inline caches, so there is nothing
monomorphic for LLVM's optimizer to exploit — it would only be a slower
subprocess producing the same output. QBE's fast compile matters more here
than in the AOT case, since it sits on the path to warming up a function.
LLVM stays off the table until a later tier has real speculative
specialization worth handing it an optimizer.

## Explicitly punted (not v0)

- **Type specialization / inline caches.** The actual source of JIT wins
  beyond dispatch-removal; a deliberately separate follow-up once v0's data
  shows it is worth the investment.
- **On-stack replacement.** The blocking compile in §2 happens at function
  entry, before the body runs, so the triggering call itself can use
  compiled code — but a frame already deep inside a long-running call (e.g.
  one call with a huge loop in its body) is never swapped mid-flight; only
  a fresh call ever crosses the trigger check.
- **A real background compiler thread, or tiering.** Only revisit once the
  concurrency-model decision (`concurrency-model-next-steps.md` item 6)
  lands — that decision determines what a second thread is even allowed to
  do with the heap.
- **LLVM as a tier.** Gate behind inline-cache-produced monomorphic code
  actually existing to optimize.

## Open questions

1. Whether embedding QBE as a library (instead of shelling out to the `qbe`
   binary) is worth it once subprocess overhead is measured. The back-of-
   envelope math above says `cc -shared`'s process-spawn-and-link cost, not
   `qbe`'s own pass, dominates the compile-cost side of the threshold — so
   embedding QBE alone would barely move the threshold. The bigger lever
   would be removing the `cc`/`ld` step entirely (an in-process linker/loader
   that relocates and `mmap`s the object directly), which is out of scope
   for v0 but worth naming as the actual next optimization if 200,000 calls
   turns out too conservative in practice. It matters more now that the
   compile blocks the interpreter thread directly (§2): cutting this cost
   doesn't just save CPU, it shortens a stall the program actually feels.
2. How much of the AOT emitter is really reusable unchanged. The AOT path
   emits a whole program that links `libloxrt.a`; a per-function `.so`
   must instead resolve runtime calls (`rt_capi`) against symbols that the
   running `loxpp` process exports. Size this before v0 claims "no new
   emitter code."

~~Whether the existing `chunk_decoder` id is sufficient to key the `.so`
cache across runs~~ — resolved, see "Cache key" above: no, it encodes tree
position only, not content; the key is `(id, arity, hash(chunk bytes),
build stamp)`.

~~Threshold tuning and per-process overhead of `fork` + `cc` at scale~~ —
resolved, see "Threshold: back-of-envelope" above: break-even is ~10⁵-10⁶
calls given a 10-50 ms subprocess-compile assumption, ~100-1000x higher than
an in-process JIT's threshold because the cost is dominated by process
spawn and linking, not codegen. v0 starts at 200,000 calls, to be replaced
with a measured number once the subprocess step exists.
