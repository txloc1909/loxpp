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
| 1 | Reflection — **introspection only** | low | situational |
| 2 | OS / world access — basics | low | essential |
| 3 | Non-local control flow (`try`/`catch`/`throw` + `defer`/`finally`) | low–medium | essential |
| 4 | Extensible protocols / operator overloading | medium | essential |
| 5 | Coroutines / generators (single-core suspension) | medium–high | high-value on-ramp |
| 6 | FFI / native extension ABI | high | highest leverage |
| 7 | True parallelism (multi-core) | very high | the must-have |

**1. Reflection — introspection only.** `type(x)`, `fields(inst)`,
`getField`/`setField`/`hasField`, `methods(cls)`, `callMethod`. Field names
compile to constant operands of `GET_PROPERTY`; no opcode reads a name computed
at runtime, so this is a hard wall, not a library. But the fix is just native
accessors over the `ObjInstance` field table and `ObjClass` method table that
*already exist* — near-zero new machinery. **`eval` is explicitly deferred** (it
needs compiler re-entrancy; introspection does not). For a dynamically-typed
language with no macros/templates/generics, runtime reflection is the *entire*
metaprogramming channel — without it every generic facility is hand-written per
type. Value is bounded to single-VM inspectability (generic tooling, frameworks,
local serialization of class instances) — **not** a prerequisite for concurrency
(see the universal-data-representation point above).

**2. OS / world access — basics.** `args`, `env`, `exit`, `time`/`sleep`, FS
metadata. Thin syscall wrappers; no library reaches a syscall the VM doesn't
expose. Sockets/subprocess are the medium-cost tail of this bucket. The
*unbounded* surface here is the standing argument for item 6.

**3. Non-local control flow.** `try`/`catch`/`throw` + `defer`/`finally`. Needs
a handler stack + frame unwinding. Three things are inexpressible because you
can't unwind from Lox++: intercepting **runtime faults** (index OOB, calling
`nil`), **non-local escape** without threading `Result` through every return,
and **cleanup-on-unwind** (also fixes the `container_objects.h` file-handle leak
TODO).

**4. Extensible protocols / operator overloading.** Today `for-in`, `[]`, `==`,
`len`, `()`, and map-key hashing are hardwired to built-in List/String/Map, so
user types are second-class — you can't make a tree iterable, a matrix
indexable, or key a map by an object. Make these opcodes dispatch to user
methods (`__iter__`/`__index__`/`__eq__`/`__hash__`/`__call__`). Subsumes the
`hash()`-protocol open question in `language-extension.md`.

**5. Coroutines / generators (single-core suspension).** Stackful suspend +
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

## Decisions reached

- **Reflection stays, rescoped.** Keep introspection (cheap, exposes existing
  structures, the only metaprogramming channel a dynamic language has); defer
  `eval`. Earlier "concurrency needs reflection for serialization" claim is
  **retracted** — primitives + maps are a universal data representation.
- **Build order is 1→7, but decide item 7's concurrency model first.** It's the
  most architecturally invasive item and constrains item 5 (shared suspension
  machinery), item 6 (FFI thread-safety), the GC, and the profiler. Choosing it
  late means redoing them. Build last, choose first.
- **Reflection is relevant to the backend plans.** Both JVM and CLR
  (`jvm-backend-plan.md`, `clr-backend-plan.md`) have rich reflection; defining
  the concept in `spec/` keeps that door open across all three targets.

## One-liner

The roadmap is short because most "missing" features are libraries. What's left
is the handful of things a program genuinely *cannot reach* from inside the
language — and they sort cleanly from cheap introspection to the architectural
weight of multi-core parallelism.
