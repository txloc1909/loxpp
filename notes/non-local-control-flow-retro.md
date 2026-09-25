# Non-local control flow: retrospective

## Purpose

`try`/`catch`/`throw`/`defer` (mission #223) was declared done on 2026-09-15.
Bug discovery didn't stop there: a second mission (#288, fault taxonomy), a
third (#414, defer semantics), and a fourth wave of unrelated gaps
(#431/#320 and #350/#388 in the JVM/CLR backends, #434 in the bootstrap
parser) all followed before the feature actually stabilized, running
through 2026-09-24. This note draws process lessons from that history, to
apply to the next feature with the same shape — coroutines/generators,
`notes/expressiveness-roadmap.md` item 5, which suspends/resumes across the
same shared-bytecode pipeline and will stress the same failure modes
harder.

Every claim below cites a GitHub issue or PR number, not a section of this or
any other note. Numbers are permanent and their content is fixed at merge/close
time. To re-derive the scale claim below yourself: `gh issue list --repo
txloc1909/loxpp --search "defer OR throw OR catch OR handler OR fault OR
non-local" --state all`.

## What happened, in four waves

**Wave 1 — the mission itself.** Tracking issue #223, seven nodes:
#224 (PR #231, prove the shared-bytecode CFG/merge-consistency risk before
building anything), #225 (PR #230, spec), #226 (PR #232, native VM), #227
(PR #237, JVM backend), #228 (PR #236, CLR backend), #229 (PR #246,
differential tests + `ObjFile` leak fix — the mission gate), #243 (PR #245,
bootstrap interpreter — added retroactively after #226 merged, when CI on
`main` went red on the bootstrap step with no node covering it). A follow-up
CLR fault-kind fix landed in PR #252. Tracking issue #261 ("Wave 1-4
correctness backlog") and #287 ("handler-stack unwind is not centralized")
opened during and after this wave to hold what didn't fit inside it.

**Wave 2 — fault taxonomy, mission #288.** Individual disagreements
between backends and the spec's Runtime Errors table kept surfacing after
#223 closed: #267, #268, #271 (native/JVM stack-overflow catchability),
#283, #284, #325, #338, #341 (catchability/identity mismatches on native
and JVM). #288 organized fixing this properly: N1 built an exhaustive
fault ground-truth table in the spec (#330, PR #339), then wired every
backend's fault sites to it — JVM (#333, PR #345), CLR (#334, PR #344),
bootstrap (#335, PR #346) — and added a differential check across all four
(#336, PR #352). Even with a table and a generator/checker
(`check_fault_table.py`) in hand, the bootstrap side alone needed a
six-item tail (#356) covering #347, #348, #349, #351, #353, #354 before
#288 actually closed, plus more disagreements afterward: #355, #358, #359,
#367, #368, #370, #371, #375, tracked to a close via #394.

**Wave 3 — defer semantics, mission #414.** #311 (`return EXPR;` ran
`RUN_DEFERS` before evaluating `EXPR`, PR #419), #319 (managed backends ran
deferred calls during an uncatchable-fault unwind that native treats as
fatal, PR #423), #326 (a stack-overflow unwind ran one fewer deferred call
than the frames it unwound, PR #427) — all three merged without referee
escalation. But #414 opened three more follow-ups in the process: #420
(this session, PR #428), #421 (CLR stops draining defers once one of them
itself throws — closed by PR #430), #426 (CLR didn't catch a stack
overflow at all in a function with a defer — fixed alongside #326 by PR
#427).

**Wave 4 — capture-analysis and compile-time gaps, on 2026-09-24.** #431
(JVM: a closure capturing a local declared *inside* a `catch` body, or
capturing the catch-bound value `e` itself, fails at emit time — "capture
analysis does not report" this slot) turned out to share one root cause
with the older #320 (the same crash on CLR): `capture_analysis.cpp`'s
Pass 1 (which captured slots are open) never seeded a catch block's entry
state, so Pass 2 treated any closure declared inside `catch` as
unreachable and silently dropped its capture. Pass 0 (frame height)
already had a hand-written seed for catch blocks, from the block's own
`PUSH_HANDLER` — Pass 1 just never got the matching one. PR #433 fixed
both issues from one change, in shared code, confirmed against both
backends. Separately, PR #432 closed #350/#388 — a JVM `VerifyError` from
the catch binding `e`'s own store reading its slot's stale content before
its first write, reachable because the classic verifier treats
catch-handler entry as reachable from anywhere in the guarded region. And
#434, found while investigating an unrelated resolver-message issue
(#410), showed bootstrap's parser had never supported the block form of a
`match` arm body at all — `break`/`continue` inside `case ... => { ... }`
failed to parse, a gap native's compiler and spec (`armBody`,
`spec/02-syntax.md`) had allowed since before mission #223 existed. Found
while building PR #435 (a differential harness for bootstrap's own
resolver/compile-time errors, closing #410 — the gap #410 had already
flagged: bootstrap's compile-time path had never been diffed against
native's the way its runtime fault path was by #288/#336) and fixed
separately by PR #439.

## Four root causes, each recurring more than once

**1. Differential testing was the second-to-last node, not a continuous
gate.** Wave 1's own node order was native (#226/PR #232) → JVM (#227/PR
#237) → CLR (#228/PR #236) → *then* differential tests (#229/PR #246).
Three independent implementations existed, and had each already gone
through their own PR review, before anything checked them against each
other. #240 (defer's list slot colliding with a catch-bound variable, JVM
+ CLR) and #242 (`match` on a caught value plus a sibling `try`/`catch`
producing wrong output) were found only after that — both by PR #269,
post-merge, re-diagnosed from scratch rather than caught by the JVM or CLR
node's own review while the design was still fresh in that implementer's
head. The same gap existed one level up, at compile time, and stayed
open even longer: #410 named it directly ("no differential harness for
bootstrap's compile-time (resolver) errors against native") on 2026-09-22,
but nothing built one until PR #435 landed on 2026-09-24, nine days after
mission #223 first closed. #434 (bootstrap's parser silently rejecting
`break`/`continue` inside a `match` arm block) was found by hand while
building that harness, hours before it merged — evidence the gap #410
flagged was real, not evidence the fix came in time to catch it
mechanically.

**2. The spec stated intent, not a contract.** #225/PR #230 wrote grammar
and prose ("mark each fault catchable via try"). Prose over a ~30-fault ×
4-backend × (catchable? kind? message? `type()`/`str()` identity?) matrix
is exactly what drifts silently, and it took a second mission (#288) to
turn it into an actual enumerated table (#330/PR #339) with a mechanical
row-count gate (`check_fault_table.py`, wired in #336/PR #352). Everything
between #223 closing and #330 merging — #267, #268, #271, #283, #284,
#325, #338, #341 — is the cost of not having that table from the start.

**3. Bug classes got fixed per occurrence, not per class.** The insight
"a compiler-declared contract (handler entry depth, catch-bound locals,
region end, stack-height accounting) must be carried through, not
rediscovered by scanning/replaying generated code" had to be found
independently at least four times: during #224/PR #231's own review,
again in #227/PR #237's review (four review rounds before it converged),
again in #228/PR #236's review (the same bug shape recurred three times
under different code paths — br-vs-leave, terminal-catch-as-last-code,
branching-inside-catch — before a structural fix), and again post-ship as
the handler-stack-leak family — #241 (PR #265, return from an open try), #273 (PR #277,
break/continue out of an open try), #286 (break/continue inside catch,
unified with #287 by #384/PR #385, "one unwind rule for handler stack").
The compiler-side version of the same class recurred separately: #240 and
#242 (both PR #269, match/catch local-slot collisions), then #420 and its
own round-2 companion bug in `dot()`'s defer branch (both PR #428, one
found only because an independent reviewer agent swept the surrounding
code instead of trusting the original fix's scope). It recurred a sixth
time on 2026-09-24: #320/#431 (PR #433) is the same "a
compiler/analysis-declared contract for catch-entry state must
be carried through every pass that touches it, not assumed" shape,
specific to `capture_analysis.cpp` — Pass 0 (frame height) had a
hand-written catch-entry seed, but nobody had propagated the same seed to
Pass 1 (open-capture tracking), so the fix that unified handler-stack
unwind (#384/PR #385, in the *VM's* unwind code) never touched the
*compile-time* capture-analysis pass that has its own, separate notion of
catch-block reachability. Naming the bug class once, per #384's own
review, was not enough to make every future pass over the same CFG apply
it — the class has to be checked against by name at each new pass's
review, not assumed inherited from a sibling pass's fix.

**4. Partial porting — some call sites converted, others missed — recurred
on every port.** Native's fault sites got converted to the catchable
`Error` path in #226/PR #232; JVM's port under- and over-caught in #253
and #254 (both fixed before #288 even started); CLR's had its own gaps
(#238, CLR caught every fault kind inside `try`, not only catchable ones);
bootstrap's had the largest tail of all (#301, then #288's own N6/#335 plus
the six-item #356 tail). #288's table-driven, mechanically-checked
approach is what finally stopped this from recurring a fifth time — it
should have been the approach from #226 onward, not bolted on after three
backends had each independently under/over-ported.

## Rules for the next feature shaped like this (coroutines/generators, roadmap item 5)

1. **Enumerate every consumer of the changed bytecode before writing the
   node list**, not during implementation. #223's original plan didn't
   include the bootstrap interpreter at all — #243 was added only after CI
   caught it red following #226. Native VM, JVM backend, CLR backend,
   bootstrap interpreter, plus anything else that parses/lowers the
   construct (tooling resolver needed #233, tree-sitter grammar needed
   #244) — list all of them in the first node-planning pass.
2. **If the spec statement ranges over an enumerable set (fault kinds,
   opcodes, exit paths, suspend points), the spec deliverable is a table
   with a mechanical completeness check** (the #330/PR #339 +
   `check_fault_table.py` pattern), not prose — built before any backend
   node starts, the way it should have been for #225/PR #230.
3. **Every backend node's own Definition of Done includes diffing clean
   against every backend that already merged**, checked at that node's own
   review — not deferred to a dedicated differential-testing node at the
   end the way #229/PR #246 was scheduled. #240 and #242 are the direct
   cost of deferring this.
4. **The moment a reviewer or referee names a structural bug class in one
   node's review, write it into the design doc as a stated constraint for
   every remaining node**, so the next implementer applies it by
   construction. The "declared contract, not scanned/replayed state"
   insight paid for itself three times before #223 even shipped (#231,
   #237, #236) and a fourth time after (#384/PR #385) — naming it once, in
   writing, before the second occurrence would have saved at least two of
   those. It cost a sixth occurrence anyway (#320/#431, PR #433) because
   "write it into the design doc" was never checked against every pass
   that shares the same underlying data (here, `capture_analysis.cpp`'s
   three passes over one CFG) — only against every *node*. A constraint on
   catch-block reachability belongs to the CFG/analysis layer itself, not
   to whichever node happened to be open when it was named; the design doc
   entry has to say so explicitly, or the next pass added to that same
   file has no reason to go looking for it.
5. **A mission's differential-testing gate has to cover every phase the
   feature touches — compile-time included, not only runtime.** #229/PR
   #246 (Wave 1) and #336/PR #352 (Wave 2, fault taxonomy) both diffed
   *runtime* behavior across all four consumers. Nothing diffed
   *compile-time* behavior (resolver/parser errors) the same way, so
   bootstrap's `match`-arm-block parser gap (#434) sat unnoticed from
   mission #223's close until #410 named the missing harness, nine days
   later, and #434 itself was only found by hand while that harness (PR
   #435) was being built — not by the harness catching it, since it
   didn't exist yet. A "differential tests" node's Definition of Done must
   name both phases, not assume compile-time parity follows from runtime
   parity.

## Cross-reference

Design doc (intent and the shared-bytecode risk analysis that gated #224):
`notes/non-local-control-flow.md`. Bug-class detail for the shared JVM/CLR
translation pipeline specifically: `notes/bytecode-translation-problems.md`.
Both are useful for *how* the fixes work; this note is about *why so many
were needed* — for that, the issue/PR numbers above are the record, not
these files' prose, which can and does get edited.
