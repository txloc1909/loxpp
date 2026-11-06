# Invariant probes for the concurrent machine

**Status:** accepted. The owner approved it on 2026-11-05 in the pull request
that moved it here (#1096).
**Issue:** #1066. Follow-up of mission #1058
(`docs/decisions/concurrency-semantics-grounding.md`).

## Goal in VISION.md

- *Fail loudly*: a broken invariant must stop a test, not pass.
- *Evaluation order*: the order of tasks is defined (D1). A probe pins it.
- The backends conform to the spec. A shared probe checks each backend
  against the same expected output.

## Why

Mission #1058 closed the forks of the concurrency semantics. It named three
bug classes that can return. The runtimes will be refactored later (#1068).
Unit tests of internals do not survive that refactor. Program-level probes
do. So each invariant gets probes that run on every backend.

## The invariants

| Id | Invariant | Spec anchor | Issues |
|---|---|---|---|
| I1 | A task never sees state that belongs to another task | The Task Machine, the table of task-owned state | #984, #1009, #729 |
| I2 | No task waits while the condition of its wait is already true | Sides rules 2 and 6; `receive` tests the mailbox before it waits | #1054, #1047, #948 |
| I3 | The ready order follows the first wait of each call | The Task Machine, moves and run queue | #950, #996 |

### Probes

One row for each case. `tools/check_invariant_probes.py` reads this table:

- The `Case` of an I1 row is a row of the task-owned state table in
  `spec/04-semantics.md`, and each such row has one I1 row.
- The `Case` of an I2 row starts with a point of the suspend point table of
  the same file of class receive, or a `sleep`, `Socket` or `Server` point,
  and each such point has one I2 row.
- A probe in the last column exists in `test/translation-probes/`, carries a
  `STATUS` line and has a row in its `README.md`. A `—` means that the probe
  is not written yet; the script accepts it until its `--strict` flag, which
  N4 turns on in CI.

| Id | Case | Probes |
|---|---|---|
| I1 | the mailbox | `420_task_state_mailbox` |
| I1 | the current task | `421_task_state_current_task` |
| I1 | the call frames | `422_task_state_call_frames` |
| I1 | the deferred calls | `423_task_state_deferred_calls` |
| I1 | the handlers | `424_task_state_handlers` |
| I1 | the current coroutine | `425_task_state_current_coroutine` |
| I1 | the contexts | `426_task_state_contexts` |
| I1 | the overflow unwind | `427_task_state_overflow_unwind` |
| I1 | the waiting call | `428_task_state_waiting_call` |
| I2 | `task.receive()` | `380_receive_message_held_does_not_suspend` |
| I2 | `task.receive_within(seconds)` | `381_receive_within_message_held_does_not_suspend` |
| I2 | `task.receive_where(want)` | `382_receive_where_selected_message_held_does_not_suspend` |
| I2 | `task.receive_where_within(want, seconds)` | `383_receive_where_within_selected_message_held_does_not_suspend` |
| I2 | `sleep(seconds)` | `384_sleep_zero_does_not_suspend` |
| I2 | `Socket.read_some(n)` | `385_read_some_bytes_held_does_not_suspend` |
| I2 | `Socket.read_bytes(n)` | `386_read_bytes_bytes_held_does_not_suspend` |
| I2 | `Socket.readline()` | `387_readline_line_held_does_not_suspend` |
| I2 | `Socket.read()` | `388_read_end_held_does_not_suspend` |
| I2 | `Socket.readlines()` | `389_readlines_end_held_does_not_suspend` |
| I2 | `Socket.write(text)` | `390_write_does_not_suspend` |
| I2 | `Socket.writeline(text)` | `391_writeline_does_not_suspend` |
| I2 | `Server.accept()` | `392_accept_pending_connection_does_not_suspend` |
| I2 | `Socket.readline()` on a closed read side | `393_read_closed_side_does_not_suspend` |
| I2 | `Socket.write(text)` on a closed write side | `394_write_closed_side_does_not_suspend` |
| I2 | `Server.accept()` on a closed Server | `395_accept_closed_server_does_not_suspend` |
| I3 | a Socket call that waits again | `400_socket_call_that_waits_again_keeps_its_place` |
| I3 | a new Socket call after a call of the same task | `401_socket_new_call_takes_a_new_place` |
| I3 | a receive that waits again | `402_receive_that_waits_again_keeps_its_limit` |

## Rules

1. **No wall-clock time where it can be avoided.** The scheduler clock seam
   (#999, #1044) is C++ only and native only. A `.lox` probe cannot reach it,
   and a new Lox surface breaks the D4 freeze. A probe uses sends, yields,
   buffered bytes and closes to make the order. It uses `clock()` only where a
   limit must pass, as probes 358 and 369 do. The seam stays for the C++ unit
   tests of native.
2. **Each probe fails on a mutation of its property.** For each probe and each
   backend, the pull request records the mutation and the result. Where a
   backend cannot be mutated, the pull request says so and names where the
   defect becomes reachable. The bootstrap keeps
   `tools/check_bootstrap_park_state.py`.
3. **A check script connects the probes.** `tools/check_invariant_probes.py`
   reads the table above. It requires that each row names an existing probe
   with a `STATUS` line and a row in `test/translation-probes/README.md`, and
   that each field of the task-owned state table has an I1 probe. CI runs it.
4. **Each probe names the spec rule it pins.** The node states that no other
   section of `spec/` decides the same case differently.
5. **No spec change.** A probe that finds a spec gap stops the node. The gap
   becomes an issue.
6. **No probe duplicates probes 347 to 373.** The node reads each of them
   first.

## Consumers

| Consumer | Owes |
|---|---|
| Native VM | Passes every new probe. Mutation evidence |
| JVM backend | The same. It is the semantic oracle |
| QBE backend | The same. QBE has no `yield`: a coroutine probe uses `NOT-BINDING: qbe` and a probe in `qbe-only/` |
| Bootstrap interpreter | The same. A probe that maps to a record field adds a mutation to the park-state check |
| Resolver, LSP, tree-sitter grammar, editor syntaxes | Nothing. No syntax changes. The tree-sitter step only parses the probes |
| `tools/check_invariant_probes.py` | New |
| `ci.yml` | Runs the new script next to `tools/check_task_machine.py` |

A probe lands with all consumers passing. If a consumer cannot pass yet, its
xfail file lists the probe.

## Nodes

The tracking issue holds the DAG: N0 this decision, N1 I2 probes and the check
script, N2 I3 probes, N3 I1 probes, N4 close. N1 to N3 run in parallel.

## Open question

- None for I3. The owner ruled on 2026-11-05 that I3 must pin #950 and #996
  without wall-clock time. If N2 finds a case that needs a limit to pass, N2
  uses `clock()` as rule 1 allows and states why.
- Does QBE need a replacement probe for the coroutine field of I1? N3 decides.

## Shipped

Mission #1098 ran this decision as a DAG of five nodes, one pull request per
node. The nodes merged on 2026-11-05. The retrospective is
`docs/records/concurrency-invariant-probes-retro.md`.

| Node | Issue | Pull request | What shipped |
|---|---|---|---|
| N0 | #1098 | #1096 | This decision |
| N1 | #1099 | #1104 | Probes 380 to 395 (I2) and `tools/check_invariant_probes.py` |
| N2 | #1100 | #1108 | Probes 400 to 402 (I3) |
| N3 | #1101 | #1126 | Probes 420 to 428 (I1) |
| N4 | #1102 | this change | `--strict` in CI, this section, the retrospective |

Every row of the table "Probes" names a probe. CI runs
`tools/check_invariant_probes.py --strict`, so a row with `—` fails.
All four backends pass every probe with an empty expected-failure list.

### Mutation evidence

For each probe, a temporary mutation made the property false. The probe failed
on native, JVM, QBE and the bootstrap, except where the table says otherwise.
The patches are not in the repository. The pull request of each node holds the
list of probes that failed for each mutation.

| Invariant | Mutations | Pull request | Where a probe does not single out the case |
|---|---|---|---|
| I2 | 14: one for each call kind, and one for each closed side | #1104 | None. Each mutation failed only the probe of its case |
| I3 | 3: a wait that waits again gets a new place, a new call keeps an old place, a receive that waits again restarts its limit | #1108 | None |
| I1 | 9: one for each row of the task-owned state table, and 3 finer ones for the mailbox and the call frames rows: a scan position that tasks share, a message delivered to another task, and at most five saved frames | #1126, #1128 | The coarse mutations of the mailbox and of the call frames fail all nine probes, but the finer ones fail one probe each (420, 420, 422). The JVM counters change no probe result (accepted, #1129) |

### What the probes cannot show

- The rows "the contexts" and "the overflow unwind" of the task-owned state
  table cannot be seen to leak at a park, because a task does not park while
  either is active. Probes 426 and 427 check the part a program can see.
- A socket probe with held bytes depends on the network, as the spec says in
  Suspend Points. Such a probe assumes that the bytes of a write on the local
  host arrive within 0.5 seconds, as probe 358 does. The scheduler clock seam
  (#999, #1044) is C++ only, so no `.lox` probe uses it.
- Probes 400 to 402 use real time: a computation of 0.5 seconds with no suspend
  point, and for probe 402 real limits.
- The write side of a call that waits again (issue #933) has no probe. It
  needs two writers whose waits end at one point and a reader on another
  worker, with timing that was not stable (#1127, closed as not planned).
- On the JVM a task is a virtual thread, so its frames, handlers, deferred
  calls and current coroutine are per task. The per-worker counters
  `frameCount` and `handlerDepth` change only the depth at which a stack
  overflow is caught, and no program sees them leak (#1129, closed as not
  planned). A defect is reachable in `LoxWorker.suspend` and `makeCurrent`.

### Follow-ups

| Issue | What |
|---|---|
| #1128 | Probes that single out the mailbox row and the call frames row of I1. Done: probe 420 starts the scan of the two tasks at different places, and three finer mutations fail one probe each |
| #1068 | The refactor of the runtimes. The strong tests it needs are ready |
