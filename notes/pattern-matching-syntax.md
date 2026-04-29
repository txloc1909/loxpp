# Pattern Matching — Syntax Specification

Decisions locked from design conversation:
- Separator: `=>`
- Wildcard arm: `default =>` (no `case` prefix; mirrors `switch`'s `default:`)
- No rename form (`{x: newName}`) anywhere — dropped entirely
- Sequence destructuring (`var [a, b]`, `var a, b`, list patterns in match) — deferred to Phase 3/4

---

## New tokens

| Token | Spelling | Scanner change |
|---|---|---|
| `MATCH` | `match` | Add to `identifierType()` under `'m'` |
| `VARIANT` | `variant` | Add under `'v'`; share `va` prefix with `var` via full-length check |
| `FAT_ARROW` | `=>` | In `'='` branch: `match('>')` before `match('=')` |
| `PIPE` | `\|` | New single-char token (Phase 2 or-patterns only) |

`_` stays `IDENTIFIER`; the parser checks `lexeme == "_"` in pattern context.  
`case` and `default` are already keywords.  
`ELLIPSIS` (`...`) is deferred to Phase 3/4 with sequence destructuring.

---

## New opcodes

Minimum set. Everything else reuses the existing ISA.

| Opcode | Phase | Operands | Behaviour |
|---|---|---|---|
| `MATCH_ERROR` | 1 | none | Raise `MatchError`; VM never returns |
| `BUILD_VARIANT` | 2 | uint8 `tag`, uint8 `field_count` | Pop N fields → push `ObjVariant{tag, fields}` |
| `GET_TAG` | 2 | none | Pop variant → push its constructor tag (integer) |

Field access inside variant patterns reuses `GET_INDEX` (positional form `Constructor(a, b)`)
and `GET_PROPERTY` (named form `Constructor{field}`). No additional field opcode needed.

`SLICE` is deferred to Phase 3/4 with sequence destructuring.

---

## Phase 1 — Match statement

### Grammar

```
matchStmt   : 'match' expression '{' matchArm* defaultArm? '}'
matchArm    : 'case' armPats ('if' expression)? '=>' armBody
defaultArm  : 'default' '=>' armBody
armPats     : armPat (',' armPat)*
armPat      : NUMBER | STRING | 'true' | 'false' | 'nil'
            | IDENTIFIER                     // variable binding — always matches
armBody     : statement
            | '{' statement* '}'
```

**Binding in multi-pattern arms:** if any `armPat` is an `IDENTIFIER` (binding), it must
be the sole pattern — no comma. A binding always matches, so mixing with literal
alternatives is a compile error.

```lox
case 1, 2 => ...          // ok
case x if x > 0 => ...    // ok: single binding + guard
case 1, x => ...          // compile error
```

### Compilation

Extends `switchStatement()` directly. Same `EQUAL + JUMP_IF_FALSE + JUMP` chain; subject
stored in a hidden local (same trick switch already uses at `compiler.cpp:558`).

- Guard: emit `JUMP_IF_FALSE` after binding, before arm body
- No `default =>`: emit `MATCH_ERROR` at the end of the arm chain
- `MATCH_ERROR` replaces the current silent fall-through behaviour of `switch`

---

## Phase 1.5 — Destructuring in `var`

### Grammar

```
varDecl   : 'var' varLhs '=' expression ';'
varLhs    : IDENTIFIER             // existing scalar form (unchanged)
          | '{' mapPat '}'         // named field destructuring

mapPat    : mapField (',' mapField)*
mapField  : IDENTIFIER             // field name = local name; no rename
```

`'{'` after `var` is always a destructuring pattern (the parser enters pattern mode
before calling `parsePrecedence`; `mapLiteral`'s prefix rule is never reached from
the LHS of `var`).

Sequence forms (`var [a, b]`, `var a, b`) and rest binding (`...`) are deferred to
Phase 3/4.

### Compilation

Subject stored in a hidden local so `GET_PROPERTY` can be re-issued without
re-evaluating the expression (same hidden-local trick switch uses at `compiler.cpp:558`).

| Pattern | Emitted sequence |
|---|---|
| `var {x, y} = e` | eval `e` → hidden local → `GET_LOCAL`, `GET_PROPERTY "x"` → define `x` → `GET_LOCAL`, `GET_PROPERTY "y"` → define `y` → pop hidden |

No new opcodes required for Phase 1.5. `GET_PROPERTY` already exists.

---

## Phase 2 — `variant`, structural match, match as expression

### `variant` declaration

```
variantDecl    : 'variant' IDENTIFIER '{' constructorDecl* '}'
constructorDecl : UPPER_IDENT ('(' IDENTIFIER (',' IDENTIFIER)* ')')?
```

`UPPER_IDENT` is an `IDENTIFIER` whose first character is uppercase — checked by the
parser (`isupper(lexeme[0])`), not the scanner. No separator between constructors:
after `)` or a bare constructor name, the next token is either another `UPPER_IDENT`
or `}`, which is unambiguous.

Constructors are injected into the global namespace at the `variant` declaration site.
Duplicate constructor names across variants are a compile error.

### Match (Phase 2)

From Phase 2, `match` is always an expression. Used as a statement, the value is
discarded (`POP`). Every arm body must leave exactly one value on the stack.

```
matchExpr   : 'match' expression '{' matchArm+ defaultArm? '}'
matchArm    : 'case' pattern ('if' expression)? '=>' armExpr
defaultArm  : 'default' '=>' armExpr
armExpr     : expression
            | '{' statement* expression '}'   // block-expr; last expression is value

pattern     : NUMBER | STRING | 'true' | 'false' | 'nil'
            | IDENTIFIER                                            // variable binding
            | UPPER_IDENT ('{' fieldPat (',' fieldPat)* '}')?      // named constructor
            | UPPER_IDENT '(' IDENTIFIER (',' IDENTIFIER)* ')'     // positional constructor
            | pattern '|' pattern                                   // or-pattern

fieldPat    : IDENTIFIER     // field "x" → local x; no rename
```

### Disambiguation rules

**Uppercase vs lowercase in pattern position:**
- `UPPER_IDENT` → constructor dispatch; must be a declared variant constructor (compile error if not)
- lowercase `IDENTIFIER` → variable binding; always matches
- `_` → wildcard; always matches, no binding introduced

**`{` after `=>` is always a block-expression**, never a map literal. To produce a map
from an arm body, parenthesise it: `case X => ({"k": v})`.

**`PIPE` (`|`) is only valid between two patterns.** It cannot appear in expression
context; `parsePrecedence` has no rule for it.

### Or-patterns

All alternatives must bind the same set of names. Mismatch is a compile error.

```lox
case A(y) | B(y) => use(y)    // ok
case A(y) | B(z) => ...       // compile error: A binds y, B binds z
case A(y) | C    => ...       // compile error: A binds y, C binds nothing
```

### Pattern binding scope

Each arm body is compiled in its own scope (`beginScope` / `endScope`). Pattern
bindings are locals declared at the start of that scope. The guard (`if cond`)
executes inside the same scope, so it may reference pattern-bound variables.
On guard failure, a `JUMP` skips the arm body; all locals go out of scope normally.

### Compilation

**`BUILD_VARIANT`:** `Ok(42)` → push `42`, emit `BUILD_VARIANT tag=0 fields=1`.
Tag is the constructor's index in declaration order (0-based).

**Dispatch in match:** `GET_LOCAL subject`, `GET_TAG` → integer on stack.  
Per arm: `CONSTANT tag_idx`, `EQUAL`, `JUMP_IF_FALSE` to next arm, `POP`, [bind fields], [body], `JUMP` to end.  
After all arms with no `default`: `MATCH_ERROR`.

**Named field binding** (`Constructor{field}`): `GET_LOCAL subject`, `GET_PROPERTY "field"` → define local `field`.  
**Positional field binding** (`Constructor(v)`): `GET_LOCAL subject`, `GET_INDEX 0` → define local `v`.  
**No-field constructor** (`case Zero`): tag check only, no field extraction.

**Or-patterns:** compile each alternative as a separate tag check; both branch to the
same field-binding and body code. The binding code is emitted once (fields are
identical by the binding-consistency rule).

---

## What is not in scope

- Rename form `{field: newName}` — dropped; use positional `Constructor(v)` for a different local name
- Sequence destructuring: `var [a, b]`, `var a, b`, list patterns in match (`case []`, `case [x, ...rest]`), rest binding, `SLICE` opcode, `ELLIPSIS` token — all Phase 3/4
- Nested `var` destructuring beyond one level
- `@`-bindings
- `not`-patterns
- First-class patterns
- `JUMP_TABLE` optimisation for variant dispatch (sequential tag comparison is correct; table is a future opt)
