# Ground the concurrency semantics before more concurrent work

**Status:** accepted. The owner decided D1 to D4 on 2026-11-03. The pull
request that adds this file records the approval. Mission #1058 shipped this
decision on 2026-11-03: see "Shipped".
**Tracking:** #1058.

## Owner decisions

| Fork | Decision |
|---|---|
| D1: task order on one worker | The order in which the tasks of one worker run is defined, and it binds every implementation. |
| D2: the bootstrap interpreter | It conforms to the whole spec, which includes the rule that a call of class `suspends` lets the other tasks of its worker run. It replaces F4 of `blocking-call-suspension.md`. |
| D3: tasks that use one Socket | One waiting call per direction. A side of a Socket or a Server holds at most one waiting call. A call on a side where a call of another task waits raises a catchable fault. |
| D4: scope | The concurrency model is frozen until the spec describes the machine. |

## Goal in VISION.md

- *Evaluation order*, under "No high-level facility removes low-level
  control": "the order of evaluation is defined, not left open". D1 applies
  the same control to the order of tasks.
- *Fail loudly*. Under D3, a second waiter gets a fault. Today it can hang,
  lose bytes, or see a false end of stream.
- *Suspension* and *cost*. D3 adds one check, only in the calls that can wait.
- The bootstrap interpreter is "a capability test: a practical language can
  implement itself". D2 makes the test cover concurrency.

## Why

From 2026-10-20 to 2026-11-03 about 30 issues were about the task scheduler,
parking, the poller and Sockets. Most came from mission #711 and its
follow-ups. They fall in a small number of classes:

| Class | Issues | Cause |
|---|---|---|
| A park overwrites state that belongs to a task | #984, #1009, #729, #1047 | Per-task state lives in a structure that the tasks of a worker share |
| A wait ignores bytes that are already buffered | #1047, #1054, #948 | A wake comes only from the readiness of the descriptor |
| The identity of a call is not the identity of a wait | #950, #996, #924, #932 | A call that waits again gets a new place in the order |
| The set of suspend points is chosen by syntax | #1008, #1048, #729 | A backend decides by syntax where a task suspends. The spec decides by context and reachability |
| Tests that depend on the load of the host, or that cannot fail | #934, #935, #974, #1021, #1024, #1033, #1053; #925, #948, #951, #956, #960, #989, #996, #1045 | Wall-clock premises, and checks that a mutation does not fail |

The documents also disagree about what binds:

- `VISION.md` says the spec "leaves limits and scheduling to each
  implementation". `spec/04-semantics.md`, Scheduling, says "The order in
  which the tasks of one worker run is therefore defined".
- F4 and `spec/README.md`, Conformance, item 5, let a one-thread
  implementation block for a call of class `suspends`. Even so, #1005 and
  #1043 made the bootstrap park and block as native does.
- #932 (decided in #1010) lets the writes of two tasks mix. The reads of two
  tasks are fully bound. Each multi-waiter bug above comes from this
  difference.

The bugs come from these open forks, not from the parts of the machine that
the spec states. So this decision closes the forks first.

## D1: what binds

These rules bind every implementation:

- the rules of `spec/04-semantics.md`, Scheduling: one run queue per worker,
  first in, first out, and no priorities;
- the order in which the tasks whose waits ended join the run queue.

These rules stay open:

- whether workers run at the same time (`spec/README.md`, Conformance,
  item 5);
- when an event outside the program ends a wait;
- the order of the steps of different workers, except where messages order
  them.

The owner changed the sentence of `VISION.md`, "The backends", in the pull
request that adds this file. It now reads: "It does not bind error messages,
and it leaves limits, and whether workers run at the same time, to each
implementation."

## D2: the bootstrap interpreter conforms

The bootstrap may keep one host thread for every worker. Then a call of class
`blocks` blocks every worker there, as `spec/README.md` item 5 allows. A call
of class `suspends` must let the other tasks of its worker run. So:

- `spec/README.md` item 5 loses the sentences that free a one-thread
  implementation from the rule of `suspends` calls. The definition of a
  portable program loses the same clause.
- `sleep` suspends in the bootstrap. `blockingSleep` stays only for the
  contexts where a task cannot suspend. After N6 (PR #1082), `BlockedCall`
  does this for `sleep` and for every other call of class `suspends`.
- Each `NOT-BINDING: bootstrap` line that is about scheduling goes. The
  bootstrap lists the probes that it does not pass yet in its expected-failure
  file.

### Can Lox++ express the machine

Yes. This question does not wait for the spec text, because D4 fixes the set of
operations that the machine needs:

| Operation | Lox++ construct | In `bootstrap/loxpp_interpreter.lox` before N6 | In `bootstrap/loxpp_interpreter.lox` after N6 (PR #1082) |
|---|---|---|---|
| Suspend a computation at any call depth and resume it | a host task, a coroutine | each interpreted task is a host task | the same. All the state of a task is in its record, `InterpTask`, so a park copies nothing |
| A first-in, first-out run queue | List | yes | the host run queue: each interpreted task is a host task |
| A mailbox, and a wake when a message arrives | List and a host `task.send` | `taskSend`, `awaitWake` | `taskSend` appends to the mailbox of the `InterpTask` and sends one wake to its host task. A plain receive waits in a host `task.receive()` |
| Wait for any of many events, in a defined order | one host task per pending wait | Socket calls park through `callSuspending`. `SelectTimer` is the limit of a receive | `sleep`, `connect` and the Socket and Server calls park through `callSuspending`. A receive with a limit or a condition is the host `task.receive_where_within` or `task.receive_where`, with `ReceiveSelection.test` as the host `want`. No relay host task is between two waits |
| Block the worker | a host call where the host cannot suspend | `BlockedCall`, `BlockingSleeper` | `BlockedCall`: the host call runs inside a host `__str__`, where the host cannot suspend. `sleep` blocks there too |
| Workers that share nothing | one host thread for all of them | yes | the same |

Two limits are constraints on the bootstrap work:

- Lox++ has no call that waits for many events at once. The bootstrap uses
  one host task for each wait. Under D1 the host order of those host tasks is
  defined, so the order of the interpreted tasks follows from it. This is
  true only if the mapping adds no step that can change the order. The extra
  send of `SelectTimer` is such a step, and the bootstrap work checks it.
  N6 (PR #1082) removed `SelectTimer`; probe 366 fails when a relay host task
  takes its place.
- Guest workers on host workers that run at the same time are not needed.
  They are possible, because a guest message holds only values that can be
  written as host messages.

## D3: one waiting call per direction

Each Socket has two sides, and each Server has one:

| Side | Calls |
|---|---|
| Socket read side | `read`, `read_bytes`, `read_some`, `readline`, `readlines` |
| Socket write side | `write`, `writeline` |
| Server accept side | `accept` |

The rule:

1. While a call of a task waits on a side, a call on the same side from
   another task raises a catchable fault. The working name of its kind is
   `BusyError`. The spec change fixes the name.
2. The fault is raised at the call, also when the call could return at once,
   for example from bytes that are already buffered. So the result does not
   depend on the buffer.
3. A call on the other side does not raise. A reader task and a writer task
   can use one Socket.
4. `close` and `close_write` do not wait, so the rule does not apply to them.
   A close from another task still wakes the waiting call, which raises
   `SocketClosedError`.
5. A call that does not wait, while no call waits on the side, is legal from
   any task. A Socket has no owner and nothing to hand over.

Whether a second call finds a waiting call can depend on the network. So a
program that uses one side from two tasks is not portable, as it is not
portable today for writes. The difference is that the result is a fault that
the program can catch, not a hang or lost bytes.

What D3 removes:

- the order of waiters on one descriptor, because each side has at most one;
- the native queue of parked writers (`ObjSocket`'s writer queue,
  `liveWriter`), and the multi-waiter wake that #1054 asks for;
- the question of #932, and its text in `spec/05-stdlib.md` (from #1010).

Rejected alternatives:

- **Ownership with transfer.** It needs an owner, a transfer, and a rule for
  an owner that ends. Lox++ has none of these, and D3 does not need them.
- **Defined semantics for many waiters.** Every backend must then implement
  the fair wake and the order among the waiters on one descriptor. The bugs
  of the last two weeks came from that code.

The precedent is the coroutine: a resume of a coroutine that another task
runs raises `RunningCoroutineError`. D3 extends the same rule to every
resource that can wait.

Issue #1054 stays open under the mission. Its program then expects
`BusyError`. PR #1056 does not merge, because it binds the multi-waiter
behavior that D3 removes. Its probe of a partial line that stays parked does
not depend on D3, and the native work can keep it.

## D4: the freeze

Until the spec describes the machine, no change adds or changes a suspend point, a class of
blocking call, a `task` primitive, a scheduling rule, or a row of the worker
and task faults, except the changes of this mission. A bug fix that makes an
implementation match the spec and this decision is allowed. A performance
change that changes nothing a program can observe is allowed.

The follow-ups that the owner closed as not planned on 2026-11-01 stay closed:
#939, #942, #944, #945, #946.

## What this decision replaces

- F4 of `blocking-call-suspension.md`. That file stays accepted for every
  other fork, and it marks each place that F4 changed.
- The outcome of #932, and its text in `spec/05-stdlib.md`. The spec text
  changes with the D3 rule.
- The reason that `concurrency-model-decision.md` gave for a one-thread
  bootstrap: whether workers run at the same time is implementation-defined,
  not scheduling.

## Constraints from named bug classes

Each class binds every node of the mission and every later change to its
layer.

| Class | Layer | Constraint |
|---|---|---|
| A park overwrites state that belongs to a task | The task record of each backend | All state that belongs to a task is in one record of that task: the deferred calls, the handlers, the coroutines it resumed, the stack-overflow unwind, the current task. A park saves nothing by hand. A new field goes in the record, and the spec's table of per-task state lists it. |
| A wait ignores buffered bytes | The park and wake path of each backend | Under D3 a side has one waiter, so only that waiter takes the bytes of its side. A call tests its buffer before it waits, and again at each wake. |
| A call that waits again gets a new place | The registry of waits | A call keeps the place of its first wait until it returns. |
| The suspend points are chosen by syntax | The shared analysis (`reached`), and the dispatch of each backend | A suspend point that the analysis does not reach gets no resume state. A context where a task cannot suspend is decided by the frames of the task at run time, never by the syntax. |

## Mission

The tracking issue holds the DAG. These are the consumers that the mission
must cover:

| Consumer | Owes |
|---|---|
| Spec | The machine (task states, moves, per-task state, run queue, order of ready waits) as tables with a check script, the D1 and D2 text, and the D3 rule with its fault |
| Native VM | The full spec: the D3 fault, and the removal of the writer queue and the multi-waiter wake |
| JVM backend | The spec as the semantic oracle: the D3 fault, and the removal of the multi-waiter path of `LoxPoller` and `LoxSocket` |
| QBE backend | What its runtime shares with native, and its own suspend-point analysis (#1048) |
| Bootstrap interpreter | The full spec (D2): the D1 order, a `sleep` that suspends, the D3 fault |
| Resolver, LSP, tree-sitter grammar, editor syntaxes | No new syntax. The spec change checks whether any of them lists the fault kinds |
| `tools/check_fault_table.py`, `tools/check_net_faults.py`, `tools/check_blocking_decision_tables.py` | The new fault row, and the sides table |
| Probes and programs | The shared probes of D1 and D3. Probes 347 to 351 (from #1051) change to expect the fault. Each program that uses one side from two tasks changes |
| Documents | Every contradiction with D1 to D4 |

The spec and the shared probes land on `main` before the consumers. Each
consumer lists the probes it does not pass yet in its expected-failure file,
so the gate shows the gap and turns green with the last consumer.

The work after this mission (invariant tests, the cleanup of weak tests, and
the refactor of the runtimes) is filed as issues when the tracking issue
opens.

## Shipped

Mission #1058 ran this decision as a DAG of seven nodes, one pull request per
node. The nodes merged from 2026-11-02 to 2026-11-03. The retrospective is
`docs/records/concurrency-semantics-grounding-retro.md`.

| Node | Issue | Pull request | What shipped |
|---|---|---|---|
| N1 | #1059 | #1069 | Comments give worker parallelism, not scheduling, as the reason for a rule of one host thread |
| N2 | #1060 | #1070 | The Task Machine in `spec/04-semantics.md` (task states, worker states, moves, per-task state, sides), with `tools/check_task_machine.py`; the D1 and D2 text; Sides rules 1 to 6 and `BusyError`; probes 347 to 365; the expected-failure lists |
| N3 | #1061 | #1077 | Native: one waiting call per side, the holder in `ObjSocket` and `ObjServer`, closed before busy. The writer queue and the multi-waiter wake are removed. Also closes #1054 |
| N4 | #1062 | #1079 | JVM: `LoxSide` for each side, closed before busy, and a `close_write()` that wakes a waiting writer at the call. The multi-waiter path of `LoxPoller` is removed |
| N6 | #1064 | #1082 | Bootstrap: `sleep` suspends; one record per task (`InterpTask`); `SelectTimer` and `BlockingSleeper` are removed; D3 is delegated to the host, with an argument rule by rule; probe 366 |
| N5 | #1063 | #1083 | QBE: an unreached suspend point gets no resume state; probe 367. Also closes #1048 |
| N7 | #1065 | this change | The differential gate tells a rejection from a halt; probe 368 (Sides rule 6 on the write side, #1080); probe 369, one sentence in rule 4 of `task.receive_within`, and the JVM fix for the order of a passed limit; probe 370 and the JVM fix for a passed limit where the task cannot suspend; the close of the mission |

Every expected-failure list under `tools/spec-probe-xfail/` is empty at close:
the directory holds only `.gitkeep`.

### Rulings during the mission

| Ruling | Issue |
|---|---|
| A call holds its side from its first wait until it returns or raises, also while its task is ready after the wait ended (owner decision) | #1060 |
| Sides rule 6: a closed side fails as a closed value, also while a woken call still holds it | #1060 |
| Sides rule 2 wins over the rule of contexts where a task cannot suspend: such a call on a held side raises `BusyError` | #1060 |
| A close by a task of the same worker ends the wait at the close, as a send on the same worker delivers at the send | #1060 |
| A message from another worker is an event outside the worker; its place among the waits seen at the same point is not defined | #1060 |
| `close()` ends the wait on each side of its value; `close_write()` ends only the wait on the write side | #1060 |
| The bootstrap may delegate D3 to the host, with an argument for each rule and a probe or a check where the mapping could break a rule | #1064 |
| N7 takes #1080 and the JVM order question of the review of #1082 | #1065 |
| The order of a passed limit is bound: a receive that selects no message makes the `receive` move again, and the passed limit is seen as a `limit` move that joins the back of the run queue | #1065 |
| A receive that must suspend where its task cannot raises `YieldAcrossNativeError`, also when its limit has passed: `TimeoutError` needs the `limit` move, which such a call cannot make | #1065 |

### The order of a passed limit and a message on one worker

The review of #1082 found that native and the JVM order two tasks
differently: a task whose limit passed with a message that it did not
select, and a task that got its message. The first form of this section
said that the order is not bound. That argument was wrong, and the review of
#1088 (F1) showed why.

The ruling on #1065: the order is bound, and The Task Machine decides it. A
receive with a condition that selects no message makes the `receive` move
again, with its limit kept. When the worker then sees the passed limit, the
task makes the `limit` move, which joins the back of the run queue. An
implementation must not raise `TimeoutError` without that move. Rule 4 of
`task.receive_within` says that a message in the mailbox is tested before the
limit counts. It does not let the call skip the move. The point where a worker
sees a passed limit is open, but each such point puts the task at the back, so
it cannot put the task ahead of a task that is already in the run queue.

The bound case is probe 369: task A waits in `task.receive_where_within` with
a limit of 1 s, and task C in `task.receive()`. Main sends A a message that
A's `want` does not select, and then sends C a message. Both `message` moves
happen at the send, so the run queue is A, C. Main then computes for 2 s, so
A's limit passes while A is `ready`. A tests the message and makes the
`receive` move again, and its `limit` move puts it behind C. Native, QBE and
the bootstrap print C first. The JVM raised `TimeoutError` at once in
`LoxWorker.awaitMessage` and printed A first. This change fixes the JVM: a
call whose limit passed still waits, and it raises only after the `limit`
move ended the wait. It adds one sentence to rule 4.

The first program of the review of #1082 (both limits pass before main sends
anything) stays open: the worker can see both limits before the sends, and
then A and B join the run queue by their `limit` moves, in deadline order.

### The differential gate

`tools/diff_runtimes.py` compares each consumer with native in two phases:
whether the program is rejected, and what it does when it runs. CI runs it
over `test/translation-probes/` for the JVM ("JVM differential suite -
probes"), for QBE (`tools/check_qbe_probes.sh`, "Check QBE probes (S7)"), and
for the bootstrap ("Bootstrap differential suite - spec probes"). The mission
probes are 260, 277, 279, 282, 323, 325 and 347 to 370.

When native halts, the gate accepted any other stdout that is a prefix of
native's. An empty stdout is a prefix of every stdout, so a consumer that
rejected a halting program matched. A mutation of the bootstrap parser that
rejects `for`-in showed this on probe 282 (`MATCH (halt)`). For a spec probe
with `STATUS: error`, the other runner must now print the CHECK lines too.
`tools/check_diff_runtimes_phases.py` (ctest `DiffRuntimesPhases`) checks
both phases with stub runners. The gap stays for a halting program that is
not a spec probe, and for the five spec probes with `STATUS: error` and no
CHECK line (#1087).

The gates on the release build of this change, with no expected-failure
list:

| Consumer | `check_spec_probes.py` | `diff_runtimes.py` against native |
|---|---|---|
| Native | 295 pass, 0 fail | — |
| JVM | 295 pass, 0 fail | 398 matched, 0 diverged |
| QBE | 295 pass, 0 fail | 374 matched, 0 diverged (`test/translation-probes/`) |
| Bootstrap | 293 pass, 0 fail, 2 exempt | 293 matched, 0 diverged, 2 exempt |

### Bug classes that the mission found

The four classes under "Constraints from named bug classes" bound every node.
The reviews found the classes below. The reviews did not name them as classes, so they were not
written here during the mission (rule 4 of "Plans for a language change").

| Class | Found in | Layer | Constraint |
|---|---|---|---|
| The spec decides one case in two ways | #1070, findings F2, F3 and F6; #1088, F1 (rule 4 of `task.receive_within` and The Task Machine) | The spec text | Each new rule is checked against every section that decides the same case |
| A fix keeps a branch that the rule it applies makes wrong | #1088, F3: the JVM raised `TimeoutError` where the task cannot suspend, and rule 6 of `task.receive_within` says `YieldAcrossNativeError` | The receive of each backend (`LoxWorker.awaitMessage` on the JVM) | A fix checks every branch of the function it rewrites against the rule, the paths where the task cannot suspend included |
| A check script passes when one row of its table is removed | #1070, finding F1 | The table check scripts | A script pins the names of each table in both directions and fails on a removed row |
| A probe passes on an implementation that breaks the rule it is for | #1070, finding F5 (probe 358) | The shared probes | A probe forces the case that its rule names; a mutation of that rule fails it |
| A rule that names several sides has a probe for only some of them | #1079 (rule 6 on the write side) | The shared probes | A rule gets a probe line for each side or value that it names (probe 368) |
| A run-time comparison hides a compile-time rejection | #1065 (this change) | `tools/diff_runtimes.py` | A comparison that accepts a prefix of the output must also tell a rejection from a halt (#1087 for programs that are not spec probes) |

### Follow-ups

| Issue | Subject | Origin |
|---|---|---|
| #1066 | Invariant tests for the concurrent machine | Decision, work after the mission |
| #1067 | Remove the weak tests of the concurrent runtime | Decision, work after the mission |
| #1068 | Refactor the runtimes to read like the concurrent machine | Decision, work after the mission |
| #1078 | An abandoned task holds no side of a Socket or a Server | Review of #1077 |
| #1081 | JVM `LoxPoller.cancelInterest` key cleanup has no test that can fail | Review of #1079 |
| #1084 | QBE: decide coroutine mode from reached instructions only | Review of #1083 |
| #1085 | The Socket methods text names only `close()` as a call that wakes a waiting task | Review of #1070, round 3 |
| #1086 | No program runs a limited receive on the JVM-hosted and QBE-hosted bootstraps | Review of #1082 |
| #1087 | `diff_runtimes.py` cannot tell a rejection from a halt for a program that is not a spec probe | This change |

#1080 closes with this change.

## Where the task-owned state lives

Decided in #1142, after #1068 moved the counters of a task into one record
of the task (`TaskOwned` in native, `LoxTask.Owned` in the JVM).

**The rule.** A task's state lives in two places of the task, never in a
place that another task can write:

- The **record** of the task (`TaskOwned`, `LoxTask.Owned`) holds the
  counters and flags: the native and re-entrant depths, the select depth,
  the stack-overflow unwind, and the others in the spec's table of
  task-owned state.
- The **snapshot** of the task's coroutine (`ObjCoroutine`) holds the call
  frames, the handlers, the deferred calls, the result checks and the open
  upvalues while the task is not running. Every task runs in its own
  coroutine, so the snapshot belongs to the task, and the coroutines that
  the task resumed hold the rest of its frames.

The frame arrays, `m_frameCount` and `m_handlerStack` in `Runtime` are the
**working set of the running task**, as a register file is. The suspend at
a task switch moves the whole working set into the snapshot, and the resume
moves it back. It is the coroutine's own suspend, not a copy of one field by
hand, so it agrees with the first constraint above. When no task runs, the
working set holds nothing of any task, and the scheduler checks this after
each task runs.

**Why the working set does not move into the record.** The frame array has
1024 frames and a reserve, the stack has 16384 values (128 KB), and four more
arrays are parallel to the frames (five with `LOXPP_PROFILE`). One copy of
each for every task costs that memory per task. A pointer to the arrays of
the running task, swapped at each switch, puts one more read on every call,
return and handler access. The JVM measured that cost: with `frameCount` in
the task record, fib(37) three times took 1.81 s, against 1.62 s with it in
`LoxWorker` (8% slower). So the JVM `frameCount` also stays in its worker, as
the working set of the running task.
