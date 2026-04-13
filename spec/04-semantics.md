# Lox++ Runtime Semantics

## Evaluation Model

**Expressions** are evaluated to produce a value.  
**Statements** are executed for their effect; they do not produce a value.

Evaluation proceeds left-to-right, depth-first, unless a specific rule states
otherwise.

---

## Scoping

Lox++ uses **lexical (static) scoping**.

- The **global scope** spans the entire program.
- A **local scope** is introduced by any `{ }` block (including function
  bodies).
- Scopes may nest arbitrarily. An inner scope may **shadow** a name from an
  outer scope by declaring a new variable with the same name; the outer
  variable is unaffected and becomes accessible again when the inner scope
  exits.
- Variables are resolved to the scope in which they were declared at the point
  where the reference appears in source, not the scope active at the time of
  execution.

---

## Variables

### Declaration

```
var x;          // x is initialized to nil
var y = expr;   // y is initialized to the value of expr
```

- A variable's declared scope is determined by where the `var` statement
  appears.
- Declaring a variable in the global scope makes it a global; inside a block
  makes it a local.
- Declaring a name that is already declared in the **same** scope is a
  **static error**.
- A variable may not appear in its own initializer expression — that is a
  **static error**.

### Reading

Reading a global variable that has never been declared is a **runtime error**.

Reading a local variable always succeeds (the declaration must have been
reached or the program would have been rejected at the declaration site with
a static error).

### Assignment

```
x = expr
```

Evaluates `expr` and stores the result in the variable named `x`. The
expression value is the assigned value. If `x` is not declared in any
reachable scope this is a **runtime error**.

---

## Expressions

### Literal Expressions

`true`, `false`, `nil`, NUMBER, STRING each evaluate to the corresponding
value.

### Identifier

Evaluating an identifier looks up the variable's current value in the scope
chain.

### Grouping

`( expr )` evaluates `expr` and returns its value; parentheses only affect
parsing precedence.

### Arithmetic

| Expression | Requirement | Result |
|---|---|---|
| `a + b` | Both Numbers, **or** both Strings | Sum (Number) or concatenated string |
| `a - b` | Both Numbers | Difference |
| `a * b` | Both Numbers | Product |
| `a / b` | Both Numbers | Quotient |
| `a % b` | Both Numbers | Remainder (floor-division semantics) |
| `-a` | Number | Negation |

Any violation of the type requirement is a **runtime error**.

String concatenation (`+`) creates a new string whose content is the
characters of the left operand followed by the characters of the right
operand.

Division follows IEEE 754; dividing by zero produces `Infinity` or
`-Infinity` (not an error).

Modulo (`%`) is computed as `fmod(a, b)` adjusted so the result has the same sign as `b` (floor-division semantics, same as Python and Lua). `a % 0` produces `NaN` (consistent with `a / 0` producing `Infinity`).

### Comparison

| Expression | Requirement | Result |
|---|---|---|
| `a < b` | Both Numbers | Boolean |
| `a <= b` | Both Numbers | Boolean |
| `a > b` | Both Numbers | Boolean |
| `a >= b` | Both Numbers | Boolean |

Any violation is a **runtime error**.

### Equality

`a == b` and `a != b` are defined for all type combinations and never produce
a runtime error. See [03-types.md § Equality](03-types.md#equality).

### Logical Operators (Short-Circuit)

`and` and `or` do **not** necessarily return a Boolean; they return one of
their operands.

- `a and b` — evaluates `a`. If `a` is falsy, returns `a` without evaluating
  `b`. Otherwise evaluates and returns `b`.
- `a or b` — evaluates `a`. If `a` is truthy, returns `a` without evaluating
  `b`. Otherwise evaluates and returns `b`.

### Logical Negation

`!a` — evaluates `a` and returns `false` if `a` is truthy, `true` if `a` is
falsy. Always returns a Boolean.

### Property Get

```
obj.name
```

1. Evaluate `obj`.
2. If `obj` is not an Instance, this is a **runtime error** ("Only instances have properties.").
3. If the instance's field table contains `name`, return that field value. Fields shadow methods.
4. Otherwise, look up `name` in the instance's class method table. If found, return a BoundMethod
   wrapping the closure and `obj` as receiver.
5. If neither step 3 nor 4 found `name`, this is a **runtime error** ("Undefined property 'name'.").

### Property Set

```
obj.name = expr
```

1. Evaluate `obj`.
2. If `obj` is not an Instance, this is a **runtime error** ("Only instances have fields.").
3. Evaluate `expr`.
4. Store the result in the instance's field table under `name` (creating the field if it does not
   exist, overwriting if it does).
5. The expression value is the assigned value.

### Method Invocation

```
obj.method(arg1, arg2, ...)
```

Semantically equivalent to looking up `obj.method` and calling the result, but the implementation
fuses both steps into a single `INVOKE` super-instruction. Fields take priority over methods: if
the instance's field table contains `method`, that value is called (and must be callable). If the
field value is not callable, that is a **runtime error**.

### `this` Expression

`this` evaluates to the instance on which the enclosing method was invoked. It is only valid inside
a method body. Using `this` outside any class method is a **static error**.

### `super` Expression

```
super.method
super.method(arg1, arg2, ...)
```

`super.method` looks up `method` starting from the **superclass**, bypassing any override defined
on the current class. It returns a BoundMethod with `this` bound to the current receiver. The
`super.method(args)` form fuses this with a call via the `SUPER_INVOKE` super-instruction.

`super` is lexically captured: it closes over the superclass value at class-definition time, not
at call time.

Using `super` outside a method body, or inside a method of a class with no declared superclass, is
a **static error**.

### Instance Creation

```
ClassName(arg1, arg2, ...)
```

Calling a Class value creates a new Instance:

1. A fresh Instance of the class is allocated with an empty field table.
2. If the class (or any inherited class) defines an `init` method, it is invoked with `this` bound
   to the new instance and the supplied arguments. Arity rules apply to `init`.
3. If no `init` method exists and arguments were supplied, this is a **runtime error**.
4. The call expression evaluates to the new Instance; the return value of `init` is ignored.

### Function Call

```
callee(arg1, arg2, ...)
```

1. Evaluate `callee`.
2. Evaluate arguments left-to-right.
3. If `callee` is not a Function, Class, or BoundMethod, this is a **runtime error**.
4. If the number of arguments does not match the function's arity, this is a
   **runtime error**.
5. A new local scope is created; each parameter is bound to the corresponding
   argument.
6. The function body executes.
7. The call expression evaluates to the function's return value (or `nil` if
   none).

Functions may be called recursively. The depth limit is implementation-defined;
exceeding it is a **runtime error**.

---

## Statements

### Expression Statement

```
expr ;
```

Evaluates `expr` and discards the result.

### Print Statement

```
print expr ;
```

Evaluates `expr` and writes its canonical string representation to standard
output, followed by a newline character. `print` is a statement, not a
function.

### Variable Declaration

See [Variables](#variables) above.

### Block

```
{ declaration* }
```

Introduces a new local scope. Declarations and statements inside execute in
order. Variables declared inside the block are destroyed when the block exits.

### `if` Statement

```
if ( condition ) thenBranch
if ( condition ) thenBranch else elseBranch
```

Evaluates `condition`. If truthy, executes `thenBranch`. If falsy and an
`else` branch is present, executes `elseBranch`. Exactly one branch executes
(or neither, if there is no `else` and the condition is falsy).

### `while` Statement

```
while ( condition ) body
```

1. Evaluate `condition`.
2. If falsy, exit the loop.
3. Execute `body`.
4. Repeat from step 1.

### `for` Statement

```
for ( init ; condition ; increment ) body
```

Semantically equivalent to:

```
{
    init ;
    while ( condition ) {
        body
        increment ;
    }
}
```

Where:
- `init` is a `var` declaration or expression statement, or omitted (bare
  `;`). A `var` declaration here is scoped to the loop, not the enclosing
  block.
- `condition` is an expression evaluated before each iteration; if omitted it
  is treated as `true`.
- `increment` is an expression evaluated after each iteration body; if omitted
  it is skipped.

### `break` Statement

```
break ;
```

Immediately exits the innermost enclosing `while`, `for`, or `switch`
statement. Any local variables declared inside the construct since its opening
are destroyed. Executing `break` outside a loop or switch is a **static
error**.

### `continue` Statement

```
continue ;
```

Skips the remainder of the current loop body and jumps to the next iteration:
- In a `while` loop: jumps to re-evaluating the condition.
- In a `for` loop: jumps to evaluating the increment expression, then the
  condition.

Any local variables declared inside the loop body since the top of the current
iteration are destroyed. Executing `continue` inside a `switch` that is itself
inside a loop targets that enclosing loop (not the switch). Executing
`continue` with no enclosing loop (even if inside a switch) is a **static
error**.

### `switch` Statement

```
switch ( subject ) {
    case value1 : statement*
    case value2, value3 : statement*
    default : statement*
}
```

1. Evaluate `subject` exactly once. The result is the **switch value**.
2. Arms are tested in source order. For each `case` arm, its values are
   evaluated left-to-right. As soon as a value equals the switch value
   (using `==` semantics), the remaining values in that arm and all subsequent
   arms are **not evaluated**; the arm's statements execute and control jumps
   to after the switch.
3. If no `case` arm matched and a `default` arm is present, execute its
   statements.
4. If no arm matched and there is no `default`, the switch body is a no-op.

**No fall-through**: only the first matching arm executes. There is no implicit
flow from one arm to the next.

**Multiple values per arm** (`case v1, v2:`): values are tested left-to-right;
the first match wins and the rest are not evaluated.

**Side effects**: because evaluation short-circuits on the first match,
case-value expressions after the matching position are never evaluated.
Programs must not rely on side effects of unmatched case expressions.

**`default` placement**: the `default` arm, if present, must be the last arm
in the switch body. A `case` arm after `default` is a **static error**.

**`break`** inside a switch exits the switch immediately (see `break`
statement). It does **not** exit any enclosing loop.

**`continue`** inside a switch searches upward for the nearest enclosing loop
and continues that loop. If there is no enclosing loop, this is a **static
error**.

**Duplicate `default`**: more than one `default` label in the same switch is a
**static error**.

### Class Declaration

```
class Name { method* }
class Name < SuperName { method* }
```

1. If `< SuperName` is present, evaluates `SuperName`. If it is not a Class, this is a
   **runtime error** ("Superclass must be a class."). A class cannot inherit from itself — that is
   a **static error**.
2. A new Class value is created with the given `Name`.
3. If a superclass is present, its method table is **copied into the new class at definition
   time** — not at call time. Later changes to the superclass do not affect the subclass.
4. Each `method` body is compiled and stored in the class's method table (overwriting any inherited
   method with the same name).
5. The class value is bound to `Name` in the current scope (global if at top level, local
   otherwise).

### `init` — Initializer Method

`init` is a specially named method that, if defined, is called automatically whenever an Instance
of the class is created (see "Instance Creation" above).

- Inside `init`, a bare `return ;` exits early; `this` is still returned as the call result.
- `return expr ;` with a non-`nil` expression inside `init` is a **static error** ("Can't return a
  value from an initializer.").
- `init` implicitly returns `this`.

### Function Declaration

```
fun name ( params ) { body }
```

Binds the name to a new Function value in the current scope. The function
captures its lexical environment at the point of declaration (closure
semantics). A function declared at the top level is a global; one declared
inside a block is a local.

### `return` Statement

```
return ;          // returns nil
return expr ;     // returns the value of expr
```

Exits the current function immediately, yielding the given value (or `nil`).
A `return` at the top level (outside any function body) is a **static error**.

---

## Closures

When a function is created, it **closes over** variables from its enclosing
lexical scopes. Accessing a closed-over variable from inside the function
reads and writes the same storage as the outer scope, not a copy. If the outer
scope exits while the closure is still reachable, the closed-over variables
remain alive as long as the closure is reachable.

```lox
fun makeCounter() {
    var count = 0;
    fun increment() {
        count = count + 1;
        return count;
    }
    return increment;
}

var c = makeCounter();
print c(); // 1
print c(); // 2
```

Multiple closures created within the same scope share the same closed-over
variable; mutating it through one closure is visible through all others.

### List Literal

```lox
[expr1, expr2, ...]
[]
```

1. Each element expression is evaluated left-to-right.
2. A new List is allocated containing those values in order.
3. The expression evaluates to the new List.

An empty `[]` produces a List with zero elements.

The maximum number of element expressions in a list literal is 255 (the same
limit as function call arguments).

### Index Get

```lox
list[index]
```

1. Evaluate `list`.
2. Evaluate `index`.
3. If `list` is not a List, this is a **runtime error** ("Only lists can be indexed.").
4. If `index` is not a Number, this is a **runtime error** ("List index must be a number.").
5. If `index` is a Number but not an integer-valued number (i.e., `index ≠ floor(index)`),
   this is a **runtime error** ("List index must be an integer.").
6. Let `i` be the integer value of `index`.
7. If `i < 0` or `i ≥ length(list)`, this is a **runtime error** ("List index out of bounds.").
8. The expression evaluates to `list[i]`.

Note: since there is no separate integer type, `list[1]` and `list[1.0]` are
identical — both are the same Number value used as index 1.

### Index Set

```lox
list[index] = expr
```

1. Evaluate `list`.
2. Evaluate `index`.
3. Perform the same type and bounds checks as Index Get (steps 3–7 above).
4. Evaluate `expr`.
5. Store the result in `list[i]`.
6. The expression evaluates to the stored value (same as assignment semantics).

---

## Runtime Errors

A runtime error halts execution immediately and reports an error message.
Common causes:

| Cause | Example |
|---|---|
| Arithmetic on non-Numbers | `"a" - 1` |
| Comparison on non-Numbers | `"a" < 1` |
| `+` on incompatible types | `1 + "a"` |
| Call of a non-callable value | `42()` |
| Wrong argument count | `fun f(a) {} f(1, 2)` |
| Undefined global variable | `print undeclared;` |
| Call stack overflow | Unbounded recursion |
| Index of non-List | `42[0]` |
| Non-Number list index | `list["a"]` |
| Fractional list index | `list[1.5]` |
| List index out of bounds | `[][0]` |
