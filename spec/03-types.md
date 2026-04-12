# Lox++ Type System

Lox++ is dynamically typed: every value carries its own type at runtime.
Variables have no declared type; the same variable may hold values of different
types over its lifetime.

---

## Runtime Types

There are eight runtime types:

### Nil

The type of the single value `nil`. Used to represent the absence of a
meaningful value. Uninitialized variables hold `nil`.

### Boolean

One of exactly two values: `true` or `false`.

### Number

A 64-bit IEEE 754 double-precision floating-point value. There is no separate
integer type; `1` and `1.0` are the same value.

Special IEEE 754 values (positive/negative infinity, NaN) are not directly
expressible as literals but may arise from certain arithmetic operations. Their
behavior follows IEEE 754.

### String

An immutable sequence of bytes (interpreted as UTF-8). Two strings with
identical content are equal regardless of how or where they were created.

### Function

A callable value that, when invoked with the correct number of arguments,
evaluates its body and produces a return value. Functions are first-class: they
can be stored in variables, passed as arguments, and returned from other
functions.

A function carries a **closure environment** — a snapshot of the lexical scope
in which it was defined. Closures allow a function to access variables from
enclosing scopes even after those scopes have exited.

### Class

A class value produced by a `class` declaration. When called as a function, it
creates a new Instance of that class. Classes are first-class: they can be
stored in variables, passed as arguments, and returned from functions.

### Instance

A runtime object created by calling a Class. Each instance has its own mutable
field table. Fields spring into existence on first assignment; there is no
pre-declared field list. An instance's class determines which methods are
available on it.

### BoundMethod

A method that has been looked up on an instance. When `obj.method` is evaluated
without an immediately following `(`, the result is a BoundMethod capturing
both the method closure and `obj` as the receiver. Calling a BoundMethod is
equivalent to calling the underlying function with `this` bound to the captured
receiver.

---

## Truthiness

Every value is either **truthy** or **falsy**. This determines behavior in
conditions (`if`, `while`, `for`) and short-circuit operators (`and`, `or`).

| Value | Truthiness |
|---|---|
| `false` | Falsy |
| `nil` | Falsy |
| Everything else | Truthy |

Note: `0`, `""` (empty string), and functions are all **truthy**.

---

## Equality

The `==` and `!=` operators compare two values.

- Values of **different types** are **never equal**: `nil == false` → `false`,
  `0 == "0"` → `false`.
- Values of the **same type** are compared by **value**:
  - `nil == nil` → `true`
  - Booleans are equal when both are `true` or both are `false`
  - Numbers are equal when they have the same numeric value (IEEE 754 equality;
    note that `NaN != NaN` follows the standard)
  - Strings are equal when they contain the same sequence of characters
  - Functions are equal only when they are the same function object (identity
    equality)
  - Classes, Instances, and BoundMethods use identity equality: two values are
    equal only if they are the exact same object

Equality never produces a runtime error regardless of the types being compared.

---

## Type Coercion

Lox++ performs **no implicit type conversions**. Passing a String where a
Number is required (e.g., in arithmetic) is a runtime error. Use the `str()`
built-in to convert explicitly.

---

## Canonical String Representation

Every value has a canonical string form, produced by `print` and by the
`str()` built-in:

| Type / Value | String form |
|---|---|
| `nil` | `nil` |
| `true` | `true` |
| `false` | `false` |
| Number (integer-valued) | Digits, no decimal point — `42`, `-3`, `0` |
| Number (fractional) | Shortest decimal representation — `3.14`, `1.5` |
| String | The string content itself (no surrounding quotes) |
| Function / BoundMethod | `<fn name>` where *name* is the declared function name |
| Native function | `<native fn>` |
| Class | The class name (e.g. `Dog`) |
| Instance | `ClassName instance` (e.g. `Dog instance`) |
