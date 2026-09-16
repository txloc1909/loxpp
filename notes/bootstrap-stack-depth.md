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
`VM::FRAMES_MAX` (`src/vm.h`). When that budget is exhausted, the VM raises
its own `"Stack overflow."` and halts — a native-level fault, not something
any counter written in `loxpp_interpreter.lox` can intercept before it
happens. Raising `FRAMES_MAX` moves this ceiling; it does not remove the
coupling, so every number below is re-measured, never assumed, whenever the
budget changes.

Empirically, at two points: `feat/nonlocal-control-flow-bootstrap` (PR #245,
`FRAMES_MAX = 256`, `STACK_MAX = 2048`) and the raised budget
(`FRAMES_MAX = 1024`, `STACK_MAX = 16384`), both `build/loxpp` release
builds:

| Ambient call depth at the `print` call site | Max nesting depth `stringify()` reaches before "Stack overflow." — `FRAMES_MAX=256` | — `FRAMES_MAX=1024` |
|---|---|---|
| 0 (top-level script) | guard fires first, any depth | guard fires first, any depth |
| 20 | 60 (61 crashes) | guard fires first, any depth up to at least 4095 |
| 40 | 0 (crashes immediately) | guard fires first, any depth up to at least 4095 |

Each level of `stringify()`'s own recursion costs about 2 native VM call
frames. "Guard fires first" means the shipped 100-level depth guard
(`this.stringifyDepth > 100`) always returns its own controlled
`MaxDepthExceededError` before native's frame count is exhausted, for every
nesting depth tried, because the guard's own recursion never runs past its
self-imposed cap of 100 regardless of how deeply nested the value actually
is. The real question is not "how deep can nesting go" but **at what ambient
call depth does native stop leaving the guard those 100 levels of room**:

| Budget | Guard reliable up to ambient depth | Native wins from |
|---|---|---|
| `FRAMES_MAX=256`  | 6   | 7   |
| `FRAMES_MAX=1024` | 134 | 135 |

Ambient call depth of 7 is unremarkable for a normal recursive Lox++
program (a simple recursive tree walk or divide-and-conquer routine reaches
that without trying); 134 is not, though it is still reachable by a deep
enough call chain. So **any fixed threshold in `stringify()`'s depth guard
still trades off against how deep the calling program already is** — a
quantity the guard cannot see or bound at counter-check time — the raised
budget makes that trade-off far less likely to matter in practice, and does
not remove it.

## Consequence for the guard

`spec/04-semantics.md`'s `MaxDepthExceededError` ("Value nested too deep to
print") describes a value "many thousand levels deep." No fixed threshold in
`stringify()` can guarantee firing before the native crash across every
ambient call depth — the guard is a best-effort mitigation, not an
architectural guarantee, at either budget. PR #245 landed it at 100 (comment
at the call site explains the trade-off); the raised budget makes that
threshold reliable up to a much deeper ambient call chain (134 frames,
against 6 before) without changing the threshold itself.

## Possible real fixes (out of scope for this note's own change)

- Track the *combined* budget — VM call frames used by the interpreter's
  own execution of `loxpp_interpreter.lox`, not just `stringify()`'s local
  counter. Not observable from Lox++ source today; would need a native
  export.
- Restructure `stringify()` to recurse fewer native frames per level (e.g.
  an explicit work-list/stack instead of `stringify -> stringifyList ->
  stringify`), buying more headroom per unit of `FRAMES_MAX`. Reduces the
  problem's severity, does not remove it.
- A global evaluator-depth counter across the interpreter's own call hubs,
  not just `stringify()` — sees the interpreter's own share of the budget
  directly instead of trading off against an unknown ambient depth. Left to
  a follow-on mission: its correct threshold depends on the interpreter's
  own call-graph shape, not on `FRAMES_MAX` alone.

Raising `VM::FRAMES_MAX` moves the ceiling this note measures; it is the one
fix of the three above that has actually been applied, and it does not
remove the shared-budget coupling — a call chain deep enough still wins the
race against the guard, just at 134 ambient frames instead of 6. If deep
recursion under the bootstrap interpreter turns out to matter beyond that,
it should be filed as its own issue against the bootstrap interpreter
generally, not folded into a future `stringify()`-specific fix. Filed as
[issue #248](https://github.com/txloc1909/loxpp/issues/248).
