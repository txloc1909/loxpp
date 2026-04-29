# Pattern Matching Design

## Context

Lox++ has `switch/case/default` (merged PR #41) that dispatches on equality of primitive values.
It is being completely replaced by nominal pattern matching with three goals:

1. Type-check and field extraction happen in one operation (not two separate if-else chains)
2. Match failure is explicit and loud (`MatchError`) rather than silently falling through
3. An `enum` declaration closes a type and enables compile-time exhaustiveness

---

## Design Decisions

### Nominal, not structural

A constructor match checks the **runtime tag** stored in the value, not the presence or shape of
fields. An object that has a `radius` field but was not constructed with `circle(...)` does not
match `case circle{r}`. This enables O(1) tag dispatch and makes compile-time exhaustiveness
possible.

### Closed `enum`, runtime fallback for open types

`enum` declarations are closed: the compiler records all constructors at parse time and checks
exhaustiveness at the `match` site. Open class hierarchies are not made into enums. Matching on
an open class gets only the runtime guarantee — if no arm matches and there is no `case _ =>`,
the VM raises `MatchError`.

### Bare identifier always binds; use guard to test existing values

In pattern position, a bare lowercase identifier always creates a new local binding (and always
matches). To test against an existing variable, use a guard clause.

```lox
var north = 0
match direction {
  case d if d == north => ...  // correct: test against north
  case north => ...            // wrong intent: binds a new 'north', always matches
}
```

### Constructor disambiguation — symbol lookup

No naming convention is required for constructor names. In pattern position, the compiler checks
its constructor table:

- Name is a known constructor → constructor dispatch pattern (tag check + field binding)
- Name is not a known constructor → variable binding (always matches)

If a constructor name shadows a local variable, the constructor wins in pattern position. This is
documented behaviour. Wildcard `_` is always a non-binding wildcard; no constructor may be named
`_`.

```lox
enum result { ok(value) err(code) }
match r {
  case ok{v}   => print v      // ok is a known constructor → dispatch
  case err{c}  => print c      // err is a known constructor → dispatch
}
```

### `=>` separator resolves `{` ambiguity

Everything before `=>` is pattern context; `{` there means field destructuring. Everything after
`=>` is expression/statement context; `{` there starts a block. No grammar ambiguity exists.

```lox
match shape {
  case circle{r}  => 3.14159 * r * r   // {r} is a field pattern
  case rect{w, h} => { var a = w * h; a }  // { after => is a block
}
```

### `case _ =>` is the sole wildcard

`default` is not a valid match arm. The only catch-all form is `case _ =>`. `_` is treated as an
identifier whose name is `"_"`; the compiler suppresses the binding — no local is created.

### Match failure raises `MatchError`

A `match` expression with no matching arm and no `case _ =>` raises `MatchError` at runtime. The
error propagates up; it is not catchable within the same `match`.

### First-class patterns — declined

Making patterns passable runtime values would:
- Force structural (non-nominal) matching → kills compile-time exhaustiveness
- Force open enums → exhaustiveness degrades to a lint hint
- Require `Option<Bindings>` as the match result, infecting error handling everywhere
- Require a pattern algebra as a core VM type

These costs are not acceptable. Patterns are syntactic constructs only.

### `not`-patterns and `@`-bindings — deferred

Not in scope for Phases 1–2. If added later: `not` is restricted to non-binding sub-patterns;
`@`-bindings require no other changes.

### Or-patterns — binding consistency is a hard error

All alternatives in an or-pattern must bind the same set of names, or the compiler emits an error.

```lox
case a(y) | b(y) => use(y)    // ok
case a(y) | b(z) => ...       // compile error: a binds y, b binds z
```

### Match-bound variables are mutable

Pattern bindings create implicit `var`s and follow the language default (mutable). No new rules.

### Scope and hygiene of pattern-bound variables

Each arm is compiled in its own `beginScope`/`endScope`. Pattern bindings are locals in that
scope. The guard (`if cond`) runs in the same scope and may reference pattern-bound names. On
guard failure, a `JUMP` skips the body; locals go out of scope normally — no rollback needed.

---

## Phases

### Phase 1 — Match statement (replaces `switch`)

**Grammar:**

```
matchStmt  : 'match' expression '{' matchArm* '}'
matchArm   : 'case' armPats ('if' expression)? '=>' armBody
armPats    : armPat (',' armPat)*
armPat     : NUMBER | STRING | 'true' | 'false' | 'nil'
           | IDENTIFIER      // _ → wildcard (no binding); other → binding (always matches)
armBody    : statement | '{' statement* '}'
```

Binding in multi-pattern arms: if any `armPat` is an `IDENTIFIER` it must be the only pattern;
mixing a binding with literal alternatives is a compile error.

**What changes from `switch`:**

- `switch (expr) { case val: ... }` → `match expr { case pattern => body }`
- No fall-through
- `MatchError` on no-match when no `case _ =>` arm exists
- Guard clauses: `case x if x > 0 => ...`
- Multi-pattern arms: `case 1, 2, 3 => ...`
- Match remains a **statement** in this phase

**New tokens:** `MATCH` (`match`), `FAT_ARROW` (`=>`)  
**New opcode:** `MATCH_ERROR` — raises `MatchError`; VM never returns

**Programs unlocked:**

| Program | What Phase 1 enables |
|---|---|
| Command dispatcher | `match input { case "quit" => ... case _ => ... }` with MatchError safety |
| HTTP status handler | `match status { case 200 => ... case x if x >= 400 => ... }` |
| Token classifier | `case "+", "-" => ...` — cleaner than switch |
| Simple state machine | Match on state string + guard on input range |

---

### Phase 1.5 — Destructuring in `var`

**Grammar addition:**

```
varDecl  : 'var' varLhs '=' expression ';'
varLhs   : IDENTIFIER
         | '{' mapPat '}'
mapPat   : IDENTIFIER (',' IDENTIFIER)*
```

`{` after `var` is unambiguously a destructuring pattern — the parser enters pattern mode and
the map literal rule never fires on the LHS of `var`.

**What is supported:**

```lox
var {x, y} = point          // field name = local name
var {width, height} = rect
```

**What is NOT supported (use two `var` declarations):**

```lox
// nested — not supported:
var {pos: {x, y}} = entity

// sequence — deferred to Phase 3/4:
var [a, b] = list
```

**No new opcodes** — uses existing `GET_PROPERTY`.

**Trailing comma** is allowed: `var {x, y,} = obj` is valid.

**Return-value optimization opportunity:** when the RHS is a call expression
(`var {a, b} = f()`), the returned instance is used only for field extraction
and then discarded. A future optimisation could fuse the call + destructure
into a single pass that reads fields directly without materialising a hidden
source variable, saving one local slot and one `GET_LOCAL` per field. This
requires a new opcode or compiler pass — deferred, not in scope for Phases 1–2.

**Programs unlocked:**

| Program | What Phase 1.5 enables |
|---|---|
| Multi-return via map | `var {quot, rem} = divmod(a, b)` |
| Coordinate unpacking | `var {x, y} = getPosition()` |

---

### Phase 2 — `enum`, structural match, match as expression

**`enum` declaration:**

```lox
enum Result {
  ok(value)
  err(code, message)
}

enum Shape {
  circle(radius)
  rect(width, height)
}

enum Tree {
  leaf(value)
  node(left, right)
}
```

- Constructors are global functions: `ok(42)` → `ObjEnum{tag=0, fields=[42]}`
- Tag is the 0-based index of the constructor in declaration order
- Duplicate constructor name across enums → compile error
- All constructors registered at parse time (exhaustiveness checking)

**Grammar:**

```
enumDecl       : 'enum' IDENTIFIER '{' constructorDecl* '}'
constructorDecl: IDENTIFIER ('(' IDENTIFIER (',' IDENTIFIER)* ')')?

matchExpr   : 'match' expression '{' matchArm+ '}'
matchArm    : 'case' pattern ('if' expression)? '=>' armExpr
armExpr     : expression | '{' statement* expression '}'

pattern     : NUMBER | STRING | 'true' | 'false' | 'nil'
            | IDENTIFIER                                       // constructor or binding (lookup)
            | IDENTIFIER '{' fieldPat (',' fieldPat)* '}'     // named constructor pattern
            | IDENTIFIER '(' IDENTIFIER (',' IDENTIFIER)* ')' // positional constructor pattern
            | pattern '|' pattern                             // or-pattern
fieldPat    : IDENTIFIER
```

**Match as expression:** from Phase 2, every arm body must leave one value on the stack. Used as
a statement, the result is `POP`ped. All arms must produce the same stack height.

```lox
var area = match shape {
  case circle{r}   => 3.14159 * r * r
  case rect{w, h}  => w * h
}
```

**Compile-time exhaustiveness:** when the scrutinee is statically known to be an `enum` type,
the compiler checks that every constructor appears as an arm or a `case _ =>` wildcard exists.
Missing constructors → compile error listing them.

**New tokens:** `ENUM` (`enum`), `PIPE` (`|`)  
**New opcodes:**

| Opcode | Operands | Behaviour |
|---|---|---|
| `BUILD_ENUM` | uint8 `tag`, uint8 `field_count` | Pop N fields → push `ObjEnum{tag, fields[]}` |
| `GET_TAG` | none | Pop `ObjEnum` → push its tag as integer |

**New runtime type:**

```cpp
struct ObjEnum : Obj {
    uint8_t tag;
    uint8_t fieldCount;
    Value fields[];   // flexible array member
};
```

**Dispatch compilation:**

```
GET_LOCAL <subject>
GET_TAG
CONSTANT <tag_idx>
EQUAL
JUMP_IF_FALSE → next_arm
POP
[bind fields via GET_PROPERTY or GET_INDEX]
[guard if any]
[arm body]
JUMP → match_end
...
MATCH_ERROR     // if no wildcard arm
```

**Decision tree:** currently sequential tag comparison (correct, O(n) in arms). A `JUMP_TABLE`
optimisation is future work.

**Programs unlocked:**

| Program | What Phase 2 enables |
|---|---|
| Result-based file I/O | `match open(...) { case ok{f} => ... case err{c} => ... }` |
| Option type | `enum option { some(value) none }` — replaces nil sentinel |
| Expression tree evaluator | `enum expr { num(n) add(l,r) mul(l,r) }` — recursive match |
| Binary tree algorithms | Insert, search, height on `enum tree { leaf(v) node(l,r) }` |
| Typed tokenizer | `enum token { number(n) ident(name) plus minus }` |
| State machine | `enum state { idle running(task) error(code) }` — exhaustive transitions |

---

## Dependency chain

```
Phase 1: match statement + MatchError
  ↓
Phase 1.5: var {x, y} destructuring
  ↓
Phase 2: enum + structural match + match-as-expression
  ↓
Phase 3/4 (future): var [a, b], list patterns in match, sequence ops
```

---

## Out of scope (Phases 1–2)

| Feature | Status |
|---|---|
| Rename form `{field: newName}` | Dropped entirely |
| Nested `var` destructuring | Not supported; chain two `var` declarations |
| Sequence destructuring (`var [a, b]`, `case []`, `case [x, ...rest]`) | Phase 3/4 |
| `@`-bindings | Deferred |
| `not`-patterns | Deferred |
| First-class patterns | Declined |
| `JUMP_TABLE` for variant dispatch | Future optimisation |
| Generics (`Result<T>`) | Not planned |
| Namespace for constructors | Not planned |
