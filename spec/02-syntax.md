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
                 | switchStmt
                 | block ;

exprStmt       ::= expression ";" ;

forStmt        ::= "for" "(" ( varDecl | exprStmt | ";" )
                              expression? ";"
                              expression? ")"
                   statement ;

ifStmt         ::= "if" "(" expression ")" statement
                   ( "else" statement )? ;

printStmt      ::= "print" expression ";" ;

returnStmt     ::= "return" expression? ";" ;

whileStmt      ::= "while" "(" expression ")" statement ;

breakStmt      ::= "break" ";" ;

continueStmt   ::= "continue" ";" ;

switchStmt     ::= "switch" "(" expression ")" "{" caseArm* defaultArm? "}" ;
caseArm        ::= "case" expression ( "," expression )* ":" statement* ;
defaultArm     ::= "default" ":" statement* ;

block          ::= "{" declaration* "}" ;

(* Expressions — ordered from lowest to highest precedence *)
expression     ::= assignment ;

assignment     ::= ( call "." IDENTIFIER | call "[" expression "]" | IDENTIFIER ) "=" assignment
                 | logicOr ;

logicOr        ::= logicAnd ( "or" logicAnd )* ;

logicAnd       ::= equality ( "and" equality )* ;

equality       ::= comparison ( ( "!=" | "==" ) comparison )* ;

comparison     ::= term ( ( ">" | ">=" | "<" | "<=" ) term )* ;

term           ::= factor ( ( "-" | "+" ) factor )* ;

factor         ::= unary ( ( "/" | "*" ) unary )* ;

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
                 | "[" ( expression ( "," expression )* )? "]" ;
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
| 5 | `<` `<=` `>` `>=` | Left |
| 6 | `+` `-` | Left |
| 7 | `*` `/` | Left |
| 8 | `!` `-` (unary) | Right (prefix) |
| 9 (highest) | `()` (function call), `.` (property access), `[]` (index) | Left |

---

## Notes on Specific Constructs

### `for` loop

The three clauses of a `for` statement are each optional:

- **Initializer**: either a `var` declaration (scoped to the loop), an
  expression statement, or omitted (bare `;`).
- **Condition**: an expression evaluated before each iteration; omitted means
  always-truthy.
- **Increment**: an expression evaluated after each iteration body; omitted
  means nothing.

### `if`/`else` dangling

The `else` clause binds to the nearest preceding `if`. To associate an `else`
with an outer `if`, enclose the inner `if` in a block.

### Assignment

Assignment is an expression, not a statement. The left-hand side may be a
bare `IDENTIFIER` (variable set), a `call "." IDENTIFIER` (property set), or a
`call "[" expression "]"` (index set); any other form is a parse error. The
value of an assignment expression is the assigned value.

### `switch` statement

A `switch` body consists of zero or more `case` arms followed by an optional
`default` arm. The `default` arm, if present, must be the final arm — a
`case` arm after `default` is a **static error**. At most one `default` arm
may appear. Only `case` and `default` labels may appear at the top level of a
switch body — bare statements outside an arm are a parse error.

### Function call arguments

A call may pass up to 255 arguments. The maximum number of parameters a
function may declare is also 255.
