# Multi-agent DAG playbook

Reusable doctrine for running a multi-agent mission that drives a DAG of
implementation nodes to green, one pull request per node. First proven by the
JVM backend mission (`#96`–`#116`). Written for the next such mission — the
maintainer plans a CLR backend next — so it does not need to be rebuilt from
scratch.

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

- `missionDir` — absolute path to the mission directory holding `brief.md`
  and `nodes/*.md`. **Never point this at `/tmp`** — on some hosts it is a
  memory filesystem a service cleans, and an agent that loses its brief this
  way may not notice. Treat a missing or unreadable brief as a hard failure,
  not as optional.
- `nodes` — a map of node id → `{ branch, title }`.
- `stages` — an array of node-id groups; each group runs in parallel, groups
  run in sequence.
- `repo`, `githubRepo`, and the doc paths a node implementer/reviewer must
  read (the DAG plan doc, the target's opcode/semantics reference, ...).

Before a new mission's first run, edit `meta.phases` in the script to name
that mission's real nodes — it drives the progress-tree preview and must stay
a literal, so it cannot be derived from `args.nodes` automatically.

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

## Referee decision format

A referee decision that ends a stagnation loop in one round has a fixed
shape: **verify the facts yourself first** (read `spec/`, `src/`, run the
code — don't take either agent's word for it), **then rule** — assign the
tag to the implementer, the reviewer, or a third option — **then bound the
next round**: state the exact action that must now happen, so the loop
cannot simply repeat with the same ambiguity.

## Node specification structure

Each node gets one file: deliverable, checkpoint, hazards, and anything it
inherits from a node it depends on. This is the live channel between nodes —
a running workflow holds its script in memory, but each agent reads its node
spec from disk when it starts, so a merged node can leave a hazard for a
later one there. Worked example, generalized from the JVM mission's N4 (a
mid-complexity node — a first straight-line code generator):

```markdown
# Node <ID> — <one-line summary>

**Branch:** `<type>/<short-description>`
**Depends on:** <ids, or "none">, all merged.
**Blocks:** <ids that cannot start until this merges>.

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

## One node, one branch, one PR, one merge

Keeps every review small enough for the reviewer to actually finish reading
it. Do not batch nodes into one PR, and do not open a second PR for a node
that already has one — an implementer resuming a node checks for an existing
open PR first and pushes to it.

## State comes from persistent storage (commonly GitHub and git), never from a written progress file

`tools/agent-workflow/plan_resume.py` reconstructs each node's status by
querying GitHub (PR state, review state) and git (branch existence, ahead/
behind main) — it does not read or trust a hand-maintained status file. This
was correct across 4 workflow launches, one interruption, and one incorrect
manual stop, and it is the reason resume never desynced from reality. A
written `STATE.md`-style file is fine as a human-readable snapshot, but never
as the resume source of truth.

## Run-observation tools

`tools/agent-workflow/snapshot.sh`, `status.py`, `watch.py`, and `watch_pr.py`
(the last also accepts a branch name and waits for its PR to open) exist so
an orchestrator can check on a long run without interrupting it. Take a
snapshot before suspending a run, and post a short recovery note on the
affected PR pointing the next agent at exactly what it should pick back up —
this recovered two otherwise-lost fix rounds in the JVM mission, both times
an implementer hit its step limit mid-fix, after compiling but before it
could commit, push, or reply.

## Orchestrator text is not an authority against the source

A node specification states constraints, not mechanisms — say what must hold
("the topmost live local", "no bare `Object[]` in a Lox value slot"), not
which specific function must implement it. An orchestrator's own commentary,
however confidently worded, is not a ruling that overrides `spec/` or `src/`;
if it conflicts with the source, the source wins and the note gets corrected.
