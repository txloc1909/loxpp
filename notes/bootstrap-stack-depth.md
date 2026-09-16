# Bootstrap interpreter: no reliable deep-recursion guard is possible

Found while resolving PR #245's `MaxDepthExceededError` depth guard
(`stringify()` in `bootstrap/loxpp_interpreter.lox`). Recorded here because
the constraint is not specific to `stringify()` — it applies to any deep
recursion in a Lox++ program run under the bootstrap interpreter — and is
worth knowing before anyone next reaches for a depth counter as a fix in
that file.

## The constraint

`bootstrap/loxpp_interpreter.lox` is itself a Lox++ program, executed by the
native VM (`build/loxpp`). Its own call recursion (`stringify()` ->
`stringifyList()`/`stringifyMap()` -> `stringify()` -> ...) consumes native
VM call frames from the same budget as the *target* program's own calls:
`VM::FRAMES_MAX = 256` (`src/vm.h`). When that budget is exhausted, the VM
raises its own `"Stack overflow."` and halts — a native-level fault, not
something any counter written in `loxpp_interpreter.lox` can intercept
before it happens.

Empirically (measured on `feat/nonlocal-control-flow-bootstrap`, PR #245,
`build/loxpp` release build):

| Ambient call depth at the `print` call site | Max nesting depth `stringify()` can reach before "Stack overflow." |
|---|---|
| 0 (top-level script) | ~122 |
| 20 | ~50 |
| 40 | < 50 (crashes at 40) |
| 100 | < 40 (crashes at 40) |

Each level of `stringify()`'s own recursion costs about 2 native VM call
frames. Ambient call depth of 40-100 is unremarkable for a normal recursive
Lox++ program (a simple recursive tree walk or divide-and-conquer routine
reaches that without trying). So **any fixed threshold in `stringify()`'s
depth guard trades off against how deep the calling program already is** —
a quantity the guard cannot see or bound at counter-check time.

## Consequence for the guard

`spec/04-semantics.md`'s `MaxDepthExceededError` ("Value nested too deep to
print") describes a value "many thousand levels deep." No fixed threshold in
`stringify()` can guarantee firing before the native crash across realistic
ambient call depths — the guard is a best-effort mitigation for the
shallow-call-site case, not an architectural guarantee. PR #245 landed it at 100
(comment at the call site explains the trade-off) specifically because that
is safely below the worst empirically-observed reachable ceiling (~122 at
zero ambient depth) while still far below the spec's own "thousands of
levels" framing, so it does not misfire on any realistically-sized value a
normal program would print.

## Possible real fixes (out of scope for PR #245)

- Track the *combined* budget — VM call frames used by the interpreter's
  own execution of `loxpp_interpreter.lox`, not just `stringify()`'s local
  counter. Not observable from Lox++ source today; would need a native
  export.
- Restructure `stringify()` to recurse fewer native frames per level (e.g.
  an explicit work-list/stack instead of `stringify -> stringifyList ->
  stringify`), buying more headroom per unit of `FRAMES_MAX`. Reduces the
  problem's severity, does not remove it.
- Raise `VM::FRAMES_MAX`. Moves the ceiling, does not remove the shared-budget
  coupling.

None of these were attempted here: they touch the bootstrap interpreter's
general call-depth behavior (any deep recursion, not just `stringify()`) and
`src/vm.cpp`, both out of scope for PR #245 (bootstrap-side `try`/`catch`/
`throw`/`defer` wiring). If deep recursion under the bootstrap interpreter turns out
to matter in practice (e.g. a bootstrap-interpreted program that legitimately
recurses deeply crashes instead of hitting Lox++-level `StackOverflowError`
handling), it should be filed as its own issue against the bootstrap
interpreter generally, not folded into a future `stringify()`-specific fix.
Filed as [issue #248](https://github.com/txloc1909/loxpp/issues/248).
