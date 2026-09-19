# Mission brief — catchable `StackOverflowError` across all four consumers

Tracking issue: #271. This file holds the rules that bind every node in this
mission. A node issue (#267, #268, #238, #248) holds that node's own
deliverable and checkpoint. Read both, in that order — this file first.

This file is scaffolding for one mission run. It gets deleted once the
mission closes, the way `notes/jvm-emission-contract.md` and
`notes/backend-implementation-dag.md` absorbed the durable parts of earlier
mission briefs. A node PR that needs to cite a lasting fact from here should
also add that fact to a permanent note (`notes/bootstrap-stack-depth.md` for
the bootstrap findings), not only cite this file.

## The ruling (already decided, do not reopen)

`spec/04-semantics.md` says every runtime error in its table is catchable,
including `StackOverflowError`. Today no backend satisfies this row, each in
a different way. Per `AGENTS.md`, when spec and implementation disagree, the
implementation gets fixed. See the ruling comment on #267 for the full
reasoning. Native's fatal-path choice (PR #232) was a real reentrancy
hazard, not a reason to drop the spec row — the fix is a frame reserve, so a
`catch` handler has room to run.

**The mechanism is not decided.** How many frames to reserve, and where, is
for each node's implementer to measure. Do not carry a number from one
node's measurement into another node as if it were a fixed constant — the
budget changed once already (`FRAMES_MAX` 256 to 1024, `STACK_MAX` 2048 to
16384, PR #300) and any old ratio needs re-measuring at the new budget
before it is trusted again.

## Node order

1. **N1 — native** (#267, `fix/native-catchable-stack-overflow`). Defines
   what "catchable stack overflow" means. Every other node is audited
   against it, not the other way around.
2. **N2 — JVM** (#268, `fix/jvm-frame-ceiling`) and **N3 — CLR**
   (#238, `fix/clr-catch-mechanism-catchability`) run in parallel, both
   depending on N1 merged. Neither depends on the other.
3. **N4 — bootstrap interpreter** (#248, second half only,
   `fix/bootstrap-catchable-stack-overflow`), last. Depends on **N1 and
   N3**, not on N2 — `loxpp_clr_bootstrap.sh` runs the bootstrap
   interpreter on the CLR host, so N4 needs N3's clean catch mechanism, not
   the JVM path. This corrects the mission's own original assumption; see
   the orchestrator comment on #271 for why.

N4's design is a host `try` at the call boundary
(`LoxFunction.call`/`interp.execBlock`) and one at the top level
(`parse()`/`resolve()`), converting the host's own `StackOverflowError` into
the bootstrap's error and restoring `pendingDefers`, `stringifyDepth`, and
the return flags on the way out — not the evaluator-depth counter #248
originally proposed. Keep that counter's prototype on record as the
fallback if N1's frame reserve turns out too small (about 3 frames) to run
a Lox++ handler; do not delete it.

N4 also owes two checks beyond the catch mechanism itself: a counter-leak
test (assert bootstrap state — `pendingDefers`, `stringifyDepth`, return
flags — is back at its start value after each of the 88 examples, not only
that the example passed), and `tools/check_bootstrap_headroom.sh` wired
into `ci.yml`'s release leg (every example survives 256 extra frames; the
7 shape probes in `research/bootstrap-depth-evidence` reach 90% of their
post-F10 ceilings). The headroom check may ship as its own small PR rather
than inside N4's.

Read branch `research/bootstrap-depth-evidence` (commit `3146398`,
`tools/research/bootstrap-depth/`) before rebuilding any measurement for
N4 — the 7 probes, `measure.sh`, and `headroom.sh` already exist there.

## Known, out-of-scope divergence

While proving N2's own ceiling, an implementer may find
`test/translation-probes/clr-only/known-divergence/52_fat_frame_stack_divergence.lox`:
a frame with many locals overflows native's value-stack guard
(`STACK_MAX`) well before the CLR's frame-count guard fires, since CLR
counts calls, not value-stack slots. This is a real, already-tracked, and
already-tested gap (see `tools/check_clr_probes.sh`'s
`known_divergence_probes` group). It is **not** part of this mission. Do
not let a review block a node on it. If a node's own fix happens to close
it as a side effect, say so in the PR; otherwise leave it as-is.

## Not part of this mission

- #263 — the bootstrap interpreter's nine missing stdlib globals. Real,
  unrelated theme.
- #264 — moving mission scaffolding onto GitHub Issues. Already done
  (PR #275); this is why this brief cites node specs by issue number rather
  than a local `nodes/*.md` file.

## Everything else

Playbook conventions (`notes/multi-agent-playbook.md`) apply as written:
roles and tags, the 3-round dispute and stagnation limits, referee format,
"state comes from GitHub and git, never a written progress file," and
ASD-STE100 for every GitHub message.
