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
- Out-of-bounds access raises a **runtime error**.
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
| `a < b` | Both Numbers, **or** both Strings | Boolean |
| `a <= b` | Both Numbers, **or** both Strings | Boolean |
| `a > b` | Both Numbers, **or** both Strings | Boolean |
| `a >= b` | Both Numbers, **or** both Strings | Boolean |

Two Numbers compare by numeric value. Two Strings compare as byte sequences:
`<` and `>` compare byte by byte from the first byte, using each byte's
unsigned value (0 through 255), and a String that is a prefix of the other is
the smaller. So `"Z" < "a"` is true and `"ab" < "abc"` is true.

Any violation is a **runtime error**. A comparison of a String with a Number,
or any other mixed pair, is a runtime error.

### Equality

`a == b` and `a != b` are defined for all type combinations and never produce
a runtime error. See [03-types.md § Equality](03-types.md#equality).

### Operator Overloading

An Instance may define special methods ("dunder" methods) that the operators
above dispatch to. The dispatch order is fixed:

1. **Built-in branch first.** The operator's ordinary behaviour applies when
   the operands match the built-in types (Numbers for arithmetic; Numbers or
   Strings for comparison; Numbers, Strings, or the concatenation case for
   `+`; List, String, or Map for `in`).
2. **Then the user method on the receiver.** When the built-in branch does not
   apply, the operator tries the method on its receiver — the left operand for
   the arithmetic, comparison, and equality operators, the container (right
   operand) for `in`, and the called value for `()`. If the receiver is an
   Instance whose class defines the method, the method is called with the
   other operand (or operands) as its argument(s).
3. **Then the reflected method on the right operand** (arithmetic, comparison,
   and equality only). When the receiver is not an Instance, or is an Instance
   whose class does not define the method, and the right operand is an
   Instance whose class defines the operator's reflected method, that method
   is called with the left operand as its single argument. `a op b` calls
   `b.__rop__(a)`; the reflected method computes the result of `a op b`. For
   equality the reflected method is the right operand's ordinary `__eq__`.
   `in`, `()`, `[]`, `len`, `for-in`, slices, and `str` have no reflected
   form: their receiver is already the right operand or the only operand.
4. **Otherwise the same error as today.** A receiver that is not an Instance,
   or an Instance whose class does not define the method, and whose right
   operand has no applicable reflected method, raises exactly the error the
   operator raised before operator overloading existed.

Method lookup reads the class **method table** only; a field whose name
happens to equal a dunder name never participates.

| Operator | Method | Receiver |
|---|---|---|
| `a + b` | `__add__(b)` | `a` |
| `a - b` | `__sub__(b)` | `a` |
| `a * b` | `__mul__(b)` | `a` |
| `a / b` | `__div__(b)` | `a` |
| `a % b` | `__mod__(b)` | `a` |
| `-a` | `__neg__()` | `a` |
| `a < b` | `__lt__(b)` | `a` |
| `a > b` | `__gt__(b)` | `a` |
| `a == b` | `__eq__(b)` | `a` |
| `x in c` | `__contains__(x)` | `c` |
| `f()` on an Instance | `__call__(...)` | `f` |
| `c[k]` | `__index_get__(k)` | `c` |
| `c[k] = v` | `__index_set__(k, v)` | `c` |
| `len(x)` | `__len__()` | `x` |
| `for (var v in x)` | `__iter__()` | `x` |
| `c[start:end]` | `__slice__(start, end)` | `c` |
| `str(x)` / `print x` | `__str__()` | `x` |

**Reflected methods.** These run only after the receiver's own method above
declined (step 3 of the dispatch order). The right operand of the operator is
the receiver; the left operand is the single argument. `b.__rsub__(a)`
therefore computes `a - b`, and `b.__rlt__(a)` computes `a < b`.

| Operator | Reflected method | Receiver | Argument |
|---|---|---|---|
| `a + b` | `__radd__` | `b` | `a` |
| `a - b` | `__rsub__` | `b` | `a` |
| `a * b` | `__rmul__` | `b` | `a` |
| `a / b` | `__rdiv__` | `b` | `a` |
| `a % b` | `__rmod__` | `b` | `a` |
| `a < b` | `__rlt__` | `b` | `a` |
| `a > b` | `__rgt__` | `b` | `a` |
| `a == b` | `__eq__` | `b` | `a` |

`a != b` is `==` then logical negation: it derives from `__eq__` (the left
operand's, or the reflected one on the right) and has no method of its own.
`a <= b` is `!(a > b)` and `a >= b` is `!(a < b)`; each dispatches `__gt__` /
`__lt__` on the left, or the reflected `__rgt__` / `__rlt__` on the right, and
negates the result, with no `__le__` / `__ge__` method.

The result of `__eq__`, `__lt__`, `__gt__`, and `__contains__` must be a
Boolean, whether the method ran on the left operand or as the reflected method
on the right. The result of `__len__` and `__hash__` must be a Number, the
result of `__iter__` must be a List, String, Map, or Coroutine, and the result of
`__str__` must be a String. Any other result raises a catchable
`OperatorResultTypeError` (see [Runtime Errors](#runtime-errors)). The
result of every other method in the tables above is unconstrained.

`__hash__` has no operator expression of its own. A Map key that is an Instance
uses it to place the key; see [Map Literal](#map-literal), [Index
Get](#index-get), [Index Set](#index-set), and [`in`](#in-operator).

`__index_get__` takes the index as its single argument and may return any
value. `__index_set__` takes the index and the assigned value; its return
value is ignored, and the assignment expression evaluates to the assigned
value (as with the built-in List and Map forms). `__iter__` takes no argument
and returns the sequence or coroutine to iterate; the existing iterator then consumes that
sequence (or resumes that coroutine), so `ITER_HAS_NEXT`/`ITER_NEXT` are unchanged. `__slice__` takes the
start and end bounds and may return any value; a List or String uses the
built-in slice below, including its bound validation, and never dispatches.

`len` is a keyword, not a global: a program can no longer name a variable
`len` or pass `len` as a first-class value. `len` on a List, String, or Map
still returns the built-in length; on an Instance it dispatches `__len__`; on
any other value it raises the same error the `len` global raised before.

`str` is likewise a keyword, not a global. `str(x)` and `print x` produce the
same canonical string form. `__str__` redefines an Instance's canonical form
everywhere it is rendered — directly, or nested inside a List, Map, or Enum
value. An Instance without `__str__` renders as `ClassName instance`, as
before. A `__str__` that calls back into `str` or `print` recurses; the
canonical-string depth limit bounds it and raises the same fault as any other
deep nesting (see [Halting Faults](#halting-faults); the limit is
implementation-defined, see [Conformance](README.md#conformance)).

Internal equality — `in` against a List and `list.remove` — always uses
identity equality and never consults `__eq__`.

A Map whose key is an Instance dispatches to that key's class. The Map calls
`__hash__` on the **lookup** key to find the bucket, then resolves a collision
by calling `__eq__` on the **stored** key with the lookup key as its single
argument: `storedKey.__eq__(lookupKey)`. Map-key equality never uses a
reflected method, so the lookup key's own `__eq__` never runs for a collision.
The two keys that compare equal must return the same `__hash__` (see
[§03-types, Map](03-types.md#map)). A `__hash__` or `__eq__` call that writes
to the same Map raises a catchable `MapChangedError`, so a key method cannot
change the Map it is being looked up in.

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
   two property-get reads of the same map's method never yield the same object. If not found, raise a runtime error.
3. If `obj` is a File, look up `name` in the file class method table. If found, return a new bound built-in method
   wrapping the native function and `obj` as receiver. Each evaluation creates a new value, so
   two property-get reads of the same file's method never yield the same object. If not found, raise a runtime error.
4. If `obj` is a Socket, Server, or Process, look up `name` in that type's own class method table. If found,
   return a new bound built-in method wrapping the native function and `obj` as receiver, again as a fresh value
   on every read. If not found, this is a **runtime error**.
5. If `obj` is an `Error` ([§03-types](03-types.md#error)): if `name` is `message` or `kind`, return that
   field's value (a String). Otherwise, this is a **runtime error**.
6. If `obj` is not an Instance, this is a **runtime error**.
7. If the instance's field table contains `name`, return that field value. Fields shadow methods.
8. Otherwise, look up `name` in the instance's class method table. If found, return a BoundMethod
   wrapping the closure and `obj` as receiver.
9. If neither step 7 nor 8 found `name`, this is a **runtime error**.

### Property Set

```
obj.name = expr
```

1. Evaluate `obj`.
2. If `obj` is not an Instance, this is a **runtime error**.
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
the instance's field table contains `method`, that value is called. The call follows the ordinary
call rules. The value can be a function, a class, an enum constructor, a bound method, or an
Instance whose class defines `__call__`. If the field value is not callable, that is a
**runtime error**.

### `this` Expression

`this` evaluates to the instance on which the enclosing method was invoked. It is only valid inside
a method body. Using `this` outside any class method is a **static error**.

`this` is **read-only**. Assigning to it is a **static error**. It is a local binding for every other purpose: each method
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

Functions may be called recursively. The depth limit is implementation-defined
(see [Conformance](README.md#conformance)); exceeding it is a **runtime
error**, except for the self tail calls described next, which do not count
toward the depth.

#### Self Tail Calls

A `return` statement whose whole operand is a call of the enclosing function
by its own name is a **self tail call**. It does not add to the call depth:
a recursion made only of self tail calls runs in constant call depth, however
many times it repeats, and never raises `StackOverflowError`.

A call is a self tail call only when all of these hold:

- The enclosing function is declared with `fun` at the top level of the
  program, and the callee is that function's own name.
- The call is the entire operand of `return`, as in `return f(a, b);`.
  `return f(a) + 1;` and `return g(f(a));` are not self tail calls.
- The number of arguments equals the function's arity.
- The name is not shadowed at the `return` — no parameter or local variable
  of the same name is in scope there.
- The function body contains none of: a nested `fun` or `class`
  declaration, a `try` statement, or a `defer` statement.
- When the `return` executes, the function's name still holds the function
  that is running. If the name was rebound to another value, the call is an
  ordinary call of that value.

When a self tail call executes, the arguments are evaluated left to right
against the current parameter values, then become the new parameter values,
and the body starts again. Every local variable of the finished iteration
ends first. The result is the same as an ordinary call, except that the
iteration does not appear as a separate frame in an error trace.

A call that is not a self tail call is an ordinary call: it counts toward
the depth limit, and unbounded recursion through it raises
`StackOverflowError`. Mutual recursion and tail calls of other functions are
ordinary calls.

### `yield` Expression

```
yield expr
```

Suspends the current coroutine and delivers `expr` to its resumer.

1. Evaluate `expr` (or `nil` if the operand is omitted).
2. Suspend the current coroutine. Every call frame currently on the
   coroutine's stack stays in place: none of them exits, no scope ends, and
   no deferred call runs.
3. Control returns to whoever resumed the coroutine: the pending `resume`
   call completes and evaluates to the yielded value.
4. When the coroutine is resumed again, the `yield` expression evaluates to
   the value that `resume` passed in (or `nil` if `resume` passed none), and
   execution continues after the `yield`.

Every task, the main task included, is a coroutine that its worker's
scheduler resumes (see [Workers and Tasks](#workers-and-tasks)). So a
`yield` always has a coroutine to suspend. A `yield` in a task, outside
every coroutine that the task resumed, suspends the task: the yielded value
is discarded, the task joins the back of its worker's run queue, and the
`yield` evaluates to `nil` when the task runs again. This holds for a
`yield` at the top level of the program, which the main task runs.

A `yield` reached while a native callback is on the call chain — for example
inside `__str__`, `__hash__`, or `__eq__` while `print` or a map operation
called it — is a **runtime error** (`YieldAcrossNativeError`; see
[Runtime Errors](#runtime-errors)). A native callback's own continuation is
not part of the coroutine's stack and cannot be suspended, so such a yield
cannot resume.

A `yield` reached while a deferred call runs — a call recorded by a
[`defer`](#defer-statement) statement whose enclosing function call has
exited — is a **runtime error** with the same kind. This holds also in a task
outside every coroutine. A deferred call runs while the call it belongs to is
mid-exit, and that exit has no resumption point, so the deferred call cannot
suspend; it runs to completion, or raises.

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

**Class patterns.** A bare name in a pattern is a class test only as a whole arm pattern (alone or in an `or`-chain) or as the sub-pattern of an @-binding, and only when a global class with that name is declared earlier in the source than the match. A class name in any other place in a pattern is a binding, including a name in braces, parentheses or brackets. A class name is also a binding when no global class of that name is declared earlier in the source than the match; this includes a class that is not global. The class test reads the global with that name when it runs; if that global does not hold a class at that time, the test matches no value.

Each name in the braces of a class pattern is a property read of the matched instance, as in [Property Get](#property-get), and the arm binds the result. A name that is neither a field nor a method of the instance is a runtime error.

**Constructor patterns.** A bare name in a pattern is a constructor tag test only as a whole arm pattern (alone or in an `or`-chain) or as the sub-pattern of an @-binding, and only when an enum constructor with that name is declared earlier in the source than the match. This holds for a constructor with any number of fields: the arm makes the same test as the constructor pattern with that name, and binds no field. A constructor name in any other place in a pattern is a binding, including a name in braces, parentheses or brackets. A constructor name is also a binding when no enum constructor of that name is declared earlier in the source than the match. When an enum constructor and a global class with the same name are both declared earlier in the source than the match, the bare name is a constructor tag test.


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

Iterates over the elements of a List or a String, over the keys of a Map, or
over the values a Coroutine yields.
A List iterates in order. A String
iterates its single-character substrings in order. A Map iterates over keys
in unspecified order. A Coroutine is resumed once per iteration, and yields
the next element each time.

1. Evaluate `expr` exactly once. The result must be a **List**, a **String**,
   a **Map**, or a **Coroutine**; any other value is a **runtime error**.
2. An internal **iterator** is created, holding a reference to the iterated
   value and a cursor starting at `0`. The iterator is not accessible to
   user code.
   For a Map, the iterator also records the structural version. For a
   Coroutine, the iterator holds the coroutine itself and uses no cursor.
3. Before each iteration, the next element is located:
   - For a **List** or **String**: if `cursor ≥ length`, the loop exits.
   - For a **Map**: the recorded version is compared to the current
     version first (see Mutation during iteration below). Then the
     cursor, a bucket index, scans forward to the next occupied bucket
     within the capacity; if there is none, the loop exits.
   - For a **Coroutine**: if it is already dead, the loop exits, with no
     resume attempt. Otherwise it is resumed; if the resume returns (the
     function has finished), the loop exits, and otherwise the value it
     yielded is the next element.
4. Otherwise, the located element is bound to `x` and `body` executes.
   - For a **List**: `x` is bound to the element value.
   - For a **String**: `x` is bound to a single-character String.
   - For a **Map**: `x` is bound to the next key.
   - For a **Coroutine**: `x` is bound to the value the coroutine yielded.
5. After the body, the cursor advances by one and the loop repeats from step 3.
   For a Coroutine there is no cursor; the next iteration resumes it again.

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
a **runtime error**. This covers
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
   **runtime error**. A class cannot inherit from itself — that is
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
- `return expr ;` with a non-`nil` expression inside `init` is a **static error**.
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

A `return` that is a [self tail call](#self-tail-calls) exits the current
function call by starting the next iteration of the same function in its
place, with the same effect on the result.

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
   calls (step 3a). Execution then halts, as an uncaught runtime error does
   (see [Conformance](README.md#conformance), Halts). Whether those
   deferred calls run before the program ends is implementation-defined, as
   for every halt, and so is the text of the report.
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
   A deferred call cannot suspend. A `yield` in one — and a
   `task.receive()` on an empty mailbox in one, by the [`task.receive`](#taskreceive)
   rule — raises `YieldAcrossNativeError`, in a task and in
   a coroutine alike. The drain continues with the remaining LIFO deferred
   calls.
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
   top level of a script (outside any function) is a **static error**, matching how `return` at the top
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

The maximum number of element expressions in a list literal is
implementation-defined (see [Conformance](README.md#conformance)).

### Map Literal

```lox
{"key": expr, key2: expr2, ...}
{}
```

1. Each key-value pair is evaluated left-to-right: key expression first, then
   value expression.
2. Each key is validated — if it is NaN, or an object that is neither a String
   nor an Instance whose class defines both `__hash__` and `__eq__`, this is a
   **runtime error** (`NaNKeyError` or `InvalidMapKeyError`).
3. A new Map is allocated and each pair is inserted in source order. If the
   same key appears more than once, the last value wins.
4. The expression evaluates to the new Map.

An empty `{}` produces a Map with zero entries.

The maximum number of key-value pairs in a map literal is
implementation-defined (see [Conformance](README.md#conformance)).

### Index Get

```lox
collection[key]
```

1. Evaluate `collection`.
2. Evaluate `key`.
3. If `collection` is a **Map**:
   a. If `key` is an invalid map key type (NaN, or a non-String object that is
      not an Instance defining both `__hash__` and `__eq__`), this is a
      **runtime error**.
   b. If `key` is present in the map, the expression evaluates to its associated
      value.
   c. If `key` is absent, the expression evaluates to `nil`.
4. If `collection` is a **List** or a **String**:
   a. If `key` is not a Number, this is a **runtime error**.
   b. If `key` is not an integer-valued number (i.e., `key ≠ floor(key)`),
      this is a **runtime error**.
   c. Let `i` be the integer value of `key`.
   d. If `i < 0` or `i ≥ length(collection)`, this is a **runtime error**.
   e. For a List, the expression evaluates to `collection[i]`. For a String,
      it evaluates to the one-character String at position `i`.
5. Otherwise, this is a **runtime error**.

Note: since there is no separate integer type, `list[1]` and `list[1.0]` are
identical — both are the same Number value used as index 1.

### Index Set

```lox
collection[key] = expr
```

1. Evaluate `collection`.
2. Evaluate `key`.
3. Evaluate `expr`.
4. If `collection` is a **Map**:
   a. Validate `key` — same rules as Index Get step 3a above.
   b. Insert or update `key → value` in the map.
   c. The expression evaluates to the stored value.
5. If `collection` is a **List**:
   a. Perform the same type and bounds checks as Index Get steps 4a–4d.
   b. Store the result in `collection[i]`.
   c. The expression evaluates to the stored value (same as assignment semantics).
6. If `collection` is an **Instance** whose class defines `__index_set__`,
   call it with `key` and the evaluated value (see [Operator
   Overloading](#operator-overloading)); the expression evaluates to the
   assigned value.
7. Otherwise, this is a **runtime error**.

The checks in steps 4a and 5a run after `expr` is evaluated, so a fault from
a check follows every effect of `expr`. This is the same order as [Map
Literal](#map-literal), which evaluates every value before it validates its
keys.

### Slice Get

```lox
seq[start:end]
```

Both `start` and `end` must be present; omitting either is a **parse error**.

**Evaluation:**

**Step 1 — Evaluate operands.**  
Evaluate `seq`, then `start`, then `end` (left-to-right).

**Step 2 — Type-check `seq`.**  
If `seq` is an Instance whose class defines `__slice__`, call it with `start`
and `end` and use its result (see [Operator Overloading](#operator-overloading));
the built-in behaviour below does not apply. Otherwise, if `seq` is not a List
or String, this is a **runtime error**.

**Step 3 — Validate `start`.**
- If `start` is not a Number → runtime error.
- If `start` is not integer-valued (i.e., `start ≠ floor(start)`) → runtime error.
- If `start < 0` → runtime error.
- Let `s = integer value of start`.

**Step 4 — Validate `end`.**
- If `end` is not a Number → runtime error.
- If `end` is not integer-valued → runtime error.
- If `end < 0` → runtime error.
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

1. Evaluate `list`. If the result is not a List, this is a **runtime error**.
2. Appends `value` to the end of the list.
3. The expression evaluates to `nil`.

#### `pop`

```lox
list.pop()
```

1. Evaluate `list`. If the result is not a List, this is a **runtime error**.
2. If the list is empty, this is a **runtime error**.
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

## Coroutines

A coroutine is a suspendable computation created with `coroutine.create(fn)`
(§05-stdlib). It runs one function at a time, can pause at a `yield`, and can
resume later — all on a single thread. See §03-types for the `Coroutine`
type and its four states.

### `coroutine.create`

`coroutine.create(fn)` returns a new, suspended `Coroutine` value. `fn` must
be callable; any other value is a runtime error. The call does not run `fn`.

### `co.resume`

`co.resume(...)` starts or continues the coroutine `co`.

1. If `co` is dead, this is a **runtime error** (`DeadCoroutineError`).
2. If `co` is running or normal, this is a **runtime error**
   (`RunningCoroutineError`).
3. On the **first** resume, `fn` is called with the resume's arguments,
   exactly as an ordinary call, so the same arity check applies.
4. On each **later** resume, the single argument (or `nil` if none) becomes
   the value of the pending `yield` expression.
5. `co` then runs until it yields, returns, or throws:
   - If `co` yields `v`, `co` becomes suspended and the `resume` call
     evaluates to `v`.
   - If `fn` returns `v`, `co` becomes dead and the `resume` call evaluates
     to `v`.
   - If a throw propagates out of `fn`, `co` becomes dead and the throw
     continues in the resumer's context, where it is catchable like any
     other throw.

### `co.status`

`co.status()` returns a String naming the coroutine's current state:
`"suspended"`, `"running"`, `"normal"`, or `"dead"`. See §03-types for what
each state means.

### Abandoned coroutines

A coroutine that is never resumed to completion when the program ends is
abandoned: its pending deferred calls do not run. This mirrors the fact that
a `yield` runs no deferred call — a deferred call in a coroutine runs only
when its function returns or a throw unwinds past the frame, exactly as in
an ordinary function.

The two `resume` faults and the `yield` fault are catchable: each is
delivered to a `catch` block as an `Error` value, with the kinds listed in
[Runtime Errors](#runtime-errors).

---

## Workers and Tasks

A program runs as a set of **tasks** on a set of **workers**. A worker owns
its tasks, its own global variables, and its own values. Workers share no
mutable state: a value passes from one worker to another only as a
[message](03-types.md#messages). A task is a computation on one worker. It
owns one **mailbox**, a first-in first-out queue of messages. A task never
moves to another worker.

The primitive is spawn, send, and receive: the members of the global `task`
object (§05-stdlib). Channels, pools, and other patterns are libraries over
these.

### The main task

A program starts with one worker, the **main worker**, which runs one task,
the **main task**. The main task runs the top-level code of the program. It
has a mailbox and a handle, like every other task.

Every task is a coroutine on its worker, and the main task is one too. A
`yield` that the main task reaches outside every coroutine it resumed
suspends the main task, as the [`yield` Expression](#yield-expression)
states for every task.

### `task.spawn`

`task.spawn(fn)` makes a new task on the current worker and returns its
handle (a Task value, §03-types).

1. `fn` must be callable (a Function or BoundMethod); any other value is a
   runtime error.
2. The call does not run `fn`. The new task joins the back of the worker's
   run queue, and the caller continues.
3. When the new task first runs, it calls `fn` with no arguments, exactly as
   an ordinary call, so the same arity check applies.

The new task shares the global variables and the values of its worker with
every other task on that worker. A closure that `fn` holds reaches the same
bindings after the spawn as before it.

### `task.spawn_worker`

`task.spawn_worker(fn, msg)` makes a new worker with one new task, and
returns the handle of that task. The caller continues at once.

1. `fn` must be a function that a `fun` declaration at the top level of the
   program made. Any other value is a runtime error.
2. If `msg` is not a message, the call raises `SendError`, and no worker is
   made.
3. The new worker starts with its own global variables. It defines every
   top-level `fun`, `class`, and `enum` declaration of the program, in
   source order, as those declaration statements define them. No other
   top-level statement runs on the new worker. A top-level `var`
   declaration does not run there, so a read of its name on the new worker
   is an undefined-variable read (`UndefinedVariableError`) until that
   worker assigns it.
4. The new task then calls the function of the same top-level `fun`
   declaration that made `fn`, with `msg` as its one argument, exactly as an
   ordinary call.

Steps 3 and 4 run in the new task. A fault while the new worker defines a
declaration — for example a `class D < Base` where `Base` is a top-level
`var`, so the name is undefined there — is a fault of that task. It occurs
before the entry runs, and it ends the program as every uncaught fault of a
task does (see [Ending](#ending)). `task.spawn_worker` has already returned
in the caller.

The two workers share nothing that can change. A write to a global variable
on one worker is not visible on another.

An enum value that one worker made and another worker received matches the
constructor patterns of its declaration on the receiving worker, exactly as
a value made there does.

### `task.send`

`task.send(t, msg)` appends `msg` to the mailbox of the task `t`, and
returns `nil`.

1. `t` must be a Task; any other value is a runtime error.
2. If `msg` is not a message, the call raises `SendError`. The check is the
   same when `t` is on the current worker and when it is on another worker.
3. If the task `t` has ended, the call raises `DeadTaskError`. This includes
   a task whose worker has ended.
4. Otherwise `msg` is appended to the mailbox of `t`. If `t` waits in
   `task.receive()`, `t` becomes ready to run (see
   [Scheduling](#scheduling)).

A send never suspends the sender, and a send never fails for lack of room:
a mailbox has no bound. The sender continues with its next step at once.

**Order.** The messages that the tasks of one worker send to one target
task arrive in the order of the sends. Messages from different workers to
one target can arrive in any order relative to each other.

**A send to another worker can race with the end of its target.** When the
target task ends on another worker at the same time as the send, the send
either raises `DeadTaskError` or appends the message to the mailbox, which
the end of the task then discards. A send that starts after the target has
ended always raises. A program that must know that a message arrived asks
the receiver for a reply.

### `task.receive`

`task.receive()` removes the message at the head of the mailbox of the
current task, and returns it.

1. If the mailbox is not empty, the call returns the head at once. It does
   not suspend.
2. If the mailbox is empty, the current task suspends until a message
   arrives. Then the call returns that message.
3. If the mailbox is empty while a native callback is on the call chain —
   for example inside `__str__` while `print` called it — or while a
   deferred call runs, the call raises `YieldAcrossNativeError`, for the
   same reason a `yield` there does (see [`yield` Expression](#yield-expression)).

A native function or bound native that runs as the entry of a task or a
coroutine is on the call chain for this rule. A receive below it raises
`YieldAcrossNativeError` in the same way: the entry has no frame that a
task switch can rebuild. This does not change the [`yield` Expression](#yield-expression)
rule: a yield inside a coroutine that such an entry resumed is legal,
because the entry's frame is below the resume point and keeps running.

A receive suspends the whole task. Every call frame of the task stays in
place, which includes the frames of a coroutine that the task resumed, and
no deferred call runs. A coroutine that holds a suspended receive keeps the
status `"running"`, so a resume of it from another task is a
`RunningCoroutineError`. When the task runs again, the receive returns
inside that coroutine.

### `task.self` and `task.pending`

`task.self()` returns the handle of the current task.

`task.pending()` returns the number of messages in the mailbox of the
current task. A task can read only the length of its own mailbox.

### Suspend Points

A task runs alone on its worker from one suspend point to the next: no
other task on the same worker runs in between, so no other task can change a
value that the task can see. A task switches to another task only at a
suspend point. There is no preemption.

The table below lists the suspend points. The `Default set` column says
whether a point is in the default set, which applies to every worker. The
`Fair mode` column is reserved. A later version can define an opt-in fair
mode per worker, which adds points under a flag: such a row has `no` in the
`Default set` column and names its flag in the `Fair mode` column, and it
changes nothing for a worker that does not enable that flag. No row of the
default set names a flag.

| Point | Class | Condition | Default set | Fair mode |
|---|---|---|---|---|
| `yield` | yield | in any task, outside every coroutine that the task resumed | yes | — |
| `task.receive()` | receive | the mailbox is empty | yes | — |
| `input()` | blocking call | every call | yes | — |
| `sleep(seconds)` | blocking call | every call | yes | — |
| `connect(host, port)` | blocking call | every call | yes | — |
| `run(program, args)` | blocking call | every call | yes | — |
| `Socket.read()` | blocking call | every call | yes | — |
| `Socket.read_bytes(n)` | blocking call | every call | yes | — |
| `Socket.readline()` | blocking call | every call | yes | — |
| `Socket.readlines()` | blocking call | every call | yes | — |
| `Socket.write(text)` | blocking call | every call | yes | — |
| `Socket.writeline(text)` | blocking call | every call | yes | — |
| `Server.accept()` | blocking call | every call | yes | — |
| `Process.read()` | blocking call | every call | yes | — |
| `Process.read_bytes(n)` | blocking call | every call | yes | — |
| `Process.readline()` | blocking call | every call | yes | — |
| `Process.readlines()` | blocking call | every call | yes | — |
| `Process.read_err()` | blocking call | every call | yes | — |
| `Process.err_read_bytes(n)` | blocking call | every call | yes | — |
| `Process.err_readline()` | blocking call | every call | yes | — |
| `Process.err_readlines()` | blocking call | every call | yes | — |
| `Process.write(text)` | blocking call | every call | yes | — |
| `Process.writeline(text)` | blocking call | every call | yes | — |
| `Process.wait()` | blocking call | every call | yes | — |

The blocking-call rows are exactly the calls in the [Blocking
calls](05-stdlib.md#blocking-calls) table of §05-stdlib. A send is never a
suspend point, and a spawn is never one.

**Blocking calls.** A blocking call blocks the worker that makes it: no task
on that worker runs until the call returns. Tasks on other workers keep
running, on an implementation that runs workers at the same time (see
[Conformance](README.md#conformance), Scheduling). A blocking call is in the table so that a later version can let
other tasks of the worker run while the call waits, with no change to the
rule above.

A task that computes without reaching a suspend point keeps every other task
of its worker from running. A program gives such a task a worker of its own.

### Scheduling

Each worker has one run queue of tasks that are ready to run. The queue is
first in, first out, and it has no priorities.

1. A new task that `task.spawn` makes joins the back of the run queue.
2. A task that suspends at a `yield` joins the back of the run queue. The
   main task follows the same rule.
3. A task that suspends in `task.receive()` leaves the run queue. When a
   message arrives in its mailbox, it joins the back of the run queue.
4. When the running task suspends or ends, the task at the head of the run
   queue runs next.

The order in which the tasks of one worker run is therefore defined. The
order of steps on different workers is not, except where messages order
them.

### Ending

A task ends when its function returns. The return value is discarded, and
every message still in its mailbox is discarded. A worker other than the
main worker ends when every task on it has ended.

The program ends when the main task ends, at the end of the top-level code.
Every other task on every worker is then abandoned, as an [abandoned
coroutine](#abandoned-coroutines) is: its pending deferred calls do not
run.

A call of `exit(code)` in any task ends the program at once, with that exit
code. A throw that no `try` statement in its task catches is uncaught: it
ends the program as an uncaught runtime error does ([`throw`
Statement](#throw-statement), step 5); every other task is abandoned. A
halting fault in any task ends the program.

A `print` statement writes its whole line at once: lines that tasks on
different workers print never mix inside one line.

### Deadlock

When the main task waits in `task.receive()`, no task on any worker is
running or ready to run, no task is in a blocking call, and no message is on
its way to a task, the program can never progress. It then ends with
`DeadlockError`. It is a halting fault (see [Halting
Faults](#halting-faults)). The report can list the tasks that wait.

### Worker and Task Faults

| Kind | Raised when | Catchable |
|---|---|---|
| `SendError` | `task.send(t, msg)` or `task.spawn_worker(fn, msg)`, where `msg` is not a message | yes |
| `DeadTaskError` | `task.send(t, msg)`, where the task `t` has ended, which includes a task whose worker has ended | yes |
| `YieldAcrossNativeError` | `task.receive()` on an empty mailbox while a native callback is on the call chain, or while a deferred call runs | yes |
| `DeadlockError` | the main task waits in `task.receive()` and no task can run again | no |

A catchable fault in this table is delivered to a `catch` block as an
`Error` value whose `kind` is the `Kind` column. `YieldAcrossNativeError` is
the kind that the [Runtime Errors](#runtime-errors) table gives a `yield`
across a native callback; a receive uses the same kind. A fault that is not
catchable is a halting fault (see [Conformance](README.md#conformance)).

### Bounding a Mailbox (Non-Normative)

This note is not part of the specification. A mailbox has no bound, so a
producer that sends faster than its consumer receives makes the mailbox of
the consumer grow without limit, and `task.pending()` shows that growth.
The bound is a library over the primitive, not a rule of the primitive: a
credit channel, in which the consumer gives the producer a fixed number N of
credits, the producer sends only while it holds one, and the consumer
returns one credit after each message it takes. A producer with no credit
waits in its own `task.receive()`, never on the queue of another task, and
the mailbox of the consumer holds at most N messages of the channel.
`examples/credit_channel.lox` is such a library. The credit channel is the
recommended form for every edge where a producer can outrun its consumer.

A program that uses credit channels stays free of deadlock when its
blocking waits form a directed acyclic graph of ranks. Give each task a
rank, and let each channel go from a task of a lower rank to a task of a
higher rank. A producer then waits only for a credit from a consumer of a
higher rank, and only while that consumer has at least one item that it
has not taken. This is true when the producer waits to send an item, and
when it waits to close the channel. The task of the highest rank waits for
no credit. So, when every consumer takes each item it gets, some task can
always run, from the highest rank down. A cycle of channels breaks this
rule, and then two full channels can wait on each other. A task between two
ranks holds two ends, so its mailbox gets the items of one channel and the
credits of the other. The library in `examples/credit_channel.lox` lets a
task hold one end only.

---

## Runtime Errors

A runtime error halts the program (see [Conformance](README.md#conformance),
Halts) — **unless** it is catchable and it happens during the execution of a `try` statement's
`tryBlock` (directly, or inside any function called from within it), in
which case it does not halt the program; instead it is delivered to that
`try` statement's `catchBlock` as an `Error` value, exactly as the
[`throw` Statement](#throw-statement) describes for any other thrown
value. An error that occurs during the execution of the `catchBlock`
itself is not caught by that same `try` statement. A `StackOverflowError`
that is raised while another `StackOverflowError` is already unwinding —
that is, by a deferred call that runs during that unwind — is not
delivered to any `catchBlock`; it is a halting fault.
Every cause in the table below is catchable this way; see
[Halting Faults](#halting-faults) for the faults that are not.

The faults of workers and tasks have their own table, [Worker and Task
Faults](#worker-and-task-faults).

The table lists each cause, an example, and the `kind` field
([§03-types](03-types.md#error)) of the `Error` value delivered to
`catchBlock` when it is caught. The `Error`'s `message` field is a String
whose text is implementation-defined.

| Cause | Example | `Error.kind` |
|---|---|---|
| Arithmetic on non-Numbers | `"a" - 1` | `"ArithmeticTypeError"` |
| Comparison on non-Numbers | `"a" < 1` | `"ComparisonTypeError"` |
| `+` on incompatible types | `1 + "a"` | `"ConcatenationTypeError"` |
| Call of a non-callable value | `42()` | `"NotCallableError"` |
| Wrong argument count | `fun f(a) {} f(1, 2)` | `"ArityError"` |
| Undefined global variable | `print undeclared;` | `"UndefinedVariableError"` |
| Call stack overflow | Unbounded recursion that is not a [self tail call](#self-tail-calls) | `"StackOverflowError"` |
| Index of non-List/non-Map | `42[0]` | `"NotIndexableError"` |
| Non-Number List or String index | `list["a"]` | `"IndexTypeError"` |
| Fractional List or String index | `list[1.5]` | `"IndexNotIntegerError"` |
| List or String index out of bounds | `[][0]` | `"IndexOutOfBoundsError"` |
| `pop` on empty list | `[].pop()` | `"EmptyListError"` |
| NaN used as map key | `m[0/0] = 1` | `"NaNKeyError"` |
| Object without `__hash__`/`__eq__` used as map key | `m[[1,2]] = 1` | `"InvalidMapKeyError"` |
| Method called on non-instance/non-list/non-map | `42.foo()` | `"InvalidReceiverError"` |
| No arm matches in a `match` expression | `match 99 { case 1 => "one" }` | `"MatchError"` |
| Constructor called with wrong arity | `ok(1, 2)` when `ok` takes one field | `"ConstructorArityError"` |
| Undefined property on an `Error` value | `try { try { [][0]; } catch (e) { e.foo; } } catch (_) { }` | `"UndefinedPropertyError"` |
| Operator method returned a non-Boolean | `class C { __eq__(o) { return 42; } } C() == C()`, or the reflected `class C { __rlt__(o) { return 42; } } 1 < C()` | `"OperatorResultTypeError"` |
| Operator method returned a non-Number | `class C { __len__() { return "x"; } } len(C())` or `class C { __hash__() { return "x"; } __eq__(o) { return true; } }` used as a map key | `"OperatorResultTypeError"` |
| A key's `__hash__` or `__eq__` writes to the Map it keys | `var m = {}; class K { __hash__() { m[1] = 1; return 1; } __eq__(o) { return true; } } m[K()] = 1;` | `"MapChangedError"` |
| `resume` on a dead coroutine | `fun f() { return 1; } var co = coroutine.create(f); co.resume(); co.resume();` | `"DeadCoroutineError"` |
| `resume` on a running coroutine (self-resume) | `fun f() { co.resume(); } var co = coroutine.create(f); co.resume();` | `"RunningCoroutineError"` |
| `yield` across a native callback | `class S { __str__() { yield 1; return "x"; } } fun f() { print S(); } var co = coroutine.create(f); co.resume();` | `"YieldAcrossNativeError"` |
| `yield` in a deferred call | `fun note() { var v = yield 1; } fun g() { defer note(); } g();` | `"YieldAcrossNativeError"` |

**The two map-key rows above cover an index read or write, a map or set
literal, and `in`.** `Map.has(key)` and `Map.del(key)` are stdlib native
methods, not one of those four forms, and an invalid key given to either
is a halting fault — see the `Map.has`/`Map.del` row of [Halting
Faults](#halting-faults).

### Halting Faults

Each cause below is a **halting fault**: it halts the program, even inside a
`try` statement's `tryBlock` (see [Conformance](README.md#conformance)). An
implementation may make a halting fault catchable instead, with an
implementation-defined `kind`; a program that catches one is not portable.
A halting fault has no `kind` that this specification defines, and the text
of its report is implementation-defined. Whether the pending deferred calls
run before the program ends is implementation-defined, as for every halt.

| Cause | Example |
|---|---|
| A constructor pattern is matched against a value that is not an enum value | `enum Result { Ok(v) Err(m) } match 1 { case Ok(v) => v case Err(m) => -1 };` |
| `print` of a value nested deeper than the implementation's canonical-string depth limit | `var a = [1]; var i = 0; while (i < 300) { a = [a]; i = i + 1; } print a;` |
| A native function is called with an argument count other than its arity | `clock(1);` |
| A `defer`red call holds a value that is not a Closure, Native, BoundMethod, or BoundNative | `fun g() { var x = 42; defer x(); } g();` (a static check cannot see that the variable holds a value that is not callable) |
| A stdlib native function reports its own error while running | `open("/no/such/path", "r");` |
| `Map.has(key)` or `Map.del(key)` called with an invalid key | `var m = {}; m.has([1, 2]);` |
| Undefined property read on a File value | `var f = open("/tmp/f.txt", "w"); f.write("x"); var g = open("/tmp/f.txt", "r"); g.bogus;` (a missing path fails first with the stdlib-error row above) |
| Undefined property read on a Socket value | `var l = listen("127.0.0.1", 0); var s = connect("127.0.0.1", l.port()); s.bogus;` |
| Undefined property read on a Server value | `var l = listen("127.0.0.1", 0); l.bogus;` |
| Undefined property read on a Process value | `var p = spawn("echo", []); p.bogus;` |
| Undefined property read on a Map value | `var m = {}; m.bogus;` |
| Property read on an Instance where the name is neither a field nor a method | `class C {} var c = C(); c.bogus;` |
| Property read on a value that is not an Instance, Map, File, or Error | `42.foo;` (see [§03-types, Error](03-types.md#error) and [Property Get](#property-get)) |
| Property write on a value that is not an Instance | `42.foo = 1;` |
| A field looked up by [Method Invocation](#method-invocation) shadows the method name but is not callable | `class C { init() { this.f = 1; } } C().f();` |
| `obj.method(...)` where `method` is not found on the instance's class | `class C {} C().bogus();` |
| `list.append(...)` called with an argument count other than 1 | `[].append();` |
| `list.pop(...)` called with any argument | `[].pop(1);` |
| `list.remove(...)` called with an argument count other than 1 | `[1].remove();` |
| `list.remove(value)` where `value` is not present | `[1, 2].remove(3);` |
| Undefined method invoked on a List | `[].bogus();` |
| Undefined method invoked on a File | `var f = open("/tmp/f.txt", "w"); f.write("x"); var g = open("/tmp/f.txt", "r"); g.bogus();` (a missing path fails first with the stdlib-error row above) |
| Undefined method invoked on a Socket | `var l = listen("127.0.0.1", 0); var s = connect("127.0.0.1", l.port()); s.bogus();` |
| Undefined method invoked on a Server | `var l = listen("127.0.0.1", 0); l.bogus();` |
| Undefined method invoked on a Process | `var p = spawn("echo", []); p.bogus();` |
| Undefined method invoked on a Map | `var m = {}; m.bogus();` |
| `class Sub < Super {}` where `Super` is not a Class | `var NotAClass = 1; class Sub < NotAClass {}` |
| `super.method(...)` where `method` is not found on the superclass | `class A {} class B < A { m() { super.zzz(); } } B().m();` |
| Enum field indexed (`enumVal[i]`) with a non-Number index | `enum E { A(x) } var v = A(1); v["a"];` |
| Enum field indexed with an out-of-range index | `enum E { A(x) } var v = A(1); v[3];` |
| Index-assignment on a String | `"abc"[0] = "x";` (strings are immutable — see [§03-types, String](03-types.md#string)) |
| `seq[start:end]` where `seq` is not a List or String | `(42)[0:1];` |
| `seq[start:end]` where `start` is not a Number | `[1, 2]["a":2];` |
| `seq[start:end]` where `start` is not integer-valued | `[1, 2][1.5:2];` |
| `seq[start:end]` where `start` is negative | `[1, 2][-1:2];` |
| `seq[start:end]` where `end` is not a Number | `[1, 2][0:"a"];` |
| `seq[start:end]` where `end` is not integer-valued | `[1, 2][0:1.5];` |
| `seq[start:end]` where `end` is negative | `[1, 2][0:-1];` |
| `elem in seq` where `seq` is a String and `elem` is not a String | `1 in "abc";` |
| `elem in seq` where `seq` is not a List, String, or Map | `1 in 42;` |
| `for (var x in expr)` where `expr` is not a List, String, Map, or Coroutine | `for (var x in 42) {}` |
| Map size changed during `for`-in iteration | `var m = {1: 1}; for (var k in m) { m[2] = 2; }` |

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
