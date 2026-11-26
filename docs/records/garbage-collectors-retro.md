# More than one garbage collector: retrospective

This record is frozen after the closing PR. The design is in
`docs/decisions/garbage-collectors.md`. The tracking issue is #1277. Each
claim cites a PR, a commit or an issue.

## Goals, and whether each one held

| Goal | Held? | Why we say so |
|---|---|---|
| 1. A limit on latency that does not depend on the heap size | Yes, for the programs in the gate. | The gate of N6 (#1304, `tools/check_gc_pause.py`) checks the largest step in work units, the p99.99 of the steps and MMU(10 ms), on the large-heap programs and on the Redis load. 15 of 15 runs passed in the N7 evidence (#1285). Gaps stay: #1299, #1300, #1301, #1302, #1303. |
| 2. More than one collector behind one interface | Yes. | N3 (#1290) made `stw` a collector behind an interface. N5 (#1298) added `incremental`. `LOXPP_GC` chooses one at init time. |
| 3. A limited cost | Yes. | N7 evidence (#1285): geometric mean of `benchmarks/` 1.005 of `stw` (limit 1.10), peak at most 2.20 times the live heap (limit 2.5). The cost of the barrier with `stw` was 1.0001 in the review of N4 (#1291, limit 1.02). |
| 4. No change to the language | Yes. | The mission changed no spec text. `tools/diff_gc_collectors.py` (#1304) compares the output of 132 examples with each collector: equal. |
| 5. A runtime that stays simple to read and to check | Partly. | The barrier mutators, the table check (`tools/check_gc_barrier_table.py`) and the verifier make the new rules checkable. The cost: a second collector, a heap with its own allocator, and 12 open follow-ups. The wall time of the large-heap programs is 1.09 to 1.49 times that of `stw` (#1285). |

## What went well

- **The prototype changed the plan before any merge.** N1 (#1288) showed that
  `malloc` and `free` stalls of up to 143 ms no collector step can bound. The
  owner put the heap layout (N8) before the collector (N5).
- **Each review ran the programs and mutated the change.** The reviewer
  built its own tree and removed each fix to see its check fail. N4, N5, N6 and N8
  each had at least one "changes requested" round, and the findings that
  follow were true.
- **A count gate beside the time gate.** The gate counts work units of a
  step, so it does not depend on the host. The time limit has a wide margin,
  because hosts stall (section 13 of the decision).
- **N7 was a report and a one-value change.** The evidence is on #1285 and
  each criterion of section 10.1 had a number.

## What cost time, with the bug class of each

| Event | Fact | Bug class |
|---|---|---|
| N5, a mailbox bug found only by the reviewer (#1298, B1) | The tracer cut a Task mailbox by index. A receive moved the elements that stay, and the barrier of a pop saw only the element that left. The reviewer found it, and the mutant without the index move prints 298709 where 300000 is right. Fix: `ObjTask::poppedCount`, commit `e54d39ac`. | A structure that the tracer cuts by index needs a rule for a removal that moves the elements that stay. |
| N5, a count raised too late (#1298) | The count of a foreign object was raised in the finish pause, so the owner could free the object while an envelope was in flight (`foreign_string.lox`). | A count that a mark reaches must be raised at the reach. |
| N5, the pacing (#1298, B3) | The pacing counted the objects of the cycle as live. The peak was 2.4 to 3.5 times the live heap on a long churn. Fix: `922f45f3`, `443e6458`. | A pacing that counts the objects of the cycle as live. |
| N5, idle (#1298, B2) | The text of section 4.5 said an idle worker finishes the cycle at once. This made MMU(10 ms) 0.3% in a test. The text changed and #1303 holds the gap. | A text rule that the measurement does not support. The owner had to confirm a change of accepted text. |
| N8, the slicing fix caused a 4.9 times peak (#1293) | The first fix for a long single free queued the blocks and gave them back too slowly. The peak RSS of a program that drops a large List was 1890 MB (4.9 times the live heap). A second fix tied the release rate to the map rate (`81e4ad8f`): 725 MB. One proof test could not fail when its fix was removed (finding N2, fixed in `6248e3d2`). | A fix that bounds one cost can move it into a queue that has no bound. |
| N6, the CI drift of the pi gate scope, twice (#1304, #1306) | N6 added a step to the `qbe-toolchain` job. `QBE_GATE_STEPS` in `.pi/extensions/lib/gate-scopes.ts` did not have it, and `pi mode guards` failed (`9450a974`). N7 changed the same job and the same list, and the reviewer found the same gap again (`e22bd8d9`). Nothing connects a change of a CI job to the local list. | A list that copies a CI job and no check ties the two. |
| N6, a gate that passed when it should fail (#1304) | The gate accepted two runs that each failed one time limit. A step ran the whole `ctest` and did not follow `CTEST_ALL`. An MMU of n/a passed. Fixes: `bd7bf7ec`, `437de685`. | A gate that is not proved to fail. |
| N4, a barrier check with a hole (#1291, #1292) | `ObjUpvalue::location()` returned a writable pointer and the check did not see it. Fixed in the PR. The same hole for a pointer to a pointer stays (#1292). | A missed barrier (section 6.1 of the decision). |
| N4, probes (#1291) | The review found sites with no probe (B2). 14 new probes were added and each has a mutant that fails. | A site that a rule names with no probe case of its own. |
| N5, a stall of the implementer | The N5 implementer stopped for a long time in `pgrep` loops that matched their own command line, so the wait never ended. This is the report of the mission orchestrator and no PR records it. | A wait that matches its own process. |
| Flaky tests under load (#1305, #1307) | A pi watchdog test failed on `main` once (#1305). The peak-ratio check of `churn_peak` read 2.51 against the limit 2.5 under `ctest -j4` (#1307). | A check that reads a value that depends on the host, with no retry or margin. |

## Rules that the mission produces (proposed, not written)

Each rule below is for all work. Each one needs its own PR to `AGENTS.md`.

1. **A copy of a CI job must have a check that ties it to the job.** Two
   PRs of this mission broke the same list. (Source: #1304, #1306.) The
   mission shows that the existing rule "give each new path a class" does not
   cover a list inside a script.
2. **A test for a structure that code traverses by index must make a removal
   during the traversal.** (Source: #1298, B1.) This is a case of the rule
   "give each side, kind, or value that a rule names its own probe case". The
   proposal is to add the removal case to the plan template for a language or
   runtime change that cuts a traversal.
3. **A fix that bounds a cost must state where the cost goes.** (Source:
   #1293, 4.9 times peak.) The fix needs a test of the total, not only of one
   step.

I did not find a rule for the `pgrep` stall in this record. The rule "stop
only a process that you started, by its exact name or process ID" exists in
`AGENTS.md`. The playbook may need one sentence: never wait on a pattern
that can match the waiting command.

## Design changes that the mission produces (proposed, not written)

- **A generational mode.** The decision names it as a later, separate
  decision (section 10, phase 9). The proposal would be a new file under
  `docs/proposals/`.
- **A bound for the C allocator calls that the heap does not own** (#1294,
  #1295, #1296). If the issues show that no bound is possible without a
  change of the containers, that is a new proposal.
- **Program control of the collector** (a standard-library call). It stays a
  non-goal, and it is a language change.

## Open follow-ups

#1292, #1294, #1295, #1296, #1297, #1299, #1300, #1301, #1302, #1303, #1305
and #1307. The decision lists each one in section 15.2. The problem #1165
stays open.

One known gap has no issue: the wall time of the large-heap programs is
1.09 to 1.49 times that of `stw` (#1285). It is a cost of the pacing and the
decision says how to choose `stw`.
