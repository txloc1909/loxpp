# Lox++ design tensions — 2026-10-08

Snapshot assessment, based on the notes changed since late September
(`import-system.md`, `jit-pipeline.md`, `coroutines-retro.md`,
`coroutines.md`, `dynamic-functional-programming.md`,
`concurrency-model-decision.md`, `actor-model-design.md`,
`expressiveness-roadmap.md`) plus the discussion on #557 and #564.

## Current tensions

### T1 — Coroutines force all of QBE into a slow mode
Any program that contains `yield` compiles entirely in QBE coroutine mode
(promotion off, trampoline; `coroutines.md`). The per-function fallback was
shown to be unsound (`coroutines-retro.md`). Two new designs are about to
repeat the whole-program rule:
- **Import system:** a generator-based stdlib module would slow down every
  program that imports it (`import-system.md`, residual edge 1).
- **JIT v0:** turns itself off for any program that contains `YIELD`
  (`jit-pipeline.md` §4).

The way out the notes name: a `may-suspend` effect analysis, carried on
explicit exports.

### T2 — Everything is "a library", but there is no way to share one
The capability test (`expressiveness-roadmap.md`) sends FP, JSON, sets, etc.
to user-space libraries and calls modules "organizational, not expressive".
The FP strategy is "library first, dogfood second" (#554). Without imports, a
library is text copied into each file (`examples/option_result.lox`).

### T3 — The concurrency decision (item 6) is due
Coroutines shipped (#523), which was the precondition. Item 6 (measured
go/no-go, actor vs CSP) has no issue. Meanwhile:
- The JIT note designs around it with a subprocess compiler.
- The import note defers to it ("revisit if per-task VMs").
- FFI (roadmap item 6), GC redesign, scheduler work and #519 are blocked on
  it.

The stale C3 claim is fixed (`500764e`). C2 and C6 in
`concurrency-model-decision.md` still cite `src/vm.h` line ranges from before
the Runtime/VM split.

### T4 — Scoping work across 4 backends (narrowed)
Already covered:
- Rule 6 (#564, adopted in #576, `AGENTS.md` planning policy) — whether a
  scoped subset ("only"/"subset"/"fallback"/"phase 1") is sound.
- Retro rules 1/3/5 (`notes/non-local-control-flow-retro.md`) — list every
  consumer up front; each backend node diffs clean against merged backends;
  the gate covers compile-time as well as runtime.

Two gaps remain:
- Rules 1–5 are written for missions and live in a note, so they may not
  bind a standalone issue such as #557.
- No rule says whether a spec section may land on `main` before every
  backend implements it, or how the gap is shown (xfail probes, or merging
  the spec with the last backend node). `AGENTS.md` only says "fix the
  implementation".

The bootstrap is still the costliest consumer. Imports would make it handle
multiple files, and today it reads one program from stdin and does not know
its own path.

### T5 — The JIT does not fit the single static binary
It needs `qbe`, `cc` and a linker on the user's machine at run time. The
note's own open question (can the AOT emitter be reused per function?) is
unsized. Cheaper interpreter wins (inline caches, shapes) are still open.

### T6 — Two separate lookup paths could appear
The import runtimepath and FFI `.so` loading should share one "loadable unit
+ search path" design (`import-system.md`, non-goals).

### T7 — Dynamic vs typed FP
Settled at the top level: dynamic FP, with optional gradual typing added
later. The detailed direction falls under T8.

### T8 — FP direction (from #557)
Three ways to name sum-type combinators compete: enum methods, a pipe
operator, or module namespaces.
- Enum methods are closed, so once imports exist a user cannot add one to a
  library's type.
- A recursive enum method such as `t.fold(...)` gets no TCO, because #567
  only optimizes self tail calls in a top-level `fun`.
- #556 (`__bool__`) cannot fix its own `None()` example without #557.
- `dynamic-functional-programming.md` contradicts itself on which of these
  is the intended direction.

## Can keep going as is
- GC tuning and observability (#577–#582 style), as long as it does not
  become a collector redesign.
- The JIT stays a proposal. Nothing depends on it, and its threshold needs a
  measured compile cost first.
- Widening TCO to nested functions and mutual recursion. Leave method
  recursion until T8 is decided.
- The editor and distribution backlog (#201–#219, #294).
- Housekeeping: fix the C2/C6 line citations.

## Should not go ahead as planned
- #557 (enum methods) and #556 (`__bool__`). Building either now would
  commit the language to receiver-based, closed combinators before the
  import system decides how names are scoped. #557 carries a dependency
  comment (issuecomment-6029403424). That comment still describes T4 as a
  missing rule; it should point at rule 6 and retro rules 1/3/5 instead.

## Should be decided soon
1. **Item 6, the concurrency go/no-go (T3).** Most urgent: unblocked, and
   three or four designs already work around the gap. Open the issue,
   collect the coroutine suspend/resume measurements it asks for, and decide
   stack and heap ownership plus the message-copy rule.
2. **The import binding model together with the FP direction (T2 + T8,
   and T1 by extension).** Approve the minimal import frame: module-as-object,
   file-relative anchoring, embedded-source stdlib, cycles as a compile
   error. Decide explicit exports, the natural carrier for `may-suspend`.
   Then choose methods, a pipe operator, or modules only, and fix the FP
   note to match. Do this before a generator-based stdlib or JIT v0 locks in
   the whole-program `yield` rule. This also settles #556 and #557.
3. **The two leftover scoping points (T4).** Decide whether rules 1–5 move
   into `AGENTS.md` so standalone issues are bound by them. Decide whether a
   spec section may lead its implementations on `main`, and if so how the gap
   is shown.

T5 and T6 can wait until decisions 1 and 2 land.

## Resolutions (2026-10-08)

The language goal is the test for each decision: Lox++ is expressive in the
dynamic functional tradition, and no high-level facility removes low-level
control. An option must add expressiveness of that kind and must not remove
control the programmer has today.

- **T8, FP direction.** Combinators are module functions chained with a pipe
  operator (`a |> f(b)` rewrites to `f(a, b)`). Enums stay closed data with no
  methods. A method table is closed to users once libraries exist, and a
  recursive method gets no tail-call elimination. The pipe is a parse-time
  rewrite, so it adds no object type and no dispatch path on any backend, and
  `t |> fold(f, z)` stays a top-level self call that #567 optimizes.
  Polymorphism over enums, if wanted later, needs an open protocol, not closed
  methods. #557 and #556 are closed. Truthiness stays `false` and `nil` only,
  because a truthiness hook runs user code on every branch of every backend.
- **T7.** Settled: dynamic FP. Gradual typing, if added, is an erasable layer.
- **T2, imports.** The minimal frame in `import-system.md` is approved, plus
  explicit exports.
- **T6.** Import resolution and FFI loading share one search-path design.
- **T1, whole-program `yield`.** The first note said explicit exports could
  carry a `may-suspend` summary. That is not enough: source inclusion shows the
  compiler every module, and dynamic dispatch is the real obstacle. Two sound
  fixes exist: a native stack per coroutine on QBE, or a call-graph analysis
  that clears only functions whose callees are all known. Both are stack
  ownership questions, so item 6 decides. Neither import nor the JIT may
  adopt the whole-program rule as final.
- **T3, concurrency.** Item 6 is the most urgent decision. Seed data: about
  112 to 144 ns per yield and resume round trip on the native release binary
  (`benchmarks/coroutine_yield.lox`, 200,000 round trips). The model must
  expose a low-level spawn and send primitive that libraries can build
  channels or actors on.
- **T4, scoping.** Retro rules 1 to 5 bind every language-surface change.
  A spec section may lead its implementations only inside a mission, with
  expected-failure probes. See `AGENTS.md`. Only
  `tools/check_coroutine_probes.sh` has an expected-failure list today.
- **T5, JIT.** An opt-in tier that needs the QBE toolchain. It is never on by
  default.
