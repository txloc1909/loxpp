# Pattern Matching Design

## Context

Lox++ has `switch/case/default` (merged PR #41) that dispatches on equality of
primitive values. While useful, it is fundamentally limited: it only answers
"does this value equal one of these scalars?" It cannot dispatch on the *shape*
or *type* of a value, cannot bind parts of a matched value to names, and its
result is discarded (statement only). Every non-trivial dispatch in user code
falls back to if-else chains regardless of switch existing.

This note covers the decision to evolve toward true structural pattern matching
in three phases, the motivations behind each design choice, and the programs
each phase unlocks.

---

## Why structural matching matters

### The current deficiencies it addresses

**1. No structured error handling.**
Lox++ has no exceptions. The only error-signalling mechanisms are nil returns
and convention-based list packing (`return [ok, value]`). Neither is enforced by
the language. A function that returns nil on failure and a value on success is
indistinguishable from a function that can legitimately return nil. File I/O
(milestone 8) makes this acute — `open()` can fail, and today there is no
idiomatic way to express that.

```lox
// today — nil as error signal, invisible to the language
var f = open("data.txt", "r");
if (f == nil) { /* failed */ }  // easily forgotten

// with Result + match — the language forces you to handle both branches
var result = open("data.txt", "r");
match result {
  case Ok{file}  => process(file)
  case Err{code} => print "failed: " + code
}
```

**2. Switch is dead weight for anything beyond scalars.**
You cannot switch on a class, a list shape, or a compound condition. The moment
data is structured, switch is useless.

**3. Verbose type-check + field extraction.**
Working with a heterogeneous value requires multiple steps that are conceptually
one operation — "what is this, and what does it contain?":

```lox
// today: type check and data extraction are separate
if (shape.kind == "circle") {
  var r = shape.radius;
  return 3.14159 * r * r;
} else if (shape.kind == "rect") {
  var w = shape.width;
  var h = shape.height;
  return w * h;
}
```

With structural matching, the type check and binding happen in one step. The
reader sees the expected shape of the data directly in the code structure.

---

## Key design decisions

### Match without destructuring is just a fancy switch

This is the insight that forced the phased plan. Match arms that can only
dispatch on type but cannot bind the matched data still require manual field
extraction inside the arm body. Adding match syntax alone does not change the
expressiveness of the language — it only changes the notation. Destructuring in
match arms is what closes the gap.

### Destructuring and match are multiplicative, not additive

Neither feature alone achieves the goal:
- Match without destructuring → type dispatch only, manual extraction remains
- Destructuring without match → shorter variable declarations (nicety only)

Together: type check, exhaustiveness, and binding happen in one expression. This
is the actual expressiveness win.

### Exhaustiveness and ADTs in a dynamic language

Full static exhaustiveness requires a **closed set of variants** known at
compile time. Open class hierarchies cannot provide this — any file can add a
new subclass. The solution is to distinguish between two levels of guarantee:

- **Open classes / arbitrary values**: runtime exhaustiveness only. If no arm
  matches and there is no wildcard, the VM raises `MatchError`. You find out at
  runtime, not compile time. This is the floor for all match expressions.
- **`variant` types (Phase 2)**: compile-time exhaustiveness. A `variant`
  declaration closes the set of constructors. The compiler knows all variants
  and rejects a match that does not cover them all.

These are not alternatives — they are layers. Runtime exhaustiveness (Option B)
is always present. Compile-time checking (Option A) applies only to `variant`
scrutinees and is the stronger guarantee you opt into by choosing `variant` over
open classes.

### Match is an expression from Phase 2 onward

In Phase 1, match remains a statement (consistent with the current switch). From
Phase 2, match becomes an expression — a value flows out of each arm. This is
the change that makes `Result`/`Option` types ergonomic rather than merely
possible:

```lox
// statement (Phase 1) — match drives side effects
match shape {
  case Circle{r} => print area(r)
  case Rect{w,h} => print w * h
}

// expression (Phase 2) — match produces a value
var a = match shape {
  case Circle{r} => 3.14159 * r * r
  case Rect{w,h} => w * h
}
```

This is a bytecode emitter design decision that cannot be cheaply retrofitted.
Phase 1 match statements compile like switch — subject consumed, no value on
stack. Phase 2 match expressions leave one value per arm on the stack, and all
arms must produce the same stack height.

---

## Phases

### Phase 1 — Match-case with runtime exhaustiveness

**What changes from switch/case:**
- `switch (expr) { case val: ... }` syntax replaced by
  `match expr { case pattern => body }`
- No fall-through between arms (switch already doesn't fall through, but the
  new syntax makes this unambiguous)
- A match with no wildcard arm (`case _ =>`) and no matching arm raises
  `MatchError` at runtime rather than silently doing nothing
- Multiple patterns per arm: `case 1, 2, 3 => ...`
- Guard clauses: `case x if x > 0 => ...`
- Match remains a **statement** in this phase

**What does NOT change in Phase 1:**
- Arms still match on value equality (primitives, strings) and class identity
- No binding of matched substructure inside arms
- No compile-time exhaustiveness

**Implementation notes:**
- The existing switch compiler (`compiler.cpp:551`) is the starting point —
  reuse the `EQUAL` + `JUMP_IF_FALSE` + `JUMP` chain
- Guard clauses compile to an additional `JUMP_IF_FALSE` inside the arm, before
  the arm body
- `MatchError` is emitted at the end of the jump chain if all arms fell through
  and no wildcard was present — one new opcode or a `THROW` to a sentinel value

**Programs unlocked:**

| Program | What Phase 1 enables |
|---|---|
| Command dispatcher (REPL-style) | `match input { case "quit" => ... case "help" => ... case _ => ... }` with MatchError safety |
| HTTP-style status handler | `match status { case 200 => ... case 404 => ... case 500 => ... }` with guard `case x if x >= 400 => ...` |
| Token classifier | `match token { case "+", "-" => ... case "*", "/" => ... }` — cleaner than switch |
| Day-of-week formatter | Guard clause: `case d if d >= 1 and d <= 5 => "weekday"` |
| Expression evaluator (operator dispatch) | Operators as strings, each arm handles one operator |
| Simple state machine | Match on state string + guard on input range |

---

### Phase 1.5 — Destructuring in `var` declarations

**What changes:**
- Positional list destructuring: `var [a, b, c] = list`
- Named field destructuring: `var {x, y} = point`
- Rest binding: `var [head, ...tail] = list`
- Nested destructuring: `var {pos: {x, y}} = entity`

**Motivation for a dedicated phase:**
Destructuring in `var` declarations is simpler to compile than destructuring in
match arms (no dispatch, no backtracking, no arm selection logic). It trains
users on the syntax and makes multi-return idioms ergonomic before Phase 2
makes those idioms load-bearing. It also validates the binding infrastructure
(new locals from patterns) before that infrastructure is needed inside match.

**Implementation notes:**
- `var [a, b] = expr` compiles to: evaluate expr, emit `GET_INDEX 0` → define
  local `a`, emit `GET_INDEX 1` → define local `b`, pop subject
- `var {x, y} = expr` compiles to: evaluate expr, emit `GET_PROPERTY x` →
  define local `x`, emit `GET_PROPERTY y` → define local `y`, pop subject
- Rest binding `var [head, ...tail]` needs a `SLICE` or list-copy-from-index
  operation — may require a new opcode or native helper

**Programs unlocked:**

| Program | What `var` destructuring enables |
|---|---|
| Multi-return functions via list | `var [quot, rem] = divmod(a, b)` — unpack without `[0]`/`[1]` |
| Coordinate unpacking | `var {x, y} = getPosition()` — cleaner than two field accesses |
| Swap without temp | `var [a, b] = [b, a]` |
| CSV row processor | `var [name, age, city] = parseLine(line)` |
| Pair iteration | `var [key, val] = entry` in a loop over a list of pairs |

---

### Phase 2 — `variant`, structural matching, match as expression

**What changes:**

**`variant` declaration:**
```lox
variant Result {
  Ok(value)
  Err(code, message)
}

variant Shape {
  Circle(radius)
  Rect(width, height)
  Triangle(base, height)
}

variant Tree {
  Leaf(value)
  Node(left, right)
}
```

- Constructors are functions: `Ok(42)` produces a `Result::Ok` value
- Values carry their constructor tag as a runtime discriminant (a small integer
  index into the variant's constructor list, stored alongside the payload)
- The compiler records all constructors per variant at parse time

**Destructuring in match arms:**
```lox
match shape {
  case Circle{radius}      => 3.14159 * radius * radius
  case Rect{width, height} => width * height
  case Triangle{base, height} => 0.5 * base * height
}
```

- Binding is simultaneous with dispatch — no manual field extraction
- Nested patterns:
  ```lox
  match tree {
    case Leaf{value}         => value
    case Node{left, right}   => sum(left) + sum(right)
  }
  ```
- List head/tail patterns:
  ```lox
  match list {
    case []           => 0
    case [x, ...rest] => x + sum(rest)
  }
  ```

**Compile-time exhaustiveness for `variant`:**
- If the scrutinee is statically known to be a `variant` type and not all
  constructors appear as arms (or a wildcard), the compiler emits an error
- Open classes and untyped values fall back to runtime `MatchError`
- A `case _ =>` wildcard always satisfies exhaustiveness

**Match as expression:**
```lox
var area = match shape {
  case Circle{r}   => 3.14159 * r * r
  case Rect{w, h}  => w * h
}

// Chains naturally:
print match classify(n) {
  case Positive => "pos"
  case Negative => "neg"
  case Zero     => "zero"
}
```

**Programs unlocked:**

| Program | What Phase 2 enables |
|---|---|
| **Result-based file I/O** | `match open("f.txt","r") { case Ok{file} => ... case Err{code} => ... }` — typed, exhaustive error handling |
| **Option type** | `variant Option { Some(value) Err }` — nil replaced by an explicit absent case |
| **Expression tree evaluator** | `variant Expr { Num(n) Add(l,r) Mul(l,r) Neg(e) }` — recursive match, no kind-strings |
| **Recursive list algorithms** | `sum`, `map`, `filter`, `fold` written as structural recursion on `[head, ...tail]` |
| **Binary tree operations** | Insert, search, height on `variant Tree { Leaf(v) Node(l,r) }` |
| **JSON-like value type** | `variant Json { Null Bool(b) Num(n) Str(s) Arr(list) Obj(list) }` — pretty-printer, accessor |
| **State machine with typed states** | `variant State { Idle Running{task} Error{code} }` — exhaustive transition logic |
| **Tokenizer with typed tokens** | `variant Token { Number(n) Ident(name) Plus Minus LParen RParen }` — parser reads clean arms |
| **Simple parser / AST builder** | Tokens as variant, AST as variant, match drives both stages |
| **Safe config reader** | Parse lines into `variant ConfigLine { KV{key,val} Comment Blank }`, exhaustive handling |

---

## Dependency chain

```
Phase 1: runtime MatchError + guard clauses
    → user code is safe on miss, guards enable conditional dispatch

Phase 1.5: destructuring in var declarations
    → multi-return idioms become ergonomic
    → binding infrastructure validated before match arms need it

Phase 2: variant + destructuring in arms + match-as-expression
    → type check and binding are one operation
    → compile-time exhaustiveness for closed types
    → Result/Option idioms become natural
    → recursive structural algorithms expressible
```

---

## What this does NOT fix

- **No generics.** `Ok(value)` can hold any value — `Option<T>` stays untyped.
  Pattern matching works but you don't get parametric safety.
- **No module/namespace.** Variant constructor names (`Ok`, `Err`, `Circle`) are
  in global scope. Name collisions between variants are the user's problem until
  a namespace mechanism exists.
- **Open class hierarchies remain open.** Matching on user-defined classes that
  are not declared as `variant` gets runtime exhaustiveness only — no compile-
  time check, because new subclasses can be added anywhere.

---

## Open questions resolved

This section records explicit answers to the design questions raised in
`pattern-matching-counterpoints.md`. Each answer is recorded here because these
decisions are non-obvious and have downstream implementation consequences.

---

### First-class patterns — explicitly declined

The counterpoints document lists first-class patterns (patterns as passable
runtime values) as a tentative "Yes". **This decision is reversed: first-class
patterns are out of scope, at least through Phase 2.**

The cascading consequences are too severe for the language at this stage:
- Forces structural (non-nominal) matching → kills compile-time exhaustiveness
  for `variant`
- Forces open variants → exhaustiveness degrades to a lint hint, not a guarantee
- Requires a pattern algebra as a core VM type (`Pattern = Wildcard | Bind | Eq
  | Constructor | Guard | Or | And | Not | Seq`)
- Requires `Option<Bindings>` as the match result type everywhere, infecting
  the entire error handling story
- Creates the pattern equality problem (undecidable in general)
- Decision-tree compilation is only possible for static arms; all dynamic arms
  fall back to O(n) sequential testing

None of these trade-offs are acceptable for a language whose primary goals are
learnability and clean semantics. First-class patterns are a research feature.
The existing design — nominal closed `variant` types, syntactic patterns only —
preserves all the ergonomic wins (destructuring, exhaustiveness, binding) without
any of the above costs.

If first-class patterns are revisited in the future, they must be scoped as a
separate opt-in facility (e.g. a `pat` object) that explicitly degrades
exhaustiveness to runtime-only for the match expressions that use them.

---

### Nominal vs. structural matching — nominal (tag-based)

A `variant` constructor match checks the **runtime constructor tag** (a small
integer discriminant stored in the value), not the presence or shape of fields.

```lox
variant Shape { Circle(radius) Rect(width, height) }

var s = circle(5)
match s {
  case circle{r} => ...   // tag check: is this a circle? then bind radius as r
  case rect{w,h} => ...   // tag check: is this a rect?
}
```

An object that happens to have a `radius` field but was not constructed with
`Circle(...)` does not match `case Circle{r}`. This is the nominal model.
Structural ("duck-typed") matching — "does this object have a `radius` field?"
— is not supported. Reasons:

1. Structural matching makes exhaustiveness impossible: the compiler cannot know
   what shapes exist in the world.
2. Tag-based matching maps directly to a jump table on the discriminant integer
   — O(1) dispatch for `variant` types.
3. Nominal semantics are what users from OCaml/Rust/Haskell expect, and what
   makes `variant` useful as a replacement for stringly-typed kind fields.

---

### Open vs. closed variants — closed, with runtime fallback for open types

`variant` declarations are **closed**: the compiler records all constructors at
parse time and rejects new constructors outside the declaring scope. This
enables compile-time exhaustiveness.

Open class hierarchies are **not** made into variants. Matching on an open class
gets runtime exhaustiveness (MatchError on miss) only. These are separate
facilities — users choose the guarantee they want by choosing `variant` vs.
`class`.

There is no "open variant" or extension mechanism. If a library variant needs to
be extended, the correct model is composition: wrap the library variant in a new
`variant` that adds cases.

---

### Bind vs. test disambiguation — bare identifier always binds

The classic ML ambiguity: does `north` in a pattern position test against the
existing variable `north` or create a new binding?

**Decision: bare identifiers in patterns always create new bindings.** To test
against an existing value, use a guard clause.

```lox
var north = 0

match direction {
  case north => ...       // WRONG intent: binds a NEW 'north', always matches
  case d if d == north => ...  // CORRECT: test against existing 'north'
  case _ => ...
}
```

Rationale:
- Consistent and unsurprising: pattern position always means "bind this"
- No pin operator (`^`) needed — that syntax is unfamiliar and adds novelty budget
- Guards already exist for Phase 1; "use a guard to test existing values" is
  one teachable rule
- Rust uses the same convention (with the addition of named constants in `const`
  position, which Lox++ does not have)

The compiler should warn when a bare identifier arm shadows an in-scope variable
with the same name, since this is almost always a mistake.

---

### Struct literal vs. block ambiguity — resolved by `=>` separator

Lox++ uses `{` for both blocks and (in Phase 2) field destructuring patterns.
Without care, `case Circle { ... }` is ambiguous: is `{ ... }` the field pattern
or the start of the arm body?

**Resolution: the `=>` separator is mandatory between pattern and body.** The
grammar rule is:

```
arm ::= 'case' pattern ('if' expr)? '=>' body
```

Everything before `=>` is pattern context; `{` in pattern context means field
destructuring. Everything after `=>` is expression/statement context; `{` there
starts a block. The ambiguity does not exist at the grammar level because the
contexts are delimited by `=>`.

```lox
match shape {
  case Circle{r}    => 3.14159 * r * r     // {r} is a field pattern
  case Rect{w, h}   => { var a = w * h; a }  // { after => is a block
}
```

This is the same resolution Rust uses for `match`. Rust additionally disallows
struct literal expressions in `if`/`while` conditions for the same reason; Lox++
inherits the same restriction: struct-pattern syntax (`Constructor{fields}`) is
only valid in pattern position.

---

### Or-patterns and binding consistency — hard error on mismatch

Or-patterns (`case A(y) | B(y)`) are supported in Phase 2. The constraint is:
**all alternatives must bind exactly the same set of names**, or the compiler
emits an error.

```lox
match x {
  case A(y) | B(y) => use(y)     // ok: both bind y
  case A(y) | B(z) => ...        // compile error: A binds y, B binds z
  case A(y) | C    => ...        // compile error: A binds y, C binds nothing
}
```

This is not optional — a body that references `y` must know `y` is bound in
every path that reaches it. Allowing mismatched bindings would require either
making unbound names nil (silent bugs) or runtime checks (defeats the point of
compile-time binding).

Note: the existing multi-value arm syntax from Phase 1 (`case 1, 2, 3 =>`) is
the degenerate case of or-patterns with no bindings. It remains valid and is not
affected by this constraint.

---

### Scope and hygiene of pattern-bound variables

Variables bound in a pattern are in scope for **both the guard clause and the
arm body**. If the guard fails, the bindings do not escape to subsequent arms —
they are implemented as locals that go out of scope at the end of the failed
arm's test sequence.

```lox
match x {
  case A(y) if y > 0 => use(y)   // y in scope for guard AND body
  case A(y)          => other(y) // fresh y, independent of the arm above
}
```

Implementation: the compiler allocates a local slot for each pattern binding
within the arm's test sequence. On guard failure, a `JUMP` skips the arm body
and the slots go out of scope. No special "rollback" is needed — the slot simply
was never committed to the user-visible scope table.

---

### Mutability of pattern-bound variables — mutable, following language default

Lox++ has no `let`/`const` distinction; all `var` bindings are mutable. Pattern
bindings follow the same convention: they create implicit `var`s and are
mutable by default.

```lox
match point {
  case Point{x, y} => {
    x = x + 1   // legal — x is a mutable local
    print x
  }
}
```

This is consistent with the rest of the language and requires no new rules.
If Lox++ ever adds immutable bindings, pattern bindings should adopt the same
syntax as the general solution rather than pattern matching having its own
mutability rules.

---

### Decision tree vs. sequential compilation — decision tree for `variant`

For nominal closed `variant` types, arms are compiled to a **decision tree**
dispatching on the constructor tag discriminant. The discriminant is a small
integer (index of the constructor in the variant's declaration order), enabling
a jump table when the number of constructors is small and dense.

```
match shape {             compiled to:
  case Circle{r} => a       LOAD subject
  case Rect{w,h} => b       GET_TAG
  case Triangle  => c       JUMP_TABLE [Circle→L1, Rect→L2, Triangle→L3]
}                         L1: bind r, jump to a
                          L2: bind w h, jump to b
                          L3: jump to c
```

This is O(1) dispatch for `variant` types. Guards break the jump table model
(a guard can fail, requiring a fall-through), so arms with guards revert to
sequential testing within their tag's slot.

For open-class and untyped scrutinees (Phase 1 and non-variant Phase 2), the
existing `EQUAL + JUMP_IF_FALSE` chain remains — sequential and O(n) in arms,
which is acceptable for the small switch-like cases that form of match is used
for.

**First-class patterns would have forced all match to be sequential.** This is
one concrete implementation cost of that decision.

---

### `not`-patterns — deferred

Negation (`not`-patterns) is not part of Phase 1 or Phase 2. The reasons:

1. Exhaustiveness with `Not` is co-NP hard in general.
2. Decision-tree compilation is not possible for arbitrary `Not` patterns.
3. Binding semantics under `Not` are undefined (`not(bind(x))` makes no sense).
4. The restricted form (non-binding `Not` only) has limited use cases that guards
   already cover: `case x if x != 0` is more readable than a hypothetical
   `case not(0)`.

If added later, the restriction from the counterpoints document applies: `Not`
is only permitted over non-binding sub-patterns, and match expressions
containing `not`-patterns are explicitly excluded from exhaustiveness checking.

---

### Recursive variants — heap-allocated, naturally supported

Recursive variants (e.g. `variant Tree { Leaf(v) Node(left, right) }`) are
supported without special treatment. All `variant` constructor values are
heap-allocated `Obj*` values — the same representation as class instances. A
`Node` holding two `Tree` sub-values holds two `Obj*` pointers, which is exactly
how the GC already traces object graphs.

No "boxing" annotation is needed and no optimization for unboxed recursive
variants is planned. If performance becomes a concern for deep recursive
structures, that is a VM-level optimization problem, not a language design one.

---

### Nested pattern depth and `@`-bindings — deferred

Deep nested patterns are supported syntactically but `@`-bindings (bind the
whole matched subtree and destructure it simultaneously, e.g. `node @ Node{l,r}`)
are deferred. They add syntax surface without unblocking any new program class.
A user who needs both the whole value and its parts can introduce a local:

```lox
case Node{left, right} => {
  var node = Node(left, right)   // reconstruct if needed
  ...
}
```

This is verbose but the workaround is clear. `@`-bindings can be added later
without changing any other part of the design.

---

### Match failure semantics — MatchError, not Option\<Bindings\>

The counterpoints document identifies `Option<Bindings>` as "the most
principled choice" for first-class patterns. Since first-class patterns are
declined, this does not apply.

For syntactic match expressions, failure semantics are simpler:
- A `match` expression with no matching arm and no wildcard raises `MatchError`
  at runtime
- The error is not catchable within a match expression (it propagates up)
- There is no implicit `None`/`nil` return on no-match

This is the Elixir/Rust model: match is *assumed exhaustive* by the programmer,
and a miss is always a bug. `Option<Bindings>` would make every match expression
partial by default, requiring callers to handle the "no match" case even when it
is structurally impossible — exactly the nil-proliferation problem the design is
trying to eliminate.

---

### Pattern equality — not applicable

Pattern equality (are two patterns equal? can patterns be hashed?) is a
consequence of first-class patterns. Since patterns are syntactic constructs
and not runtime values, this question does not arise.
