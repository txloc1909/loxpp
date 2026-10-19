# Native VM: diagnostics, exit codes, and limits

This page describes choices of the native VM (`build/loxpp`) that the spec
leaves to each implementation (`spec/README.md`, Conformance). Other code
relies on some of them: the differential gates read native's exit codes, and
native's own tests pin its messages. No other implementation has to match
anything on this page.

## Exit codes

| Outcome | Exit status |
|---|---|
| The program ends at the end of its top-level code | 0 |
| `exit(code)` | `code` |
| A static error (the program is rejected) | 65 |
| A halt: an uncaught fault, an uncaught `throw`, or a halting fault | 70 |

`tools/diff_runtimes.py` uses 65 and 70 to tell a rejection and a halt of
native apart from a normal end. It never requires another implementation to
use them.

## Reports

A static error writes `[line N] Error at '<token>': <message>` to stderr, or
`[line N] Error at end: <message>` at the end of the file. The parser goes on
after an error (panic mode) and can report more than one.

A halt writes the message of the fault to stderr, then a stack trace. An
uncaught `throw` of an `Error` value reports its `message` field. An uncaught
`throw` of any other value reports its canonical string, with no prefix (for
example, `throw 42;` reports `42`).

## Faults

Native makes every halting fault of the spec halt. It makes no halting fault
catchable. When a catchable fault is not caught, native runs every pending
deferred call on the way to the top before it ends the program. When a
halting fault occurs, native skips every pending deferred call, including
the ones of the function where the fault happened.

The message texts are not listed here, because no contract binds them. They
are in `src/vm.cpp` (run-time faults), `src/stdlib/` (stdlib natives),
and `src/parser.cpp`, `src/compiler.cpp`, and `src/scanner.cpp` (static
errors). Native's own tests pin some of them.

## Limits

| Limit | Value |
|---|---|
| Call depth | 1024 frames (`kFramesMax`, `src/vm_limits.h`). See `docs/reference/bootstrap-stack-depth.md` for how the bootstrap interpreter shares this budget. |
| Arguments of a call, parameters of a function | 255 |
| Elements of a list literal, pairs of a map literal | 255 |
| Nesting depth of a canonical string | 200 heap levels (`kMaxStringifyDepth`, `src/object.h`) |

## Implementation notes

**`print`'s depth-limit fault is a halting fault on native.** `Op::PRINT`
clears any pending stdlib error before it stringifies its operand, so this
fault is not a stdlib-error path; it is the canonical-string depth guard
in `stringifyObj` (`src/object.cpp`), which also fires the same way when
`str()` calls into the same guard through a native call.

**A stdlib native's own error text is not listed here.** The text a stdlib
native reports differs call by call and is defined by that native.

**Internal invariant checks are excluded.** A handful of `RAISE_ERROR` call
sites in `src/vm.cpp` guard invariants the compiler itself is responsible
for (for example, that `GET_ITER` always pushes an `ObjIterator` before
`ITER_HAS_NEXT`/`ITER_NEXT` run). Their messages are prefixed `BUG:`. No
valid Lox++ program can reach them; seeing one means the implementation,
not the program, is broken. They are not part of the language's observable
behavior.

**Call stack overflow's fatal fast path.** The catchable `"StackOverflowError"`
row of the spec's Runtime Errors table covers the general case. Native takes a direct fatal
path — bypassing the catchable machinery — in two situations: when no
`try` statement is active in the running task (so a catch search would
fail immediately regardless), and for the re-entrant case that the spec's
Runtime Errors section describes, where a second
`StackOverflowError` arrives while the first is still unwinding. Both
produce the same `"Stack overflow."` message as the catchable row; this is
not a distinct fault.

**Shared uncaught-fault reporting is not a distinct fault.** `handleThrow`
reports the final "no handler found" message for every catchable fault
from two call sites of its own. When the unhandled value is an `Error`
value — including an explicit uncaught `throw` of an `Error` — it reuses
that value's own `message` field unchanged. When an explicit uncaught
`throw` carries a value that is **not** an `Error` value, native reports
that value's canonical string representation with no added prefix (for
example, `throw 42;` reports `42`). Likewise,
`raiseThrowableError`'s fallback for when the `Error` class itself is not
yet initialized cannot fire once any Lox++ program has started running —
the class is created during VM setup, before user code executes — so no
valid program can reach it.

**Wrong argument count's fatal fast path.** The catchable `"ArityError"` row of the spec's
Runtime Errors table covers the general case: calling a Lox++ function with the
wrong number of arguments while a `try` statement's handler is active. When
no handler is active in the running task, native skips constructing the
`Error` value entirely and reports the same `"Expected 0 arguments but got
1."`-shaped message as a direct fatal error (native's own arity and the
call's argument count) — the ordinary "no handler, so uncaught" outcome, not a distinct fault.
