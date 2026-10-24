# Lox++ Type System

Lox++ is dynamically typed: every value carries its own type at runtime.
Variables have no declared type; the same variable may hold values of different
types over its lifetime.

---

## Runtime Types

There are fourteen runtime types:

### Nil

The type of the single value `nil`. Used to represent the absence of a
meaningful value. Uninitialized variables hold `nil`.

### Boolean

One of exactly two values: `true` or `false`.

### Number

A 64-bit IEEE 754 double-precision floating-point value. `1` and `1.0` are the
same value. An integer-valued Number is still a Number: it is never an
[Int](#int), and the two kinds do not mix.

Special IEEE 754 values (positive/negative infinity, NaN) may arise from an
arithmetic operation. A `NUMBER` literal also gives an infinity when its
decimal is nearer to infinity than to the largest finite double, and zero when
its decimal is nearer to zero than to the smallest subnormal
([§01-lexical](01-lexical.md#number-and-int-literals)). Their behavior follows
IEEE 754.

### Int

A signed 64-bit integer, from -9223372036854775808 to 9223372036854775807. An
Int is a kind of its own, beside Number. A program writes an Int as a literal
with the suffix `i`, or as a hexadecimal or binary literal
([§01-lexical](01-lexical.md#number-and-int-literals)), or makes one with the
`int` function ([§05-stdlib](05-stdlib.md#intvalue--int)).

- **No mixing.** An operator with one Int operand and one Number operand is a
  runtime error. `1 + 1i` is the same kind of fault as `1 + "1"`. Equality is
  the one exception: it never faults, so `1 == 1i` is `false`. The table in
  [§04-semantics, Operand Kinds](04-semantics.md#operand-kinds) gives every
  operator.
- **Wrap.** Every Int operation wraps in two's complement: the largest Int
  plus `1i` is the smallest Int. No Int operation raises an overflow fault.
  A program that must detect overflow checks the range itself.
- **Bit operators.** The operators `&`, `|`, `^`, `~`, `<<`, `>>`, and `>>>`
  work on Ints only (see [§04-semantics, Bit
  Operators](04-semantics.md#bit-operators)).
- **Equality and hashing.** Two Ints are equal when they hold the same
  value. An Int is never equal to a value of another kind, a Number
  included, so `1 == 1i` is `false` and `1 != 1i` is `true`. An Int is a
  valid map key and it hashes by its value, so `1` and `1i` are two different
  keys of one Map. No Int is a NaN, so no Int is an invalid key. A literal
  pattern of `match` follows `==`: the pattern `1i` matches the Int `1i` and
  not the Number `1`.
- **Truthiness.** An Int is always truthy, `0i` included.
- **Printing.** An Int prints as its exact decimal digits, with a `-` sign
  when it is negative, and never in an exponent form: `str(1i)` is `"1"` and
  `str(-1i)` is `"-1"`.
- **Messages.** An Int is always a [message](#messages).
- **The standard library.** A function of the standard library that takes a
  Number does not take an Int, and a function that gives a Number does not give
  an Int, unless [§05-stdlib](05-stdlib.md) says otherwise for `int`, `float`,
  and the `integer` module. `len(x)` gives a Number. `chr(65i)` and
  `math.floor(1i)` are runtime errors.
- **Width.** The spec binds 64 bits. How an implementation stores an Int is
  implementation-defined, and no program can observe it: an Int that an
  implementation keeps in a register, in a tagged word, or in a heap object
  behaves the same way.

```lox
print 255i;                     // 255
print 0xFF;                     // 255
print 9223372036854775807i + 1i; // -9223372036854775808
print 1 == 1i;                  // false
print type(1i);                 // Int
```

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

Two Strings order by their bytes. `a < b` compares the two Strings byte by
byte from the first byte, using each byte's unsigned value (0 through 255);
a String that is a prefix of the other is the smaller. See
[§04-semantics Comparison](04-semantics.md#comparison).

`ord(s)` returns the byte value of a one-byte String, and `chr(n)` returns the
one-byte String for a byte value `n`. See [§05-stdlib](05-stdlib.md).

### Function

A callable value that, when invoked with the correct number of arguments,
evaluates its body and produces a return value. Functions are first-class: they
can be stored in variables, passed as arguments, and returned from other
functions.

A function carries a **closure environment**. It reaches the local bindings of
the enclosing scopes that the function actually uses, and it is not a snapshot:
the function reads and writes the same storage as the scope it came from, so a
write through the function is visible outside it, and a later write outside is
visible to the function. (`this` is the one exception to the write direction,
because `this` is read-only.)
Closures let a function reach those bindings even after the scopes have exited.
See [Binding Identity](04-semantics.md#binding-identity) for which binding a
closure captures, and when a binding ends.

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

The maximum number of elements in a list literal is implementation-defined
(see [Conformance](README.md#conformance)).

### Map

A mutable, unordered mapping from **scalar keys** to heterogeneous values. Maps
are first-class: they can be stored in variables, passed as arguments, and
returned from functions. A map literal is written as comma-separated
`key: value` pairs enclosed in curly braces:

```lox
var m = {"a": 1, 2: "two", true: 3, nil: 4};
var empty = {};
```

**Valid key types**: Nil, Boolean, Number (not NaN), Int, String, and an Instance
whose class defines **both** `__hash__` and `__eq__` (see
[§04-semantics, Operator Overloading](04-semantics.md#operator-overloading)).
Any other type as a key is a **runtime error**. NaN is explicitly rejected even
though it is a Number, because `NaN != NaN` makes it impossible to reliably
retrieve a value stored under it.

Keys are looked up by **value equality**: scalars and Strings compare by value,
and an Instance key uses its own `__eq__`. For an Instance key, the map calls
`__hash__` to place the key, and resolves a collision with `__eq__` on the
**stored** key against the lookup key — never a reflected method. A class must
keep `__hash__` consistent with `__eq__`: two keys that compare equal must
return the same `__hash__`. A class that breaks this contract gives undefined
lookup results.

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

The maximum number of key-value pairs in a map literal is
implementation-defined (see [Conformance](README.md#conformance)).

### Error

Produced by the runtime when a fault caught by a `try` statement is
delivered to its `catchBlock` (see [§04-semantics, Runtime
Errors](04-semantics.md#runtime-errors) and [`try`
Statement](04-semantics.md#try-statement)). No built-in global name or
literal syntax constructs an `Error` value directly — the only way a Lox++
program obtains one is by catching a runtime fault with `try`/`catch`.

Every `Error` value has exactly two fields, both Strings and both
read-only:

| Field | Meaning |
|---|---|
| `message` | Human-readable description of the fault. Its text is implementation-defined (see [Conformance](README.md#conformance)). |
| `kind` | A string identifying the category of fault; see the [Runtime Errors](04-semantics.md#runtime-errors) table for the kind used by each fault defined in this specification. |

```lox
try {
    var xs = [];
    print xs[0];
} catch (e) {
    print e.kind;     // IndexOutOfBoundsError
    print e.message;  // implementation-defined text
}
```

`e.message` and `e.kind` are read with the same `.` syntax used for
Instance fields. Reading any other property name on an `Error` value, or
assigning to `message`, `kind`, or any other name, is a **runtime error**.

Two `Error` values are equal only if they are the exact same object
(identity equality — see [Equality](#equality)).

`Error` is not a sequence type: it does not support `len()`, `[]`, `in`, or
`for-in`.

Because `throw` accepts any value, an `Error` is not the only thing a
`catch` clause can ever bind — it is specifically what the language itself
delivers for a runtime fault. A `throw` written by a program can throw a
String, a Number, an Instance of a user-defined class, or any other value.

### Coroutine

A value produced by `coroutine.create(fn)` (see §05-stdlib). A coroutine is
a suspendable computation: its function runs until it yields or returns, and
it can be paused and resumed by its resumer. A coroutine is always in one of
four states, reported by its `status()` method:

| State | Meaning |
|---|---|
| `"suspended"` | The coroutine has not started, or has yielded, and can be resumed. |
| `"running"` | The coroutine is the one currently executing. |
| `"normal"` | The coroutine resumed another coroutine and is waiting for that coroutine to yield or return. |
| `"dead"` | The coroutine's function returned, or a throw from it propagated out to the resumer. |

Coroutines use **identity equality**: two coroutine values are equal only if
they are the exact same object.

A coroutine is always **truthy**.

A coroutine is **not** a sequence type: it does not support `len()`, `[]`,
`in`, or slicing. A coroutine is not directly iterable either — but a
`for-in` loop over a coroutine resumes it one element at a time (see
[§04-semantics, `for`-in Statement](04-semantics.md#for-in-statement)), and
a user type's `__iter__` may return a coroutine to the same effect.

The canonical string form of a coroutine is `<coroutine>`.

A coroutine is created with `coroutine.create(fn)`, where `fn` is a
callable value. The call does not run `fn`; `resume` starts or continues the
coroutine, and `status` reports its state. See §05-stdlib and
[§04-semantics, Coroutines](04-semantics.md#coroutines) for the full rules.

### Task

A handle to a task, produced by `task.spawn`, `task.spawn_worker`, and
`task.self` (see §05-stdlib). A task is a computation that runs on one
worker and owns one mailbox. See [§04-semantics, Workers and
Tasks](04-semantics.md#workers-and-tasks) for the full rules.

A Task value is an identity, not a container. A program can send to the
task it names (`task.send`), compare the handle with `==`, and use the
handle as any value: test its truthiness, read `type()`, and turn it into
its canonical string form. Nothing else about the task is observable
through the handle: not its state, not its worker, not the length of its
mailbox. The one exception is that a send to a task
that has ended raises `DeadTaskError`.

Two Task values are equal when they name the same task. This holds for a
handle that was made on another worker and received in a message: it is
equal to every other handle of the same task.

A Task is always **truthy**. It is not a sequence type and is not
iterable. It is not a valid map key.

The canonical string form of a Task is `<task>`.

A Task value is a [message](#messages).

**Workers are not values.** A worker is the unit that owns tasks, a heap of
values, and a set of global variables (see [§04-semantics, Workers and
Tasks](04-semantics.md#workers-and-tasks)). No value names a worker. A
program reaches a worker only through a handle to one of its tasks.

---

## Sequence Protocol

**List**, **String**, and **Map** implement the sequence protocol: they support
`len()`, `[]` access, membership testing with `in`, and `for-in` iteration.
List and String use integer indexing; Map uses key-based access. An integer
index, and a slice bound, is an integer-valued Number or an Int (see
[§04-semantics, Index Get](04-semantics.md#index-get)). String is
immutable — index assignment (`s[i] = v`) is always a runtime error.

A **Coroutine** is iterable by `for-in` (the loop resumes it one element at a
time) even though it is not otherwise a sequence — it supports neither
`len()`, `[]`, nor `in`. See [§04-semantics, `for`-in
Statement](04-semantics.md#for-in-statement).

Slice syntax (`seq[start:end]`) is supported on **List** and **String**; it produces a new value of the same type. Both bounds must be provided. Slicing is not supported on Map — a runtime error is raised. Slice expressions are read-only; they cannot appear as an assignment target.

---

## Messages

A **message** is a value that can pass from one task to another, on the
same worker or on a different one (see [§04-semantics, Workers and
Tasks](04-semantics.md#workers-and-tasks)). The message rule:

- A value of kind Nil, Boolean, Number, Int, String, or Task is always a message.
- A value of kind Enum is a message when each of its fields is a message.
- A value of any other kind is never a message.

The kind of a value is the name `type(value)` returns (§05-stdlib). The
table below gives the rule for every kind:

| Kind | Message | Reason |
|---|---|---|
| `Nil` | always | A scalar. |
| `Boolean` | always | A scalar. |
| `Number` | always | A scalar. |
| `Int` | always | A scalar. |
| `String` | always | Immutable. |
| `Task` | always | An identity, not a container. |
| `Enum` | when each field is a message | Its fields cannot change. |
| `Function` | never | It can capture mutable bindings. |
| `BoundMethod` | never | It holds an Instance. |
| `Class` | never | Its methods are functions. |
| `Instance` | never | Its fields can change. |
| `List` | never | Its elements can change. |
| `Map` | never | Its entries can change. |
| `File` | never | It holds a resource of one worker. |
| `Socket` | never | It holds a resource of one worker. |
| `Server` | never | It holds a resource of one worker. |
| `Process` | never | It holds a resource of one worker. |
| `Error` | never | It is an object the runtime makes for one fault; a program sends its `kind` and `message` Strings instead. |
| `Coroutine` | never | Its state can change. |
| `Iterator` | never | Its position can change. |
| `EnumConstructor` | never | Every worker has its own constructors from the same declaration; a program sends a value it builds. |

**The property is fixed when the value is made.** Whether a value is a
message is decided when the value is constructed, and it never changes
after that. No construct assigns a field of an enum value (an index
assignment on one is a runtime error, and an enum value has no
properties), so a field that was a message stays a message. No program can observe a value becoming a message, or
stopping being one.

**A send copies nothing.** A message passes by reference, on the same
worker and between workers alike. An Enum value that a task receives is
the value that was sent: it is `==` to the original, and a value that comes
back to its sender is `==` to the value the sender holds. A String that a
task receives is `==` to every String with the same characters, as every
String is; nothing else about the identity of a String is observable.

To send a List, a Map, or an Instance, a program converts it first, for
example into an Enum value whose fields are messages. The conversion is
ordinary code that the program writes where it pays for it.

---

## Truthiness

Every value is either **truthy** or **falsy**. This determines behavior in
conditions (`if`, `while`, `for`) and short-circuit operators (`and`, `or`).

| Value | Truthiness |
|---|---|
| `false` | Falsy |
| `nil` | Falsy |
| Everything else | Truthy |

Note: `0`, `0i`, `""` (empty string), functions, `[]` (empty list), and `{}` (empty map) are all **truthy**.

---

## Equality

The `==` and `!=` operators compare two values.

- Values of **different types** are **never equal**: `nil == false` → `false`,
  `0 == "0"` → `false`, `1 == 1i` → `false`.
- Values of the **same type** are compared by the rule for that type:
  - `nil == nil` → `true`
  - Booleans are equal when both are `true` or both are `false`
  - Numbers are equal when they have the same numeric value (IEEE 754 equality;
    note that `NaN != NaN` follows the standard)
  - Ints are equal when they hold the same value, for every value of the 64
    bits: `9007199254740993i == 9007199254740992i` is `false`
  - Strings are equal when they contain the same sequence of characters
  - Functions are equal only when they are the same function object (identity
    equality)
  - Classes, Instances, BoundMethods, Bound built-in methods, Lists, Maps, Files,
    Sockets, Servers, Processes, Errors, and Coroutines use identity
    equality: two values are equal only if they are the exact same object
  - Tasks are equal when they name the same task (see [Task](#task))
  - Enum values use identity equality: an enum value is equal only to
    itself. Two constructor calls make two different values, even with
    equal fields and even for a constructor with no fields. With
    `enum M { Box(v) None }`, `M.Box(1) == M.Box(1)` is `false` and `M.None() == M.None()` is `false`. An
    enum value that passed to another worker in a message is the same
    value, so it stays equal to itself on every worker (see
    [Messages](#messages))
  - Enum constructors use identity equality: a constructor is equal only
    to itself, so `M.Box == M.Box` is `true` and `M.Box == M.None` is `false`

Equality never produces a runtime error regardless of the types being compared,
**except** that an Instance dispatches to its class's `__eq__` — the left
operand's first, then the right operand's as the reflected method (see
[§04-semantics, Operator Overloading](04-semantics.md#operator-overloading)) —
and a `__eq__` that does not return a Boolean raises a catchable
`OperatorResultTypeError`. Internal equality (`in` against a List and
`list.remove`) always uses identity equality and never consults `__eq__`.
Map-key equality is different and never uses a reflected method: an Instance
key dispatches its own `__eq__` (see the Map section above).

---

## Type Coercion

Lox++ performs **no implicit type conversions**. Passing a String where a
Number is required (e.g., in arithmetic) is a runtime error. Use the `str()`
built-in to convert explicitly. An Int and a Number do not convert into each
other either: `1i + 1` is a runtime error, and a program writes `int(x)` or
`float(x)` ([§05-stdlib](05-stdlib.md)) to say which kind it wants.

---

## Canonical String Representation

Every value has a canonical string form, produced by `print` and by the
`str()` built-in:

| Type / Value | String form |
|---|---|
| `nil` | `nil` |
| `true` | `true` |
| `false` | `false` |
| Number | The shortest decimal that round-trips to the same IEEE 754 double (ECMAScript `Number::toString`, radix 10). An integral value below `1e21` prints its bare digits (`42`, `-3`, `1000000`); a fraction prints with a decimal point (`3.14`, `0.30000000000000004`); a value outside `[1e-6, 1e21)` prints in scientific notation (`1e+21`, `1e-7`). The special values print `inf`, `-inf`, `nan`, and `-nan` (the sign of a NaN is not guaranteed), and both zeroes print `0`. |
| Int | The exact decimal digits, with a `-` sign when negative, and no exponent form — `255`, `-1`, `0`, `9223372036854775807` |
| String | The string content itself (no surrounding quotes) |
| Function / BoundMethod | `<fn name>` where *name* is the declared function name |
| Native function | `<native fn>` |
| Bound built-in method | `<native fn>` |
| Class | The class name (e.g. `Dog`) |
| Instance | `ClassName instance` (e.g. `Dog instance`), or the value of the class's `__str__` method when it defines one — see [§04-semantics](04-semantics.md) |
| List | `[elem0, elem1, ...]` — each element in its canonical string form, comma-space separated, enclosed in `[` and `]`. An empty list is `[]`. |
| Map | `{key0: value0, key1: value1, ...}` — each pair as `key: value` in canonical form, comma-space separated, enclosed in `{` and `}`. An empty map is `{}`. |
| File | `<file>` |
| Socket | `<socket>` |
| Server | `<server>` |
| Process | `<process>` |
| EnumConstructor | `<ctor E::A>` for the constructor `A` of the enum `E` — see [§04-semantics](04-semantics.md#enum-types) |
| Enum | `E::A(f1, f2, ...)` — each field in its canonical string form, comma-space separated — or `E::A` for a constructor with no fields |
| Error | `kind: message` — the value of the `kind` field, then `": "`, then the value of the `message` field. |
| Coroutine | `<coroutine>` |
| Task | `<task>` |

The Number form is pinned by this edge table; a row added or removed is a
language change. `tools/check_number_format.py` proves the table complete.

| Case | Example | String |
|---|---|---|
| positive-zero | `0.0` | `0` |
| negative-zero | `-0.0` | `0` |
| small-integer | `42.0` | `42` |
| negative-integer | `-3.0` | `-3` |
| fraction | `3.14` | `3.14` |
| half | `0.5` | `0.5` |
| imprecise-sum | `0.1 + 0.2` | `0.30000000000000004` |
| seven-digits | `1000000.0` | `1000000` |
| nine-digits | `123456789.0` | `123456789` |
| fixed-upper-bound | `100000000000000000000` | `100000000000000000000` |
| scientific-lower-bound | `1000000000000000000000` | `1e+21` |
| fixed-lower-bound | `0.000001` | `0.000001` |
| scientific-upper-bound | `0.0000001` | `1e-7` |
| smallest-subnormal | `1` halved 1074 times | `5e-324` |
| positive-infinity | `math.inf` | `inf` |
| negative-infinity | `-math.inf` | `-inf` |
| not-a-number | `math.nan` | `nan` |

The nesting depth of a value that a canonical string form can hold is
implementation-defined (see [Conformance](README.md#conformance)). A value
more deeply nested than that limit causes a **runtime error**.
