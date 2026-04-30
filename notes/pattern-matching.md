# Pattern Matching Design

## Context

Lox++ had `switch/case/default` (PR #41) that dispatched on equality of primitive values.
It has been replaced by nominal pattern matching with three goals:

1. Type-check and field extraction happen in one operation (not two separate if-else chains)
2. Match failure is explicit and loud (`MatchError`) rather than silently falling through
3. An `enum` declaration closes a type and enables compile-time exhaustiveness

---

## Design Decisions

### Nominal, not structural

A constructor match checks the **runtime tag** stored in the value, not the presence or shape of
fields. An object that has a `radius` field but was not constructed with `Circle(...)` does not
match `case Circle{r}`. This enables O(1) tag dispatch and makes compile-time exhaustiveness
possible.

### Closed `enum`, runtime fallback for open types

`enum` declarations are closed: the compiler records all constructors at parse time and checks
exhaustiveness at the `match` site. Open class hierarchies are not made into enums. Matching on
an open class gets only the runtime guarantee — if no arm matches and there is no `case _ =>`,
the VM raises `MatchError`.

### Bare identifier always binds; use guard to test existing values

In pattern position, a bare identifier always creates a new local binding (and always matches),
unless it resolves to a known constructor (see below). To test against an existing variable, use
a guard clause.

```lox
var north = 0
match direction {
  case d if d == north => ...  // correct: test against north
  case north => ...            // wrong intent: binds a new 'north', always matches
}
```

### Pattern disambiguation — symbol lookup

No naming convention is required by the compiler, but **uppercase names are encouraged** for both
enum constructors (`Ok`, `Err`) and class names (`Circle`, `Point`) to visually distinguish them
from variable bindings.

In pattern position, the compiler resolves a name in this priority order:

1. Name is a known enum constructor → **enum pattern** (tag check + positional/named field binding)
2. Name is a known class → **class pattern** (`instanceof` check + named field binding)
3. Otherwise → **variable binding** (always matches)

Wildcard `_` is always a non-binding wildcard regardless of resolution. No constructor or class
may be named `_`.

```lox
enum Result { Ok(value) Err(code) }
class Point { init(x, y) { this.x = x; this.y = y; } }

match r {
  case Ok{v}      => print v     // known constructor → enum pattern
  case Err{c}     => print c     // known constructor → enum pattern
}

match p {
  case Point{x, y} => print x   // known class → class pattern
  case other       => print "?" // unknown name → binding
}
```

### `=>` separator resolves `{` ambiguity

Everything before `=>` is pattern context; `{` there means field destructuring. Everything after
`=>` is expression/statement context; `{` there starts a block. No grammar ambiguity exists.

```lox
match shape {
  case Circle{r}  => 3.14159 * r * r       // {r} is a field pattern
  case Rect{w, h} => { var a = w * h; a }  // { after => is a block
}
```

### `case _ =>` is the sole wildcard

`default` is not a valid match arm. The only catch-all form is `case _ =>`. `_` is treated as an
identifier whose name is `"_"`; the compiler suppresses the binding — no local is created.

### Match failure raises `MatchError`

A `match` with no matching arm and no `case _ =>` raises `MatchError` at runtime. The error
propagates up; it is not catchable within the same `match`.

### First-class patterns — declined

Making patterns passable runtime values would:
- Force structural (non-nominal) matching → kills compile-time exhaustiveness
- Force open enums → exhaustiveness degrades to a lint hint
- Require `Option<Bindings>` as the match result, infecting error handling everywhere
- Require a pattern algebra as a core VM type

These costs are not acceptable. Patterns are syntactic constructs only.

### `not`-patterns and `@`-bindings — deferred to Phase 4

Not in scope for Phases 1–3. If added later: `not` is restricted to non-binding sub-patterns;
`@`-bindings require no other changes.

### Or-patterns — binding consistency is a hard error

All alternatives in an or-pattern must bind the same set of names, or the compiler emits an error.

```lox
case A(y) | B(y) => use(y)    // ok — both bind y
case A(y) | B(z) => ...       // compile error: A binds y, B binds z
```

### Match-bound variables are mutable

Pattern bindings create implicit `var`s and follow the language default (mutable). No new rules.

### Scope and hygiene of pattern-bound variables

Each arm is compiled in its own `beginScope`/`endScope`. Pattern bindings are locals in that
scope. The guard (`if cond`) runs in the same scope and may reference pattern-bound names. On
guard failure, a `JUMP` skips the body; locals go out of scope normally — no rollback needed.

---

## Phases

### Phase 1 — Match statement ✅ (PR #61)

Replaced `switch/case/default` with a `match` statement.

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

**What changed from `switch`:**

- `switch (expr) { case val: ... }` → `match expr { case pattern => body }`
- No fall-through
- `MatchError` on no-match when no `case _ =>` arm exists
- Guard clauses: `case x if x > 0 => ...`
- Multi-pattern arms: `case 1, 2, 3 => ...`
- Match is a **statement**

**New tokens:** `MATCH` (`match`), `FAT_ARROW` (`=>`)  
**New opcode:** `MATCH_ERROR` — raises `MatchError`; VM never returns from this instruction

---

### Phase 1.5 — Destructuring in `var` ✅ (PR #62)

Named-field destructuring into `var` declarations.

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

**No new opcodes** — uses existing `GET_PROPERTY`. Trailing comma is allowed.

**Return-value optimization opportunity:** when the RHS is a call expression
(`var {a, b} = f()`), the returned instance is used only for field extraction
and then discarded. A future optimisation could fuse the call + destructure into
a single pass that reads fields directly without materialising a hidden source
variable, saving one local slot and one `GET_LOCAL` per field. Requires a new
opcode or compiler pass — deferred.

---

### Phase 2 — `enum` declaration + constructor pattern match ✅ (PR #64)

Closed algebraic types with compile-time exhaustiveness checking.

**`enum` declaration (global scope only):**

```lox
enum Result { Ok(value) Err(code, message) }
enum Option { Some(v) None }
enum Tree   { Leaf(val) Node(left, right) }
```

- Each constructor becomes a globally-scoped callable (`ObjEnumCtor`)
- Calling it with the declared arity creates an `ObjEnum` value
- Constructor tag = 0-based index in declaration order
- Duplicate constructor name → compile error
- `enum` at non-global scope → compile error

**Constructor pattern match:**

```lox
match r {
  case Ok(v)       => print v            // positional binding
  case Err{code}   => print code         // named field binding
}

match opt {
  case Some(v) => print v
  case None    => print "nothing"        // zero-field constructor
}

match r {
  case Ok(v) if v > 0 => print "positive"  // guard on constructor arm
  case Ok(v)          => print "non-positive"
  case Err(m)         => print m
}
```

**Compile-time exhaustiveness:** when a match contains at least one constructor arm from enum `E`,
the compiler verifies all constructors of `E` appear as arms, or an unguarded `_` wildcard is
present. Missing constructors → compile error.

**New token:** `ENUM` (`enum`)  
**New opcode:** `GET_TAG` — pops `ObjEnum`, pushes its tag as a Number  
**New runtime types:** `ObjEnumCtor` (callable), `ObjEnum` (value with `VmVector<Value> fields`)  
**Constructor calls:** dispatched through the existing `Op::CALL` path — no `BUILD_ENUM` opcode needed  
**Stringify:** `ObjEnum` renders as `EnumName::CtorName(f1, f2)` (or bare `EnumName::CtorName` for zero-field)

---

### Phase 2.5 — Class patterns (planned)

Named-field pattern matching on open class instances, using `instanceof` dispatch at runtime.

**Syntax** — identical to enum named-field patterns, no new tokens:

```lox
class Point  { init(x, y)    { this.x = x; this.y = y; } }
class Circle { init(center, radius) { this.center = center; this.radius = radius; } }
class Rect   { init(tl, br)  { this.tl = tl; this.br = br; } }

match shape {
  case Circle{center, radius} => print radius;
  case Rect{tl, br}           => print tl.x;
  case _                      => print "unknown";
}
```

**Disambiguation:** in pattern position, if the name resolves to a known class (registered in the
compiler's class table at the point of the `match`), the arm compiles to an `instanceof` check
followed by `GET_PROPERTY` for each bound field. This is the same grammar production as enum
named-field patterns — the compiler decides which path to take based on symbol lookup alone.

**No exhaustiveness** — open class hierarchies are never fully enumerable. A `case _` arm is
always required, or `MatchError` is the runtime fallback. This is consistent with the existing
behavior for unguarded open-type matches.

**Composing with enum patterns** — class patterns and enum patterns can appear in the same match
when the subject is known to be one or the other, but not mixed across the same value:

```lox
enum Result { Ok(value) Err(msg) }
class NetworkError { init(code, msg) { this.code = code; this.msg = msg; } }
class ParseError   { init(line, msg) { this.line = line; this.msg = msg; } }

match r {
  case Ok(v) => match v {                       // outer: enum (exhaustive)
    case NetworkError{code, msg} => print code; // inner: class (open)
    case ParseError{line, msg}   => print line;
    case _                       => print "?";
  }
  case Err(m) => print m;
}
```

**New opcode:** `INSTANCEOF` — pops a value, reads a class constant operand, pushes `true` if the
value is an `ObjInstance` of that class (exact match or subclass), `false` otherwise. Used in
place of `GET_TAG + EQUAL` for class pattern arms.

**Compiler change:** rather than maintaining a separate class table alongside the existing
constructor table, both are unified into a single **pattern symbol table**:

```cpp
enum class PatternSymbolKind { EnumCtor, Class };

struct PatternSymbol {
    PatternSymbolKind kind;
    union {
        const ConstructorInfo* ctor;  // kind == EnumCtor
        ObjClass* klass;              // kind == Class
    };
};

std::unordered_map<std::string, PatternSymbol> m_patternSymbols;
```

`enum` declaration registers each constructor as `EnumCtor`; `class` declaration registers the
class name as `Class`. Pattern resolution becomes one lookup, one switch — no parallel map walks.
The `m_enumCtors` table (enum name → constructor list) stays separate: it is only consulted for
exhaustiveness checking after all arms are compiled, a different access pattern.

The current `m_constructors` per-root-compiler design carries an O(scope depth) chain walk on
every pattern lookup. Phase 2.5 is the right moment to fix this: every `Compiler` instance
receives a `PatternSymbol` table pointer at construction time pointing at the root's map. All
registrations write through that pointer; all lookups are a direct dereference — O(1) regardless
of nesting depth. `findRootCompiler()` is eliminated entirely.

**Out of scope for this phase:**
- Positional class patterns `case Point(x, y)` — classes have no declared field order; named-only
- Or-patterns mixing class arms `case Circle{r} | Rect{w, h}` — deferred to Phase 3
- Exhaustiveness for `sealed` classes — not planned; use `enum` for closed hierarchies

---

### Phase 3 — Or-patterns + match as expression (planned)

**Or-patterns** allow a single arm to match multiple constructors that bind the same names:

```lox
match msg {
  case Quit or Pause => stop()
  case Move(x) or Teleport(x) => go(x)   // both alternatives must bind x
}
```

**Match as expression** — every arm body produces a value on the stack:

```lox
var area = match shape {
  case Circle{r}  => 3.14159 * r * r
  case Rect{w, h} => w * h
}
```

All arms must leave the same stack height. Used as a statement the result is `POP`ped.

**`var [a, b]` sequence destructuring:**

```lox
var [head, tail] = list    // positional destructure into a list
```

New opcode: `GET_INDEX` already exists; may need a length guard.

---

### Phase 4 — List patterns in match + advanced patterns (future)

**List patterns in `match`:**

```lox
match lst {
  case []          => print "empty"
  case [x]         => print "singleton"
  case [x, ...rest] => print x
}
```

**`@`-bindings** — bind a whole matched value while also inspecting its structure:

```lox
case node @ Node(l, r) => process(node, l, r)
```

**`not`-patterns** — match anything that does not match a sub-pattern (non-binding only):

```lox
case not None => ...
```

---

### Phase 5 — `JUMP_TABLE` optimization (future)

Replace the sequential tag-comparison chain emitted for `enum` match arms with a single jump
table dispatch when the match covers a dense range of tags. No semantic change — purely a
code-generation optimization.

New opcode: `JUMP_TABLE` — pops a tag integer, indexes into an inline jump offset table, and
branches to the matching arm. Falls back to the existing sequential chain when the tag range is
sparse.

---

## Dependency chain

```
Phase 1:   match statement + MatchError              ✅ PR #61
  ↓
Phase 1.5: var {x, y} destructuring                  ✅ PR #62
  ↓
Phase 2:   enum + constructor pattern + exhaustiveness ✅ PR #64
  ↓
Phase 2.5: class patterns (instanceof dispatch, named-field binding)
  ↓
Phase 3:   or-patterns, match-as-expression, var [a,b]
  ↓
Phase 4:   list patterns, @-bindings, not-patterns
  ↓
Phase 5:   JUMP_TABLE optimization
```

---

## Out of scope (permanently)

| Feature | Reason |
|---|---|
| Rename form `{field: newName}` | Dropped — use positional `Ctor(v)` for aliasing |
| Nested `var` destructuring | Not supported; chain two `var` declarations |
| First-class patterns | Declined — kills exhaustiveness and requires pattern algebra |
| Generics (`Result<T>`) | Not planned |
| Namespace for constructors (`Result::Ok`) | Not planned — global ctor names encouraged to be unique |
