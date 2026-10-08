# Dynamic Functional Programming — Direction

## Why this note

Enum + pattern matching were added to make the self-hosted interpreter
ergonomic (`bootstrap/loxpp_interpreter.lox`: 2583 lines, ~215 `match`/`case`
sites, AST modelled as `enum Expr` / `enum Stmt` / `enum Pattern`). The open
question was how far to push "FP/Rust niceties" from there.

Conclusion: **keep mining FP, but the *dynamic* FP tradition, not the typed
one.** The wall we hit is real but narrow — it sits exactly on the line between
two different traditions, and only one of them fights a dynamically-typed VM.

## The distinction that resolves it

- **Typed FP (ML, Haskell, Rust):** exhaustiveness *is* totality, `?`,
  `Option` everywhere. The static type on the scrutinee does the work. This is
  the *only* thing that fights dynamic typing — without a static type the
  scrutinee can be any runtime value, so exhaustiveness is a lint over intent
  (not a totality guarantee) and the runtime match-failure trap
  (`vm.cpp` `MatchError: no matching arm`) stays permanently reachable.
- **Dynamic FP (Scheme, Clojure, Elixir, Erlang):** pattern matching,
  immutable-ish data, sum types **by convention**, runtime match-failure as an
  honest feature, combinators as plain library code. This *is* dynamic typing
  done well — no type checker required.

Everything that fit cleanly earlier (`if let`/`while let`, enum methods,
combinators, nested patterns) is dynamic-FP-native. Everything that fought
(`?`, sound exhaustiveness, `Option`-everywhere) is typed-FP. We embrace the
former and explicitly retire the latter.

## The 3 salvage points

### 1. Re-anchor the yardstick to the use case
Stop measuring against Rust; measure against Clojure/Elixir. Let the bootstrap
interpreter drive the feature list. Two demands already visible:
- **Nested patterns.** The object language's `Pattern` enum supports
  `CtorPat(cname, subpats)`, `ListPat(elems, rest_name)`, `AtPat(...)`, but the
  host grammar only allows flat `IDENTIFIER` fields in a ctor pattern
  (`spec/02-syntax.md:114`). We can't write the evaluator using the features it
  implements. The use case is asking for nested patterns — not Rust-envy.
- **Exhaustiveness-as-a-lint.** The self-hosting bug is "added an `Expr`
  variant, forgot it in `eval`." We already have the bones
  (`compiler.cpp` `checkEnumExhaustiveness`); keep it as a *lint*, not a
  totality claim.

### 2. Adopt the dynamic-FP vocabulary; retire the typed imports
- Result-by-convention via existing enums (`Ok`/`Err`, `Some`/`None`), matched
  by hand — Elixir's `{:ok, _}` / `{:error, _}` style.
- Combinators as plain module functions, chained with a pipe operator
  (`opt |> map(f) |> unwrapOr(0)` rewrites to `unwrapOr(map(opt, f), 0)`).
  Dynamic typing makes these cheap, with no type params to thread. Enums stay
  closed data and carry no methods: a method table is closed to users once
  libraries exist, and a recursive method gets no tail-call elimination.
- Match failure as a clean runtime error.
- **Explicitly out of scope:** `?` operator, sound exhaustiveness,
  `Option`-everywhere. They are not "missing" — they belong to typed FP.
  (`Option`-everywhere also collides with Lox truthiness: `None()` is an enum
  instance, hence truthy, so `if (opt)` is always true — `spec/03-types.md:208`.)

### 3. If static safety ever becomes a real goal, add it as a layer
Gradual typing (TypeScript-over-JS, Typed Racket-over-Racket): a separate,
erasable checker pass that leaves the VM dynamic. Big subsystem — only worth it
if static safety becomes a genuine goal rather than FOMO. Key property: it is
**purely additive**, so the dynamic-typing decision forecloses nothing.

## One-liner
The architecture decision is fine; the only thing that needed salvaging was the
ambition's reference frame. Lox++ is a dynamically-typed scripting language
whose enums + matching make tree-walking pleasant — by that standard the
feature is a success, and dynamic FP is the lane with room left to run.

## Roadmap & work items

### Why the language drifted after this note

Since this note, the language moved toward imperative and system features, not
FP ergonomics. Two forces explain the drift:

- **The capability litmus test.** `notes/expressiveness-roadmap.md` counts a
  feature as a gap only if it cannot be bootstrapped from closures + lists +
  maps + recursion + enums + `match`. Under that test every FP ergonomic
  (`map`/`filter`/`reduce`, lambdas, `const`, `Result`/`Option`, modules) is a
  "library, not a gap". The genuine gaps that clear the bar — reflection, OS
  access, non-local control flow, operator overloading, coroutines, FFI,
  parallelism — are all runtime and system capabilities. The test does not
  reject FP; it never prioritises it.
- **Multi-backend parity economics.** Every feature was emitted for native +
  JVM + CLR + QBE and kept differential-green. High-level FP sugar had the
  worst cost/benefit under that constraint (emission work multiplied across
  four backends), while low-level native primitives were cheap to add
  uniformly. The premise was wrong. Under the backend roles in `AGENTS.md`, a
  feature is owed by the native VM and the bootstrap interpreter. The JVM owes
  it only as an oracle, and QBE only when an experiment needs it. A feature
  that is a compiler rewrite, such as a pipe operator, costs the native
  compiler and the bootstrap parser, not four emitters.

The note also closed rather than opened work: its salvage points (nested
patterns, exhaustiveness-as-lint) were already done, and the follow-up commit
removed the pattern-matching note with "no work item remaining". No FP work
stream survived to pull the language.

### The wedges, re-verdicted

FP-as-library was the intended outcome all along ("combinators as plain library
code"). Three things a library cannot cross, and their verdicts:

- **TCO — the real blocker.** Recursion-heavy FP exhausts the VM frame budget
  (`notes/bootstrap-stack-depth.md`). A library cannot add it. Tracked: #555.
- **`__bool__` truthiness — decided against.** Only `false` and `nil` are
  falsy; `None()` is truthy. Clojure sidesteps this with `nil`, Elixir with
  match-only consumption, and Lox++ follows them: consume an `Option` with
  `match`. A truthiness hook would run user code on every branch of every
  backend. Closed: #556.
- **Enum methods (`.map`/`.unwrapOr`) — decided against.** Enums take
  constructors only. Combinators are module functions chained with a pipe
  operator. Closed: #557.

### The strategy: library first, dogfood second

Write FP features as user-space example programs, not as language or stdlib
changes, then make a real program use them. Success is a program that *uses*
the library, not a library that merely exists. Tracked: #554.

### TCO design record

Self-tail-call elimination in `src/compiler.cpp`. When `return <self-call>` has
a callee that resolves to the function being compiled, emit the argument values,
store them into the parameter slots (slots 1..arity), and `LOOP` back to the
body start instead of `GET_*/CALL/RETURN`. The compiler produces the single
shared bytecode chunk, so the native VM and the JVM, CLR, and QBE backends all
get this for free.

Bail-outs — do not transform when the function:

- creates a closure or captures an upvalue (slot reuse would leak a later
  iteration's value into an earlier iteration's closure),
- contains `defer` or `try` (a loop must not re-run handlers or defers per
  iteration),
- has a shadowed or reassigned self-name.

Spec change in the same PR: `spec/04-semantics.md` (Function Call, `return`,
stack overflow) — a tail self-call no longer grows the stack, so deep tail
recursion stops raising `StackOverflowError`. Tracked: #555.

As built, the first cut is narrower than the sketch above, so it stays easy to
widen: only a function declared with `fun` at global scope qualifies, because
its own name then resolves to a global and no upvalue is involved. Because any
code may rebind a global, the return site compares the global with the running
function (slot 0) and takes the ordinary call when they differ; the call is
compiled twice from the same source for this. The body scan rejects any nested
`fun` or `class`, `try`, and `defer`. Gaps left on purpose: nested and method
self calls (resolve the name through slot 0 or an upvalue instead of a
global) and mutual recursion. General tail calls would reuse the frame in the
VM instead and do not conflict with this compile-time path.
