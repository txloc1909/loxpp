# Mission brief: non-local-control-flow follow-up fixes (Wave 1 onward)

> **SCRATCH.** Not git-tracked, per `notes/missions/`'s standing convention
> (agent-facing planning surface only — see `AGENTS.md`'s "Backlog and task
> tracking" and `notes/multi-agent-playbook.md`). Do not `git add` this file.
> The durable record of *why* this backlog exists is the GitHub issues it
> cites, not this file. This is a handoff doc, prepared but not executed in
> the session that wrote it.

## Context

The non-local-control-flow mission (`#223`, nodes X1-X7, PRs `#230`-`#252`)
shipped `try`/`catch`/`throw`/`defer` across the native VM, JVM backend, CLR
backend, and bootstrap interpreter. Closing it out surfaced 12 follow-up
issues that were filed but never fixed. Two of them (`#244` tree-sitter
grammar, `#255` Managed-toolchains CI job) were directly reddening CI on
`main` and have already been fixed as **Wave 0**:

- `#244` → PR `#259` (merged) — tree-sitter grammar rules for
  `try`/`catch`/`throw`/`defer`.
- `#255` → PR `#260` (merged) — root-caused `LoxRuntime.current()` being
  null in 4 JVM test suites, plus isolated the CI job so a JVM-only failure
  can't hide every CLR check again.

`main`'s CI is green as of `041636c` (run `35043302099`, all 9 jobs pass).

**This brief covers what's left: 9 real bugs plus 1 test-coverage gap,
none of them currently reddening CI, but all still open.** They are a
bug-fix tail, not a new-capability mission — small enough that the full
`.claude/workflows/backend-dag.js` harness is probably overkill; running
each wave's nodes as direct implementer passes (the same way `#257`-`#260`
were done) is likely the right shape. Use the harness only if wave 1's
3-way parallelism turns out to need its stall/escalation tooling in
practice.

## The DAG

```
Wave 1 (parallel, no code dependency between them)
  F1 (#241)  F2 (#242)  F3 (#240)
       \         |         /
        \        |        /
Wave 2   -----  F4 (#253+#254)   [blocked from real CI signal until
                                   Wave 0 landed — already true now]
                    |
Wave 3 (parallel)
  F5 (#233)     F6 (#250)
                    |
Wave 4 (parallel, each needs a posted+approved plan before code)
  F7 (#251)     F8 (#248)

Leaf
  F9 (#235)  — depends on F1, F2, F3 (wave 1) merged
```

No hard code dependency crosses waves except F9's. The wave grouping is
priority/severity ordering (crash and wrong-output bugs before cosmetic and
design-needed ones), not a blocking chain — feel free to pull a later-wave
node forward if it's more urgent when this is picked up.

---

## F1 — Native VM: handler-stack leak on return from a still-open outer `try`

**Issue:** `#241`
**Branch:** `fix/vm-handler-stack-leak-on-return`
**Depends on:** none
**Blocks:** F9

### Deliverable

`Op::RETURN` (`src/vm.cpp:1142`) pops the call frame and pushes the return
value but never pops `m_handlerStack`. Returning from inside a nested `try`
whose *outer* protected region is still open leaks that outer
`HandlerRecord`. A later, unrelated `throw` at the same call-frame depth
matches the stale record and the index math goes out of range —
`terminate() ... vector::_M_range_check` abort, reachable by any ordinary
program that returns from inside a `try` and later throws again at the same
or deeper depth.

Fix: `Op::RETURN`'s handler must pop any handler records whose `frameCount`
belongs to the frame being returned from, mirroring how it already unwinds
other per-frame state (upvalues) on return.

### Scope for this node

`src/vm.cpp` only. Do not touch `cfg.cpp`/`abstract_stack.cpp` — this is a
runtime bug, not a translation-pipeline one; nothing here should need JVM
or CLR emitter changes (confirm that assumption before merging, don't just
assume it).

### Checkpoint (must pass before merge)

- The exact repro in `#241`'s body, added as a permanent example
  (`examples/`) or `test/test_vm.cpp`-style unit test — must fail on
  unpatched `main` (prove it can fail, per `AGENTS.md`'s engineering rules)
  and pass after the fix.
- Full `cmake --build build` (not partial) + full
  `ctest --test-dir build --output-on-failure -j$(nproc)` green.
- Run `tools/diff_runtimes.py` (or the equivalent manual JVM/CLR diff) on
  the repro to confirm this was in fact native-only and JVM/CLR were never
  affected — don't assume, verify.

### Hazards

- The repro throws *after* an unrelated `return`, at the same frame depth —
  a test that only checks "does `try`/`catch` still work" won't catch a
  regression here. The regression test must reproduce the leak shape
  exactly (return from inner try while an outer try is still open, then an
  unrelated later throw).
- This bug predates PR `#237` (confirmed identical on `main`'s merge-base) —
  it is not new damage from this mission's own nodes, just newly found by
  one of their review rounds. Don't scope-creep into auditing every other
  per-frame stack for the same leak shape unless you find a second instance
  while fixing this one.

---

## F2 — Compiler: local-slot miscount for match-on-caught-value + branch-selected throw + sibling try/catch

**Issue:** `#242`
**Branch:** `fix/compiler-catch-match-slot-miscount`
**Depends on:** none
**Blocks:** F9

### Deliverable

Combining (1) `match` discriminating a caught value inside a `catch` block,
(2) an `if`/`else` choosing which variant gets thrown, and (3) a sibling
`try`/`catch` after the first, produces wrong output — reproduces
byte-for-byte identically on native and JVM, confirming this is a single
`src/compiler.cpp` local-slot allocation bug, not two independent ones. Fix
it once in the compiler; neither backend emitter needs its own fix for this
issue.

### Scope for this node

`src/compiler.cpp`'s local-slot allocation only. Do not touch
`jvm_emitter.cpp`/`clr_emitter.cpp` — the issue body confirms both
backends already reproduce whatever the compiler emits faithfully.

### Checkpoint (must pass before merge)

- `#242`'s exact repro, as a permanent example, produces the documented
  expected output (`caught a circle r=3` / `sibling caught: sibling-fault`)
  on native, JVM, *and* CLR (the issue only confirmed native+JVM; confirm
  CLR too before closing, since it shares the same compiler output).
- Full rebuild + full `ctest` green.
- Re-run the corpus/differential suites (`tools/diff_runtimes.py` or the
  CI-equivalent commands) to catch any other local-slot-miscount shape this
  fix's root cause might also explain — don't fix only the one reported
  repro shape if the root cause is broader.

### Hazards

- **Check F3 (`#240`) before concluding this is fully separate.** Both are
  "a catch-bound variable's slot isn't correctly accounted for" bugs, one
  in `src/compiler.cpp`'s own local-slot allocation (this node) and one in
  the shared bytecode local-count analysis both backend emitters read
  (`#240`). They may be two symptoms of the same underlying miscount
  pattern, or genuinely separate bugs in separate subsystems. Per
  `notes/multi-agent-playbook.md`'s "Audit against a countable ground
  truth" lesson from this same mission: don't patch the one reported repro
  shape and stop — enumerate every place a catch-bound identifier's slot
  gets counted and confirm each one is now correct.

---

## F3 — Shared local-count analysis undercounts a catch-bound variable's slot (JVM + CLR)

**Issue:** `#240`
**Branch:** `fix/shared-local-count-catch-binding`
**Depends on:** none
**Blocks:** F9

### Deliverable

A function using both `defer` and its own `try`/`catch (e)` gets `defer`'s
list slot silently overwritten by the caught value on **both** JVM and CLR
— the deferred calls recorded before `catch` runs never execute, no crash,
no error, just missing output (silent data loss). Root cause: both
backends pick the defer list's slot as one-past the function's own highest
*counted* local, via `computeMaxLocalCount` (`clr_emitter.cpp`) and its JVM
twin — and that count does not include the catch-bound identifier's slot.
Fix wherever that count is actually computed (issue body says "whatever
bytecode-level accounting feeds both" — confirm whether that's each
emitter's own local copy of the logic or a genuinely shared pass before
deciding where the real fix goes).

### Scope for this node

Whichever file(s) the investigation identifies as the actual source of the
local count — likely `clr_emitter.cpp` and `jvm_emitter.cpp` if the logic
is duplicated per-backend (fix both, identically), or one shared pass if
it isn't. Do not touch `src/compiler.cpp` unless the investigation proves
the miscount originates there too (see F2's hazard note — check for
overlap, but this node's own fix should land in the backend-translation
layer unless proven otherwise).

### Checkpoint (must pass before merge)

- `#240`'s exact repro (`ownTryCatch`) produces output byte-identical to
  native on both `--target jvm` and `--target clr`.
- The "not affected" cases the issue already verified (`defer` without the
  function's own `try`/`catch`; `defer` with a plain `var`) stay unaffected
  — add them as permanent regression examples if they aren't already.
- Full rebuild + full `ctest`, plus the JVM/CLR differential suites, green.

### Hazards

- See F2's hazard — same bug-class family, different subsystem. If you land
  this node first, flag the finding for whoever picks up F2 (and vice
  versa) rather than letting both investigate the same miscount pattern
  from scratch independently.
- The issue explicitly says CLR's own `.try{}`/`.finally{}` region emission
  (fixed in PR `#236`) is *not* where this bug lives — don't re-open that
  code path looking for it.

---

## F4 — JVM fault-kind wiring audit (over-catching + under-catching)

**Issues:** `#253` (over-catching in `GetProperty`/`SetProperty`), `#254`
(under-catching `ConstructorArityError`)
**Branch:** `fix/jvm-fault-kind-audit`
**Depends on:** none (Wave 0's `#255` fix means this now gets a real CI
signal on the CLR-mirroring differential suite, unlike when these were
filed)
**Blocks:** none

### Deliverable

One PR closing both issues via the same exhaustive-audit methodology PR
`#252` already used for the equivalent CLR-side bugs: walk every
`GetProperty`/`SetProperty`/constructor-arity call site in
`runtime/jvm/src/lox/LoxOps.java`/`LoxClass.java` against `src/vm.cpp`'s
`tryCatchableError`/`CATCHABLE_OR_RETURN` vs `RAISE_ERROR`/
`nativeRuntimeError` split as ground truth (not against
`spec/04-semantics.md`'s table directly — the issue's own reference PR
explains why: native itself doesn't implement every table row as
catchable). Fix `#254`'s under-catching site (likely
`runtime/jvm/src/lox/LoxClass.java` near line 60, per PR `#252`'s
reviewer's spot-check) and pull back `#253`'s over-catching sites to match
native.

### Scope for this node

`runtime/jvm/src/lox/` only. This mirrors PR `#252`'s CLR-side fix; do not
re-touch `runtime/clr/` here.

### Checkpoint (must pass before merge)

- `#254`'s repro (`class Point { describe() {...} }` called with args)
  prints `caught: ConstructorArityError` / `after` on `--target jvm`,
  matching native, exit 0.
- `#253`'s over-catching sites are enumerated explicitly (write down the
  count, the way PR `#245`'s bootstrap audit and PR `#252`'s CLR audit
  both did — "N sites checked, M fixed, 0 remaining") rather than fixed
  one at a time against whatever a test happens to hit.
- Full rebuild + full `ctest`, full JVM differential suite, green.

### Hazards

- `#253` is the *opposite* direction from every other fault-wiring bug this
  mission found (JVM over-catching, not under-catching) — don't assume the
  fix is always "wire it to `makeError`"; some sites may need the opposite,
  a plain uncatchable throw.
- Per the issue: confirm against `spec/04-semantics.md`'s table whether
  native is right to leave a given case uncatchable before assuming JVM's
  behavior (not native's) is the bug — the issue itself flags this as
  "less likely, but worth confirming," not settled.

---

## F5 — Tooling resolver: bind `catch (e)`'s identifier

**Issue:** `#233`
**Branch:** `fix/tooling-resolver-catch-binding`
**Depends on:** none
**Blocks:** none

### Deliverable

`src/tooling/`'s `DocumentModel`/resolver has no `try`/`catch`/`throw`/
`defer` AST nodes, so `catch (e) { ... }` never binds `e` — any use inside
the catch body reports a false `unknown name 'e'` warning. Give the
resolver's own parser/AST `try`/`catch`/`throw`/`defer` nodes and bind
`catch`'s identifier the same way a function parameter or `var` gets bound
today.

### Scope for this node

`src/tooling/` only — this is the LSP-facing resolver built for the
editor-tooling mission (`notes/editor-tooling.md`), not the compiler or
native VM.

### Checkpoint (must pass before merge)

- `test/test_tooling_resolver.cpp`'s `ToolingResolverCorpus.NoCrashAndFewWarnings`
  warning budget comment (added during this mission's own comment-reference
  cleanup, PR `#257`) currently documents these as known false positives —
  remove that carve-out and lower the budget back down once fixed, the same
  way `test_chunk_decoder.cpp`'s stale carve-out was removed in that PR.
- Every `examples/*.lox` file using `catch (e) { ... print e; ... }` (or a
  rethrow) produces zero resolver warnings for `e`.
- Full rebuild + full `ctest` green.

### Hazards

- This is the same root pattern `#244` (tree-sitter) already was: a
  Lox++-consuming tool outside the shared bytecode pipeline that the
  mission's own scope missed. No code hazard, but worth a
  `notes/multi-agent-playbook.md` cross-reference in the PR description
  rather than treating it as an isolated one-off.

---

## F6 — Bootstrap interpreter: fault-kind strings alias unrelated spec kinds

**Issue:** `#250`
**Branch:** `fix/bootstrap-fault-kind-aliasing`
**Depends on:** none
**Blocks:** none

### Deliverable

7 of the 17 fault sites PR `#245` (X7) wired to `setError()` reuse an
existing `spec/04-semantics.md` table kind string for an unrelated cause
(e.g. the reflection API's field-name-type error reuses
`InvalidMapKeyError`; `list.remove()`'s value-not-found reuses
`IndexOutOfBoundsError`). Give these their own distinct kind string(s) (the
issue suggests a `"ReflectionError"` family, or per-site kinds) so
`catch (e) { if (e.kind == "...") }` can tell them apart from the
spec-table cause.

### Scope for this node

`bootstrap/loxpp_interpreter.lox` only, the 7 sites `#250` names (reflection
API branches + `list.remove()`). None of these 7 causes appear in the
spec's 19-row catchable-fault table, so this is purely a bootstrap-internal
naming fix — no `spec/` change needed.

### Checkpoint (must pass before merge)

- Each of the two concrete collisions in `#250`'s body no longer aliases:
  distinct `.kind` values for the reflection field-name-type error vs. a
  genuine `InvalidMapKeyError`, and for `list.remove()`'s not-found case vs.
  a genuine `IndexOutOfBoundsError`.
- Run every `examples/*.lox` program that exercises the reflection API or
  `list.remove()` through both `build/loxpp` and the bootstrap interpreter,
  diffed.
- Full rebuild + full `ctest` green.

### Hazards

- Low severity, cosmetic-leaning — don't let this block or delay F1-F4 if
  time is limited on a given pass.

---

## F7 — File write/read visibility divergence across backends

**Issue:** `#251`
**Branch:** *(none yet — needs a plan first)*
**Depends on:** none
**Blocks:** none

### Deliverable

Native and CLR buffer file writes until `close()`; JVM's write path makes
data visible to a fresh read handle immediately, no close required. Not
currently a spec violation (`spec/05-stdlib.md`'s File section is silent on
flush timing), but a silent, backend-dependent divergence. **This needs a
plan posted as an issue comment before any code**, per `AGENTS.md`'s
planning policy — it is a design decision, not a bug with one correct fix:

1. Spec the timing explicitly as implementation-defined (closes the gap by
   declaring it acceptable), or
2. Align native/CLR's buffering behavior with JVM's (or vice versa).

### Scope for this node

Whichever option the approved plan picks: `spec/05-stdlib.md` only for
option 1; `src/` (native file I/O) and/or `runtime/clr/` for option 2 —
not `runtime/jvm/`, since JVM's behavior is the one everyone would be
aligning toward or documenting as the baseline.

### Checkpoint (must pass before merge)

- If option 1: the repro in `#251` is added to `spec/05-stdlib.md` as a
  documented implementation-defined case, plus a comment in whichever test
  exercises file I/O noting the divergence is intentional.
- If option 2: `#251`'s repro produces identical `nil`/`"data"` behavior
  across all three backends, confirmed deterministic across at least 3
  repeats each (matching the issue's own verification method).

### Hazards

- Don't skip the plan-and-approval step because the repro is small — this
  changes observable behavior (or documents a permanent behavioral
  asymmetry) either way, which is exactly what `AGENTS.md`'s "always plan
  before implementing" exists for.

---

## F8 — Bootstrap interpreter: no reliable deep-recursion guard

**Issue:** `#248`
**Branch:** *(none yet — needs a plan first)*
**Depends on:** none
**Blocks:** none

### Deliverable

`bootstrap/loxpp_interpreter.lox`'s own recursion shares the native VM's
`FRAMES_MAX = 256` call-frame budget with the *target* program's own call
depth (`notes/bootstrap-stack-depth.md` has the full measurement table). No
counter written in Lox++ can see or bound that combined budget, so any
bootstrap-interpreter-level depth guard can be preempted by an unguarded
native "Stack overflow." crash. X7 already shipped a disclosed best-effort
mitigation (`stringify()`'s guard at threshold 100); this issue is the
general case. **Needs a design plan before code** — three directions are
named in the issue, none attempted:

1. Export the native VM's own current call-frame count so
   `loxpp_interpreter.lox` can see the combined budget.
2. Restructure recursive bootstrap-interpreter functions to use fewer
   native frames per level of Lox++-level recursion.
3. Raise `VM::FRAMES_MAX` (moves the ceiling, doesn't remove the coupling).

Also worth folding in per the issue: whether native's own stack-overflow
path should route through the catchable-error mechanism at all (currently
it doesn't, on any backend) — a separate, pre-existing spec question the
issue surfaces but doesn't resolve.

### Scope for this node

Depends entirely on which direction the approved plan picks — could touch
`src/vm.h`/`vm.cpp` (option 1 or 3) or only
`bootstrap/loxpp_interpreter.lox` (option 2). Post the plan first; this is
explicitly the largest-scope, least-defined node in this backlog.

### Checkpoint (must pass before merge)

Depends on the approved plan. At minimum: the existing measurement table in
`notes/bootstrap-stack-depth.md` should be re-run and either confirmed
still accurate or updated, and whatever guard results should be
demonstrated against ambient call depths of 0, 20, 40, and 100+ (the same
points the existing table measures).

### Hazards

- This is the one node in this backlog large enough that it might deserve
  splitting into its own small DAG rather than a single node, once the plan
  is written — don't force a one-PR fix if the approved direction turns out
  to need more.

---

## F9 — Permanent test coverage: defer runs on the uncaught-throw path

**Issue:** `#235`
**Branch:** `test/defer-runs-on-uncaught-throw`
**Depends on:** F1, F2, F3 (wave 1) merged
**Blocks:** none

### Deliverable

No permanent example or unit test currently verifies that `defer` blocks
run when a throw escapes its protected region entirely (uncaught) rather
than being caught — `spec/04-semantics.md` defer step 5 requires this, and
PR `#232` (X3) fixed the underlying reentrancy/root-marking bugs across two
review rounds but never captured the case as a checked example.

### Scope for this node

Add an example (`examples/`) and/or `test/`-level test: a function with
`defer` blocks inside a `try`/`catch`, throwing an exception that escapes
uncaught, verifying LIFO defer order both normally and under
`LOXPP_STRESS_GC=1`.

### Checkpoint (must pass before merge)

- New test fails if the defer-list draining logic regresses (prove it can
  fail — temporarily break the drain-on-unwind path, confirm the new test
  catches it, then restore).
- Runs clean under `LOXPP_STRESS_GC=1`.
- Full rebuild + full `ctest` green.

### Hazards

- Wait for F1-F3 to land first — they touch the same handler-stack/local
  accounting machinery this test exercises. Writing this test before they
  land risks asserting behavior that's about to change.

---

## Execution notes for whoever picks this up

- Wave 0's precedent (`#257`/`#259`/`#260`) worked well as direct
  implementer forks with no separate reviewer round, because each fix was
  independently, exhaustively self-verified (full rebuild, full `ctest`,
  the exact repro from the issue) before opening its PR. The same shape is
  probably right for F1, F2, F3, F5, F6, F9 — none of them need a design
  decision, just a correct fix.
- F4 (JVM audit) should explicitly reuse PR `#252`'s audit methodology —
  cite it in the PR description, don't rediscover the approach.
- F7 and F8 are the two nodes that must not skip `AGENTS.md`'s
  plan-then-approve step. Post the plan as a comment on the issue first.
- If wave 1's three nodes are run in parallel by three different agents,
  consider a lightweight referee/reviewer pass given `#240`/`#242`'s
  possible shared root cause (see each node's hazards) — this is exactly
  the "recurring bug shape across sibling nodes" pattern
  `notes/multi-agent-playbook.md` now documents from this same mission.
