# Workers and tasks: mission retrospective

## Purpose

Mission #595 implemented the concurrency model of
`docs/decisions/concurrency-model-decision.md` (#594, amended by #603):
workers that share no mutable state, tasks as coroutines inside a worker,
and spawn, send, and receive as the primitive. It ran from 2026-10-16 to
2026-10-18, by hand, one node at a time, per
`docs/process/multi-agent-playbook.md`, "The loop". This record states what
the mission did and what went wrong in the process. Each claim cites an
issue, a pull request, or a commit. The design is in the decision, not here.

## Nodes

| Node | Issue | PR | Review rounds | Result of the reviews |
|---|---|---|---|---|
| N1 spec and probes | #596 | #605 | 3 (2 with changes requested) | The main task became a coroutine, as the decision says; probes for the blocking-call rule and the exact exit code; the Enum equality rule in §03; probes that tell "the program ends" from "the task ends". |
| N2 native, one worker | #597 | #607 | 4 (3) | `YieldOutsideCoroutineError` left the C API path; a helper used at every site; a QBE gap made visible; probe 89 removed by the referee. |
| N3 native workers | #598 | #611 | 4 (3) | No code defect. The decision got two bug classes, a corrected TSan claim, and the full list of count changes. |
| N4 bootstrap | #599 | #615 | 2 (1) | Gate records only. Two older bootstrap gaps became #616 and #617. |
| N5 JVM | #600 | #618 | 2 (1) | An emitter comparison got a check that can fail. Two limits became #619 and #620. |
| N6 QBE | #601 | #621 | 1 (0) | Approved at the first review. |
| N8 credit channel | #604 | #622 | 2 (1) | Probe 90 had passed with a wrong error; probe numbers moved to 90 and 91 to avoid #612. |
| N7 gate and close | #602 | this record's PR | — | The compile phase, the bootstrap differential suite, the print-line check, and the close. |

Two referee rulings were made. On #596, the main task is a coroutine, because
the decision says that a task is one; the spec does not add an exception
that the decision does not state. On #607, probe 89 (a `yield` in a deferred
call) left the spec probe set, because the spec did not state its rule; the
question went to the owner as #610, the owner chose "a deferred call cannot
suspend", and #612 put the probe back.

## Bug classes written into the decision

- The holder counts of one heap, and strings of two heaps (N3, #611).
- A Lox run on a C++ frame, and the QBE coroutine-mode rule (N6, #621).
- A carrier per processor on the JVM (#626, after #619 from the N5 review).

## Follow-ups closed during the mission

#606 (#625), #608 (#628), #609 (#627), #610 (#612), #613 (#624), #619 (#626),
and #620 (#623) merged before the close. Seven pull requests merged on `main`
while N7 ran, so N7 rebased once before its PR.

## Process faults

1. **A closing keyword in a negation closed an issue.** The squash commit of
   #603 has the text "the mission alone does not close #519". GitHub reads
   "close #519" and closed #519 at the merge (2026-10-16T19:32:19Z). The
   decision says the opposite. N7 reopened #519.
2. **The consumer table was wrong.** The mission issue said that the
   resolver and the LSP owe nothing. The N1 review found that both keep
   lists of stdlib names. The orchestrator corrected #595 in a comment, and
   N2 did the work.
3. **A wrong reachability claim.** The N2 implementer said that a QBE gap
   becomes reachable at N6. The reviewer showed that it was reachable at
   once through `tools/loxpp_qbe.sh` (#607, third review).
4. **An agent stopped processes of other agents.** The orchestrator reports
   that one agent ran `pkill` with a pattern that matched processes in
   containers that other agents had started. GitHub has no record of it.
5. **The run stopped and resumed.** The orchestrator reports stops on the
   spend limit. The GitHub record shows a 7.1-hour gap after the first N3
   review (02:33 to 09:38 UTC, 2026-10-17) and a 4-hour gap after the N5 PR
   opened (11:41 to 15:21 UTC). Each time, the loop resumed from the GitHub
   state: the PR, its review comments, and the node issue. No work was lost,
   because each agent had pushed its commits.
6. **The commit trailers differ.** The squash commits of N1, N2, N4, N5, and
   N6 carry only `Claude Haiku 5.5` trailers. N3 carries both `Claude Haiku
   5.5` and `Claude Opus 5.5` trailers with a `Claude-Session` line, and N8
   carries only `Claude Opus 5.5`. The orchestrator prompt set one trailer;
   the harness gave another.
7. **Two branches took one probe number.** #612 and N8 both used number 89.
   The N8 review found it, and N8 moved to 90 and 91 (#622).

## Lessons

- GitHub reads a closing keyword without its context. A record that says
  "does not close #N" must not use that phrase.
- A pattern kill is not local to one agent when containers share a host.
- The loop survives a stop when state is on GitHub and each commit is
  pushed. That rule held here.
- The run-time diff alone did not show compile-time parity. The N7 gate
  found that the bootstrap reports some compile errors with a different
  message, line, or not at all, and that 12 older translation probes differ
  on the bootstrap. Each became an issue (#634 to #642). A gate for both phases on every
  consumer from the first node would have found them before this mission.

## Rules for AGENTS.md

The N7 PR adds three rules for the owner to confirm: no closing keyword
before an issue number that the change does not close; stop only what you
started, by exact name; a commit trailer names the model that wrote the
commit, as the harness gives it.
