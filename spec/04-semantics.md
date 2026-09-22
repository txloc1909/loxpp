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

### Destructuring Declaration

```
var {a, b} = expr
```

Evaluates `expr` exactly once to produce a source object. For each field name
in the pattern, reads that named property from the source object (equivalent to
`source.fieldName`) and binds it to a new variable in the current scope.

- The initializer is **required**; `var {a, b};` is a **static error**.
- Each field name must be a distinct identifier. Duplicate names in the same
  pattern are a **static error**.
- The source object is not exposed as a user-accessible variable.
- Pattern-bound variables are mutable (same default as `var`).
- If a named property is absent from the source object, this is a **runtime
  error** (same as `obj.missingField`).

### Sequence Destructuring Declaration

```
var [a, b] = expr
```

Evaluates `expr` exactly once to produce a source sequence (List or String).
For each name at position `i` in the pattern, reads `source[i]` and binds it
to a new variable in the current scope.

- The initializer is **required**; `var [a, b];` is a **static error**.
- Each name must be a distinct identifier. Duplicate names are a **static error**.
- `_` is a positional wildcard: `source[i]` is accessed and discarded; no
  variable is created. The duplicate-name restriction does not apply to `_`.
- Elements beyond the last pattern position are never accessed.
- Out-of-bounds access raises a **runtime error** ("List index out of bounds.").
- The source sequence is not exposed as a user-accessible variable.
- Pattern-bound variables are mutable (same default as `var`).
- Trailing comma in the pattern is allowed.

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
2. If `obj` is a Map, look up `name` in the map class method table. If found, return a new bound built-in method
   wrapping the native function and `obj` as receiver. Each evaluation creates a new value, so
   two property-get reads of the same map's method never yield the same object. If not found, raise a runtime error
   ("Undefined property 'name' on map.").
3. If `obj` is a File, look up `name` in the file class method table. If found, return a new bound built-in method
   wrapping the native function and `obj` as receiver. Each evaluation creates a new value, so
   two property-get reads of the same file's method never yield the same object. If not found, raise a runtime error
   ("Undefined property 'name' on file.").
4. If `obj` is an `Error` ([§03-types](03-types.md#error)): if `name` is `message` or `kind`, return that
   field's value (a String). Otherwise, this is a **runtime error** ("Undefined property 'name' on error.").
5. If `obj` is not an Instance, this is a **runtime error** ("Only instances have properties.").
6. If the instance's field table contains `name`, return that field value. Fields shadow methods.
7. Otherwise, look up `name` in the instance's class method table. If found, return a BoundMethod
   wrapping the closure and `obj` as receiver.
8. If neither step 6 nor 7 found `name`, this is a **runtime error** ("Undefined property 'name'.").

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

`this` is **read-only**. Assigning to it is a **static error** ("Invalid
assignment target."). It is a local binding for every other purpose: each method
call makes a new one, and a nested function can capture it. See
[Binding Identity](#binding-identity).

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

### match Expression

```
match subject { matchArm* }
```

1. Evaluate `subject` exactly once and store it internally (not user-accessible).
2. Arms are tested in source order. For each arm:
   a. Evaluate the pattern against the stored subject (tag check, literal equality, or wildcard).
   b. If the pattern does not match, proceed to the next arm.
   c. If the pattern matches and a guard (`if expr`) is present, evaluate the guard. If the guard is falsy, proceed to the next arm.
   d. If the pattern matches (and the guard, if present, is truthy), evaluate the arm's body **expression** and return its value as the result of the `match` expression.
3. If no arm matches: raise **`MatchError`** at runtime. The `match` expression does not produce a value.

All arms must be written so that their body expressions leave exactly one value; this is enforced structurally by the compiler (each arm stores its result into a pre-allocated slot before cleanup).

When a `match` expression is used as a statement (`exprStmt`), the result value is discarded.

**`break`** and **`continue`** inside a match arm body behave like in a loop. `break`
exits the `match` immediately (see the `break` statement); it does **not** exit any
enclosing loop. `continue` searches upward for the nearest enclosing loop and continues
that loop; if there is no enclosing loop, this is a **static error** (see the `continue`
statement).

**@-bindings.** A pattern of the form `name @ subPat` evaluates `subPat` against the subject; if it matches, `name` is bound to the whole subject value. Both `name` and any bindings introduced by `subPat` are visible in the arm body. `_` is not allowed as an @-binding name. The sub-pattern must be a structural pattern (constructor, class, or sequence); a plain binding or wildcard sub-pattern is a compile error.


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

### `for`-in Statement

```
for (var x in expr) body
```

Iterates over the elements of a List or a String, or over the keys of a Map.
A List iterates in order. A String
iterates its single-character substrings in order. A Map iterates over keys
in unspecified order.

1. Evaluate `expr` exactly once. The result must be a **List**, a **String**,
   or a **Map**; any other value is a **runtime error**
   ("Value is not iterable (expected list, string, or map).").
2. An internal **iterator** is created, holding a reference to the iterated
   value and a cursor starting at `0`. The iterator is not accessible to
   user code.
   For a Map, the iterator also records the structural version.
3. Before each iteration, the next element is located:
   - For a **List** or **String**: if `cursor ≥ length`, the loop exits.
   - For a **Map**: the recorded version is compared to the current
     version first (see Mutation during iteration below). Then the
     cursor, a bucket index, scans forward to the next occupied bucket
     within the capacity; if there is none, the loop exits.
4. Otherwise, the located element is bound to `x` and `body` executes.
   - For a **List**: `x` is bound to the element value.
   - For a **String**: `x` is bound to a single-character String.
   - For a **Map**: `x` is bound to the next key.
5. After the body, the cursor advances by one and the loop repeats from step 3.

`x` is scoped to the loop statement; it is not accessible after the loop exits.
One binding of `x` serves the whole execution of the loop, and each iteration
assigns to it. It is not a new binding per iteration. See
[Binding Identity](#binding-identity).

**Mutation during iteration**: modifying the List while iterating is defined.
Elements appended to the List at indices beyond the current cursor will be
visited. Removing elements is not directly possible (List has no `remove`).
Each Map holds a structural version that grows on every insert of a new key
and every erase of a live key. For a Map, the iterator records the version
at creation; if the version differs at the next iterator step, that step is
a **runtime error** ("Map changed size during iteration."). This covers
inserting a new key and removing a key, including growth that rehashes the
Map, and also a paired erase plus insert that restores the net size. It
matches Python, which reports the same fault for a dictionary size change.
The check runs at the next iterator step only: `break`, `return`, or `throw`
before that step exits the loop without an error. Assigning a value to a key
that already exists changes nothing structural and is permitted, as is
deleting a key that is not present.

**`break`** and **`continue`** work as described below.

### `break` Statement

```
break ;
```

Immediately exits the innermost enclosing `while`, `for`, `for`-in, or `match`.
Any local variables declared inside the construct since its opening
are destroyed. Executing `break` outside a loop or match is a **static error**.

### `continue` Statement

```
continue ;
```

Skips the remainder of the current loop body and jumps to the next iteration:
- In a `while` loop: jumps to re-evaluating the condition.
- In a `for` loop: jumps to evaluating the increment expression, then the
  condition.
- In a `for`-in loop: jumps to checking whether the iterator has a next
  element (cursor advance already happened via `ITER_NEXT` before the body).

Any local variables declared inside the loop body since the top of the current
iteration are destroyed. Executing `continue` inside a `match` that is itself
inside a loop targets that enclosing loop. Executing `continue` with no
enclosing loop (even if inside a match) is a **static error**.

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

Binds the name to a new Function value in the current scope. A function
captures the bindings it uses at the point of declaration (closure semantics;
see [Binding Identity](#binding-identity)). A function declared at the top
level is a global; one declared inside a block is a local.

### `return` Statement

```
return ;          // returns nil
return expr ;     // returns the value of expr
```

Exits the current function immediately, yielding the given value (or `nil`).
A `return` at the top level (outside any function body) is a **static error**.

A function call may also exit early because of an in-flight `throw`,
skipping any remaining statements in the body — see [`throw`
Statement](#throw-statement) below.

### `try` Statement

```
try tryBlock catch ( name ) catchBlock
```

1. Execute `tryBlock`.
2. If `tryBlock` runs to completion without an in-flight `throw` reaching
   this `try` statement — whether it falls off the end, or exits via
   `break`, `continue`, or `return` — the `try` statement completes the
   same way; `catchBlock` does not execute.
3. If a `throw` statement executes anywhere during step 1 — inside
   `tryBlock` itself, or inside any function called (directly or
   indirectly) from within `tryBlock` — and no `try` statement nested more
   tightly around the point of that `throw` has already caught it, this
   `try` statement catches the thrown value:
   a. A new local variable named `name` is declared, scoped to
      `catchBlock` only, and bound to the thrown value.
   b. `catchBlock` executes with `name` in scope.
   c. Every local binding created since `tryBlock` began — including ones
      created inside functions that were exited while unwinding to reach
      this `catch` — has already ended by the time `catchBlock` starts
      (see [Binding Identity](#binding-identity)).
4. How the `try` statement as a whole exits is decided by `catchBlock`: if
   `catchBlock` runs to completion, the `try` statement completes normally
   after it; if `catchBlock` executes `return`, `break`, or `continue`,
   the `try` statement exits that way; if `catchBlock` executes `throw` —
   whether throwing a new value or re-throwing `name` — that follows the
   ordinary [`throw` Statement](#throw-statement) rules below and is
   **not** caught by this same `try` statement's own `catchBlock`.
5. There is no `finally` clause. Code that must run on every exit from a
   function — including an exit caused by an in-flight `throw` — is
   written with [`defer`](#defer-statement) instead.

### `throw` Statement

```
throw expr ;
```

1. Evaluate `expr`.
2. Control does not return to the statement after this `throw`. Instead,
   Lox++ searches for the nearest `try` statement able to catch it:
   starting at the point of the `throw` and continuing outward through the
   chain of function calls currently in progress (the function containing
   the `throw`, the function that called it, and so on, up to the
   top-level script), Lox++ finds the innermost `try` statement whose
   `tryBlock` is still executing at that point and that has not already
   caught an in-flight throw.
3. Every function call that this search exits — every call between the
   point of the `throw` and the function that contains the catching `try`
   statement, exclusive — is exited immediately:
   a. That call's pending deferred calls run first, in the order described
      under [`defer` Statement](#defer-statement), before the call is
      considered exited.
   b. The call's local bindings end (see [Binding Identity](#binding-identity)).
   c. No return value is produced by that call.
4. Once the catching `try` statement is found, its `catchBlock` runs as
   described under [`try` Statement](#try-statement), with the value from
   step 1 bound to its identifier.
5. If no `try` statement catches it — the search in step 2 reaches the top
   of the program with no match — the `throw` is **uncaught**. Every
   function call on the way to the top still runs its pending deferred
   calls (step 3a). Execution then halts and the error is reported exactly
   as an uncaught runtime error is reported (see [Runtime
   Errors](#runtime-errors)):
   - If the thrown value is an `Error` value ([§03-types](03-types.md#error)),
     the reported message is that value's `message` field, unchanged.
   - For any other value, the reported message is that value's canonical
     string representation ([§03-types, Canonical String
     Representation](03-types.md#canonical-string-representation)) — the
     same text `str()` would produce.
6. A `throw` statement never completes normally: no statement written
   after it in the same block ever executes.
7. `throw` may appear anywhere a statement may appear, including inside a
   function called from the top level, or directly at the top level of the
   script — it is not restricted to appearing inside a function body. A
   `throw` at the top level that nothing catches behaves exactly as step 5
   describes; a `throw` at the top level caught by a `try` statement also
   at the top level behaves exactly as steps 1-4 describe.
8. A `throw` written inside a `catchBlock` — including one that re-throws
   the value just bound by that `catch` clause (`throw name;`) — is not a
   distinct construct. It follows exactly the rules above: it is caught by
   an enclosing `try` statement the same way a fresh `throw` would be, and
   it is never caught by the `catchBlock` it was written inside.

### `defer` Statement

```
defer callee ( arguments ) ;
```

1. Evaluate `callee` and each argument expression, left-to-right — exactly
   as steps 1-2 of [Function Call](#function-call) — at the point where
   this `defer` statement executes. The call itself is **not** performed
   yet.
2. The evaluated callee and argument values are recorded as one pending
   deferred call, belonging to the function call whose body is currently
   executing. Recording happens independently on each execution of a
   `defer` statement: executing the same `defer` statement more than once
   (for example, on each iteration of a loop) records one pending deferred
   call per execution.
3. Execution continues with the statement after `defer`. The recorded call
   has not run yet.
4. When the enclosing function call exits — by falling off the end of the
   function body, by `return`, or by a `throw` unwinding past this
   function call (see [`throw` Statement](#throw-statement) above) — every
   pending deferred call recorded during this function call runs,
   most-recently-recorded first (LIFO order), before the call is
   considered fully exited. Each deferred call's own return value is
   discarded. For `return expression;`, `expression` is evaluated before
   any pending deferred call runs, so a deferred call cannot change the
   value that `return` yields.
5. If running a deferred call itself executes a `throw`, that new throw
   replaces whatever was already causing this function call to exit: a
   `return` value in progress is discarded, and an in-flight throw already
   unwinding through this function call is replaced by the new one. Any
   deferred calls recorded before the one that threw still run, in the
   same LIFO order, before the new throw continues propagating — searching
   for an enclosing `try` statement starting from this function call's own
   caller, exactly as step 2 of [`throw` Statement](#throw-statement)
   describes.
6. `defer` is scoped to the **function**, not to the block it textually
   appears in: a deferred call recorded inside a nested block, loop, or
   `try`/`catch` still runs when the whole enclosing function call exits,
   not when that inner construct exits.
7. `defer` is permitted only inside a function body. Using `defer` at the
   top level of a script (outside any function) is a **static error**
   ("Can't defer at the top level."), matching how `return` at the top
   level is a static error.

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

Multiple closures created in the **same execution** of a scope share the same
closed-over variable; mutating it through one closure is visible through all
others. Two closures made by **different** executions of the same source scope
do not share anything. The next section makes this precise.

### Binding Identity

A closure captures a **binding**, not a value and not a copy. A closure reads
and writes that binding when it is **called**. What it sees therefore depends on
when you call it, not on when it was created.

**This section is about LOCAL bindings. Globals are not bindings.** A global
name is looked up in one global table when the access **runs**, not when the
function that holds the access is created. A function can therefore read a
global that did not exist when the function was made, and it always sees the
current value.

```lox
fun get() { return later; }
var later = 42;
print get(); // 42
```

```lox
var g = 1;
fun get() { return g; }
var g = 2;
print get(); // 2
```

Nothing in the rest of this section applies to a global.

**What creates a local binding.** All of these create local bindings:

- a local `var` declaration;
- the variable of a `for` initializer, and the variable of a `for ... in`
  statement;
- a function parameter;
- the name of a local `fun` or `class` declaration;
- `this`, inside a method;
- the identifier in a `catch (identifier)` clause;
- a pattern binding in a `match` arm.

A `match` pattern binding is a local binding, but no closure can capture it. A
`match` arm body is an expression, and Lox++ has no function-expression form, so
a `fun` cannot appear there.

`super` is different again. It is captured lexically at class-definition time,
as the [`super` Expression](#super-expression) section states.

**Each execution of the construct that creates a binding makes a NEW binding.**
One place in the source can therefore make many bindings, one per execution. A
`var` declaration in a loop body makes a distinct binding on every iteration,
and a closure made in one iteration keeps that iteration's binding.

```lox
var fns = [nil, nil, nil];
for (var i = 0; i < 3; i = i + 1) {
    var snapshot = i;
    fun f() { return snapshot; }
    fns[i] = f;
}
print fns[0](); // 0
print fns[1](); // 1
print fns[2](); // 2
```

A function parameter obeys the same rule. Each call makes a new one.

```lox
var fns = [nil, nil, nil];
fun rec(n) {
    fun get() { return n; }
    fns[n] = get;
    if (n > 0) rec(n - 1);
}
rec(2);
print fns[0](); // 0
print fns[1](); // 1
print fns[2](); // 2
```

`this` obeys it too. Each method call makes a new binding of `this`.

```lox
class C {
    init(v) { this.v = v; }
    getter() {
        fun g() { return this.v; }
        return g;
    }
}
var a = C(1).getter();
var b = C(2).getter();
print a(); // 1
print b(); // 2
```

**A loop variable is one binding per execution of the loop statement.** The
variable of a `for` initializer, and the variable of a `for ... in` statement,
are each made once when that loop starts. The loop then assigns to that one
binding: the `for` increment clause assigns to it if the loop has one, and
`for ... in` assigns the next element to it on each iteration. A `for` with no
increment clause never changes its variable. In every case the binding itself is
**not** made again per iteration. Running the same loop
statement a second time — a `for` inside a `while`, for example — makes a new
binding.

```lox
var fns = [nil, nil, nil];
for (var i = 0; i < 3; i = i + 1) {
    fun f() { return i; }
    fns[i] = f;
}
print fns[0](); // 3
print fns[1](); // 3
print fns[2](); // 3
```

```lox
var fns = [nil, nil, nil];
var n = 0;
for (var x in [0, 1, 2]) {
    fun f() { return x; }
    fns[n] = f;
    n = n + 1;
}
print fns[0](); // 2
print fns[1](); // 2
print fns[2](); // 2
```

In both examples above, every call happens after its loop ends, so each closure
reads the one binding at the value it then holds: `3` for the counting loop, and
`2` for the `for ... in` loop, whose variable holds the last element. A closure
called **during** the loop reads the value at that moment instead, so do not
read this rule as "a closure sees the final value".

Sharing one binding across the iterations of a `for ... in` loop is a deliberate
choice. It makes the `for ... in` variable behave like the `for` initializer
variable.

```lox
var f = nil;
for (var i = 0; i < 3; i = i + 1) {
    if (i == 0) { fun g() { return i; } f = g; }
    if (i == 1) print f(); // 1
}
print f(); // 3
```

A `for` inside a `while` shows that each execution of the loop statement makes
its own binding.

```lox
var fns = [nil, nil];
var k = 0;
while (k < 2) {
    for (var i = 0; i < k + 2; i = i + 1) {
        fun f() { return i; }
        fns[k] = f;
    }
    k = k + 1;
}
print fns[0](); // 2
print fns[1](); // 3
```

To capture a per-iteration value, declare a variable in the loop body and
capture that, as the first example does.

**A binding ends when its scope exits, by any path.** A binding **ends** when
its name goes out of scope and no later execution can reach that same binding
again. Ending a binding does not destroy its storage: a closure that captured it
keeps it alive and can still read and write it. Ending it means only that the
next execution of the same declaration makes a **different** binding.

Falling off the end of a block, `break`, `continue`, `return`, leaving a
`match` arm, and an in-flight `throw` unwinding past a scope all end the
bindings of the scopes they leave. A `throw` ends every binding of every
scope between the point of the `throw` and whichever `try` statement's
`catchBlock` stops it — or, if nothing catches it, every scope up to the
top of the program. See the [`throw` Statement](#throw-statement).

```lox
var fns = [nil, nil, nil];
for (var i = 0; i < 3; i = i + 1) {
    var snapshot = i;
    fun f() { return snapshot; }
    fns[i] = f;
    continue;
}
print fns[0](); // 0
print fns[1](); // 1
print fns[2](); // 2
```

**Closures that capture one binding share it.** This holds for **any** two
closures that capture the same binding, at any nesting depth. A capture made
through an intermediate function reaches the same binding, not a copy of it, so
a write through one closure is visible through the other.

```lox
fun makePair() {
    var x = 0;
    fun get() { return x; }
    fun set(v) { x = v; }
    return [get, set];
}
var p = makePair();
p[1](5);
print p[0](); // 5
```

A capture two levels deep reaches the same binding.

```lox
fun outer() {
    var x = 0;
    fun near() { return x; }
    fun mid() {
        fun inner() { x = x + 7; }
        return inner;
    }
    mid()();
    return near();
}
print outer(); // 7
```

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

### Map Literal

```lox
{"key": expr, key2: expr2, ...}
{}
```

1. Each key-value pair is evaluated left-to-right: key expression first, then
   value expression.
2. Each key is validated — if it is NaN or an object type other than String,
   this is a **runtime error** ("Map key must be a scalar (Nil, Bool, Number, or String).").
3. A new Map is allocated and each pair is inserted in source order. If the
   same key appears more than once, the last value wins.
4. The expression evaluates to the new Map.

An empty `{}` produces a Map with zero entries.

The maximum number of key-value pairs in a map literal is 255.

### Index Get

```lox
collection[key]
```

1. Evaluate `collection`.
2. Evaluate `key`.
3. If `collection` is a **Map**:
   a. If `key` is an invalid map key type (NaN, or an object other than String),
      this is a **runtime error**.
   b. If `key` is present in the map, the expression evaluates to its associated
      value.
   c. If `key` is absent, the expression evaluates to `nil`.
4. If `collection` is a **List** or a **String**:
   a. If `key` is not a Number, this is a **runtime error** ("List index must be a number.").
   b. If `key` is not an integer-valued number (i.e., `key ≠ floor(key)`),
      this is a **runtime error** ("List index must be an integer.").
   c. Let `i` be the integer value of `key`.
   d. If `i < 0` or `i ≥ length(collection)`, this is a **runtime error** ("List index out of bounds.").
   e. For a List, the expression evaluates to `collection[i]`. For a String,
      it evaluates to the one-character String at position `i`.
5. Otherwise, this is a **runtime error** ("Only lists and maps can be indexed.").

Note: since there is no separate integer type, `list[1]` and `list[1.0]` are
identical — both are the same Number value used as index 1.

### Index Set

```lox
collection[key] = expr
```

1. Evaluate `collection`.
2. Evaluate `key`.
3. If `collection` is a **Map**:
   a. Validate `key` — same rules as Index Get step 3a above.
   b. Evaluate `expr`.
   c. Insert or update `key → value` in the map.
   d. The expression evaluates to the stored value.
4. If `collection` is a **List**:
   a. Perform the same type and bounds checks as Index Get steps 4a–4d.
   b. Evaluate `expr`.
   c. Store the result in `collection[i]`.
   d. The expression evaluates to the stored value (same as assignment semantics).
5. Otherwise, this is a **runtime error** ("Only lists and maps can be indexed.").

### Slice Get

```lox
seq[start:end]
```

Both `start` and `end` must be present; omitting either is a **parse error**.

**Evaluation:**

**Step 1 — Evaluate operands.**  
Evaluate `seq`, then `start`, then `end` (left-to-right).

**Step 2 — Type-check `seq`.**  
If `seq` is not a List or String, this is a **runtime error** ("Slice requires a List or String.").

**Step 3 — Validate `start`.**
- If `start` is not a Number → runtime error ("Slice index must be a number.").
- If `start` is not integer-valued (i.e., `start ≠ floor(start)`) → runtime error ("Slice index must be an integer.").
- If `start < 0` → runtime error ("Slice index must be non-negative.").
- Let `s = integer value of start`.

**Step 4 — Validate `end`.**
- If `end` is not a Number → runtime error ("Slice index must be a number.").
- If `end` is not integer-valued → runtime error ("Slice index must be an integer.").
- If `end < 0` → runtime error ("Slice index must be non-negative.").
- Let `e = integer value of end`.

**Step 5 — Clamp bounds.**  
Let `n = len(seq)`.
- `s = min(s, n)`
- `e = min(e, n)`

**Step 6 — Collect and return.**
- If `s >= e`, return an empty List (`[]`) or empty String (`""`).
- Otherwise, collect elements at indices `s, s+1, …, e−1`.
- If `seq` is a **List**: return a new List containing those elements in order.
- If `seq` is a **String**: return a new String formed by copying the bytes at those indices. The result is a freshly allocated string value, not a view into the original.
- The original `seq` is not modified.

**Slice assignment is not supported.** A slice expression on the left side of `=` is a **parse error**.

### List Methods

#### `append`

```lox
list.append(value)
```

1. Evaluate `list`. If the result is not a List, this is a **runtime error** ("Only instances have methods.").
2. Appends `value` to the end of the list.
3. The expression evaluates to `nil`.

#### `pop`

```lox
list.pop()
```

1. Evaluate `list`. If the result is not a List, this is a **runtime error** ("Only instances have methods.").
2. If the list is empty, this is a **runtime error** ("Cannot pop from an empty list.").
3. Removes the last element from the list.
4. The expression evaluates to the removed element.

### Map Methods

Accessing a map method without a call (for example, `var k = m.keys;`) yields a bound built-in method.
Calling that method (for example, `k()`) is equivalent to calling the method directly (for example, `m.keys()`).

#### `has`

```lox
m.has(key)
```

1. Evaluate `m`. If the result is not a Map, this is a **runtime error**.
2. Validate `key` — if it is an invalid map key type, this is a **runtime error**.
3. The expression evaluates to `true` if `key` is present in the map, `false` otherwise.

#### `del`

```lox
m.del(key)
```

1. Evaluate `m`. If the result is not a Map, this is a **runtime error**.
2. Validate `key` — if it is an invalid map key type, this is a **runtime error**.
3. If `key` is present, remove it from the map. If absent, this is a no-op.
4. The expression evaluates to `nil`.

#### `keys`

```lox
m.keys()
```

1. Evaluate `m`. If the result is not a Map, this is a **runtime error**.
2. A new List is allocated containing all keys currently in the map, in
   unspecified order.
3. The expression evaluates to the new List.

#### `values`

```lox
m.values()
```

1. Evaluate `m`. If the result is not a Map, this is a **runtime error**.
2. A new List is allocated containing all values currently in the map, in the
   same order as `m.keys()`.
3. The expression evaluates to the new List.

#### `entries`

```lox
m.entries()
```

1. Evaluate `m`. If the result is not a Map, this is a **runtime error**.
2. A new List is allocated. For each key-value pair in the map (in the same
   order as `m.keys()`), a two-element List `[key, value]` is appended.
3. The expression evaluates to the new List.

### `len` (global native)

```lox
len(seq)
```

1. If the argument is a **List**, the expression evaluates to the number of
   elements.
2. If the argument is a **String**, the expression evaluates to the number of
   bytes.
3. If the argument is a **Map**, the expression evaluates to the number of
   key-value pairs (deleted keys are excluded).
4. Otherwise, this is a **runtime error**.

---

## Runtime Errors

A runtime error halts execution immediately and reports an error message —
**unless** it happens during the execution of a `try` statement's
`tryBlock` (directly, or inside any function called from within it), in
which case it does not halt the program; instead it is delivered to that
`try` statement's `catchBlock` as an `Error` value, exactly as the
[`throw` Statement](#throw-statement) describes for any other thrown
value. An error that occurs during the execution of the `catchBlock`
itself is not caught by that same `try` statement. A `StackOverflowError`
that is raised while another `StackOverflowError` is already unwinding —
that is, by a deferred call that runs during that unwind — is not
delivered to any `catchBlock`; it halts the program as an uncaught error.
Every cause in the table below is catchable this way; see
[Fatal Runtime Errors](#fatal-runtime-errors) for the faults that are not.

The table lists each cause, an example, and the `kind` field
([§03-types](03-types.md#error)) of the `Error` value delivered to
`catchBlock` when it is caught. The `Error`'s `message` field holds the
same text the implementation reports when the fault is left uncaught.

| Cause | Example | `Error.kind` |
|---|---|---|
| Arithmetic on non-Numbers | `"a" - 1` | `"ArithmeticTypeError"` |
| Comparison on non-Numbers | `"a" < 1` | `"ComparisonTypeError"` |
| `+` on incompatible types | `1 + "a"` | `"ConcatenationTypeError"` |
| Call of a non-callable value | `42()` | `"NotCallableError"` |
| Wrong argument count | `fun f(a) {} f(1, 2)` | `"ArityError"` |
| Undefined global variable | `print undeclared;` | `"UndefinedVariableError"` |
| Call stack overflow | Unbounded recursion | `"StackOverflowError"` |
| Index of non-List/non-Map | `42[0]` | `"NotIndexableError"` |
| Non-Number List or String index | `list["a"]` | `"IndexTypeError"` |
| Fractional List or String index | `list[1.5]` | `"IndexNotIntegerError"` |
| List or String index out of bounds | `[][0]` | `"IndexOutOfBoundsError"` |
| `pop` on empty list | `[].pop()` | `"EmptyListError"` |
| NaN used as map key | `m[0/0] = 1` | `"NaNKeyError"` |
| Object (non-String) used as map key | `m[[1,2]] = 1` | `"InvalidMapKeyError"` |
| Method called on non-instance/non-list/non-map | `42.foo()` | `"InvalidReceiverError"` |
| No arm matches in a `match` expression | `match 99 { case 1 => "one" }` | `"MatchError"` |
| Constructor called with wrong arity | `ok(1, 2)` when `ok` takes one field | `"ConstructorArityError"` |
| Undefined property on an `Error` value | `try { try { [][0]; } catch (e) { e.foo; } } catch (_) { }` | `"UndefinedPropertyError"` |

**The two map-key rows above cover an index read or write, a map or set
literal, and `in`.** `Map.has(key)` and `Map.del(key)` are stdlib native
methods, not one of those four forms, and an invalid key given to either
is fatal — see the Fatal Runtime Errors table's own `Map.has`/`Map.del`
row below, and [Fatal Runtime Errors](#fatal-runtime-errors) generally.

### Fatal Runtime Errors

The causes above are every fault the native implementation delivers as a
catchable `Error` value. Native also has a second, **larger** set of faults
that always halt the program: they are never delivered to a `catchBlock`,
even from inside a `try` statement's `tryBlock`. Each row below is fatal
today — this is a record of current native behavior, not a design decision
that a future version must keep. There is no `Error.kind` for a fatal
fault; the message is the only text the implementation reports.

A fatal fault also skips every pending deferred call — not only the ones
belonging to a function whose call the fault unwinds past, but the ones
belonging to the function where the fault happened. This differs from a
catchable fault left uncaught, where [`throw`
Statement](#throw-statement) step 5 still runs every pending deferred call
on the way to the top: a fatal fault halts the program immediately at the
point of the fault, the same way an uncaught catchable fault halts it only
after every pending deferred call on the unwind path has already run.

| Cause | Example | Message |
|---|---|---|
| `GET_TAG` applied to a non-enum value | `enum Result { Ok(v) Err(m) } match 1 { case Ok(v) => v case Err(m) => -1 };` | `GET_TAG: expected an enum value.` |
| `print` of a value nested deeper than the implementation's canonical-string depth limit (200 heap levels today) | `var a = [1]; var i = 0; while (i < 300) { a = [a]; i = i + 1; } print a;` | `Value nesting is too deep.` |
| A native function is called with an argument count other than its arity | `clock(1);` | `Expected 0 arguments but got 1.` |
| A `defer`red call holds a value that is not a Closure, Native, BoundMethod, or BoundNative | `fun g() { var x = 42; defer x(); } g();` (the compiler checks only that `defer` is followed by a call expression — `defer 42;` fails to compile with `Expect a call expression after 'defer'.` — not that the callee is callable, so a variable holding a non-callable value reaches this check at run time) | `Deferred callable has unexpected type.` |
| A stdlib native function reports its own error while running | `open("/no/such/path", "r");` | `open(): cannot open '/no/such/path': No such file or directory` |
| `Map.has(key)` or `Map.del(key)` called with an invalid key | `var m = {}; m.has([1, 2]);` | `Map keys must be Bool, Number, Nil, or String. NaN is not allowed.` |
| Undefined property read on a File value | `var f = open("/tmp/f.txt", "w"); f.write("x"); var g = open("/tmp/f.txt", "r"); g.bogus;` (a missing path fails first with the stdlib-error row above) | `Undefined property 'bogus' on file.` |
| Undefined property read on a Map value | `var m = {}; m.bogus;` | `Undefined property 'bogus' on map.` |
| Property read on an Instance where the name is neither a field nor a method | `class C {} var c = C(); c.bogus;` | `Undefined property 'bogus'.` |
| Property read on a value that is not an Instance, Map, File, or Error | `42.foo;` (see [§03-types, Error](03-types.md#error) and [Property Get](#property-get)) | `Only instances have properties.` |
| Property write on a value that is not an Instance | `42.foo = 1;` | `Only instances have fields.` |
| A field looked up by [Method Invocation](#method-invocation) shadows the method name but is not callable | `class C { init() { this.f = 1; } } C().f();` | `Can only call functions, classes and enums.` |
| `obj.method(...)` where `method` is not found on the instance's class | `class C {} C().bogus();` | `Undefined property 'bogus'.` |
| `list.append(...)` called with an argument count other than 1 | `[].append();` | `'append' expects 1 argument but got 0.` |
| `list.pop(...)` called with any argument | `[].pop(1);` | `'pop' expects 0 arguments but got 1.` |
| `list.remove(...)` called with an argument count other than 1 | `[1].remove();` | `'remove' expects 1 argument but got 0.` |
| `list.remove(value)` where `value` is not present | `[1, 2].remove(3);` | `Value not found in list.` |
| Undefined method invoked on a List | `[].bogus();` | `Undefined method 'bogus' on list.` |
| Undefined method invoked on a File | `var f = open("/tmp/f.txt", "w"); f.write("x"); var g = open("/tmp/f.txt", "r"); g.bogus();` (a missing path fails first with the stdlib-error row above) | `Undefined method 'bogus' on file.` |
| Undefined method invoked on a Map | `var m = {}; m.bogus();` | `Undefined method 'bogus' on map.` |
| `class Sub < Super {}` where `Super` is not a Class | `var NotAClass = 1; class Sub < NotAClass {}` | `Superclass must be a class.` |
| `super.method(...)` where `method` is not found on the superclass | `class A {} class B < A { m() { super.zzz(); } } B().m();` | `Undefined property 'zzz'.` |
| Enum field indexed (`enumVal[i]`) with a non-Number index | `enum E { A(x) } var v = A(1); v["a"];` | `Enum field index must be a number.` |
| Enum field indexed with an out-of-range index | `enum E { A(x) } var v = A(1); v[3];` | `Enum field index 3 out of range.` |
| Index-assignment on a String | `"abc"[0] = "x";` (strings are immutable — see [§03-types, String](03-types.md#string)) | `Strings are immutable and cannot be indexed for assignment.` |
| `seq[start:end]` where `seq` is not a List or String | `(42)[0:1];` | `Slice requires a List or String.` |
| `seq[start:end]` where `start` is not a Number | `[1, 2]["a":2];` | `Slice index must be a number.` |
| `seq[start:end]` where `start` is not integer-valued | `[1, 2][1.5:2];` | `Slice index must be an integer.` |
| `seq[start:end]` where `start` is negative | `[1, 2][-1:2];` | `Slice index must be non-negative.` |
| `seq[start:end]` where `end` is not a Number | `[1, 2][0:"a"];` | `Slice index must be a number.` |
| `seq[start:end]` where `end` is not integer-valued | `[1, 2][0:1.5];` | `Slice index must be an integer.` |
| `seq[start:end]` where `end` is negative | `[1, 2][0:-1];` | `Slice index must be non-negative.` |
| `elem in seq` where `seq` is a String and `elem` is not a String | `1 in "abc";` | `Left operand of 'in' on a string must be a string.` |
| `elem in seq` where `seq` is not a List, String, or Map | `1 in 42;` | `Right operand of 'in' must be a list, string, or map.` |
| `for (var x in expr)` where `expr` is not a List, String, or Map | `for (var x in 42) {}` | `Value is not iterable (expected list, string, or map).` |
| Map size changed during `for`-in iteration | `var m = {1: 1}; for (var k in m) { m[2] = 2; }` | `Map changed size during iteration.` |

**`print`'s depth-limit fault is fatal, with no `Error.kind`, on every
consumer.** `Op::PRINT` clears any pending stdlib error before it
stringifies its operand, so the row above is not a stdlib-error path; it is
the canonical-string depth guard in `stringifyObj` (`src/object.cpp`), which
also fires the same way, with the same message, when `str()` calls into the
same guard through a native call. Native, the JVM backend, and the CLR
backend all halt the program on this fault, even inside a `try` statement —
the JVM and CLR guards say so explicitly, in a comment, at their own
matching depth check. There is no catchable-table row for this fault: an
earlier draft of this table gave it one (kind `MaxDepthExceededError`), the
opposite disposition from every implementation; the bootstrap interpreter
was the only consumer that followed that draft, and it now matches the
other three instead (see #325).

**A stdlib native's own error text is not enumerated here.** The row above
gives one example message; the text a stdlib native reports differs call
by call and is defined by that native, not by `src/vm.cpp`. Enumerating
every `nativeRuntimeError` string under `src/stdlib/` is out of this
node's scope (`runtimeError` call sites in `src/vm.cpp` only) and is a
follow-up issue if it is wanted.

**Internal invariant checks are excluded.** A handful of `RAISE_ERROR` call
sites in `src/vm.cpp` guard invariants the compiler itself is responsible
for (for example, that `GET_ITER` always pushes an `ObjIterator` before
`ITER_HAS_NEXT`/`ITER_NEXT` run). Their messages are prefixed `BUG:`. No
valid Lox++ program can reach them; seeing one means the implementation,
not the program, is broken. They are excluded from the table above and are
not part of the language's observable behavior.

**Call stack overflow's fatal fast path.** The catchable `"StackOverflowError"`
row above already covers the general case. Native takes a direct fatal
path — bypassing the catchable machinery — in two situations: when no
`try` statement is active anywhere in the program (so a catch search would
fail immediately regardless), and for the re-entrant case the paragraph
above the catchable table already describes, where a second
`StackOverflowError` arrives while the first is still unwinding. Both
produce the same `"Stack overflow."` message as the catchable row; this is
not a distinct fault.

**Shared uncaught-fault reporting is not a distinct fault.** `handleThrow`
reports the final "no handler found" message for every catchable row above
from two call sites of its own. When the unhandled value is an `Error`
value — including an explicit uncaught `throw` of an `Error` — it reuses
that value's own `message` field unchanged. When an explicit uncaught
`throw` carries a value that is **not** an `Error` value, native reports
that value's canonical string representation with no added prefix (for
example, `throw 42;` reports `42`) — both branches match the [`throw`
Statement](#throw-statement)'s own text this way. Likewise,
`raiseThrowableError`'s fallback for when the `Error` class itself is not
yet initialized cannot fire once any Lox++ program has started running —
the class is created during VM setup, before user code executes — so no
valid program can reach it.

**Wrong argument count's fatal fast path.** The catchable `"ArityError"` row
above already covers the general case: calling a Lox++ function with the
wrong number of arguments while a `try` statement's handler is active. When
no handler is active anywhere in the program, native skips constructing the
`Error` value entirely and reports the same `"Expected 0 arguments but got
1."`-shaped message as a direct fatal error (native's own arity and the
call's argument count) — the ordinary "no handler, so uncaught" outcome the
[Runtime Errors](#runtime-errors) section already describes, not a distinct
fault.

---

## Enum Types

### `ObjEnumCtor` — constructor object

Produced by an `enum` declaration. Each constructor in the declaration becomes
a globally-scoped `ObjEnumCtor` value. It is callable: calling it with
`arity` arguments creates and returns an `ObjEnum` value.

Fields: `tag` (0-based index in declaration order), `arity`, `ctorName`,
`enumName`. Stringifies as `<ctor name>`.

### `ObjEnum` — enum value

Produced by calling an `ObjEnumCtor`. Fields are stored in a positional array;
named access is resolved at compile time (the compiler emits the index).

Stringifies as `ctorName(f1, f2, ...)` with fields expanded, or just
`ctorName` for zero-field constructors.

### `GET_TAG` opcode

Pops an `ObjEnum` value and pushes its constructor's tag as a Number. Used by
constructor pattern arms in `match` for dispatch. Applying `GET_TAG` to a
non-enum value is a runtime error.

### Exhaustiveness at compile time

When a match expression contains at least one constructor-pattern arm for enum
`E`, the compiler checks that:
1. Every constructor of `E` appears as a pattern arm, **or**
2. An unguarded catch-all arm (`case _` or an unguarded binding) is present.

A violation is a **compile error** listing the missing constructor names.
