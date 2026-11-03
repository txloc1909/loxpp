# Concurrency semantics grounding mission (mission #1058) — retrospective

**Status:** frozen at mission close, 2026-11-03. The decision is
`docs/decisions/concurrency-semantics-grounding.md`. Every statement here comes
from a command, a pull request comment, an issue comment, or a CI run named in
this file. A sentence that has no such evidence says so. The sections "Rules
this retrospective produces" and "Lessons" are the reading of the author. This
record is history. It does not bind later work.

## Purpose

The mission closed the open forks of the concurrency model before more
concurrent work: the order of the tasks of one worker binds every
implementation (D1), the bootstrap interpreter conforms to the whole spec
(D2), a side of a Socket or a Server holds one waiting call (D3), and the
model is frozen until the spec describes the machine (D4). It serves the
*evaluation order*, *suspension* and *cost* controls and "Fail loudly" in
`VISION.md`, and the role of the bootstrap interpreter as a capability test.

The decision merged as #1057. The tracking issue is #1058. Seven nodes ran,
one branch and one pull request each.

| Node | Issue | Pull request | Merged (UTC) | Commit on `main` |
|---|---|---|---|---|
| Decision | — | #1057 | 2026-11-02 18:18 | `a8961e1` |
| N1: comments give worker parallelism as the reason | #1059 | #1069 | 2026-11-02 18:46 | `3fa7952` |
| N2: the Task Machine, D2 and D3, probes, lists | #1060 | #1070 | 2026-11-03 03:32 | `f119cf0` |
| N3: native, one waiting call per side | #1061 | #1077 | 2026-11-03 04:51 | `497c7f2` |
| N4: JVM, one waiting call per side | #1062 | #1079 | 2026-11-03 05:27 | `6b3cdd8` |
| N6: the bootstrap conforms | #1064 | #1082 | 2026-11-03 13:28 | `65de33d` |
| N5: QBE, unreached suspend points | #1063 | #1083 | 2026-11-03 14:14 | `ed38e4b` |
| N7: gate on both phases, close | #1065 | this change | | |

N5 (wave 4) merged after N6 (wave 3). That is the order of the DAG: N5
needed only N3.

## Review rounds

A round is one reviewer comment with a verdict.

| Pull request | Rounds | Blocking findings | Where the blocking findings were |
|---|---|---|---|
| #1069 (N1) | 1 | 0 | None |
| #1070 (N2) | 3 | 6 | Spec text (F2, F3, F4, F6), a check script (F1), a probe (F5) |
| #1077 (N3) | 1 | 0 | None. Two non-blocking items, filed as #1078 |
| #1079 (N4) | 1 | 0 | None. Two non-blocking items, filed as #1080 and #1081 |
| #1082 (N6) | 1 | 0 | None. Four non-blocking items: #1086, two with no action, and the decision table that this change updates |
| #1083 (N5) | 1 | 0 | None. One non-blocking item, filed as #1084 |
| #1088 (N7) | 1 when this record was written; the answer to it is this record's last change | 2 | The order question (F1). The playbook still told an agent to use `git stash` (F2) |

Every blocking finding of the code nodes was at N2. After N2, each code node
passed in one round. The close node, #1088, had two blocking findings in its
first round.

## What worked

1. **The spec came first, and the lists counted the gap down.** #1070 landed
   the Task Machine, the D3 rules, probes 347 to 365 and the lists before any
   consumer. Its round-3 review gives 12 listed probes on native and QBE, 13
   on the JVM and 16 on the bootstrap. The review of #1077 gives `290 pass,
   0 fail` on native with no list. The review of #1079 gives `290 pass, 0
   fail, 0 xfail` on the JVM. At close, `tools/spec-probe-xfail/` holds only
   `.gitkeep`.
2. **The reviewers ran mutations, and each one is in a thread.** The review of
   #1079 lists five mutations of `LoxSide` and `LoxSocket`, each with the probe
   that failed. The review of #1082 replaced the host limit with a relay host
   task (the `SelectTimer` form) and saw probe 366 fail 3 of 3. The review of
   #1083 removed the `reached` check and saw probe 367 fail on QBE.
3. **One finding of a review became one probe.** The programs of F2, F3, F5
   and F6 in the review of #1070 became probes 362, 363, 358 and 364/365. The
   program h of the review of #1079 became probe 368 in this change.
4. **The code nodes needed one round each.** After the spec was exact, the
   reviews of #1077, #1079, #1082 and #1083 found no blocking defect.
5. **D3 removed code.** The review of #1077 shows that a `grep` for the
   writer queue names (`writers`, `liveWriter`, `pumpWriters` and others) finds
   nothing in `src`, `test` and `tools`. The review of #1082 shows that
   `SelectTimer`, `saveHostSuspendState` and `restoreHostSuspendState` are
   gone from the bootstrap.

## What went wrong

### Six spec gaps were found only at the review of N2, over three rounds

The first review of #1070 (round 1) made five blocking findings:

- F1: `tools/check_task_machine.py` passed when one row of a table was
  removed. Seven single-row removals passed.
- F2: the rule of contexts where a task cannot suspend said "It raises no
  fault", and Sides rule 2 said `BusyError`.
- F3: §05 said a close wakes the task at the close; §04 let the
  implementation choose the point.
- F4: the Task Machine sent the reader to Scheduling for a message from
  another worker, and Scheduling did not decide it.
- F5: probe 358 passed on an implementation that frees the side at the end
  of the wait.

Round 2 found F6: rule 4 and §05 decided a `close_write()` by another task,
while a reader waits, in two ways. The reviewer wrote: "I did not see it in
round 1, because rule 4 had the same gap". Each of F2, F3 and F6 is one case
that two sections decide in two ways. The orchestrator ruled on each one on
#1060. Round 3 approved.

The decision had listed the spec as one consumer with one owed item. It did
not ask the spec node to check each new rule against the sections that
decide the same case. The reviewer did this check by hand, and it took three
rounds.

### The gate hid a compile-time rejection behind a halt

`tools/diff_runtimes.py` compared a halt by a prefix rule: one stdout must be
a prefix of the other. An empty stdout is a prefix of every stdout. In this
change, a mutation of the bootstrap parser that rejects `for`-in made probe
282 report `MATCH (halt)` on the gate of `main`. Rule 5 of "Plans for a
language change" says that run-time parity does not prove compile-time
parity. The prefix rule was a case of exactly that. This change fixes it for
spec probes (#1087 for other programs).

### A rule with three sides had probes for two

Probe 361 checked Sides rule 6 on the read side and the accept side. The
write side had no probe. The reviewer of #1079 found it: with the busy check
before the closed check in `enterWrite`, only the JVM unit test `SideTest`
failed. Native, QBE and the bootstrap had nothing. Probe 368 closes this
(#1080). In this change, the same mutation on native (`checkSocketWrite`) and
on the JVM (`LoxSocket.enterWrite`) fails probe 368 and no other probe.

### An orchestrator hazard that no program can check

The first hazard on #1064 asked N6 to raise `BusyError` by its own side
tracking and to show it with a mutation. The orchestrator corrected it on
#1064: each guest task is a host task, and the host checks the side first,
so no program can tell a bootstrap tracker from the host rule. The corrected
hazard asked for an argument rule by rule and a probe where the mapping could
break a rule. The review of #1082 checked that argument.

### Process faults

1. **The `pull_request` trigger did not start.** On #1070 the implementer
   wrote three times that the push did not start a `pull_request` run, and
   started CI by `workflow_dispatch` (runs 41507001715, 41541063623,
   41550546545). The reviewer of #1070 confirmed that `gh pr checks` showed no
   `pull_request` run for the head.
2. **A stop for a rate limit in the middle of a node.** The task of N7 says
   that one node stopped for a rate limit. No pull request or issue comment
   records it, so this record has no evidence for which node, or for its cost.
3. **`git stash pop` applied the entry of another session.** The task of N7
   says that an agent's `git stash pop` applied an entry that another session
   had made. No comment records it. The mechanism is real: `git rev-parse
   --git-path refs/stash` in the worktree of this node names the main
   `.git/refs/stash`, and the stash reflog there holds 7 entries from three
   branches (`main`, `feat/nonlocal-control-flow-jvm`, `feat/textmate-bundle`).
   One stash list serves every worktree.
4. **A probe needed two workers at the same time.** The review of #1082 shows
   that the old probe 277 needed the peer worker to run while the entry task
   blocked the main worker. That is not portable under `spec/README.md`,
   Conformance, item 5. It passed on the bootstrap only because the old
   `sleep` blocked. N6 rewrote it.

## The order question, and a wrong argument at close

The review of #1082 found that native and the JVM order two tasks
differently: a task whose limit passed with a message that it did not
select, and a task that got its message. The first tip of #1088 said that the
order is not bound, because Scheduling lets a worker see a passed limit at
points that the implementation chooses.

The review of #1088 (F1) showed that the argument was wrong. Seeing a limit
is the `limit` move, and that move joins the back of the run queue. So no
point that the implementation chooses can put the task ahead of a task that
is already in the run queue. The reviewer gave a variant in which A's
message arrives before its limit and the limit passes while A is `ready`:
native, QBE and the bootstrap print C first, and the JVM printed A first.
The orchestrator ruled on #1065 that the order is bound. #1088 adds probe
369, one sentence in rule 4 of `task.receive_within`, and the JVM fix in
`LoxWorker.awaitMessage`. With the old JVM code, probe 369 fails 5 of 5 runs.

The first program (both limits pass before any send) stays open, for another
reason than the one first given: the worker can see both limits before the
sends.

The cause is the class that rule 6 of "Plans for a language change" now
names. Rule 4 of `task.receive_within` ("tested before the limit counts")
could be read as "raises at once", and The Task Machine says the call makes
the `receive` move again. The implementer read one section and not the
other.

## Rules this retrospective produces

Each rule below is a rule for all future work. This change puts each one in
`AGENTS.md`.

1. **Check each new spec rule against every section that decides the same
   case** ("Plans for a language change", rule 6). Evidence: F2, F3 and F6 of
   #1070; F4, where one section sent the reader to another that did not
   decide the case; and F1 of #1088 (rule 4 of `task.receive_within` and The
   Task Machine).
2. **A comparison of a halt must still tell a rejection from a halt** (added
   to rule 5), when the halt has output before its fault. Evidence: the
   mutation of the bootstrap parser on probe 282 in this change. A halt with
   no output before its fault looks like a rejection under the behavior the
   spec binds (#1087).
3. **A probe set covers each side, kind, or value that a rule names**
   ("Engineering rules"). Evidence: #1080 (rule 6 had no write-side probe),
   F5 of #1070 (probe 358 did not force its case).
4. **Do not use `git stash`** ("Engineering rules"). Evidence: the shared
   stash reflog above. The incident itself has no thread. The review of #1088
   (F2) found that `docs/process/multi-agent-playbook.md` told an agent to use
   `git stash push`; this change makes it agree.

No rule is proposed for the `pull_request` trigger: it is a fault of the CI
host, and the orchestrator's task already says to start CI by
`workflow_dispatch` and to say so in a comment. No rule is proposed for the
rate-limit stop, because no thread records it.

## Follow-ups

Every follow-up is one issue. The decision lists them in "Shipped". New issues
are marked "new".

| Issue | Subject |
|---|---|
| #1066, #1067, #1068 | Invariant tests, removal of weak tests, refactor of the runtimes (filed when the tracking issue opened) |
| #1078 | An abandoned task holds no side of a Socket or a Server |
| #1081 | JVM `LoxPoller.cancelInterest` key cleanup has no test that can fail |
| #1084 | QBE: decide coroutine mode from reached instructions only |
| #1085 (new) | The Socket methods text names only `close()` as a call that wakes a waiting task |
| #1086 (new) | No program runs a limited receive on the JVM-hosted and QBE-hosted bootstraps |
| #1087 (new) | `diff_runtimes.py` cannot tell a rejection from a halt for a program that is not a spec probe |

## Lessons

- **Put the review of the spec text into the plan of the spec node.** The code
  nodes passed in one round each because the spec was exact by then. The
  three rounds of #1070 were the cost of making it exact.
- **A tolerance in a comparison can hide a whole phase.** The prefix rule
  for a halt was correct for run time. It also accepted a rejection.
- **Count the sides of a rule when you write its probes.** Rule 6 named three
  sides and had probes for two.
- **A hazard must name a check that a program can fail.** The first N6 hazard
  asked for a difference that no program can show.
