# Pattern Matching & Variant Types: Language Design Reference

> Design notes for a dynamic scripting language with structural pattern
> matching and variant types as first-class citizens.

---

## Table of Contents

1. [Core Design Decisions](#core-design-decisions)
2. [Syntax Ambiguity](#syntax-ambiguity)
3. [Variant Type Design](#variant-type-design)
4. [Ergonomic Conflicts](#ergonomic-conflicts)
5. [Technical Implementation](#technical-implementation)
6. [First-Class Patterns: Cascading Consequences](#first-class-patterns-cascading-consequences)
7. [The Emergent Runtime Architecture](#the-emergent-runtime-architecture)
8. [Open Questions Summary](#open-questions-summary)

---

## Core Design Decisions

These decisions must be made explicitly and early, as each one cascades into
other areas.

| Decision | Options | Status |
|---|---|---|
| Nominal vs. structural variants | Nominal / Structural / Hybrid | — |
| Open vs. closed variants | Open / Closed / Closed-with-escape-hatch | — |
| Bind vs. test disambiguation | Pin operator / Naming convention / Explicit algebra | — |
| First-class patterns | Yes / No | **Yes** |
| `not`-patterns | Allow / Restrict to non-binding / Disallow | — |
| Match failure semantics | `Option` / Exception / Sentinel / Fall-through | — |
| Pattern equality & identity | Syntactic / Reference / Semantic (undecidable) | — |
| Exhaustiveness checking | Hard guarantee / Lint/warning / None | — |

---

## Syntax Ambiguity

### Expression vs. Pattern Duality

The same surface syntax must mean different things depending on position:

```
# Pattern position: destructure
match x {
  Some(y) => ...
}

# Expression position: construct
let z = Some(y)
```

The parser requires **contextual grammar** — the same token sequence parses
differently inside a `match` arm's LHS vs. everywhere else. This complicates
recursive-descent parsers significantly.

### Literal vs. Variable Binding

The classic ML ambiguity:

```
let north = 0
match direction {
  north => ...  # Binds a NEW variable 'north', or tests against the value 0?
}
```

Every language resolves this differently:

| Language | Resolution |
|---|---|
| Rust | Naming convention + `if` guards |
| Erlang | Uppercase = existing value, lowercase = bind |
| OCaml | `when` guards |
| Elixir | `^` pin operator for existing values |

**A resolution must be chosen — each has ergonomic costs.**

### Struct Literal vs. Block Ambiguity

```
match foo { x, y } { ... }
#          ^^^^^   Block or struct pattern?
```

Rust disallows struct literals in `if`/`while` conditions for exactly this
reason. This needs an explicit grammar rule.

---

## Variant Type Design

### Nominal vs. Structural Variants

**Structural** (e.g. TypeScript discriminated unions): flexible, but
exhaustiveness checking is weakened or impossible — the compiler cannot know
all shapes at compile time.

**Nominal**: only values constructed via a declared constructor match that
pattern. Enables exhaustiveness, but requires upfront declaration — which
conflicts with the dynamic nature of the language.

**With first-class patterns, structural is strongly pressured** (see
[First-Class Patterns](#first-class-patterns-cascading-consequences)).

### Open vs. Closed Variants

| Style | Pro | Con |
|---|---|---|
| **Closed** (ML/Haskell) | Exhaustiveness is checkable | Extending a type in a library breaks all match sites |
| **Open** (Clojure multimethods) | Extensible | Exhaustiveness becomes best-effort only |

For a dynamic language, closed variants feel unnatural. Open variants mean
exhaustiveness warnings are hints, not guarantees. **With first-class patterns,
open is forced.**

### Recursive Variants & Representation

```
type Tree = Leaf | Node(Tree, int, Tree)
```

Recursive variants require heap allocation (boxed representation). In a dynamic
language this is likely already the case, but unboxed recursive variants are
hard to retrofit if optimization is later desired.

---

## Ergonomic Conflicts

### Binding vs. Equality Testing

In patterns, identifiers usually *bind*. But users often want to test against
an existing value:

```
match val {
  x    => ...   # Always matches, binds x — probably not intended
  0    => ...   # Tests for literal 0
  CONST => ...  # What happens here?
}
```

Guards (`if`-clauses on arms) reduce pressure but add verbosity. A pin operator
(`^x`) is explicit but surprising to newcomers. **With first-class patterns,
this resolves into an explicit `Bind` vs `Eq` algebra.**

### Nested Pattern Depth

Deep structural matching is powerful but can become unreadable:

```
match msg {
  Response({ status: Ok({ code: 200 }), body: Some(text) }) => ...
}
```

`@`-bindings (bind the whole subtree *and* destructure it) help, but add syntax
surface. A failing inner pattern should short-circuit to the next arm — this
must be specified clearly.

### Or-Patterns and Binding Consistency

```
match x {
  A(y) | B(y) => use(y)   # y must be bound in BOTH alternatives — ok
  A(y) | B(z) => ...      # error? shadow?
}
```

Or-patterns require that all alternatives bind the **same set of names**. This
is a non-trivial constraint to check and report with good error messages.

---

## Technical Implementation

### Exhaustiveness Checking in a Dynamic Language

Static exhaustiveness requires knowing all variants at compile time. Options:

| Approach | Pro | Con |
|---|---|---|
| No check | Simple to implement | Silent runtime bugs |
| Runtime "match error" on no-arm | Easy | Errors surface at runtime |
| Optional static analysis pass | Best of both | Two-phase compiler complexity |
| Require explicit `_ =>` wildcard | Forces acknowledgment | Verbose |
| Lint/warning for static arms only | Pragmatic | Partial guarantee only |

**With first-class patterns, exhaustiveness can only be guaranteed for
fully-static match expressions** (all arms are syntactic pattern literals). The
moment a pattern *value* appears in an arm, exhaustiveness silently degrades to
a runtime concern.

### Decision Tree Compilation

Pattern matching is typically compiled to decision trees (or DAGs) to avoid
redundant tests. The standard algorithm (Maranget's) has subtleties:

- **Redundant pattern detection** requires the same analysis as exhaustiveness
- **Or-patterns** can cause exponential blowup in naive compilation
- **Guard clauses** (`if cond`) break the decision tree model — guards are
  arbitrary expressions, requiring re-testing on guard failure

**With first-class patterns:**
- Static match arms (syntactic): still compilable to decision trees
- Pattern values in arms: must fall back to **sequential left-to-right
  testing** — O(n) in number of arms

Mitigations for sequential testing cost:
- Cache compiled matchers when the same pattern value is reused
- Allow patterns to expose an optional *tag hint* as a fast pre-filter
- JIT-compile pattern values on first use (if the runtime supports it)

### Interaction with Dynamic Dispatch / Duck Typing

If the language is duck-typed, what does matching on a variant *mean* at
runtime?

```
match obj {
  Circle({ radius }) => ...  # Checks a tag? Checks for 'radius' field? Both?
}
```

Likely options for the matching semantics of a runtime value:

- **Tag-based**: every value carries a runtime tag; match checks it (nominal
  feel)
- **Field-based**: match checks for presence/shape of fields (structural feel;
  harder to make exhaustive)
- **Protocol-based**: types opt into matchability (most flexible, most complex)

### `not`-Patterns

Negation is the natural completion of `And`/`Or` in the pattern algebra, and
users will want it:

```
let not_null   = pat.not(pat.eq(null))
let positive   = pat.and(pat.type("int"), pat.guard(x => x > 0))
```

Consequences:

- **Exhaustiveness with `Not` is co-NP hard in general**
- **Decision tree compilation is not possible** for arbitrary `Not` patterns
- **Capture semantics under `Not` are undefined**: `not(bind(x))` makes no sense

Reasonable restriction: **`Not` is only permitted over non-binding patterns**
(pure tests). This preserves most utility while keeping semantics sound.

### Scope & Hygiene of Bound Variables

Variables bound in a pattern must scope correctly across guards and the arm
body:

```
match x {
  A(y) if y > 0 => use(y)  # y in scope for guard AND body
  A(y)          => use(y)  # y re-bound; does it shadow?
}
```

If a guard fails, its bindings must be *rolled back* — they must not leak into
subsequent arms. This interacts with any destructor/drop semantics the language
has.

### Mutability of Pattern-Bound Variables

Should variables bound in patterns be mutable by default? This feeds into the
broader mutability story but needs an explicit answer because patterns are a
major binding site.

### Interaction with Error Handling

If the language uses variants for errors (`Result(Ok, Err)`), pattern matching
becomes the primary error-handling mechanism. Nested result matching under
fallibility matters enormously ergonomically — consider how a `?`-operator
style desugars and interacts with match.

---

## First-Class Patterns: Cascading Consequences

Making patterns first-class is the most consequential single decision. It
forces or strongly pressures answers to nearly every other open question.

### Forces Structural (or Hybrid) Variants

With patterns as passable values:

```
let my_pat = get_pattern_from_library()  # opaque at this call site
match x { my_pat => ... }
```

A nominal system requires knowing *which variant* a pattern covers at its
*definition* site — unavailable when patterns are first-class. Structural
matching becomes necessary. A hybrid is workable: declared variants for the
common case, first-class patterns opting out of exhaustiveness guarantees.

### Forces Open Variants (or Degrades Closed to a Local Hint)

If a pattern can be constructed at runtime, the compiler cannot close the
world. Exhaustiveness checking becomes best-effort:

- Fully static match expressions (all arms are syntactic literals): still checkable
- Any arm containing a pattern value: exhaustiveness silently degrades to runtime

**Practical resolution**: distinguish *static match* from *dynamic match*, and
provide exhaustiveness guarantees only for the former, requiring an explicit
wildcard for the latter.

### Forces Explicit Bind vs. Test Syntax

Naming conventions and lexical context are no longer sufficient. The pattern
value must carry intent explicitly in its representation:

```
let p = pat { x: bind(y), status: eq(200) }
#                 ^^^^          ^^^
#              capture slot   equality test
```

The pin-operator question dissolves — `Bind` and `Eq` become explicit
constructors in the pattern algebra.

### Requires Explicit Match Failure Semantics

With static patterns, a missing wildcard is a static error. With first-class
patterns used as values, match is inherently partial:

```
let handlers = [(pat_a, fn_a), (pat_b, fn_b)]
match_first(x, handlers)  # what if nothing matches?
```

| Behavior | Consequence |
|---|---|
| Return `None`/`Option` | Match becomes a query; callers must always handle failure |
| Throw a runtime error | Ergonomic for "shouldn't fail" cases; painful otherwise |
| Fall through to next expression | Requires a "match chain" operator |
| Return a composable sentinel | Most powerful, most complex |

**`Option<Bindings>` is the most principled choice** — it composes, and allows
`match_first`, `match_all`, `match_any` to be library functions rather than
language primitives.

### Creates the Pattern Equality Problem

Once patterns are values, new questions arise that don't exist otherwise:

- Are two patterns matching the same set of values *equal*? (`pat.eq(1) ==
  pat.eq(1)`?)
- Can patterns be placed in a hash map or set?
- Is `pat.or(A, B)` the same pattern as `pat.or(B, A)`?

**Semantic equality** (same extension) is undecidable in general. Practical
options:

| Equality model | Behavior | Cost |
|---|---|---|
| Reference identity | `pat.eq(1) !== pat.eq(1)` | Surprising; inconsistent with value feel |
| Syntactic (structural tree) | Same tree = equal | Easy; `or(A,B) != or(B,A)` edge cases |
| Semantic | Same values matched = equal | Undecidable in general; off the table |

---

## The Emergent Runtime Architecture

First-class patterns force a **pattern algebra as a core runtime type**:

```
type Pattern =
  | Wildcard
  | Bind(name: String)
  | Eq(value: Any)
  | Type(typetag: Tag)
  | Field(name: String, inner: Pattern)
  | Constructor(tag: Tag, fields: [Pattern])
  | Guard(inner: Pattern, pred: Fn)
  | Or(a: Pattern, b: Pattern)
  | And(a: Pattern, b: Pattern)
  | Not(inner: Pattern)         # restricted: inner must be non-binding
  | Seq(patterns: [Pattern])    # for sequence/array destructuring
```

And a **match execution primitive**:

```
fn match(pattern: Pattern, scrutinee: Any) -> Option<Bindings>
# where Bindings = Map<String, Any>
```

All match syntax desugars into this primitive. All higher-level constructs
(`match` expressions, destructuring assignment, function argument patterns)
become sugar over it. `match_first`, `match_all`, `match_any` become library
functions.

**This means the pattern executor is a core VM component, not a compiler pass.**

---

## Open Questions Summary

### Forced by First-Class Patterns

| Question | Answer |
|---|---|
| Nominal vs. structural | Structural (or hybrid with degraded guarantees) |
| Open vs. closed | Open (exhaustiveness becomes a lint, not a guarantee) |
| Bind vs. test disambiguation | Explicit `Bind`/`Eq` nodes in pattern algebra |
| `not`-patterns | Allow, but restrict to non-binding sub-patterns |
| Decision tree compilation | Only for static arms; sequential fallback for pattern values |
| Match failure semantics | Return `Option<Bindings>` or equivalent |
| Exhaustiveness | Guaranteed only for fully-static match expressions |

### Still Open

| Question | Options to consider |
|---|---|
| Pattern equality model | Reference identity vs. syntactic equality |
| Mutability of bound variables | Mutable / immutable / follows language default |
| Protocol for matchability | How do user types opt into structural matching? |
| Surface syntax for the pattern algebra | Literal DSL vs. combinator functions vs. both |
| Tag hint / fast-path for pattern values | Optional optimization protocol on pattern type |
| Error handling integration | How does `?`-style sugar interact with match? |
| Exhaustiveness for `Not` patterns | Restrict to non-binding, document as non-checkable |
| Or-pattern binding consistency | Hard error if alternatives don't bind same names |
