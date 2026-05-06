# Lox++ Type System

Lox++ is dynamically typed: every value carries its own type at runtime.
Variables have no declared type; the same variable may hold values of different
types over its lifetime.

---

## Runtime Types

There are ten runtime types:

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

An immutable sequence of bytes (interpreted as ASCII). Two strings with
identical content are equal regardless of how or where they were created.

String is a **sequence type** and supports the following sequence operations:

| Operation | Description |
|---|---|
| `len(s)` | Number of bytes in the string. |
| `s[i]` | Single-character string at byte index `i` (0-based). Runtime error if `i` is not an integer or is out of range. |
| `s[i] = v` | Runtime error — strings are immutable. |
| `s[start:end]` | New String of bytes from index `start` (inclusive) to `end` (exclusive). Both bounds required. Out-of-range bounds are clamped to `[0, len(s)]`. Runtime error if either index is negative, non-integer, or non-number. |
| `sub in s` | `true` if `sub` (a String) appears as a substring of `s`. |
| `for (var c in s)` | Iterates over each byte as a single-character String. |

```lox
var s = "hello";
print len(s);        // 5
print len("");       // 0

print s[0];          // h
print s[4];          // o
// s[0] = "H";      // runtime error: strings are immutable

print ("ell" in s);  // true
print ("xyz" in s);  // false

var reversed = "";
for (var c in s) {
    reversed = c + reversed;
}
print reversed;      // olleh
```

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

### List

A mutable, ordered, heterogeneous sequence of values. Lists are first-class:
they can be stored in variables, passed as arguments, and returned from
functions. A list literal is written as comma-separated expressions enclosed in
square brackets:

```lox
var xs = [1, "hello", true, nil];
var empty = [];
```

Elements are accessed and mutated by integer index using `[]` notation:

```lox
xs[0]        // read element at index 0
xs[0] = 42   // write element at index 0
```

Lists use **identity equality**: two list values are equal only if they are the
exact same object. A list containing the same elements as another list is not
equal to it.

A list is always **truthy**, even when empty.

List is a **sequence type** and supports the following sequence operations in
addition to the mutation methods:

| Operation | Description |
|---|---|
| `list.append(value)` | Appends `value` to the end. Returns `nil`. |
| `list.pop()` | Removes and returns the last element. Runtime error on empty list. |
| `len(list)` | Number of elements as a Number. |
| `list[i]` | Element at index `i` (0-based). Runtime error if `i` is not an integer or is out of range. |
| `list[i] = v` | Sets element at index `i`. Runtime error if out of range. |
| `list[start:end]` | New List of elements from index `start` (inclusive) to `end` (exclusive). Both bounds required. Out-of-range bounds are clamped to `[0, len(list)]`. Runtime error if either index is negative, non-integer, or non-number. |
| `elem in list` | `true` if any element compares equal to `elem` under `==`. |
| `for (var x in list)` | Iterates over elements in order. |

The maximum number of elements in a list literal is 255.

### Map

A mutable, unordered mapping from **scalar keys** to heterogeneous values. Maps
are first-class: they can be stored in variables, passed as arguments, and
returned from functions. A map literal is written as comma-separated
`key: value` pairs enclosed in curly braces:

```lox
var m = {"a": 1, 2: "two", true: 3, nil: 4};
var empty = {};
```

**Valid key types**: Nil, Boolean, Number (not NaN), and String. Any other type
as a key is a **runtime error**. NaN is explicitly rejected even though it is a
Number, because `NaN != NaN` makes it impossible to reliably retrieve a value
stored under it.

Keys are looked up by **value equality** (the same semantics as `==`): any two
values that compare equal under `==` map to the same slot.

Values are accessed and mutated by key using `[]` notation:

```lox
m["a"]       // read value for key "a" — nil if absent
m["a"] = 42  // insert or update key "a"
```

Maps use **identity equality**: two map values are equal only if they are the
exact same object.

A map is always **truthy**, even when empty.

Map is a **sequence type** and supports the following sequence operations in
addition to the mutation methods. Note that `[]` uses key-based access, not
integer index.

| Operation | Description |
|---|---|
| `m[key]` | Returns the value for `key`, or `nil` if the key is absent. Runtime error if `key` is an invalid key type. |
| `m[key] = value` | Inserts or updates `key → value`. Runtime error if `key` is an invalid key type. |
| `len(m)` | Number of key-value pairs as a Number. |
| `key in m` | `true` if `key` is present in the map. |
| `for (var k in m)` | Iterates over all keys in unspecified order. |
| `m.has(key)` | `true` if `key` is present in the map. Equivalent to `key in m`. |
| `m.del(key)` | Removes `key` from the map. No-op if the key is absent. Returns `nil`. |
| `m.keys()` | Returns a List of all keys (order matches `m.values()` and `m.entries()`). |
| `m.values()` | Returns a List of all values (order matches `m.keys()`). |
| `m.entries()` | Returns a List of `[key, value]` two-element Lists (order matches `m.keys()`). |

The maximum number of key-value pairs in a map literal is 255.

---

## Sequence Protocol

**List**, **String**, and **Map** implement the sequence protocol: they support
`len()`, `[]` access, membership testing with `in`, and `for-in` iteration.
List and String use integer indexing; Map uses key-based access. String is
immutable — index assignment (`s[i] = v`) is always a runtime error.

Slice syntax (`seq[start:end]`) is supported on **List** and **String**; it produces a new value of the same type. Both bounds must be provided. Slicing is not supported on Map — a runtime error is raised. Slice expressions are read-only; they cannot appear as an assignment target.

---

## Truthiness

Every value is either **truthy** or **falsy**. This determines behavior in
conditions (`if`, `while`, `for`) and short-circuit operators (`and`, `or`).

| Value | Truthiness |
|---|---|
| `false` | Falsy |
| `nil` | Falsy |
| Everything else | Truthy |

Note: `0`, `""` (empty string), functions, `[]` (empty list), and `{}` (empty map) are all **truthy**.

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
  - Classes, Instances, BoundMethods, Lists, and Maps use identity equality:
    two values are equal only if they are the exact same object

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
| List | `[elem0, elem1, ...]` — each element in its canonical string form, comma-space separated, enclosed in `[` and `]`. An empty list is `[]`. |
| Map | `{key0: value0, key1: value1, ...}` — each pair as `key: value` in canonical form, comma-space separated, enclosed in `{` and `}`. An empty map is `{}`. |
