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

`true`, `false`, `nil`, NUMBER, INT, STRING each evaluate to the corresponding
value. A NUMBER gives a Number and an INT gives an Int
([§01-lexical](01-lexical.md#number-and-int-literals)).

### Identifier

Evaluating an identifier looks up the variable's current value in the scope
chain.

### Constructor Reference

```
E.A
```

When `E` is the name of an enum, `E.A` is not a property get. It evaluates to
the constructor `A` of `E`, and it cannot fault at run time. The compiler
resolves it: an `E.A` that names no constructor of `E`, or that comes before
the declaration of `E`, is a compile error
([§02-syntax](02-syntax.md#enum-declaration)).

### Grouping

`( expr )` evaluates `expr` and returns its value; parentheses only affect
parsing precedence.

### Arithmetic

| Expression | Requirement | Result |
|---|---|---|
| `a + b` | Both Numbers, both Ints, **or** both Strings | Sum (Number or Int) or concatenated string |
| `a - b` | Both Numbers, **or** both Ints | Difference |
| `a * b` | Both Numbers, **or** both Ints | Product |
| `a / b` | Both Numbers, **or** both Ints | Quotient |
| `a % b` | Both Numbers, **or** both Ints | Remainder (floor-division semantics) |
| `-a` | Number **or** Int | Negation |

Any violation of the type requirement is a **runtime error**. A Number and an
Int together violate it: the two kinds do not mix (see [§03-types,
Int](03-types.md#int) and [Operand Kinds](#operand-kinds)).

String concatenation (`+`) creates a new string whose content is the
characters of the left operand followed by the characters of the right
operand.

Number division follows IEEE 754; dividing by zero produces `Infinity` or
`-Infinity` (not an error).

Number modulo (`%`) is computed as `fmod(a, b)` adjusted so the result has the same sign as `b` (floor-division semantics, same as Python and Lua). `a % 0` produces `NaN` (consistent with `a / 0` producing `Infinity`).

**Int arithmetic.** On two Ints, `+`, `-`, `*`, and unary `-` wrap in two's
complement. `a / b` is floor division, which rounds toward negative infinity,
and `a % b` is floor modulo, which has the sign of `b`, so
`a == (a / b) * b + a % b` for every `a` and every `b` other than `0i`. A
zero divisor of `/` or `%` is a catchable fault of kind `DivisionByZeroError`.
The smallest Int divided by `-1i` wraps to the smallest Int, and the smallest
Int modulo `-1i` is `0i`: neither is a fault. An Int has no infinity and no
NaN.

### Comparison

| Expression | Requirement | Result |
|---|---|---|
| `a < b` | Both Numbers, both Ints, **or** both Strings | Boolean |
| `a <= b` | Both Numbers, both Ints, **or** both Strings | Boolean |
| `a > b` | Both Numbers, both Ints, **or** both Strings | Boolean |
| `a >= b` | Both Numbers, both Ints, **or** both Strings | Boolean |

Two Numbers compare by numeric value. Two Ints compare by their signed
value, for every value of the 64 bits. Two Strings compare as byte sequences:
`<` and `>` compare byte by byte from the first byte, using each byte's
unsigned value (0 through 255), and a String that is a prefix of the other is
the smaller. So `"Z" < "a"` is true and `"ab" < "abc"` is true.

Any violation is a **runtime error**. A comparison of a String with a Number,
a Number with an Int, or any other mixed pair, is a runtime error.

### Equality

`a == b` and `a != b` are defined for all type combinations and never produce
a runtime error. See [03-types.md § Equality](03-types.md#equality). An Int
and a Number are never equal: `1 == 1i` is `false` and `1 != 1i` is `true`.

### Operand Kinds

The table gives, for every arithmetic, comparison, equality, bit, and
negation operator, the result for two Ints, for two Numbers, and for an Int
together with a Number in either order. A fault in a cell is a catchable
fault of the kind named (see [Runtime Errors](#runtime-errors)). For
a unary operator the first column is an Int operand, the second a Number
operand, and the third does not apply. A cell that says "as today" is the
behavior of the Number operators before the Int existed. An operand that is a
String, an Instance, or any other kind follows [Arithmetic](#arithmetic),
[Comparison](#comparison), and [Operator Overloading](#operator-overloading)
as before, and the bit operators fault on it (see [Bit
Operators](#bit-operators)).

| Operation | Int, Int | Number, Number | Int and Number |
|---|---|---|---|
| `a + b` | Int, wraps | Number, as today | Fault `ConcatenationTypeError` |
| `a - b` | Int, wraps | Number, as today | Fault `ArithmeticTypeError` |
| `a * b` | Int, wraps | Number, as today | Fault `ArithmeticTypeError` |
| `a / b` | Int, floor division; a zero divisor is `DivisionByZeroError`; the smallest Int divided by `-1i` is the smallest Int | Number, as today | Fault `ArithmeticTypeError` |
| `a % b` | Int, floor modulo; a zero divisor is `DivisionByZeroError`; the smallest Int modulo `-1i` is `0i` | Number, as today | Fault `ArithmeticTypeError` |
| `a < b` | Boolean | Boolean, as today | Fault `ComparisonTypeError` |
| `a <= b` | Boolean | Boolean, as today | Fault `ComparisonTypeError` |
| `a > b` | Boolean | Boolean, as today | Fault `ComparisonTypeError` |
| `a >= b` | Boolean | Boolean, as today | Fault `ComparisonTypeError` |
| `a == b` | Boolean, by value | Boolean, as today | `false`, never a fault |
| `a != b` | Boolean, by value | Boolean, as today | `true`, never a fault |
| `a & b` | Int | Fault `BitwiseTypeError` | Fault `BitwiseTypeError` |
| `a \| b` | Int | Fault `BitwiseTypeError` | Fault `BitwiseTypeError` |
| `a ^ b` | Int | Fault `BitwiseTypeError` | Fault `BitwiseTypeError` |
| `a << b` | Int, wraps; a count outside `0i` to `63i` is `ShiftCountError` | Fault `BitwiseTypeError` | Fault `BitwiseTypeError` |
| `a >> b` | Int; a count outside `0i` to `63i` is `ShiftCountError` | Fault `BitwiseTypeError` | Fault `BitwiseTypeError` |
| `a >>> b` | Int; a count outside `0i` to `63i` is `ShiftCountError` | Fault `BitwiseTypeError` | Fault `BitwiseTypeError` |
| `-a` | Int, wraps; the negation of the smallest Int is itself | Number, as today | — |
| `~a` | Int | Fault `BitwiseTypeError` | — |
| `!a` | `false`: an Int is always truthy, `0i` included | As today | — |

In C, the smallest Int divided by `-1` and the smallest Int modulo `-1` are
undefined. Here both are defined, and neither is a fault. No Int operation is
an overflow fault.

### Bit Operators

The operators `&`, `|`, `^`, `~`, `<<`, `>>`, and `>>>` work on Ints only.

- Every operand must be an Int. A Number operand is a fault, an integer-valued
  one included: `1 & 1` and `1i & 1` are both `BitwiseTypeError`. So is an
  operand of any other kind, a String, an Instance, and `nil` included. The
  shift count is an operand, so a count that is not an Int is also a
  `BitwiseTypeError`.
- `a & b`, `a | b`, and `a ^ b` work on all 64 bits. `~a` inverts every bit:
  `~0i` is `-1i`.
- `a << n` shifts the 64 bits of `a` left by `n` places and fills the right
  with zero bits. Bits shifted out of the left end are lost, so the result
  wraps: `1i << 63i` is the smallest Int.
- `a >> n` shifts right by `n` places and copies the sign bit into the left
  end: `-8i >> 1i` is `-4i`, and `-1i >> 63i` is `-1i`.
- `a >>> n` shifts right by `n` places and fills the left end with zero bits:
  `-1i >>> 63i` is `1i`.
- The count `n` must be an Int from `0i` to `63i`, and a count of `0i` leaves
  `a` as it is. A count outside that range is a catchable fault of kind
  `ShiftCountError`. The operands are evaluated left to right, and the kind of
  both operands is checked before the range of the count, so `1 << 64i` is a
  `BitwiseTypeError`.
- The bit operators have no operator protocol and no reflected form (see
  [Operator Overloading](#operator-overloading)).

### Int Edge Cases

Each row is an Int operation whose result is defined and is not a fault. A
program prints the result of the example, and the result column is the text it
prints.

| Row | Case | Example | Result |
|---|---|---|---|
| `min-div-neg1` | The smallest Int divided by `-1i` wraps to itself | `print -9223372036854775808i / -1i;` | `-9223372036854775808` |
| `min-mod-neg1` | The smallest Int modulo `-1i` is `0i` | `print -9223372036854775808i % -1i;` | `0` |
| `neg-min` | The negation of the smallest Int is itself | `print -(-9223372036854775808i);` | `-9223372036854775808` |
| `max-plus-1` | The largest Int plus `1i` wraps to the smallest | `print 9223372036854775807i + 1i;` | `-9223372036854775808` |
| `int-number-equal` | An Int is never equal to a Number | `print 1 == 1i;` | `false` |
| `zero-truthy` | `0i` is truthy | `print 0i and "truthy";` | `truthy` |

### Operator Overloading

An Instance may define special methods ("dunder" methods) that the operators
above dispatch to. The dispatch order is fixed:

1. **Built-in branch first.** The operator's ordinary behaviour applies when
   the operands match the built-in types (two Numbers or two Ints for
   arithmetic; two Numbers, two Ints, or two Strings for comparison; two
   Numbers, two Ints, two Strings, or the concatenation case for `+`; List,
   String, or Map for `in`). A Number together with an Int does not match, so
   the operator goes on to step 2.
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
| `a & b`, `a \| b`, `a ^ b`, `~a`, `a << b`, `a >> b`, `a >>> b` | none: a bit operator has no protocol | — |
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

**Bit operators have no protocol.** The operators `&`, `|`, `^`, `~`, `<<`,
`>>`, and `>>>` work on Ints only. They have no method in the first table, no
reflected method in the second, and no other hook. An Instance operand, on
either side, raises `BitwiseTypeError` even when its class defines a method
that is named like an operator, because method lookup reads only the names
that the tables list. A program that wants a bit operator on its own type
writes a method and calls it by name.

`a != b` is `==` then logical negation: it derives from `__eq__` (the left
operand's, or the reflected one on the right) and has no method of its own.
`a <= b` is `!(a > b)` and `a >= b` is `!(a < b)`; each dispatches `__gt__` /
`__lt__` on the left, or the reflected `__rgt__` / `__rlt__` on the right, and
negates the result, with no `__le__` / `__ge__` method.

The result of `__eq__`, `__lt__`, `__gt__`, and `__contains__` must be a
Boolean, whether the method ran on the left operand or as the reflected method
on the right. The result of `__len__` and `__hash__` must be a Number, the
result of `__iter__` must be a List, String, Map, or Coroutine, and the result of
`__str__` must be a String. An Int is not a Number, so an Int result of
`__len__` or `__hash__` is an error too. Any other result raises a catchable
`OperatorResultTypeError` (see [Runtime Errors](#runtime-errors)). The
result of every other method in the tables above is unconstrained: `__add__`
may return an Int.

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
falsy. Always returns a Boolean. An Int is always truthy, so `!0i` is `false`.

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

A `yield` reached while a native callback is between the resume of the
coroutine and the `yield` — for example inside `__str__`, `__hash__`, or
`__eq__` while `print` or a map operation called it, and that call runs
inside the coroutine — is a **runtime error** (`YieldAcrossNativeError`; see
[Runtime Errors](#runtime-errors)). A native callback's own continuation is
not part of the coroutine's stack and cannot be suspended, so such a yield
cannot resume. A coroutine that the callback itself creates and resumes can
yield to the callback, because the resume and the `yield` are in the same
native frame.

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
   a. Evaluate the pattern against the stored subject (constructor test, literal equality, class test, or wildcard). The alternatives of an arm are tried from left to right, and the first that matches is used.
   b. If the pattern does not match, proceed to the next arm.
   c. If the pattern matches and a guard (`if expr`) is present, evaluate the guard. If the guard is falsy, proceed to the next arm.
   d. If the pattern matches (and the guard, if present, is truthy), evaluate the arm's body **expression** and return its value as the result of the `match` expression.
3. If no arm matches: raise **`MatchError`** at runtime. The `match` expression does not produce a value.

An arm that follows an unguarded catch-all arm (`case _` or a plain binding) is unreachable and is a compile error.

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

**Constructor patterns.** A pattern of the form `E.A`, `E.A(...)` or `E.A{...}` is a constructor pattern ([§02-syntax](02-syntax.md#extended-match-arm-patterns) states which forms are compile errors). It matches a value only when the constructor `A` of the declaration of `E` made that value. A value that a constructor of another enum made does not match, even when that constructor has the same name or the same position in its declaration. A value that is not an enum value does not match either; the test never faults. When the pattern matches, `E.A(x, y)` binds each name to the field at its position, and `E.A{f}` binds each name to the field of that name; `_` binds nothing. A bare name is never a constructor pattern, even when a constructor of that name exists: a bare name is the wildcard `_`, a binding, or a class test.


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

A task can suspend inside the body of a `for`-in statement (see [Suspend
Points](#suspend-points)). Another task of the worker then runs, and it can
change the iterated Map. The next iterator step of the suspended task is then a
runtime error that halts the program, as above. `catch` cannot stop it. So a
`for`-in over a Map that another task can change must not contain a call that
can suspend. A program that must change such a Map lets one task own it and
makes the other tasks ask that task by message.

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
   a. If `key` is neither a Number nor an Int, this is a **runtime error**.
   b. If `key` is a Number and not an integer-valued number (i.e.,
      `key ≠ floor(key)`), this is a **runtime error**. An Int is always
      integer-valued.
   c. Let `i` be the integer value of `key`.
   d. If `i < 0` or `i ≥ length(collection)`, this is a **runtime error**.
      The comparison is exact for every Int: an Int that no index can reach,
      such as the largest or the smallest Int, is out of bounds, never a
      wrapped or truncated index.
   e. For a List, the expression evaluates to `collection[i]`. For a String,
      it evaluates to the one-character String at position `i`.
5. If `collection` is an **Enum** value:
   a. If `key` is neither a Number nor an Int, this is a **halting fault**
      (see [Halting Faults](#halting-faults)).
   b. If `key` is a Number and not integer-valued (i.e., `key ≠ floor(key)`),
      this is a **halting fault**.
   c. Let `i` be the integer value of `key`.
   d. If `i < 0` or `i ≥` the number of fields of the Enum value, this is a
      **halting fault**.
   e. The expression evaluates to the field at position `i`.
6. If `collection` is an **Instance** whose class defines `__index_get__`,
   call it with `key` (see [Operator
   Overloading](#operator-overloading)); the expression evaluates to the
   method's return value.
7. Otherwise, this is a **runtime error**.

Note: `list[1]` and `list[1.0]` are identical — both are the same Number value
used as index 1 — and `list[1i]` reads the same element. A Map key is not an
index: `m[1]` and `m[1i]` are two different keys (see [§03-types,
Int](03-types.md#int)).

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
- If `start` is neither a Number nor an Int → runtime error.
- If `start` is a Number and not integer-valued (i.e., `start ≠ floor(start)`) → runtime error.
- If `start < 0` → runtime error.
- Let `s = integer value of start`.

**Step 4 — Validate `end`.**
- If `end` is neither a Number nor an Int → runtime error.
- If `end` is a Number and not integer-valued → runtime error.
- If `end < 0` → runtime error.
- Let `e = integer value of end`.

**Step 5 — Clamp bounds.**  
Let `n = len(seq)`.
- `s = min(s, n)`
- `e = min(e, n)`

The clamp is exact for every Int, so `[1, 2][0i:9223372036854775807i]` is
`[1, 2]`.

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

A coroutine whose function waits in a call of class suspends (see [Suspend
Points](#suspend-points)) keeps the status `"running"` for the whole wait,
also while other tasks run. So `co.status()` from another task returns
`"running"`, and a resume of `co` from another task is a
`RunningCoroutineError`.

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
object (§05-stdlib). Receive has three more forms, which take a message with
a limit on the wait, with a condition on the message, or with both. Channels,
pools, and other patterns are libraries over these.

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
   worker assigns it. In a session, the program is every line entered so far
   (see [A Session](#a-session)).
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
3. If the mailbox is empty while a native callback is between the start of
   the task and the receive — for example inside `__str__` while `print`
   called it — or while a
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

### `task.receive_within`, `task.receive_where`, and `task.receive_where_within`

These three calls are receives with a limit, with a condition, or with both.
Each has a fixed number of arguments.

- `task.receive_within(seconds)` removes the message at the head of the
  mailbox and returns it, as `task.receive()` does. It raises `TimeoutError`
  when no message arrives within `seconds`.
- `task.receive_where(want)` removes the first message of the mailbox for
  which `want` is true, and returns it. It waits until there is such a
  message.
- `task.receive_where_within(want, seconds)` is `task.receive_where(want)`
  with the limit of `task.receive_within(seconds)`.

1. `want`, when the call has it, must be a Function or a BoundMethod; any
   other value is a runtime error. `seconds`, when the call has it, must be a
   Number greater than zero; any other value, a NaN included, is a runtime
   error. `want` is checked first. A `seconds` so large that the wait cannot
   end is a limit that never passes.
2. The call tests the messages of the mailbox in order, from the head. For
   each message `m` it calls `want(m)` (for `task.receive_within`, the head
   is the selected message and `want` is not called). A message is
   **selected** when `want(m)` returns a value that is not `nil` and not
   `false`. The call removes the first selected message from the mailbox and
   returns it. A message that is not selected stays in the mailbox, and the
   messages keep their order, so a later receive sees them as if this call
   had not run.
3. When no message of the mailbox is selected, the current task suspends, as
   in `task.receive()`. Each time the task runs again after a message
   arrived, the call tests the messages that arrived since its last test, in
   order. The call calls `want` at most once for each message. The call ends
   when a message is selected.
4. The limit starts at the start of the call and covers the whole call: every
   wait, and the time that `want` takes. When the limit passes, the call
   raises `TimeoutError`, and the mailbox is unchanged. A message that is in
   the mailbox when the task runs again is tested before the limit counts: a
   message that arrives as the limit passes is not lost. It is returned if it
   is selected, and it stays in the mailbox if it is not.
5. A task that waits in one of these calls is not deadlocked while it has a
   limit: its limit can pass (see [Deadlock](#deadlock)).
6. If the call must suspend while a native callback is between the start of
   the task and the call, or while a deferred call runs, the call raises
   `YieldAcrossNativeError`, for the same reason as in
   [`task.receive`](#taskreceive). The limit does not change this rule: a
   call that finds a selected message does not suspend.

**The `want` function.** `want` runs in the current task, between two
suspend points of the task: the call is not a suspend point while `want`
runs. It is a context where a task cannot suspend (see [Contexts where a task
cannot suspend](#contexts-where-a-task-cannot-suspend)).

- A `yield` in `want`, or in a function that `want` calls, raises
  `YieldAcrossNativeError`. A call of class suspends blocks the worker, as the
  contexts rule says.
- A call of `task.receive()`, `task.receive_within`, `task.receive_where`, or
  `task.receive_where_within` in `want`, or in a function that `want` calls,
  raises `YieldAcrossNativeError`, also when the mailbox holds a message.
  Such a call would change the mailbox that this call is testing.
- A `task.send` in `want` works. A message that `want` sends to its own task
  is appended, and the call tests it, after the messages that were there.
  `task.pending()` in `want` counts the messages that are in the mailbox,
  the tested ones included.
- A throw that `want` does not catch ends the call, and it propagates from
  the call as from any other call. No message is removed. A halting fault in
  `want` halts the program, as everywhere.

**Selective receive.** A task that holds two channel ends, as in [Bounding a
Mailbox](#bounding-a-mailbox-non-normative), takes the items of one channel
and the credits of the other from one mailbox with `task.receive_where`:

```lox
var msg = task.receive_where(isCredit);
```

The items that `isCredit` does not select wait in the mailbox, in order,
until a call selects them.

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
| `task.receive_within(seconds)` | receive | the mailbox is empty | yes | — |
| `task.receive_where(want)` | receive | no message of the mailbox is selected | yes | — |
| `task.receive_where_within(want, seconds)` | receive | no message of the mailbox is selected | yes | — |
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

**Blocking calls.** A blocking call has one of two classes, which the
`Class` column of the [Blocking calls](05-stdlib.md#blocking-calls) table
gives.

- A call of class **suspends** waits for the network or for time: `sleep`,
  `connect`, and the Socket and Server calls. It suspends its task until the
  wait ends. The task leaves the run queue, and every other task of the worker
  runs while the wait lasts. Only the task is suspended.
- A call of class **blocks** waits for standard input or for another process:
  `input()`, `run`, and the Process calls. It blocks the worker that makes it:
  no task on that worker runs until the call returns. Tasks on other workers
  keep running, on an implementation that runs workers at the same time (see
  [Conformance](README.md#conformance), Scheduling).

A call of class suspends blocks the worker in two cases: where the task cannot
suspend (see [Contexts where a task cannot
suspend](#contexts-where-a-task-cannot-suspend)), and during the name
resolution of `connect`. Then the call waits as a call of class blocks does.

A call of class suspends waits only when the wait is not over at the call. A
`read` of a Socket that holds bytes returns at once, and a `sleep` of zero
seconds returns at once. Whether the wait is over at the call can depend on
the network, so a portable program does not depend on it.

A task that waits in a call of class suspends keeps all its call frames in
place, which includes the frames of a coroutine that the task resumed, and no
deferred call runs. A coroutine that holds such a call keeps the status
`"running"`, so a resume of it from another task is a `RunningCoroutineError`,
as for a suspended receive. When the wait ends, the call returns the value it
would have returned from a blocking call, or raises the fault it would have
raised. A fault of the wait is raised in the waiting task when it runs again.

A task that computes without reaching a suspend point keeps every other task
of its worker from running. A program gives such a task a worker of its own.

### Contexts where a task cannot suspend

The runtime calls some Lox++ code and needs its result before it continues. A
task cannot suspend inside such a call. A call of class suspends in one of
these contexts blocks the worker, as a call of class blocks does. It raises no
fault, and the result does not depend on the state of the network.

| Context | The runtime calls Lox++ code for |
|---|---|
| `__str__` | `print`, `str()`, an element or key of a List or Map that is printed, a `__str__` inside a `__str__`, an uncaught throw of an instance that is not an `Error` |
| a map key's `__hash__` and `__eq__` | `m[k]`, `m[k] = v`, a Map literal, `k in m`, `Map.has`, `Map.del`, and a hash collision |
| `reflect.callMethod` | the method, or a callable field |
| a receive condition | `want`, for each message that `task.receive_where` or `task.receive_where_within` tests |
| a deferred call | every drain: at a normal return, after a caught throw, after a run-time fault, after an uncaught throw, at the end of a task function, at the end of a coroutine function |
| a native entry | a task or coroutine whose entry is a native function or a bound native |
| a nested coroutine | each coroutine that the task resumes inside one of the contexts above |

A task can suspend at the top level, in a function, in a task, in `init`, in
every operator method and its reflected form, in `__lt__`, `__gt__`,
`__contains__`, `__call__`, `__index_get__`, `__index_set__`, `__len__`,
`__iter__`, and `__slice__`, in `==` and in `match` patterns, and in a
coroutine that the task resumes outside the contexts above.

`task.receive()` on an empty mailbox raises `YieldAcrossNativeError` in the
same contexts (see [`task.receive`](#taskreceive)), and so does a receive with
a limit or a condition that must suspend (see
[`task.receive_within`](#taskreceive_within-taskreceive_where-and-taskreceive_where_within)).
The two rules differ
because a receive has no way to continue without a message, and a call of
class suspends has a way: it waits in place.

### Scheduling

Each worker has one run queue of tasks that are ready to run. The queue is
first in, first out, and it has no priorities.

1. A new task that `task.spawn` makes joins the back of the run queue.
2. A task that suspends at a `yield` joins the back of the run queue. The
   main task follows the same rule.
3. A task that suspends in `task.receive()` leaves the run queue. When a
   message arrives in its mailbox, it joins the back of the run queue. A task
   that suspends in a receive with a limit or a condition follows the same
   rule, and it also joins the back of the run queue when its limit passes.
4. A task that suspends in a call that waits for the network or for time
   leaves the run queue. When its wait ends, it joins the back of the run
   queue.
5. When the running task suspends or ends, the task at the head of the run
   queue runs next.

When several waits have ended by the time the worker has no task that is
ready to run, the tasks join the run queue in this order: first the tasks
whose wait is for a connection, a Socket, or a Server and has ended, in the
order in which their calls began to wait, then the tasks whose sleep has
expired or whose limit has passed, in the order of their deadlines.

The order in which the tasks of one worker run is therefore defined, with one
limit: an event outside the program decides when a wait ends, and so which
tasks are in the run queue when a task suspends. The order of steps on
different workers is not defined either, except where messages order them.

### Ending

A task ends when its function returns. The return value is discarded, and
every message still in its mailbox is discarded. A worker other than the
main worker ends when every task on it has ended.

The program ends when the main task ends, at the end of the top-level code.
Every other task on every worker is then abandoned, as an [abandoned
coroutine](#abandoned-coroutines) is: its pending deferred calls do not
run. In a session the end of a line is not the end of the program, and the
main task does not end with it: see [A Session](#a-session).

When the program ends, a task that waits in a call of class suspends is
abandoned like any other task. The runtime closes the Sockets and Servers that
such a task waits on, because no deferred call will.

A call of `exit(code)` in any task ends the program at once, with that exit
code. A throw that no `try` statement in its task catches is uncaught: it
ends the program as an uncaught runtime error does ([`throw`
Statement](#throw-statement), step 5); every other task is abandoned. A
halting fault in any task ends the program.

A `print` statement writes its whole line at once: lines that tasks on
different workers print never mix inside one line.

### Deadlock

When the main task waits in a receive without a limit (`task.receive()` or
`task.receive_where(want)`), no task on any worker is running or ready to
run, no task waits in a blocking call, no task waits in a receive with a
limit, and no message is on its way to a task, the program can never
progress. A task that waits in a call of class suspends is a task that can
continue: its event can arrive, so it prevents a deadlock. A task that waits
in a receive with a limit is one too: its limit can pass. The program then
ends with `DeadlockError`. It is a halting fault (see [Halting
Faults](#halting-faults)). The report can list the tasks that wait in a
receive without a limit. It does not list a task that waits in a blocking call
or in a receive with a limit.

### Worker and Task Faults

| Kind | Raised when | Catchable |
|---|---|---|
| `SendError` | `task.send(t, msg)` or `task.spawn_worker(fn, msg)`, where `msg` is not a message | yes |
| `DeadTaskError` | `task.send(t, msg)`, where the task `t` has ended, which includes a task whose worker has ended | yes |
| `YieldAcrossNativeError` | `task.receive()` on an empty mailbox while a native callback is between the start of the task and the receive, or while a deferred call runs; a receive with a limit or a condition that must suspend in the same places; any receive in the `want` function of `task.receive_where` or `task.receive_where_within` | yes |
| `TimeoutError` | `task.receive_within(seconds)` or `task.receive_where_within(want, seconds)`, where the limit passes and no message is selected | yes |
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
credits of the other. Such a task takes each kind with `task.receive_where`,
which leaves the other kind in the mailbox. The library in
`examples/credit_channel.lox` lets a task hold one end only.

---

## A Session

A **session** is one program that a reader gives to the REPL one line at a
time. Each line compiles alone and runs at once, in order, and the program is
every line entered so far.

1. A line compiles in the program that the earlier lines made: a `fun`,
   `class`, or `enum` declaration of an earlier line is a declaration that
   the line can name, and a constructor tag never repeats in a session. A
   line that does not compile changes nothing.
2. The main task of a session is one task. It runs the top-level code of each
   line in turn, and the end of a line does not end it. Between two lines it
   runs nothing, and a message that a task sends to it waits in its mailbox.
3. A worker keeps its tasks and their mailboxes after the line that spawned
   it ends, and a task on it runs as soon as it can. The worker ends when
   every task on it has ended (see [Ending](#ending)).
4. A task that `task.spawn` made on the main worker does not outlive its line:
   the end of the line abandons it, as the end of a program abandons every
   task.
5. A fault of the main task that no `try` statement catches, and a deadlock,
   end the line. The session then runs the next line, and the main task is
   not dead, so a later line can receive a message that a worker sends after
   the line ended. A fault of a task on a worker ends the program, as
   [Ending](#ending) says.

The program that a new worker defines is the program of its line: a worker
that a later line spawns defines the declarations of every line up to and
including that line, in source order, as
[`task.spawn_worker`](#task.spawn_worker) step 3 requires. In a session that
program is every line entered so far.

A program that runs from a file is one run, and these rules do not change
it. `spec/README.md`, Conformance, binds both a session and a file alike.

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
| Arithmetic on non-Numbers, or on a Number together with an Int | `"a" - 1`, `1i - 1` | `"ArithmeticTypeError"` |
| Comparison on non-Numbers, or of a Number with an Int | `"a" < 1`, `1i < 1` | `"ComparisonTypeError"` |
| `+` on incompatible types, a Number together with an Int included | `1 + "a"`, `1i + 1` | `"ConcatenationTypeError"` |
| A bit operator with an operand that is not an Int, a shift count included | `1i & 1`, `1i << 1`, `class C {} C() & 1i` | `"BitwiseTypeError"` |
| A shift count outside `0i` to `63i` | `1i << 64i` | `"ShiftCountError"` |
| Int `/` or `%` with a zero divisor | `1i / 0i`, `1i % 0i` | `"DivisionByZeroError"` |
| Call of a non-callable value | `42()` | `"NotCallableError"` |
| Wrong argument count | `fun f(a) {} f(1, 2)` | `"ArityError"` |
| Undefined global variable | `print undeclared;` | `"UndefinedVariableError"` |
| Call stack overflow | Unbounded recursion that is not a [self tail call](#self-tail-calls) | `"StackOverflowError"` |
| Index of non-List/non-Map | `42[0]` | `"NotIndexableError"` |
| List or String index that is neither a Number nor an Int | `list["a"]` | `"IndexTypeError"` |
| Fractional List or String index | `list[1.5]` | `"IndexNotIntegerError"` |
| List or String index out of bounds | `[][0]` | `"IndexOutOfBoundsError"` |
| `pop` on empty list | `[].pop()` | `"EmptyListError"` |
| NaN used as map key | `m[0/0] = 1` | `"NaNKeyError"` |
| Object without `__hash__`/`__eq__` used as map key | `m[[1,2]] = 1` | `"InvalidMapKeyError"` |
| Method called on non-instance/non-list/non-map | `42.foo()` | `"InvalidReceiverError"` |
| No arm matches in a `match` expression | `match 99 { case 1 => "one" }` | `"MatchError"` |
| Constructor called with wrong arity | `Result.Ok(1, 2)` when `Ok` takes one field | `"ConstructorArityError"` |
| Undefined property on an `Error` value | `try { try { [][0]; } catch (e) { e.foo; } } catch (_) { }` | `"UndefinedPropertyError"` |
| Operator method returned a non-Boolean | `class C { __eq__(o) { return 42; } } C() == C()`, or the reflected `class C { __rlt__(o) { return 42; } } 1 < C()` | `"OperatorResultTypeError"` |
| Operator method returned a non-Number, an Int included | `class C { __len__() { return "x"; } } len(C())`, `class C { __len__() { return 1i; } } len(C())`, or `class C { __hash__() { return "x"; } __eq__(o) { return true; } }` used as a map key | `"OperatorResultTypeError"` |
| A key's `__hash__` or `__eq__` writes to the Map it keys | `var m = {}; class K { __hash__() { m[1] = 1; return 1; } __eq__(o) { return true; } } m[K()] = 1;` | `"MapChangedError"` |
| `resume` on a dead coroutine | `fun f() { return 1; } var co = coroutine.create(f); co.resume(); co.resume();` | `"DeadCoroutineError"` |
| `resume` on a running coroutine (self-resume) | `fun f() { co.resume(); } var co = coroutine.create(f); co.resume();` | `"RunningCoroutineError"` |
| `yield` across a native callback | `class S { __str__() { yield 1; return "x"; } } fun f() { print S(); } var co = coroutine.create(f); co.resume();` | `"YieldAcrossNativeError"` |
| `yield` in a deferred call | `fun note() { var v = yield 1; } fun g() { defer note(); } g();` | `"YieldAcrossNativeError"` |
| A Socket call on a closed Socket, a write after `close_write()`, or `accept()` on a closed Server | `var l = listen("127.0.0.1", 0); l.close(); l.accept();` | `"SocketClosedError"` |
| A failure of the network in `connect`, `listen`, `accept`, or a Socket read or write | `var l = listen("127.0.0.1", 0); var p = l.port(); l.close(); connect("127.0.0.1", p);` | `"NetworkError"` |

**The two map-key rows above cover an index read or write, a map literal,
and `in`.** `Map.has(key)` and `Map.del(key)` are stdlib native
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
| `print` of a value nested deeper than the implementation's canonical-string depth limit | `var a = [1]; var i = 0; while (i < 300) { a = [a]; i = i + 1; } print a;` |
| A native function is called with an argument count other than its arity | `clock(1);` |
| A `defer`red call holds a value that is not a Closure, Native, BoundMethod, or BoundNative | `fun g() { var x = 42; defer x(); } g();` (a static check cannot see that the variable holds a value that is not callable) |
| A stdlib native function reports its own error while running, except the faults of the two Socket rows of the [Runtime Errors](#runtime-errors) table | `open("/no/such/path", "r");` |
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
| Enum field indexed (`enumVal[i]`) with an index that is neither a Number nor an Int | `enum E { A(x) } var v = E.A(1); v["a"];` |
| Enum field indexed with a fractional index | `enum E { A(x) } var v = E.A(1); v[0.5];` |
| Enum field indexed with an out-of-range index | `enum E { A(x) } var v = E.A(1); v[3];` |
| Index-assignment on a String | `"abc"[0] = "x";` (strings are immutable — see [§03-types, String](03-types.md#string)) |
| `seq[start:end]` where `seq` is not a List or String | `(42)[0:1];` |
| `seq[start:end]` where `start` is neither a Number nor an Int | `[1, 2]["a":2];` |
| `seq[start:end]` where `start` is not integer-valued | `[1, 2][1.5:2];` |
| `seq[start:end]` where `start` is negative | `[1, 2][-1:2];` |
| `seq[start:end]` where `end` is neither a Number nor an Int | `[1, 2][0:"a"];` |
| `seq[start:end]` where `end` is not integer-valued | `[1, 2][0:1.5];` |
| `seq[start:end]` where `end` is negative | `[1, 2][0:-1];` |
| `elem in seq` where `seq` is a String and `elem` is not a String | `1 in "abc";` |
| `elem in seq` where `seq` is not a List, String, or Map | `1 in 42;` |
| `for (var x in expr)` where `expr` is not a List, String, Map, or Coroutine | `for (var x in 42) {}` |
| Map size changed during `for`-in iteration | `var m = {1: 1}; for (var k in m) { m[2] = 2; }` |
| `int(x)` where `x` is a fractional Number | `int(1.5);` |
| `int(x)` where `x` is a NaN | `int(math.nan);` |
| `int(x)` where `x` is an infinity | `int(math.inf);` |
| `int(x)` where `x` is a Number outside the Int range | `int(9223372036854775808.0);` |
| `int(s)` where `s` is a String that is not an optionally signed decimal integer in the Int range | `int("1.5");` |
| `int(x)` where `x` is not an Int, a Number, or a String | `int(nil);` |
| `float(s)` where `s` is a String that is not a decimal number | `float("abc");` |
| `float(x)` where `x` is not an Int, a Number, or a String | `float(nil);` |
| A function of the `integer` module called with an argument that is not an Int (a String for `parse_unsigned`) | `integer.abs(1);` |
| `integer.udiv(a, b)` or `integer.urem(a, b)` with `b == 0i` | `integer.udiv(1i, 0i);` |
| `integer.parse_unsigned(s)` where `s` is not an unsigned decimal integer, or is above 18446744073709551615 | `integer.parse_unsigned("18446744073709551616");` |

---

## Enum Types

### Enum constructors

An `enum` declaration makes one constructor value for each constructor that it
lists. A program reads the constructor `A` of the enum `E` as `E.A`
([§02-syntax](02-syntax.md#enum-declaration)). A constructor is callable:
calling it with as many arguments as it has fields creates and returns a new
enum value. Calling it with another number of arguments is the catchable
`ConstructorArityError`. The constructor `A` of the enum `E` stringifies as
`<ctor E::A>`.

### Enum values

An enum value holds the constructor that made it and one field for each
field of that constructor, in declaration order. Its fields do not change.
An enum value that the constructor `A` of the enum `E` made stringifies as
`E::A(f1, f2, ...)`, with each field in its canonical string form, or as
`E::A` for a constructor with no fields. The string form names the enum, so
values of two enums whose constructors have the same name have different
string forms.

### Exhaustiveness at compile time

When a match expression contains at least one constructor pattern of enum
`E`, the compiler checks that:
1. Every constructor of `E` appears in an arm, across all alternatives, **or**
2. An unguarded catch-all arm (`case _` or an unguarded binding) is present.

A violation is a **compile error** listing the missing constructor names. A
match with constructor patterns of more than one enum makes this check for
each enum.
