# Ground the concurrency semantics before more concurrent work

**Status:** accepted. The owner decided D1 to D4 on 2026-11-03. The pull
request that adds this file records the approval.
**Tracking:** the mission issue opens when this file is on `main`.

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

`VISION.md` changes, and only the owner changes it. The proposed text for the
sentence in "The backends" is: "It does not bind error messages, and it leaves
limits, and whether workers run at the same time, to each implementation."

## D2: the bootstrap interpreter conforms

The bootstrap may keep one host thread for every worker. Then a call of class
`blocks` blocks every worker there, as `spec/README.md` item 5 allows. A call
of class `suspends` must let the other tasks of its worker run. So:

- `spec/README.md` item 5 loses the sentences that free a one-thread
  implementation from the rule of `suspends` calls. The definition of a
  portable program loses the same clause.
- `sleep` suspends in the bootstrap. `blockingSleep` stays only for the
  contexts where a task cannot suspend.
- Each `NOT-BINDING: bootstrap` line that is about scheduling goes. The
  bootstrap lists the probes that it does not pass yet in its expected-failure
  file.

### Can Lox++ express the machine

Yes. This question does not wait for the spec text, because D4 fixes the set of
operations that the machine needs:

| Operation | Lox++ construct | In `bootstrap/loxpp_interpreter.lox` today |
|---|---|---|
| Suspend a computation at any call depth and resume it | a host task, a coroutine | each interpreted task is a host task |
| A first-in, first-out run queue | List | yes |
| A mailbox, and a wake when a message arrives | List and a host `task.send` | `taskSend`, `awaitWake` |
| Wait for any of many events, in a defined order | one host task per pending wait | Socket calls park through `callSuspending`. `SelectTimer` is the limit of a receive |
| Block the worker | a host call where the host cannot suspend | `BlockedCall`, `BlockingSleeper` |
| Workers that share nothing | one host thread for all of them | yes |

Two limits are constraints on the bootstrap work:

- Lox++ has no call that waits for many events at once. The bootstrap uses
  one host task for each wait. Under D1 the host order of those host tasks is
  defined, so the order of the interpreted tasks follows from it. This is
  true only if the mapping adds no step that can change the order. The extra
  send of `SelectTimer` is such a step, and the bootstrap work checks it.
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

- F4 of `blocking-call-suspension.md`.
- The outcome of #932, and its text in `spec/05-stdlib.md`.

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
