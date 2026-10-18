# Bootstrap interpreter: accepted limits on fatal faults

## The limit

Eight fault kinds halt the program on the native VM and are catchable on the
bootstrap interpreter (`bootstrap/loxpp_interpreter.lox`). The spec's Fatal
Runtime Errors table does not list them. On the bootstrap, a `catch` block
receives the fault, and pending deferred calls run.

| Kind | Cause (native is fatal, the bootstrap is catchable) |
|---|---|
| `ReflectionFieldNameError` | a reflection call with a field name that is not valid |
| `ReflectionUndefinedMemberError` | a reflection call on a member that is not defined |
| `ReflectionUnsupportedError` | a reflection call on a value it does not support |
| `ReflectionArityError` | a reflection call with the wrong argument count |
| `ReflectionNotCallableError` | a reflection call on a value that is not callable |
| `ReflectionNativeArityError` | a reflection call to a native with the wrong argument count |
| `LengthTypeError` | `len` of a value that has no length (for example `len(42)`) |
| `InvalidDestructureReceiverError` | a destructuring target that is not a valid receiver |

## The decision

The owner decided to accept this difference as a limit of the bootstrap
interpreter. The spec is not changed. The bootstrap is not required to match
native for these eight kinds. Native is the reference for every other fatal
row, which the bootstrap matches (tracked by `tools/check_fatal_table.py`,
whose expected-failure list is empty).

## Where it would change

If a row for one of these kinds is added to the spec's Fatal Runtime Errors
table, the bootstrap must match it. The row then moves out of this document,
and the check in `tools/check_fatal_table.py` covers it.

Related: [bootstrap-stack-depth](bootstrap-stack-depth.md) (the call-depth
limit, which is a separate exception in the spec).
