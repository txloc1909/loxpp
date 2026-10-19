# Lox++ vision

This file states what Lox++ is for and what it must not become. The owner
writes it. Every design decision is tested against it: a decision must add
expressiveness of the kind named here, and it must not remove a control the
programmer has today. When a decision and this file conflict, the decision
changes, or the owner changes this file.

The specification (`spec/`) defines what the language does. This file says
why. The rules for working on the language are in `AGENTS.md`. The map of the
design documents is `docs/README.md`.

## What Lox++ is

Lox++ is a dynamically typed, expression-oriented scripting language that
descends from the Lox of *Crafting Interpreters*. It keeps the simplicity and
clarity of Lox and adds pragmatic extensions for real programs: pattern
matching, algebraic types, destructuring, collections, non-local control
flow, coroutines, and access to files, the network, and processes. A complete
Lox++ interpreter is written in Lox++ (`bootstrap/loxpp_interpreter.lox`).

## Design goals

- **Simple**: a small, learnable surface area; no hidden complexity
- **Dynamically typed**: types are associated with values, not variable
  declarations
- **Expression-oriented**: most constructs produce a value
- **First-class functions**: functions are values; closures are the fundamental
  unit of abstraction
- **Fail loudly**: type errors and undefined behavior are detected at runtime
  and reported as errors, never silently ignored
- **Expressive in the dynamic functional tradition**: pattern matching, closures,
  immutable-by-convention data and sum types, with no static type system
  required
- **No high-level facility removes low-level control**: a high-level feature
  must not take away a control the programmer has. The controls are:
  - *evaluation order*: the order of evaluation is defined, not left open;
  - *mutation*: state changes only where the program writes;
  - *resource release*: `defer` releases a resource at a defined point, and
    the program does not wait for a collector to do it;
  - *suspension*: a program pays for suspension only where it uses it;
  - *failure*: a fault is a value that the program can catch or let end the
    program;
  - *memory*: the program can observe and bound the memory it uses, and can
    reach native code and raw bytes through an explicit interface;
  - *cost*: a construct has a cost that the programmer can predict. No
    construct runs user code or adds work at a site that does not name it.

## What counts as a language gap

> A feature counts as a real gap only if it cannot be bootstrapped by
> composing primitives the language already exposes. If a user could write it
> in pure Lox++ (closures + lists + maps + recursion + enums + `match`), it is
> a library, not a language gap, however tedious it is to hand-write.

Tedium is not the axis. Capability is. The capability list and its order are
in `docs/decisions/expressiveness-roadmap.md`.

## The direction: dynamic functional programming

Lox++ mines the dynamic functional tradition (Scheme, Clojure, Elixir,
Erlang), not the typed one (ML, Haskell, Rust): pattern matching,
immutable-by-convention data, sum types by convention, runtime match failure
as an honest error, and combinators as plain library code. The typed imports
are out of scope: the `?` operator, sound exhaustiveness, `Option`
everywhere. If static safety ever becomes a goal, it is added as an erasable
layer (gradual typing) that leaves the VM dynamic.

Combinators are module functions chained with a pipe operator. Enums stay
closed data with no methods. Truthiness stays `false` and `nil` only. The
reasoning is in `docs/decisions/dynamic-functional-programming.md`.

## The backends

The native VM is the product. The other backends exist to serve it, and each
one owes conformance only for its role. Weigh every design by the native VM
first.

| Backend | Role | Shipped | Obligation |
|---|---|---|---|
| Native VM | The runtime users run. It must be simple, and fast for a dynamic language. Performance work targets it first. | Yes | Implements the full spec. It is the reference for all other backends. |
| JVM | A semantic oracle: a mature managed runtime that checks the meaning of Lox++ bytecode. Also a performance baseline to work toward. | No | Conforms to the spec where it serves as an oracle. It does not have to implement a feature that exists only to give native code low-level access, such as FFI or a JIT. It may implement one when the JVM makes it cheap. |
| QBE | An experiment: the first lightweight path to an AOT binary, and tests along the AOT-to-JIT range. | No | Owes what the current experiment needs. A language change does not have to wait for QBE. |
| Bootstrap interpreter | A capability test: a practical language can implement itself. It is not expected to be fast today. Once the native VM has a solid JIT, the speed of the bootstrap interpreter becomes a meaningful benchmark of that JIT. | No | Conforms to the spec. |

Every backend conforms to the same spec. The spec binds what a program can
observe: its output, its exit status, the faults it raises, and which faults
it can catch. It does not bind error messages, and it leaves limits and
scheduling to each implementation. So a backend does not copy the error paths
of the native VM, and a new backend is free to experiment.

The JVM backend is never shipped: the JVM already has many languages.

## Performance

Expressiveness and performance are separate axes. The native VM is the
performance target: it must be fast for a dynamic language.

- Interpreter wins come first: inline caches and object shapes.
- The long-term path is a real AOT compiler or a high-performance JIT for the
  native VM.
- QBE and the JIT proposal are experiments along that path, not the path
  itself.
- The JVM backend is the performance baseline to work toward.
- The bootstrap interpreter is a benchmark for the JIT once that JIT exists.
  Its speed is not a goal before then.

## Distribution

Lox++ ships the way Go does: a user downloads one binary for a target and it
runs. No installer, no runtime files beside it, no system packages. The
standard library ships inside the binary as embedded source. There are two
packages:

1. **Simple**: the single binary plus source-only libraries. Simplicity and
   easy installation come first.
2. **Full**: the native VM plus native code: the runtime library, the JIT
   toolchain, compiled libraries, FFI. Performance and low-level capability
   come first.

The only supported target is `x86_64-linux`. Windows is not a goal.
`aarch64-linux` and macOS stay possible. The rules that keep them cheap are
in `docs/decisions/distribution.md`.

## How this file changes

Only the owner changes this file. A pull request that changes it carries no
code. A document under `docs/` cites the goal it serves and does not restate
this file.
