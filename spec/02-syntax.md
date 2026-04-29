# Lox++ Syntax Grammar

## Complete EBNF Grammar

```
program        ::= declaration* EOF ;

(* Declarations *)
declaration    ::= classDecl
                 | funDecl
                 | varDecl
                 | statement ;

classDecl      ::= "class" IDENTIFIER ( "<" IDENTIFIER )? "{" method* "}" ;
method         ::= IDENTIFIER "(" parameters? ")" block ;

funDecl        ::= "fun" IDENTIFIER "(" parameters? ")" block ;
parameters     ::= IDENTIFIER ( "," IDENTIFIER )* ;

varDecl        ::= "var" IDENTIFIER ( "=" expression )? ";" ;

(* Statements *)
statement      ::= exprStmt
                 | forStmt
                 | ifStmt
                 | printStmt
                 | returnStmt
                 | whileStmt
                 | breakStmt
                 | continueStmt
                 | matchStmt
                 | block ;

exprStmt       ::= expression ";" ;

forStmt        ::= "for" "(" ( varDecl | exprStmt | ";" )
                              expression? ";"
                              expression? ")"
                   statement                                  (* C-style *)
                 | "for" "(" "var" IDENTIFIER "in" expression ")"
                   statement ;                               (* for-in *)

ifStmt         ::= "if" "(" expression ")" statement
                   ( "else" statement )? ;

printStmt      ::= "print" expression ";" ;

returnStmt     ::= "return" expression? ";" ;

whileStmt      ::= "while" "(" expression ")" statement ;

breakStmt      ::= "break" ";" ;

continueStmt   ::= "continue" ";" ;

matchStmt      ::= "match" expression "{" matchArm* "}" ;
matchArm       ::= "case" armPats ( "if" expression )? "=>" armBody ;
armPats        ::= armPat ( "," armPat )* ;
armPat         ::= NUMBER | STRING | "true" | "false" | "nil"
                 | IDENTIFIER ;          (* IDENTIFIER: "_" = wildcard; other = binding *)
armBody        ::= statement | "{" declaration* "}" ;

block          ::= "{" declaration* "}" ;

(* Expressions — ordered from lowest to highest precedence *)
expression     ::= assignment ;

assignment     ::= ( call "." IDENTIFIER | call "[" expression "]" | IDENTIFIER ) "=" assignment
                 | logicOr ;

logicOr        ::= logicAnd ( "or" logicAnd )* ;

logicAnd       ::= equality ( "and" equality )* ;

equality       ::= comparison ( ( "!=" | "==" ) comparison )* ;

comparison     ::= term ( ( ">" | ">=" | "<" | "<=" | "in" ) term )* ;

term           ::= factor ( ( "-" | "+" ) factor )* ;

factor         ::= unary ( ( "/" | "*" | "%" ) unary )* ;

unary          ::= ( "!" | "-" ) unary
                 | call ;

call           ::= primary ( "(" arguments? ")" | "." IDENTIFIER | "[" expression "]" )* ;

arguments      ::= expression ( "," expression )* ;

primary        ::= "true"
                 | "false"
                 | "nil"
                 | NUMBER
                 | STRING
                 | IDENTIFIER
                 | "this"
                 | "super" "." IDENTIFIER
                 | "(" expression ")"
                 | "[" ( expression ( "," expression )* )? "]"
                 | "{" ( mapEntry ( "," mapEntry )* )? "}" ;

mapEntry       ::= expression ":" expression ;
```

---

## Operator Precedence

The table below lists operator groups from **lowest** to **highest** precedence.
Operators within the same group are left-associative unless noted otherwise.

| Precedence | Operators | Associativity |
|---|---|---|
| 1 (lowest) | `=` (assignment) | Right |
| 2 | `or` | Left |
| 3 | `and` | Left |
| 4 | `==` `!=` | Left |
| 5 | `<` `<=` `>` `>=` `in` | Left |
| 6 | `+` `-` | Left |
| 7 | `*` `/` `%` | Left |
| 8 | `!` `-` (unary) | Right (prefix) |
| 9 (highest) | `()` (function call), `.` (property access), `[]` (index) | Left |

---

## Notes on Specific Constructs

### `for` loop (C-style)

The three clauses of a `for` statement are each optional:

- **Initializer**: either a `var` declaration (scoped to the loop), an
  expression statement, or omitted (bare `;`).
- **Condition**: an expression evaluated before each iteration; omitted means
  always-truthy.
- **Increment**: an expression evaluated after each iteration body; omitted
  means nothing.

### `for`-in loop

`for (var x in seq) body` iterates over the elements of `seq` in order,
binding each element to `x` for each iteration of `body`.

- `seq` is evaluated once before the loop begins and stored internally.
- `x` is scoped to the loop; it is not accessible after the loop exits.
- `break` and `continue` work as in C-style `for` loops.
- The sequence expression may be any List, String, or Map value. Iterating a
  List visits elements in order; iterating a String visits single-character
  strings in order; iterating a Map visits keys in unspecified order.
- Modifying a list during iteration is defined: elements appended before the
  current index is reached will be visited.

### `in` expression

`elem in seq` is a binary expression that evaluates to a Boolean:

- If `seq` is a **List**: `true` if any element compares equal to `elem` under
  the `==` operator.
- If `seq` is a **String**: `elem` must be a String; `true` if `elem` appears
  as a substring of `seq`.
- If `seq` is a **Map**: `true` if `elem` is a valid map key and is present in
  the map. Equivalent to `seq.has(elem)`.
- Runtime error if `seq` is neither a List, String, nor Map.
- Runtime error if `seq` is a String and `elem` is not a String.
- Runtime error if `seq` is a Map and `elem` is an invalid map key type (NaN,
  or an object other than String).

`in` has comparison precedence (same level as `<`, `<=`, `>`, `>=`), so
`a + b in c` parses as `(a + b) in c`.

### `if`/`else` dangling

The `else` clause binds to the nearest preceding `if`. To associate an `else`
with an outer `if`, enclose the inner `if` in a block.

### Assignment

Assignment is an expression, not a statement. The left-hand side may be a
bare `IDENTIFIER` (variable set), a `call "." IDENTIFIER` (property set), or a
`call "[" expression "]"` (index set); any other form is a parse error. The
value of an assignment expression is the assigned value.

### `match` statement

A `match` body consists of one or more `case` arms. Each arm has a pattern,
an optional guard clause (`if expr`), the `=>` separator, and a body (either a
single statement or a braced block of declarations).

**Patterns:**
- `NUMBER`, `STRING`, `true`, `false`, `nil` — equality test against the subject.
- `IDENTIFIER` other than `_` — variable binding: always matches, introduces a
  new local bound to the subject value, visible in the guard and the body.
- `_` (the identifier `_`) — wildcard: always matches, no binding introduced.
- Multiple comma-separated literals: `case 1, 2, 3 =>` — matches if the subject
  equals any of them. A binding identifier must be the sole pattern in its arm.

**Guard clauses:** `case x if x > 0 =>` — after the pattern matches, the guard
expression is evaluated; if falsy the arm is skipped.

**No fall-through:** each arm executes its body and then jumps past the match.

**Wildcard:** `case _ =>` is the canonical catch-all. An unguarded `_` (or any
unguarded binding) must come last; arms after it are unreachable and are a
compile error.

**`MatchError`:** if no arm matches and no unguarded catch-all arm exists, the
VM raises `MatchError` at runtime.

**`break`/`continue`:** `break` exits the match; `continue` targets the nearest
enclosing loop (not the match itself). `continue` with no enclosing loop is a
compile error.

### Function call arguments

A call may pass up to 255 arguments. The maximum number of parameters a
function may declare is also 255.
