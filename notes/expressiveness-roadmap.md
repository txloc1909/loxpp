# Expressiveness Roadmap — What's Inexpressible Today

## Why this note

Recurring question: "what does Lox++ need to be useful for real programs?"
Answered the wrong way it produces a wishlist of stdlib conveniences. Answered
the right way it's a short list of *capability gaps*. This note pins down the
right framing and the resulting priority order so future feature debates start
from the capability ceiling, not from ergonomics.

## The litmus test

> A feature counts as a real gap **only if it cannot be bootstrapped by
> composing primitives the language already exposes.** If a user could write it
> in pure Lox++ (closures + lists + maps + recursion + enums + `match`), it's a
> *library*, not a language gap — however tedious it is to hand-write.

Tedium is not the axis. Capability is.

## Ruled out by the test (libraries, not gaps)

All fully expressible today; none belong on the roadmap:

- String/list methods (`split`, `join`, `map`, `filter`, `reduce`, `sort`) —
  user code over existing primitives.
- JSON, sets, queues, trees, sorting — a JSON parser is string-indexing +
  recursion + maps; a set is a map.
- String interpolation, lambdas, `const`, default args — syntax sugar over
  things that already exist (a lambda is an unnamed `fun`).
- Modules / namespaces — **organizational, not expressive.** One file can
  express any program; modules scale code, they don't add capability. (Dynamic
  *loading* is the exception — that's `eval`, deferred below.)
- Error **values** (`Result`/`Option`) — already expressible via enums +
  exhaustive `match`. See `dynamic-functional-programming.md` (Result-by-
  convention). This sharpens the real gap: error *control flow*, not error
  *values* (see item 3).
- Data modelling in general — primitives + maps are a **universal data
  representation** (it's what JSON/msgpack/protobuf reduce to). This is why
  message-passing concurrency needs no reflection: share-nothing on maps +
  primitives is complete.

## The genuine gaps, sorted by complexity (ascending)

| # | Gap | Complexity | Essentialness |
|---|---|---|---|
| 1 | Reflection — **introspection only (done)** | low | situational |
| 2 | OS / world access — basics **(done)** | low | essential |
| 3 | Non-local control flow (`try`/`catch`/`throw` + `defer`/`finally`) | low–medium | essential |
| 4 | Extensible protocols / operator overloading | medium | essential |
| 5 | Coroutines / generators (single-core suspension) — **(done)** | medium–high | high-value on-ramp |
| 6 | FFI / native extension ABI | high | highest leverage |
| 7 | True parallelism (multi-core) | very high | the must-have |

**1. Reflection — introspection only. DONE** (#167, all three back ends;
`src/stdlib/reflect_api.cpp` + `LoxRuntime.{java,cs}`). `type(x)`,
`fields(inst)`, `getField`/`setField`/`hasField`, `methods(cls)`, `callMethod`.
Field names compile to constant operands of `GET_PROPERTY`; no opcode reads a
name computed at runtime, so this was a hard wall, not a library. The fix was
just native accessors over the `ObjInstance` field table and `ObjClass` method
table that *already exist* — near-zero new machinery. **`eval` is explicitly
deferred** (it needs compiler re-entrancy; introspection does not). For a dynamically-typed
language with no macros/templates/generics, runtime reflection is the *entire*
metaprogramming channel — without it every generic facility is hand-written per
type. Value is bounded to single-VM inspectability (generic tooling, frameworks,
local serialization of class instances) — **not** a prerequisite for concurrency
(see the universal-data-representation point above).

**Downstream note:** the native-VM optimisation items 5 (inline caches) and 9
(slot-based fields / shapes) in `benchmark_report_2026-08-26.md` §5 must now be
built around this merged API — `getField`/`setField`/`hasField`/`fields`/
`callMethod` read `ObjInstance::fields` directly, so item 9 rewrites them in
its own PR, and item 5's property cache key must carry a shape identity
because `setField` can add a field under a runtime-computed name. See that
report's "Dependencies on the expressiveness roadmap" table.

**`callMethod` on closure-backed methods. DONE** (#496). A resolved method
that is closure-backed (an ordinary user-defined method) now runs to
completion and returns its value. Native gained the bounded re-entrant call
path it lacked — a native may now run an interpreted frame to completion via
a nested `run()` and get a synchronous result (`Runtime::reentrantCall` /
`invokeCallableFromNative` / `invokeMethodFromNative`, `src/runtime.h`). The
JVM needed no such machinery (a user method is an ordinary Java call) but
lifted its cap in the same change, so the differential suite stays green. The
same primitive is the piece item 5 (coroutines) builds its suspend/resume on.

The QBE backend never had the limitation: every function has attached code in
a whole-program `--target qbe` build, so `callCompiled()` already returns
synchronously. The `Runtime::reentrantCall` path asserts an interpreter loop
is installed and is only reached on the interpreter.

**2. OS / world access — basics. DONE** (#516, all three backends). `args`,
`env`, `exit`, `time`/`sleep`, FS metadata (`exists`, `is_dir`, `is_file`,
`stat`) live in `src/stdlib/os_api.cpp`; `clock` is in
`src/stdlib/globals.cpp`. The medium-cost tail is closed: `connect`/`listen`
(TCP client and server, `Socket`/`Server` values) in `src/stdlib/net_api.cpp`,
and `spawn`/`run` (child processes with pipes, a `Process` value) in
`src/stdlib/process_api.cpp`. The JVM runtime mirrors them (`LoxSocket`,
`LoxServer`, `LoxProcess`) and the self-hosted bootstrap interpreter delegates
to the host. All I/O blocks the VM, exactly like `sleep`, because item 5's
coroutines do not exist yet. The *unbounded* surface of the tail — crypto,
databases, compression — remains the standing argument for item 6.

One residual gap surfaced while building the CodeCrafters HTTP server example
(`examples/codecrafters/build-your-own-http-server`), so item 2 is *not* fully
closed:

- **No concurrency.** One `accept`/`readline` blocks the VM, so simultaneous
  keep-alive connections cannot be served without item 5. Tracking: #519.

The former bounded-read gap was closed by `read_bytes(n)` (#518), which blocks
until `n` bytes arrive or the peer closes and is exposed on `Socket` and
`Process` (stdout and stderr) on all three backends. A `Content-Length` body is
now one call, so `base-08` is reachable.

The HTTP server reaches 13 of the 14 public stages in pure Lox++; concurrency is
the one it cannot. The example also depends on one correctness fix to the
`socket` streams merged in #516: `readline()` followed by `write()` failed with
`ESPIPE` because stdio's `r+` update mode wants a seek on the input→output
transition. Opening socket streams unbuffered removes the seek and restores the
bidirectional byte stream `spec/05-stdlib.md` already specifies.

**3. Non-local control flow (`try`/`catch`/`throw`/`defer`). DONE** (#223, all three backends; `src/vm.cpp`, `src/backend/jvm_emitter.cpp`, `runtime/clr/src/LoxRuntime.cs`, `spec/04-semantics.md`). Handler stack + frame unwinding. Closes expressiveness roadmap item 3: runtime faults are now catchable via `try`/`catch`, non-local escape works without threading `Result` through every return, and `defer` provides cleanup-on-unwind (also fixes the `container_objects.h` file-handle leak TODO). See `notes/non-local-control-flow.md` for the design record and tracking issue `#223` for the implementation breakdown.

**4. Extensible protocols / operator overloading. DONE** (#472). `for-in`,
`[]`, `==`, `len`, `()`, and map-key hashing now dispatch to user methods
(`__iter__`/`__index_get__`/`__index_set__`/`__eq__`/`__hash__`/`__call__`),
so user types are first-class. Map keys through `__hash__`/`__eq__` landed in
#469: a valid Instance key defines both methods, the map hashes the key and
resolves a collision with the stored key's `__eq__`, and a key method that
writes its own map raises `MapChangedError`. The `hash()`-protocol open
question is closed. The one deferred piece is
`__str__` (D2, #470) for a user canonical string form.

**5. Coroutines / generators (single-core suspension). DONE** (#523, all four
backends; design record `notes/coroutines.md`). Stackful suspend +
`yield`. Lazy/infinite sequences, async I/O, cooperative scheduling, suspendable
custom iterators. No thread-safe GC required — the cheap on-ramp to "any
concurrency" and a stepping stone to item 7, not throwaway work.

**6. FFI / native extension ABI.** Stable native ABI + dynamic `.so` loading.
The meta-capability: collapses the unbounded tail of item 2 (plus crypto, DBs,
compression, the C ecosystem) from "modify the VM forever" into "write a
binding." Makes the *ecosystem* expressive by borrowing C, not the *language*.
FFI'd code escapes the GC, memory safety, and the concurrency model — so it must
be designed **after** item 7's model is chosen.

**7. True parallelism (multi-core).** The only un-bootstrappable item and the
flagged must-have. Model choice (threads/actors/CSP), thread-safe/concurrent GC,
VM reentrancy, profiler rework. Design space already mapped in
`concurrency_in_bytecode_vms.md` and `profiler-concurrency-notes.md`.

## Performance direction

Expressiveness and performance are separate axes. This note orders the
first. For the second, the native VM is the target (`AGENTS.md`, "Backend
roles"): it must be fast for a dynamic language.

- Interpreter wins come first: inline caches and object shapes
  (`benchmark_report_2026-08-26.md` §5, items 5 and 9).
- The long-term path is a real AOT compiler or a high-performance JIT for the
  native VM.
- QBE (`qbe-backend.md`) and the JIT proposal (`jit-pipeline.md`) are
  experiments along that path, not the path itself.
- The JVM backend is the performance baseline to work toward.

## Decisions reached

- **Reflection stays, rescoped.** Keep introspection (cheap, exposes existing
  structures, the only metaprogramming channel a dynamic language has); defer
  `eval`. Earlier "concurrency needs reflection for serialization" claim is
  **retracted** — primitives + maps are a universal data representation.
- **Build order is 1→7, but decide item 7's concurrency model first.** Items 1
  (reflection, #167) and 2 (OS/world access, #516) are done; next up is
  item 3. Item 7 is the most
  architecturally invasive item and constrains item 5 (shared suspension
  machinery), item 6 (FFI thread-safety), the GC, and the profiler. Choosing it
  late means redoing them. Build last, choose first.
- **Reflection is relevant to the JVM and CLR backends.** Both host platforms
  have rich reflection; defining the concept in `spec/` keeps that door open
  across all three targets. Confirmed in #167: this cost **zero
  codegen/emission changes** in either backend — `CALL`/`INVOKE` already
  dispatch through one shared callable interface (`LoxCallable`/
  `ILoxCallable`) spanning closures, classes, and natives alike, so the new
  natives were purely a runtime-library addition (`LoxRuntime.java`/`.cs`), the
  same way `stat` was. The one piece of real per-backend work was `type(x)`'s
  type-name mapping, which has no shared implementation and is written
  once per language, kept in sync by hand.

## One-liner

The roadmap is short because most "missing" features are libraries. What's left
is the handful of things a program genuinely *cannot reach* from inside the
language — and they sort cleanly from cheap introspection to the architectural
weight of multi-core parallelism.
