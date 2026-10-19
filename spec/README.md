# Lox++ Language Specification

## What Is Lox++?

Lox++ is a dynamically typed, expression-oriented scripting language descended
from the Lox language designed by Robert Nystrom in *Crafting Interpreters*. It
preserves the simplicity and clarity of the original Lox while introducing
pragmatic extensions for real-world use.

This specification defines the Lox++ language independently of any particular
implementation. A conforming implementation may use any execution strategy —
tree-walking interpretation, bytecode compilation, native compilation, or
otherwise — provided that the observable behavior matches what is described
here.

---

## Design Goals

The design goals are in [`VISION.md`](../VISION.md). This specification
defines the behavior; the vision says what the behavior is for.

---

## Notation

Grammar throughout this specification is written in **EBNF**:

| Notation | Meaning |
|---|---|
| `::=` | Defines a non-terminal |
| `"terminal"` | Literal token |
| `UPPER_CASE` | Token category (e.g. `IDENTIFIER`, `NUMBER`) |
| `rule*` | Zero or more repetitions |
| `rule+` | One or more repetitions |
| `rule?` | Optional (zero or one) |
| `a \| b` | Alternation: a or b |
| `( a b )` | Grouping |
| `;` | End of a production |

---

## Error Taxonomy

**Static error** — detected before any code executes (e.g. during parsing or
variable resolution). A conforming implementation rejects a program that
contains one: it runs none of the program, writes nothing to standard output,
writes a report to standard error, and exits with a non-zero status. It
accepts every program that contains no static error.

Examples: `break` outside a loop, `return` at the top level, `defer` at the
top level, self-referential variable initializer, duplicate name in the same
scope.

**Runtime error** — detected during execution, also called a **fault**. A
fault is delivered to a `catch` clause as an `Error` value when it occurs
during the execution of a `try` statement's protected block (directly, or
inside a function called from within it) and the fault is catchable. Any
other fault **halts** the program. See [§04-semantics, `try`
Statement](04-semantics.md#try-statement) and [§04-semantics, Runtime
Errors](04-semantics.md#runtime-errors).

Examples: arithmetic on a non-Number, calling a non-function, wrong number of
arguments, reading an undeclared global variable.

---

## Conformance

This specification binds what a program can observe and depend on. It does
not bind how an implementation reports an error. The rules below apply to
every implementation alike.

**Static errors.** An implementation rejects every program that has a static
error, as the [Error Taxonomy](#error-taxonomy) says, and accepts every other
program.

**Faults.** A fault occurs in every case that this specification names. An
implementation must not give a value, or continue, where this specification
raises a fault. Faults are in two sets:

- A **catchable fault** is listed with its `kind` in [§04-semantics, Runtime
  Errors](04-semantics.md#runtime-errors) or [Worker and Task
  Faults](04-semantics.md#worker-and-task-faults). Every implementation
  delivers it to a `catch` clause as an `Error` value with that `kind`.
- A **halting fault** is listed by its cause in [§04-semantics, Halting
  Faults](04-semantics.md#halting-faults). It halts the program. An
  implementation may make a halting fault catchable instead, with an
  implementation-defined `kind`. A program that catches a halting fault is
  not portable.

**Halts.** A program halts when a fault is not caught: a halting fault, a
catchable fault that no `try` statement catches, or a `throw` that no `try`
statement catches. When a program halts:

1. the exit status is non-zero;
2. every effect of the program before the fault holds, in the order this
   specification defines: what it wrote to standard output, and what it did
   to files, sockets, and child processes;
3. the implementation writes a report to standard error;
4. whether the pending deferred calls run before the program ends is
   implementation-defined.

A call of `exit(code)` is not a halt: the program ends with exactly that
exit status. A program that ends at the end of its top-level code exits with
status 0.

**Implementation-defined behavior.** The following are implementation-defined
for every implementation, and no implementation has to match another on
them:

1. **Diagnostics**: the text of every report and every message, at compile
   time or at run time, including the `message` field of an `Error` and its
   canonical string; the token and line that a static error names; the
   recovery after a static error, and the number of static errors reported;
   the exit status of a rejection or a halt, other than that it is non-zero.
2. **Fault disposition**: whether a halting fault is catchable instead, and
   its `kind` when it is.
3. **The deferred calls of a halt**: whether the pending deferred calls run
   before a halted program ends, and what they do.
4. **Limits**: the call depth; the number of arguments of a call, of
   parameters of a function, of elements of a list literal, and of pairs of a
   map literal; the nesting depth of a canonical string; memory use; and the
   timing of garbage collection. Going past a limit is a static error or a
   fault; this specification does not say which. A
   [self tail call](04-semantics.md#self-tail-calls) is not subject to the
   call-depth limit.
5. **Scheduling**: whether workers run at the same time, and whether a
   blocking call blocks only its own worker or every worker. A program whose
   progress needs two workers to run at the same time is not portable.
6. **Speed**.

Map iteration order is unspecified ([§03-types](03-types.md)), which is a
separate rule.

A program is **portable** when it does not catch a halting fault, does not
depend on the text of a message, stays within the limits of each
implementation, and does not need two workers to run at the same time. Every
conforming implementation gives a portable program the same output and the
same exit status.

---

## Relationship to Book Lox

Lox++ begins from the Lox specification in *Crafting Interpreters* (clox
variant) and adds the following:

| Feature | Status |
|---|---|
| `break` statement | Added in Lox++ |
| `continue` statement | Added in Lox++ |
| `%` modulo operator | Added in Lox++ |
| `clock()` built-in | Added in Lox++ |
| `input()` built-in | Added in Lox++ |
| `str()` built-in | Added in Lox++ |
| `len()` built-in | Added in Lox++ |
| `math` global object (unary and binary numeric functions, constants `pi` `e` `inf` `nan`) | Added in Lox++ |
| `open()` built-in + File methods (`read`, `readline`, `readlines`, `write`, `writeline`, `close`) | Added in Lox++ |
| List type with `[...]` literal, indexing, and methods (`append`, `pop`, `remove`) | Added in Lox++ |
| String indexing `s[i]` (read-only, single-character) | Added in Lox++ |
| `seq[start:end]` slice syntax for List and String | Added in Lox++ |
| `for (var x in seq)` iteration over List, String, and Map | Added in Lox++ |
| Map type with `{}` literal and `[]` key access | Added in Lox++ |
| `match` expression with literal, wildcard, or-arm, and guard patterns | Added in Lox++ |
| `var {x, y} = expr` destructuring | Added in Lox++ |
| `enum` declaration with constructor variants | Added in Lox++ |
| Constructor patterns and class (instanceof) patterns in `match` | Added in Lox++ |
| `try`/`catch`, `throw`, `defer` non-local control flow + built-in `Error` type | Added in Lox++ |

---

## Files in This Specification

| File | Contents |
|---|---|
| [`01-lexical.md`](01-lexical.md) | Source encoding, whitespace, comments, all token forms |
| [`02-syntax.md`](02-syntax.md) | Complete EBNF grammar and operator precedence table |
| [`03-types.md`](03-types.md) | Runtime types, truthiness, equality, canonical string form |
| [`04-semantics.md`](04-semantics.md) | Evaluation rules for every expression and statement |
| [`05-stdlib.md`](05-stdlib.md) | Built-in functions available at program start |
