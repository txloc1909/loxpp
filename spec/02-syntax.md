# Lox++ Syntax Grammar

## Complete EBNF Grammar

```
program        ::= declaration* EOF ;

(* Declarations *)
declaration    ::= classDecl
                 | funDecl
                 | varDecl
                 | enumDecl
                 | statement ;

classDecl      ::= "class" IDENTIFIER ( "<" IDENTIFIER )? "{" method* "}" ;
method         ::= IDENTIFIER "(" parameters? ")" block ;

funDecl        ::= "fun" IDENTIFIER "(" parameters? ")" block ;
parameters     ::= IDENTIFIER ( "," IDENTIFIER )* ;

varDecl        ::= "var" IDENTIFIER ( "=" expression )? ";"
               | "var" "{" IDENTIFIER ( "," IDENTIFIER )* ","? "}" "=" expression ";"
               | "var" "[" IDENTIFIER ( "," IDENTIFIER )* ","? "]" "=" expression ";" ;

enumDecl       ::= "enum" IDENTIFIER "{" enumCtor* "}" ;
enumCtor       ::= IDENTIFIER ( "(" IDENTIFIER ( "," IDENTIFIER )* ")" )? ;

(* Statements *)
statement      ::= exprStmt
                 | forStmt
                 | ifStmt
                 | printStmt
                 | returnStmt
                 | whileStmt
                 | breakStmt
                 | continueStmt
                 | tryStmt
                 | throwStmt
                 | deferStmt
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

tryStmt        ::= "try" block "catch" "(" IDENTIFIER ")" block ;

throwStmt      ::= "throw" expression ";" ;

deferStmt      ::= "defer" call "(" arguments? ")" ";" ;
                   (* call permits method calls and subscript access; the grammar
                      requires one more () pair, so defer requires the full
                      syntactic form of a function or method call, not a bare
                      identifier or property access. *)

block          ::= "{" declaration* "}" ;

(* Expressions — ordered from lowest to highest precedence *)
expression     ::= assignment ;

assignment     ::= ( call "." IDENTIFIER | call "[" expression "]" | IDENTIFIER ) "=" assignment
                 | yieldExpr ;

yieldExpr      ::= "yield" expression?
                 | logicOr ;

logicOr        ::= logicAnd ( "or" logicAnd )* ;

logicAnd       ::= equality ( "and" equality )* ;

equality       ::= comparison ( ( "!=" | "==" ) comparison )* ;

comparison     ::= bitOr ( ( ">" | ">=" | "<" | "<=" | "in" ) bitOr )* ;

bitOr          ::= bitXor ( "|" bitXor )* ;

bitXor         ::= bitAnd ( "^" bitAnd )* ;

bitAnd         ::= shift ( "&" shift )* ;

shift          ::= term ( ( "<<" | ">>" | ">>>" ) term )* ;

term           ::= factor ( ( "-" | "+" ) factor )* ;

factor         ::= unary ( ( "/" | "*" | "%" ) unary )* ;

unary          ::= ( "!" | "-" | "~" ) unary
                 | "len" "(" expression ")"
                 | "str" "(" expression ")"
                 | call ;

call             ::= primary ( "(" arguments? ")" | "." IDENTIFIER | "[" subscriptOrSlice "]" )* ;

subscriptOrSlice ::= expression                       (* plain index *)
                   | expression ":" expression ;      (* slice — both bounds required *)

arguments      ::= expression ( "," expression )* ;

primary        ::= "true"
                 | "false"
                 | "nil"
                 | NUMBER
                 | INT
                 | STRING
                 | IDENTIFIER
                 | "this"
                 | "super" "." IDENTIFIER
                 | "(" expression ")"
                 | "[" ( expression ( "," expression )* )? "]"
                 | "{" ( mapEntry ( "," mapEntry )* )? "}"
                 | "match" expression "{" matchArm* "}" ;

matchArm       ::= "case" armPats ( "if" expression )? "=>" armBody ;

armPats        ::= armPat ( "or" armPat )* ;           (* alternatives *)

armPat         ::= literalPat
                 | IDENTIFIER "@" subPat                               (* @-binding: bind whole value, then inspect structure *)
                 | ctorPat
                 | IDENTIFIER "{" IDENTIFIER ( "," IDENTIFIER )* "}"   (* class pattern *)
                 | IDENTIFIER ;                                        (* "_" = wildcard, else binding or class test *)
literalPat     ::= NUMBER | INT | STRING | "true" | "false" | "nil" ;

subPat         ::= ctorPat
                 | IDENTIFIER "{" IDENTIFIER ( "," IDENTIFIER )* "}"   (* class pattern *)
                 | IDENTIFIER                                          (* class test *)
                 | "[" ( seqElem ( "," seqElem )* )? "]" ;

ctorPat        ::= IDENTIFIER "." IDENTIFIER                           (* enum name "." constructor name *)
                   ( "(" IDENTIFIER ( "," IDENTIFIER )* ")"            (* positional fields *)
                   | "{" IDENTIFIER ( "," IDENTIFIER )* "}" )? ;       (* named fields *)

seqElem        ::= "..." IDENTIFIER | IDENTIFIER ;

armBody        ::= "{" declaration* expression "}"
                 | expression ;

mapEntry       ::= expression ":" expression ;
```

**Note on `match`:** `match` is an **expression** construct. It always produces a value. Arms can contain multiple declarations followed by a final expression, or a single expression. When a `match` expression is used as a statement (followed by `;`), the produced value is discarded.

**Class patterns.** A class pattern matches an instance of the class or of any subclass of it.

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
| 6 | `\|` | Left |
| 7 | `^` | Left |
| 8 | `&` | Left |
| 9 | `<<` `>>` `>>>` | Left |
| 10 | `+` `-` | Left |
| 11 | `*` `/` `%` | Left |
| 12 | `!` `-` `~` (unary) | Right (prefix) |
| 13 (highest) | `()` (function call), `.` (property access), `[]` (index) | Left |

The bit operators `|`, `^`, `&`, `<<`, `>>`, and `>>>` bind tighter than the
comparisons and looser than `+` and `-`, in the order of Python, not of C. So
`x & m == 0i` parses as `(x & m) == 0i`, `a | b in c` parses as `(a | b) in c`,
`1i << 2i + 1i` parses as `1i << (2i + 1i)`, and `a | b ^ c & d` parses as
`a | (b ^ (c & d))`.

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
binding each element to `x` for each iteration of `body`. The precise binding
rule is in [Binding Identity](04-semantics.md#binding-identity).

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

- If `seq` is a **List**: `true` if any element is equal to `elem` under internal
  equality, which never calls `__eq__` (see
  [Equality](03-types.md#equality)).
- If `seq` is a **String**: `elem` must be a String; `true` if `elem` appears
  as a substring of `seq`.
- If `seq` is a **Map**: `true` if `elem` is a valid map key and is present in
  the map. Equivalent to `seq.has(elem)`.
- Runtime error if `seq` is neither a List, String, nor Map.
- Runtime error if `seq` is a String and `elem` is not a String.
- Runtime error if `seq` is a Map and `elem` is an invalid map key type (NaN,
  or a non-String object that is not an Instance defining both `__hash__` and
  `__eq__`).

`in` has comparison precedence (same level as `<`, `<=`, `>`, `>=`), so
`a + b in c` parses as `(a + b) in c`.

### `if`/`else` dangling

The `else` clause binds to the nearest preceding `if`. To associate an `else`
with an outer `if`, enclose the inner `if` in a block.

### Slice expression

`seq[start:end]` produces a new sequence value of the same type containing the
elements from index `start` (inclusive) to `end` (exclusive). Both bounds are
required — omitting either is a parse error. A slice cannot appear as an
assignment target. See §04-semantics for full evaluation rules.

The disambiguation rule: a `:` inside `[...]` makes the form a slice; without
`:` it is a plain index. Both bind at CALL precedence (level 13).

### Assignment

Assignment is an expression, not a statement. The left-hand side may be a
bare `IDENTIFIER` (variable set), a `call "." IDENTIFIER` (property set), or a
`call "[" expression "]"` (index set); any other form is a parse error. The
value of an assignment expression is the assigned value.

### `enum` declaration

See `enumDecl` / `enumCtor` in the main EBNF.

An `enum` declaration introduces a closed type with one or more constructors.
It declares the enum name at global scope. It does not declare a name for a
constructor: a program writes the constructor `A` of the enum `E` as `E.A`.
Calling `E.A` with the declared number of arguments returns an enum value.
Constructors with no field list take zero arguments.

```lox
enum Result { Ok(value) Err(msg) }
enum Option { Some(v) None }
enum Shape { Circle(r) None }      // a second None is valid
var r = Result.Ok(42);
var n = Option.None();
var c = Option.Some;               // a constructor is a value
print c(1);                        // Option::Some(1)
```

- Enum declarations are only permitted at global scope.
- Two enums with the same name, and two constructors with the same name in
  one enum, are compile errors. Constructors of different enums may have the
  same name.
- No variable that the program declares may have the enum name, at any
  scope and anywhere in the source, before or after the enum declaration.
  This covers a global or local variable, a function, a class, another enum,
  a parameter, a pattern binding (also a name in `E.A{...}`), an
  `@`-binding name, a `catch` variable, a `for`-in variable, and a name in a
  destructuring declaration. Each is a compile error. So `E` in `E.A` always
  names the enum. A member name is not a variable, so it may be the enum
  name: a constructor (`enum E { E }`), a constructor field, a method, or a
  property.
- The enum name means the enum in the whole program, before and after its
  declaration. A use of the enum name that comes before the declaration in
  the source, alone or as `E.A`, is a compile error.
- The enum name may be the name of a built-in global
  ([§05-stdlib](05-stdlib.md)). The enum hides the built-in in the whole
  program. For example, in a program that declares `enum type { A }`,
  `type(x)` is a compile error, because `type` alone is not a value.
- In an expression, `E.A` is the constructor value. These are compile errors:
  `E.B` when `E` has no constructor `B`; the enum name `E` when a `.` and a
  constructor name do not follow it; an assignment to `E` or to `E.A`.
- A constructor name alone is an ordinary variable name. When no variable of
  that name exists, a read of it is an undefined-variable fault
  (`UndefinedVariableError`, `spec/04-semantics.md`).
- Calling a constructor with the wrong number of arguments is a runtime error.

### Extended `match` arm patterns

See `armPats` / `armPat` / `ctorPat` in the main EBNF.

A pattern of the form `E.A` is a constructor pattern. `E` must be an enum
declared earlier in the source, and `A` must be a constructor of `E`;
otherwise the pattern is a compile error. A bare identifier in a pattern is
never a constructor pattern: it is the wildcard `_`, a binding, or a class
test.
`_` alone is the wildcard. `_` followed by `{` is the name of a class pattern
and resolves like any other name.
A class pattern whose name is an enum name (`E{x}`) is a compile error.

- `E.A` tests the constructor and binds no field.
- `E.A(x, y)` lists one name for each field of `A`, in declaration order. A
  list with more or fewer names than `A` has fields is a compile error. The
  name `_` binds nothing.
- `E.A{f, g}` binds the named fields. It may list a subset of the fields.
  A name that is not a field of `A` is a compile error.

**Alternatives:** the alternatives of an arm are separated by `or`, and an
alternative can be any pattern, a literal included. All alternatives must
bind the same names in the same order; a mismatch is a compile error. A bare
binding or wildcard may be only the last alternative. The alternatives are
tried from left to right, and the first that matches is used. The guard and
body run once for whichever alternative matched. A constructor pattern does
not match a value that is not an enum value, so `case 0 or E.A` is a valid
arm for a subject of any type.

```lox
case 1 or 2                          => "small"
case Cmd.Quit or Cmd.Pause           => stop()
case Cmd.Move(x) or Cmd.Teleport(x)  => go(x)   // both bind x
case Cmd.Move(y) or Cmd.Teleport(z)  => ...     // compile error: y ≠ z
case Cmd.Move(x) or x                => x       // valid: the last alternative binds x
case x or y                          => ...     // compile error: a bare binding is first
case 1, 2                            => ...     // parse error: use or
```

**Exhaustiveness:** if a match contains at least one constructor pattern of
enum `E`, the compiler verifies that every constructor of `E` appears in an
arm (across all alternatives), or that an unguarded `_`/binding wildcard is
present. A missing constructor is a compile error listing the absent names.
A match with constructor patterns of more than one enum makes this check for
each enum.

An arm that follows an unguarded catch-all arm (`case _` or a plain binding)
is unreachable and is a compile error.

### Function call arguments

The maximum number of arguments of a call, and of parameters of a function,
is implementation-defined (see [Conformance](README.md#conformance)).

### `try`/`catch`, `throw`, `defer`

See `tryStmt` / `throwStmt` / `deferStmt` in the main EBNF.

- `try` and `catch` each require an explicit `{ }` block — an unbraced
  single statement is a parse error, unlike `if`/`while`/`for`.
- `catch` binds exactly one identifier. There is no typed catch-clause
  syntax; a program that needs to discriminate the shape of a thrown value
  does so with `match` inside `catchBlock`.
- `throw`'s operand is a plain `expression` — any expression form is
  allowed, and the thrown value is not restricted to any particular type.
- `defer`'s operand must have the syntactic form of a function or method
  call, `callee(arguments)`. Any expression that is not shaped like a call
  — a bare identifier, a property access with no trailing `(...)`, a
  literal, a parenthesised call, a call joined to another operand by an
  operator such as `or`, and so on — is a parse error. `super.m(...)` has
  the form of a method call and is a valid operand.

See [§04-semantics](04-semantics.md#try-statement) for the full evaluation
rules of all three statements, and [§03-types](03-types.md#error) for the
built-in `Error` type a caught runtime fault is delivered as.

### `yield` expression

See `yieldExpr` in the main EBNF.

`yield` is an expression, not a statement: `yield e` produces a value, so it
may stand wherever a whole assignment-level expression is expected — as the
right-hand side of an assignment (`var x = yield e`), as a call argument
(`f(yield e)`), as the operand of `return` (`return yield e`), or standing
alone as an expression statement (`yield e;`). Its operand is a full
`expression`; the operand may be omitted, in which case the value yielded is
`nil`.

`yield` sits at the assignment level, the lowest precedence. It forms a
complete expression: it cannot be an operand of any operator, and no operator
may follow it. Thus `yield a + b` parses as `yield (a + b)` and `a = yield b`
parses as `a = (yield b)`, but `1 + yield 2`, `yield + 2`, and `-yield 1` are
grammar errors.
