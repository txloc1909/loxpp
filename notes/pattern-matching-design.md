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
