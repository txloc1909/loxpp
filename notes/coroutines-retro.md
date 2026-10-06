# Coroutines / generators: planning retrospective

## Purpose

The single-core suspension mission (tracking issue #523) planned QBE backend
support as two nodes: node 7 (#530) would make functions that contain `YIELD`
fall back to the interpreter loop, and a follow-up (#535, blocked on #530)
would later compile those functions to native QBE code and drop the fallback.
During #530's planning, the implementer found the split unsound and both
issues were resolved by one change (PR #559), with no interpreter fallback
ever landed.

This note records *why the original two-phase breakdown did not foresee that
the fallback was unsound*. It is a planning retrospective, not a design
record — the design lives in `notes/coroutines.md` and `notes/qbe-backend.md`.
Every claim cites a GitHub issue or PR number, or a file path in the tree at
the time of writing; numbers are permanent and fixed at merge/close time.

## The unsoundness, stated precisely

Node 7's scope (issue #530) and `notes/coroutines.md` ("Backends", as first
merged in #534) both named the fallback set the same way: *functions that
contain `YIELD` run through the interpreter; everything else is compiled.*
PR #559 rewrote that section to the whole-program coroutine mode that
shipped.

"Contains `YIELD`" is a syntactic property of one chunk. The property that
actually governs suspension is semantic and call-chain-wide: *a frame must
suspend if it can be live beneath a suspended callee.* When `inner` yields,
every caller on the coroutine's stack suspends too — including a caller that
contains no `YIELD`. QBE compiles each caller to a real C frame, which cannot
be frozen mid-call. Because `CALL`/`INVOKE`/dunder dispatch is dynamic, the
static set of functions that *can* be on a coroutine's call chain is the whole
program. So the only *sound* fallback is whole-program interpretation — a
second interpreter inside the QBE binary, which defeats the backend. Phase 1
therefore had no sound, useful form; #530 and #535 were one indivisible change.

The decisive counterexample was `test/coroutine-probes/01_yield_across_call.lox`
(added at node 2, #525). Its own header comment states the mechanism: "A yield
inside a function the coroutine calls suspends the whole coroutine: the
caller's frame stays live beneath the suspended callee."

## Why the breakdown missed it

**1. It selected the fallback set by a syntactic proxy for a semantic
property.** The node wrote down "contains `YIELD`" and never tested that it
equalled "must suspend." The two differ exactly because suspension propagates
up the call chain and the call graph is dynamic. This is the root slip; the
rest is why it survived review.

**2. The native-VM mental model was carried to QBE without re-checking the
boundary.** In native the whole machine is the interpreter: `YIELD` exits the
nested run and snapshots the entire stack slice, so caller frames come along
for free regardless of which one yields (`notes/coroutines.md`, "Native VM": a
coroutine "owns exactly the interpreter state whose frames lie inside its
slice"). There is no "which functions need the interpreter?" question. QBE
introduces a partition — some functions compiled, some interpreted — and the
suspension boundary falls exactly on that partition line. The fallback framing
reused native's "it's all interpreted anyway" intuition and hid QBE's one hard
part.

**3. The governing constraint was on record but never pulled down to node
granularity.** `notes/concurrency-model-decision.md` C9 (lines 107–116) says
QBE's real-C-frame model is "hostile to mid-frame stackful suspension: suspend
points [would need] to unwind out of every compiled frame… Any suspend/GC-root
design must be proven on QBE, not only in the interpreter loop," and its
recommendation (lines 196–198) says "Item 5's plan must now include QBE: a
suspend point has to be expressible in QBE-compiled code, not only in the
interpreter loop (C9)." "Unwind out of every compiled frame" is literally the
caller-frame problem. The mission plan (#523) acknowledged C9 as the key risk,
then resolved it with "fall back to the interpreter loop" for `YIELD`
functions — precisely the outcome the note warned was insufficient. The
constraint was known strategically and contradicted at the task-breakdown
level.

**4. The counterexample was already in the planned corpus — used as a runtime
oracle, not as a design check.** Probe `01_yield_across_call.lox` was written
at node 2 (#525), the research gate that exists "before any backend code." But
the corpus was treated purely as a pass/fail oracle (does QBE output match
native?). The probe passes trivially on the *native* primitive (#526), so at
the gate it looked settled. No one ran it on paper against the *proposed QBE
fallback design*. The hard case was written down, then not applied to the plan
it refutes.

**5. It was mis-shaped as a fast/slow pair with a clean dependency.** "#530
correct-but-slow fallback → #535 optimize to native" is the standard "make it
work, then make it fast" decomposition — which is why it read as reasonable.
That shape is only valid when phase 1 is independently correct. Here phase 1 is
wrong for any program whose call chain crosses a compiled frame, and the only
correct fallback is the very whole-program interpretation #535 was meant to
remove. There was no monotone path from a correct #530 to #535. A tell was
visible in the artifacts: #535's own body already carried the C9 reason the
fallback cannot be extended — the follow-up issue described the same single
problem from the other end.

**6. The process rules the mission adopted did not target this failure mode.**
`notes/coroutines.md` says the mission follows the five rules in
`non-local-control-flow-retro.md`. All five are about coverage/consistency
across already-identified consumers (enumerate every backend, mechanical spec
tables, per-node differential Definition of Done, write named bug-classes into
the design doc, diff compile-time too). None asks "is each node's internal
correctness criterion sound, or just a plausible subset?" Those rules were
written to prevent #223's failures (a forgotten consumer, a deferred diff), not
an "the phase-1 scope is unsound as scoped" failure.

## What actually caught it, and the cost

The catch came at #530's own planning/review, before any fallback code shipped,
because each node re-plans before coding (`AGENTS.md` planning policy) and the
research-gate probe made the refutation immediate. This was a foresight gap in
the breakdown, not a shipped bug; the cost was one re-plan.

## Rule for the next breakdown shaped like this

Proposed as a sixth rule for the list in `non-local-control-flow-retro.md`
(same spirit). Issue #564 tracks adopting it as a process rule:

6. **When a node scopes a feature as "handle only the subset that
   syntactically contains construct X," prove that subset is closed under X's
   propagation — data flow, control flow, and the *dynamic* call graph —
   before writing it as a node.** A syntactic selector ("contains the `YIELD`
   opcode") is not the semantic set ("frame can be live under a suspension")
   unless propagation is bounded, and dynamic dispatch rarely bounds it. Run
   the research-gate probes against the *proposed per-backend design on paper*,
   not only against native output; a probe that passes on the native primitive
   proves nothing about a backend whose fallback boundary the probe crosses.
   Corollary: re-apply the strategic constraint list (here the C-list in
   `notes/concurrency-model-decision.md`) at each node, not only at mission
   top — C9 named this exact hazard and was not consulted when node 7 was
   scoped.

## Cross-reference

- Mission: #523. Nodes cited: #525 (probe corpus / research gate), #526
  (native primitive), #530 and #535 (the two QBE nodes), PR #559 (the single
  change that resolved both), #564 (the proposed rule 6).
- Design records: `notes/coroutines.md` (coroutine design),
  `notes/qbe-backend.md` (QBE backend; the real-C-frame model C9 cites),
  `notes/concurrency-model-decision.md` (C9 and the item-5 implications).
- Prior retro whose rule list this extends: `notes/non-local-control-flow-retro.md`.
