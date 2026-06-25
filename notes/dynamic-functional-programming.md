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
- Combinators as methods on enums (`.map`, `.unwrapOr`). Dynamic typing makes
  these *cheaper*, not harder — no type params to thread.
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
