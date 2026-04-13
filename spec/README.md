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

- **Simple**: a small, learnable surface area; no hidden complexity
- **Dynamically typed**: types are associated with values, not variable
  declarations
- **Expression-oriented**: most constructs produce a value
- **First-class functions**: functions are values; closures are the fundamental
  unit of abstraction
- **Fail loudly**: type errors and undefined behavior are detected at runtime
  and reported as errors, never silently ignored

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
variable resolution). A conforming implementation must report a static error
and must not execute the program that contains one.

Examples: `break` outside a loop, `return` at the top level, self-referential
variable initializer, duplicate name in the same scope.

**Runtime error** — detected during execution. A conforming implementation
must halt execution and report the error.

Examples: arithmetic on a non-Number, calling a non-function, wrong number of
arguments, reading an undeclared global variable.

---

## Relationship to Book Lox

Lox++ begins from the Lox specification in *Crafting Interpreters* (clox
variant) and adds the following:

| Feature | Status |
|---|---|
| `break` statement | Added in Lox++ |
| `continue` statement | Added in Lox++ |
| `switch`/`case`/`default` statement | Added in Lox++ |
| `clock()` built-in | Added in Lox++ |
| `input()` built-in | Added in Lox++ |
| `str()` built-in | Added in Lox++ |
| Classes (`class`, `super`, `this`) | Added in Lox++ |
| List type with `[]` literal and indexing | Added in Lox++ |

---

## Files in This Specification

| File | Contents |
|---|---|
| [`01-lexical.md`](01-lexical.md) | Source encoding, whitespace, comments, all token forms |
| [`02-syntax.md`](02-syntax.md) | Complete EBNF grammar and operator precedence table |
| [`03-types.md`](03-types.md) | Runtime types, truthiness, equality, canonical string form |
| [`04-semantics.md`](04-semantics.md) | Evaluation rules for every expression and statement |
| [`05-stdlib.md`](05-stdlib.md) | Built-in functions available at program start |
