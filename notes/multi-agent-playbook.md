# Multi-agent DAG playbook

Reusable doctrine for running a multi-agent mission that drives a DAG of
implementation nodes to green, one pull request per node. Proven three times:
by the JVM backend mission (`#96`–`#116`), the CLR backend mission
(`#125`–`#153`), and the non-local-control-flow mission (`#223`, `#230`–`#252`)
that threaded `try`/`catch`/`throw`/`defer` through the native VM, the JVM
backend, and the CLR backend, plus a fourth target the mission's own risk
analysis had not named at the start — see "Scope a mission around every
consumer of the surface it changes" below. Written for the next such mission,
so it does not need to be rebuilt from scratch.

The CLR mission reused all four analysis modules the JVM mission built, without
a change. It therefore paid none of the cost of the JVM mission's two most
expensive nodes. Read `notes/jvm-emission-contract.md` before you plan a third
target: the reuse point it names is why the second target skipped that cost
entirely, even though it opened more PRs overall (`#125`–`#153`, 29 PRs, versus
`#96`–`#116`'s 21).

This file describes the workflow. Engineering rules that apply outside a
multi-agent run live in `AGENTS.md`, not here.

---

## Roles

Three roles, tagged per `AGENTS.md`'s multi-agent conventions
(`[Implementer]`, `[Reviewer]`, `[Researcher]`) whenever they share a GitHub
account:

- **Implementer.** Delivers the first working version of a node and opens its
  PR. Resolves or rebuts every reviewer finding. Never waits for CI — see
  below. Merges once the reviewer approves.
- **Reviewer.** Reviews adversarially: assume the change is wrong until you
  prove it right. Reproduces the node's checkpoint in its own worktree —
  never approves on trust. Does not fix code itself; it reports.
- **Researcher.** Normally idle. Starts under one of three conditions (below)
  and either unblocks the implementer or, as referee, hands down a binding
  ruling.

## The harness

`.claude/workflows/backend-dag.js` runs this loop. It is generic — no target
(JVM, CLR, ...) is hardcoded into its control flow. A mission run supplies,
via `args`:

- `missionIssue` — the tracking issue number that sequences this mission's
  nodes (see "Node specification structure" below). The stage order and the
  full node-to-issue list live there, in git-hosted, reviewable, permanent
  GitHub state — never in a local directory a host can lose.
- The mission brief needs no argument of its own. Every agent reads it with
  `gh issue view <missionIssue> --repo <githubRepo> --comments` — the same
  tracking issue `missionIssue` already names, read the same way a node
  reads its own spec. The brief holds binding mission-wide rules: role
  assignments, execution notes, and any correction to the tracking issue's
  own body (a changed dependency, a redesigned node) posted as a later
  comment. It does **not** hold per-node specifications — those are GitHub
  issue bodies, one issue per node (see below). A committed local file for
  this (an earlier design of this harness used `briefPath`, a
  `notes/missions/<name>.md` file) is what #264 set out to remove: mission
  state belongs in git-hosted, reviewable, permanent GitHub state, not in a
  file a host can lose or that goes stale the moment the mission closes.
  Treat a missing or unreadable brief (the `gh issue view` call itself
  failing) as a hard failure, not as optional.
- `nodes` — a map of node id → `{ branch, title, issue }`. `issue` is the
  node's own GitHub issue number; `id` is any mnemonic the mission wants for
  logging (it does not have to match the issue number, though matching it is
  the simplest choice for a new mission). This map is still authored by
  whoever launches the run — `tools/agent-workflow/plan_resume.py
  <mission-issue>` derives the dynamic `stages`/`resume` args from GitHub,
  but `nodes` itself (branch names, titles) is not tracked anywhere in git or
  GitHub, so merge it in by hand each time.
- `stages` — an array of node-id groups; each group runs in parallel, groups
  run in sequence. `plan_resume.py` derives this from the tracking issue.
- `repo`, `githubRepo`, and the doc paths a node implementer/reviewer must
  read (the DAG plan doc, the target's opcode/semantics reference, ...).

Before a new mission's first run, edit `meta.phases` in the script to name
that mission's real nodes — it drives the progress-tree preview and must stay
a literal, so it cannot be derived from `args.nodes` automatically.

Every agent reads its node specification with
`gh issue view <issue> --repo <githubRepo> --comments` — **never** the plain
`gh issue view <issue>`. The plain form prints the body only; it gives no
sign that comments exist, and a cross-node hazard left by an earlier node
lives only in a comment (see "Node specification structure"). An agent that
uses the plain form gets a spec with a silently missing hazard, the same
failure mode a lost mission directory used to cause. The harness prompt
always gives the full command, and an agent whose `gh issue view --comments`
call fails stops with `blocked_surprise` rather than continuing on a partial
read.

## Escalation limits

Two defaults, both tested across the past mission's nodes:

- **3-round dispute limit.** A reviewer finding the implementer disputes goes
  to a referee after 3 rounds without agreement.
- **3-round stagnation limit.** The same file yields a new blocking finding
  for 3 rounds straight, even with no dispute. See "Escalate on stagnation"
  below.

## Implementers do not wait on CI

An implementer returns as soon as its PR is open (or its fix is pushed). It
never polls CI in a loop. Early nodes in the JVM mission burned roughly a
quarter of their tool calls on CI polling before this rule was added; the
reviewer starts immediately and verifies independently, and CI is watched
once, at the merge step, by the merge step alone.

## Escalate on stagnation, not only on dispute

Both design-changing referee decisions in the past mission (`#113`, `#115`)
started while the implementer and reviewer *agreed with each other* every
round — nothing was in dispute. Each round just found one more consumer of
the same wrong mechanism. A referee trigger that only fires on disagreement
misses this pattern entirely. Keep both triggers.

**Expect false positives, and keep the trigger anyway.** Seven referee
decisions in the CLR mission came from the stagnation trigger, and none came
from a dispute — the implementers accepted almost every finding. Five found a
real design fault. Two fired because three different defect classes happened to
share one file name, and the referee correctly ruled that the series had
converged. That is two false positives in seven firings, and each costs a
single cheap referee round. Ruling 6 of that mission, which the trigger caught,
found thirty-six unchecked sites whose failure mode was a module that exits 0,
assembles, and then dies at run time naming no instruction. One such catch pays
for many false positives.

**A trigger can also fail to fire.** The same mission ran four rounds on one
interlocked pair — a stack limit, and the probe whose ability to fail depended
on that limit — with a different blocking fault each round, and no referee
started. The counter only advances when the reviewer attaches the same file to
a blocking finding, so a series that moves between neighbouring files slips past
it. If you see the same *mechanism* fault three rounds running, escalate by hand
and say that is what you are doing.

**A trigger can also fire on the wrong file and still be right.** The
stack-overflow-catchability mission's node N2 (`#268`) hit this from the
other direction: the counter advanced on `test/translation-probes/README.md`
for three rounds running, because every fix round happened to add a new
probe file and touch the index — the README itself was never the defect.
The real recurring mechanism was the JVM's `s_unwindingStackOverflow`
guard: three differently-shaped findings (round 1's R1, round 2's R5/R6,
round 3's R8) each caught a case where clearing that guard by matching a
delivered value's `kind` field was neither necessary nor sufficient. The
referee's own first step — verify the facts yourself, never the trigger's
file label — is what caught this: the ruling named the guard's lifetime as
the fault and treated the README as coincidental, not the other way round.
Read the trigger's file name as "look closely at what these three rounds'
findings have in common," not as the diagnosis itself.

**The trigger's hit rate can run much higher than "expect false positives"
implies.** The non-local-control-flow mission escalated 5 of its 7 nodes to a
referee, and every one found a genuine structural bug, not a false positive:
`#231`'s referee found `cfg.cpp` had several independent edge-adding call
sites and only one had been patched; `#232`'s found a missing
found-a-handler/exhausted-the-stack flag that made every successful throw fall
through into the uncaught-throw report path; `#236`'s found the CLR emitter
generating `br` where ECMA-335 requires `leave` to exit a protected region;
`#237`'s found one fixpoint-seeding pass being treated as sufficient when
handler seeding is itself a fixpoint; `#245`'s exhaustive audit (see "Audit
against a countable ground truth" below) found the 17 sites round 3's own fix
had already named, and no others. Do not read "expect false positives" as
license to raise the trigger's threshold — a mission this size validated the
3-round default at close to 100% precision. The stack-overflow-catchability
mission adds two more data points at the same precision: node N1's referee
(`#267`) found a redundant threshold and an unrooted thrown value behind
four rounds of one-symptom-at-a-time findings in `src/vm.cpp`, and node
N2's referee (above) found the guard-lifetime fault the trigger had
mislabelled. Both were genuine redesigns, not false positives, out of a
mission with only two stagnation calls total.

## Referee decision format

A referee decision that ends a stagnation loop in one round has a fixed
shape: **verify the facts yourself first** (read `spec/`, `src/`, run the
code — don't take either agent's word for it), **then rule** — assign the
tag to the implementer, the reviewer, or a third option — **then bound the
next round**: state the exact action that must now happen, so the loop
cannot simply repeat with the same ambiguity.

## Node specification structure

Each node gets one GitHub issue: deliverable, scope, checkpoint, and hazards
live in the issue body. This is the live channel between nodes — a merged
node that finds a hazard for a node it blocks posts a **comment** on that
later node's issue, never a body edit. A comment records who found the
hazard and when; a body edit hides both. This is why every agent reads its
node with `gh issue view <n> --repo <githubRepo> --comments` and never with
the plain form (see "The harness" above) — a hazard that arrived after the
issue was filed is invisible to the plain form.

Worked shape, generalized from the JVM mission's N4 (a mid-complexity node —
a first straight-line code generator), as an issue body:

```markdown
## Deliverable

<What this node builds, in one paragraph. List the files it touches.>

<Anything an earlier node already built that this one must reuse exactly,
not rewrite — name the file and the interface. If the shared harness is
genuinely wrong for this node's needs, say so in the PR and fix it there,
with the reason; don't fork it silently.>

### Scope for this node

<Handle only named subset of the problem. Anything out of scope must fail
loudly with a message that names this node, so a later node fails visibly
if it hits something unhandled — never silently.>

## Checkpoint (must pass before merge)

<The exact commands that prove the node works, and what their output must
match. "Compare with diff, not by eye." Say which existing suites (unit
tests, example-program tests, format/lint) must also stay green.>

## Hazards

<Specific traps for this node's problem: a proven confusable pair, a rule
that the runtime/host enforces silently, a naming or determinism concern —
whatever a later reader would otherwise have to rediscover the hard way.>
```

The implementer's PR body must contain the line `Closes #<node-issue>`. A
node issue closes at PR **merge**, not at reviewer approval — GitHub does
this automatically from that line, so no agent has to remember a separate
close step, and no agent can close a node whose PR never merged. Approval is
unrelated and unchanged: it is still the PR comment whose first line is
exactly `[Reviewer] APPROVED` (`plan_resume.py` reads that, not GitHub's own
review-approval feature, because the reviewer shares the implementer's
GitHub account and cannot use `gh pr review --approve`).

### The tracking issue

The DAG structure — which nodes exist, which GitHub issue backs each one,
and the stage/wave order — lives in one tracking issue per mission, in the
shape issue #261 already uses: a `### Wave N` heading per stage, followed by
`- [ ] #<node-issue> — <description>` lines, one per node in that stage.
`tools/agent-workflow/plan_resume.py <mission-issue>` parses exactly this
shape. The checkbox itself is never read as truth — only a convenience for a
human skimming the issue — a node's real state always comes from a live
search of PR bodies for `Closes #<node-issue>`, the same string the
implementer prompt requires. A tracking issue is scope-only: "do not scope
code work directly against this issue number," the way #261 itself says.

## One node, one branch, one PR, one merge

Keeps every review small enough for the reviewer to actually finish reading
it. Do not batch nodes into one PR, and do not open a second PR for a node
that already has one — an implementer resuming a node checks for an existing
open PR first and pushes to it.

## State comes from persistent storage (commonly GitHub and git), never from a written progress file

`tools/agent-workflow/plan_resume.py` reconstructs each node's status by
querying GitHub alone — the tracking issue for stage order, and every PR's
body for the `Closes #<node-issue>` line that names its node — it does not
read or trust a hand-maintained status file, and as of the GitHub-issue
migration (#264) it needs no local git checkout either. This was correct
across 4 workflow launches, one interruption, and one incorrect manual stop,
and it is the reason resume never desynced from reality. A written
`STATE.md`-style file is fine as a human-readable snapshot, but never as the
resume source of truth.

**Match PR bodies literally, never by relevance search.** `plan_resume.py`
fetches every PR's body and regex-matches `Closes #<n>` itself; it does not
call `gh search prs`. That command was measured returning a PR for the query
`"Closes #240"` whose body did not contain the string `"240"` anywhere —
`gh search` ranks by relevance, not by literal substring, so it reported a
node as `merged` while its issue was still open. A literal match against
fetched text is the only form of this check that cannot manufacture a false
positive out of ranking.

**This applies to a guard the harness itself runs, not only to resume.** The
CLR mission added a check that skips a review round when the branch tip has not
moved, because a review against an unchanged commit can only repeat its own
findings. The check learned the tip from the implementer's own reported value.
An implementer returned without one, the guard never fired, and a full reviewer
round read a tree whose fixes existed only in a worktree. The rule has no
exception: read the tip with `git fetch origin && git rev-parse
origin/<branch>` — a bare `rev-parse` reads a possibly stale local
remote-tracking ref, not the true tip. An agent's report
of its own state is a claim, and a guard built on a claim guards nothing.

**This applies to a completion claim too, not only to a branch tip.** An
agent returning "done" is also a claim. Before treating a node, a review
round, or a cleanup pass as finished, check `git status`/`git log` in the
actual worktree it worked in — an agent can report success having made no
change at all, most often after losing track of which worktree it was in.
Cross-checking the claim against git costs one command; trusting a false one
costs a full round discovered only at the next stage.

## Run-observation tools

`tools/agent-workflow/snapshot.sh`, `status.py`, `watch.py`, and `watch_pr.py`
(the last also accepts a branch name and waits for its PR to open) exist so
an orchestrator can check on a long run without interrupting it. Take a
snapshot before suspending a run, and post a short recovery note on the
affected PR pointing the next agent at exactly what it should pick back up —
this recovered two otherwise-lost fix rounds in the JVM mission, both times
an implementer hit its step limit mid-fix, after compiling but before it
could commit, push, or reply.

`snapshot.sh` takes its output directory and its branch-name filter as
explicit flags — `--out <dir> --filter <substr>`, plus an optional
`--issue <mission-issue>` to also write a derived `state.json` via
`plan_resume.py`. It no longer infers either one from a mission directory's
basename: there is no mission directory left to infer them from, and an
inferred value that happens to be wrong fails exactly as silently as a
missing one. Snapshots themselves stay on disk — they are a backup for a
reboot or an interruption, never a source of truth, and nothing in the
harness reads them back.

## Diagnose a stall from three signals together

A long-running agent that has gone quiet is either working or wedged, and the
difference is not visible from any one signal. The CLR mission hit three
distinct stall modes:

1. **A self-matching pattern search.** An agent waited for its own build to
   finish with `until ! pgrep -f "clang-tidy -p <dir>"`. The shell running that
   loop carries the pattern in its own command line, so the search matched
   itself and the loop never ended. No build was running.
2. **A step limit in the middle of a merge.** A merge agent rebased onto a moved
   `main`, resolved a conflict, and stopped before it could push. The rebase
   existed only in the worktree.
3. **A tool call with no process behind it.** A reviewer issued one shell call
   and stopped. Forty minutes later no process was running that command, the
   transcript had not grown, and the run journal held no result for the agent.

**The reliable test is all three of these at once: the transcript file has not
grown, the host load is near zero, and no container is running.** Any one alone
is a false signal — a slow lint pass also has a flat transcript, and a reading
phase also has low load.

Two observation tools mislead here and must not be trusted for liveness:

- The turn counter in `status.py` can sit unchanged for forty minutes while the
  agent works, because it reports the last recorded transcript entry.
- `ls -t` is `eza` on some hosts and does not sort by modification time as a
  reader expects. Use `stat -c %Y` and sort numerically.

When the three signals agree, read the last transcript entry: a `tool_use` with
no matching result names the command that is stuck.

## One agent system per repository at a time

Two agent systems writing the same repository and its worktrees can corrupt
each other's work in ways no review catches, because each sees a tree the other
is changing underneath it. The CLR mission ran for over three hours beside an
unrelated agent tool whose working directory was the same repository, on a host
at nearly twice its core count in load. Fourteen agents died in that period to
an abort rather than an error, and the deaths stopped after the load fell.

That is a correlation with one clean counter-example — one agent died inside a
thirty-minute window with no orchestrator activity at all — and the cause was
never established. Record it as an unexplained failure, not as a proven one.
The rule stands on the correctness hazard alone: one agent system per
repository, and give a second one its own clone.

## A mission's backlog and follow-up work leaves as GitHub issues

`notes/` is for brainstorming and design records only (see `AGENTS.md`'s
"Backlog and task tracking"); a mission's own backlog lives in GitHub
Issues, never in a bullet list inside a mission brief or design note.

When an implementer or the orchestrator identifies work that is explicitly
out of scope for the current DAG — a node descoped from the plan, a
follow-up target the brief names but does not build — file one GitHub issue
per discrete item before closing the mission out:

- **One issue per item**, scoped the way a node spec is: what it is, why it
  was deferred, and what already exists for it to build on (a shared
  harness, a manifest slot, a reused analysis module).
- **Label it** with the repo's existing labels (`enhancement`, plus any area
  label the deferred work needs) so it is discoverable outside the mission's
  own history.
- **Link it from the design note**, if the note's reasoning for deferring the
  work still matters — the note keeps the *why*, the issue tracks the *when*
  and *by whom*.
- **A follow-up mission with several deferred nodes gets a GitHub Project**,
  not a new `notes/` DAG-plan-shaped list, so the items can be sequenced and
  tracked the same way an active mission tracks its own nodes through PR and
  review state (see "State comes from persistent storage" above).

This mirrors the mission's own rule for live state: GitHub, not a
hand-maintained file, is authoritative. A `notes/` design doc may still say
*why* something was deferred; it must never be the place where "is this done
yet" gets answered.

## Orchestrator text is not an authority against the source

A node specification states constraints, not mechanisms — say what must hold
("the topmost live local", "no bare `Object[]` in a Lox value slot"), not
which specific function must implement it. An orchestrator's own commentary,
however confidently worded, is not a ruling that overrides `spec/` or `src/`;
if it conflicts with the source, the source wins and the note gets corrected.

**An orchestrator may rule, and must label the ruling as its own.** When a
stagnation series does not trigger a referee, the orchestrator can state a
binding constraint itself rather than let a fifth per-site fix proceed. The CLR
mission did this once. The ruling was directionally right, and the referee that
fired afterwards reached the same verdict — and produced three measured facts
the orchestrator did not have, including the decisive one: the probe under
repair was still disarmed by ambient state after the fix that was supposed to
arm it.

The lesson is not that the orchestrator should stay silent. It is that an
orchestrator ruling is weaker evidence than a referee ruling, because the
orchestrator does not build a worktree and run the code. So:

- Write the ruling as a constraint, never as a mechanism. A mechanism stated by
  someone who has not run the code gets disproved inside one round.
- Say in the ruling that it comes from the orchestrator, so a later reader can
  weigh it correctly.
- Prefer waiting for a referee when one is possible. Rule by hand only when the
  trigger has demonstrably failed to fire.

## Scope a mission around every consumer of the surface it changes, not only the pipeline under risk analysis

The non-local-control-flow mission's brief (`#223`) built its entire risk
analysis around one fact: the native VM, the JVM backend, and the CLR backend
all consume the same compiled bytecode chunk through one shared pipeline
(`cfg.cpp`, `abstract_stack.cpp`). That fact was correct and the resulting
node (`#231`, a probe-driven research node gating everything else) earned its
keep — see the referee findings above. But the brief's scope followed that
one risk analysis to its edges and stopped: the bootstrap interpreter
(`bootstrap/loxpp_interpreter.lox`, a self-hosted Lox++-in-Lox++ parser and
evaluator) was added as a node only after the rest of the mission had
already merged, because it never touches the shared bytecode pipeline at all
— it runs as an ordinary Lox++ program on top of the finished native VM — so
a risk analysis scoped to that pipeline could not see it. A standing lesson
already on file (`feedback_loxpp_bootstrap_limitations.md`, predating this
mission) said plainly that a new language feature must be added to the
bootstrap interpreter too; the brief did not consult a "what else parses or
evaluates Lox++ source" checklist, so that lesson never reached the scoping
step. The same blind spot would have missed the tree-sitter grammar and the
`loxpp-lsp` resolver for the same reason — neither consumes the shared
pipeline either.

**The fix is procedural, not a smarter risk analysis.** Before closing a
mission's node list, enumerate every tool that parses, compiles, or
evaluates Lox++ source — grep the repo for anything that ships its own
scanner/parser (`bootstrap/`, `editors/tree-sitter-loxpp/`,
`src/lsp/`, and whatever the next one turns out to be) — and check
each one off against the feature the mission is adding, independently of
which specific execution pipeline the mission's main risk analysis is about.
A pipeline-shaped risk analysis is necessary for the pipeline's own nodes; it
is not a substitute for a surface-shaped inventory of every consumer of the
language.

## Audit against a countable ground truth, not incrementally against reported cases

Three different nodes in the non-local-control-flow mission hit the same
failure shape: a fault-site or call-site retrofit that a reviewer's specific
test cases progressively picked apart, several rounds running, because each
fix addressed only the sites a test had actually exercised. `#232` (native)
started at 0 of 19 spec-listed runtime-fault kinds wired to be catchable;
`#237` (JVM) and `#236` (CLR) each converged the same way, one review round
at a time, on their own per-backend fault-site list. `#245` (bootstrap)
broke the pattern: rather than trust a visual grep over `.setError(` calls —
which a comma inside a string literal argument can silently mislead — the
referee wrote a small parser that balances parens and brackets to count each
call's real argument count, found 76 total calls and exactly the 17
still-unconverted sites the previous round had already named, and confirmed
zero remained after the fix. The difference is not effort, it is method: a
site-by-site fix converges only on the sites a test happens to reach, while
counting every site against a ground truth (a spec table, a `grep -c`, a
small purpose-built scanner) either proves completeness or gives an exact
remaining count. Reach for the second one whenever a node's deliverable is
"every site of kind X does Y" — that shape recurs across nodes.

## A gate that silently skips a failing example can hide a missing feature entirely

`#236`'s reviewer found the CLR backend had not implemented `defer` at
all — `Op::DEFER_RECORD`/`Op::RUN_DEFERS` were bare `notImplemented(in.op)`
stubs, a missing core deliverable, not a fault-site gap. The mission's own
regression gate (`tools/check_clr_probes.sh`) never flagged it, because that
gate's corpus sweep silently skips any example where either side's run does
not reach exit 0 — a total crash looks identical to "not part of this sweep"
from the gate's own output. A corpus-sweep gate proves the examples it
successfully compares are correct; it says nothing about the examples it
quietly dropped. When a node's checkpoint relies on a sweep like this, check
what the sweep does with a run that never reaches exit 0 before trusting a
clean report from it — a silently-skipped crash is a false green, not
missing coverage.

## Rebuild fully before trusting a test result that will decide a merge

`ctest` does not rebuild anything; it runs whatever binaries already exist
under `build/`. Building only the specific targets touched by a change
(`cmake --build build --target <targets>`) and then running the full suite
against that partially-rebuilt tree can fail tests that have nothing to do
with the change, because an unrelated binary still reflects source from
before some earlier, unrelated edit in the same long-lived checkout. This
mission's own comment-reference cleanup hit exactly that: three unrelated
tests failed on a hardcoded corpus-size mismatch that the checked-in source
had already fixed — the failing binary was stale, not the source. A full
`cmake --build build` before the run that will inform a merge or cleanup
decision costs one rebuild; treating a partial-rebuild failure as real costs
a debugging session chasing a regression that does not exist.

## Long-lived checkouts and worktrees accumulate state that outlives any one agent's turn

A background process started by one agent, or a stray uncommitted diff left
by one that edited the wrong worktree, does not go away when that agent's
turn ends or when the orchestrator's own context gets compacted — nothing
in either event stops a running process or reverts an edit. On a long
mission, sweep for both periodically rather than assuming a quiet
transcript means a quiet checkout: `git status --short` in every worktree in
use catches a stray diff before it is mistaken for the next agent's own
work, and it is worth checking before trusting that a worktree is clean.
When a stray diff turns up, read it before deciding what to do with it — it
may be superseded WIP from an abandoned attempt (safe to set aside with
`git stash push -u -m "<tag>"`, never bare `git stash`, since the stash stack
is shared with every other worktree) rather than something to discard
outright.
