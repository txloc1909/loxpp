# Network calls and `sleep` suspend only their own task

**Status:** proposal. Waits for the owner's approval. On acceptance it moves to
`docs/decisions/` and names its tracking issue.
**Tracking (draft):** #711. The DAG and the node body drafts are there. Node
issues open only after acceptance.

## Owner decisions

The review on #711 found five blockers. The owner decided the open forks on
2026-10-19. This revision applies the decisions.

| Fork | Decision |
|---|---|
| F1: scope | Calls on the network and `sleep` suspend only their task. `input()`, `run()`, and the Process calls continue to block the worker. |
| F2: contexts where a task cannot suspend | A blocking call in such a context blocks the worker, as it does today. It does not raise. |
| F3: name resolution in `connect` | The worker blocks while `connect` resolves a host name. The TCP connect suspends. |
| F4: the bootstrap interpreter | It keeps its one-thread form. `spec/README.md`, Conformance, makes scheduling implementation-defined for every implementation, so a blocking call may block every worker there. |
| Socket faults | A socket fault becomes catchable. This is a prerequisite, #726, not a node of this mission. |
| Close while parked | Follows the socket-fault rule: the parked call raises the fault of a call on a closed socket. |
| A `for`-in over a Map that another task changes | The fault stays fatal. The spec warns about it (see "Constraints", C1). |

Three node-level questions take the recommendation of the first draft:

- a write keeps the program's string and an offset, and copies nothing;
- `sleep(0)` and a negative duration return at once, with no suspension;
- the JVM adds park and resume to its handoff scheduler, and keeps the FIFO
  order.

## Why

This proposal serves two goals in `VISION.md`.

- **"What Lox++ is"**: the language adds "access to files, the network, and
  processes" for real programs. A program can reach the network today, but it
  serves one connection at a time: a blocking call stops its worker and every
  task on it.
- **"What counts as a language gap"**: a feature is a real gap only when a
  program cannot compose it from the exposed primitives. Asynchronous network
  I/O cannot be composed today (see "The gap is not bootstrappable").

The spec already reserved this change:

> "A blocking call is in the table so that a later version can let other tasks
> of the worker run while the call waits, with no change to the rule above."
> — `spec/04-semantics.md`, Suspend Points

The rule above that sentence is: a task runs alone on its worker from one
suspend point to the next. Every call in the scope is already a suspend point.
This proposal adds no construct; it changes the effect of calls that the spec
already names.

The proposal is bound by these controls in `VISION.md`.

- **suspension**: "a program pays for suspension only where it uses it". The
  poller is created lazily. A blocking call in a context where the task cannot
  suspend blocks the worker, so a program gets no new fault from this change
  (see "Contexts where a task cannot suspend").
- **cost**: "no construct ... adds work at a site that does not name it". The
  added work is at the blocking call and at an idle worker's wait. No other
  call and no loop pays.
- **evaluation order**: "the order of evaluation is defined, not left open".
  The order of the tasks of one worker stays defined, except for the time at
  which an outside event completes a wait. "Ordering" states the limit.
- **memory**: "the program can observe and bound the memory it uses". A write
  keeps the program's string; it adds no output queue.
- **failure**: "a fault is a value that the program can catch". The
  socket-fault prerequisite makes this true for sockets. Without it, a server
  with many connections ends at the first fault of one connection.
- **The backends**: the native VM decides; the JVM is a semantic oracle; the
  bootstrap keeps its one-thread form (`spec/README.md`, Conformance); QBE owes what the current
  experiment needs.
- **Distribution**: the poller stays behind the Linux platform boundary.

It does not serve true parallelism (roadmap item 7), FFI, or the dynamic
functional direction.

## Today

One `accept()` or `readline()` blocks its worker and every task on it. Probe
83 (`test/translation-probes/83_blocking_call_blocks_one_worker.lox`) covers
the cross-worker case: a task on another worker keeps running.

The same-worker case: a peer on another worker writes a line after one second.
A ticker task is on the same worker as the reader.

```lox
var srv = listen("127.0.0.1", 0);
var me = task.self();
fun peer(port) {
    var c = connect("127.0.0.1", port);
    sleep(1);
    c.writeline("hello");
    c.close();
}
fun ticker() { print "ticker ran"; task.send(me, "done"); }
task.spawn_worker(peer, srv.port());
task.spawn(ticker);
var conn = srv.accept();
print "reader got: " + conn.readline();
task.receive();
```

On `build/loxpp`, `reader got: hello` prints before `ticker ran`, after one
second. On the JVM backend the result is the same: `LoxWorker` is a
cooperative handoff, and a blocking native never calls `switchTo`. Both
backends need this mission.

## The gap is not bootstrappable

A pure-Lox++ netpoller needs four things the language does not expose:

1. a descriptor and a nonblocking mode;
2. a readiness primitive (`poll`, `select`, `epoll`);
3. a timeout on a socket call;
4. a way to wake a task when a descriptor becomes ready.

`src/stdlib/net_api.cpp` hands the descriptor to `fdopen` and reads with
`fread`/`fgetc`. It has no `fcntl`, no `O_NONBLOCK`, and no `setsockopt`
surface.

Two workarounds are also closed.

- A `Socket` is not a message (`src/container_objects.h`, `isMessage`), so a
  worker cannot accept a connection and hand it to another worker.
- `listen` sets `SO_REUSEADDR`, not `SO_REUSEPORT`, so workers cannot share
  one listener.

Coroutines are stackful, so a readiness primitive with a timeout would let a
Lox++ library do this work. This proposal does not add one: the spec reserved
invisible suspension, and it keeps the surface unchanged. The cost is that the
bootstrap interpreter cannot implement the change; scheduling is
implementation-defined (`spec/README.md`, Conformance), so it does not have to.

## Prerequisites

Two issues must land before the spec node of this mission.

1. **#723: a task or coroutine whose entry is a native.** Since #628, such an
   entry passes `taskCanSuspend()`, then ends with
   `BUG: a task ended through a handler outside it.` when it suspends. Under
   this proposal, `task.spawn(srv.accept)` would take that path. The fix makes
   a native entry a context where the task cannot suspend.
2. **#726: catchable socket faults.** Today a socket fault is fatal:
   it uses the row "A stdlib native function reports its own error" of the
   Halting Faults table. A write to a peer that closed gives
   `write(): Broken pipe` and exit 70, inside `try`. A refused connection does
   the same. After this mission a server holds many connections on one worker,
   and the first fault of one connection would end all of them. That issue
   defines the fault kinds and messages for `connect`, `listen`, `accept`, and
   the Socket calls, and makes them catchable. #519 depends on it.

## Scope

The rule has two classes. Both are rules of the spec, not stages.

**Calls that suspend their task.** Each waits for the network or for time:

| Call | Waits for | Mechanism |
|---|---|---|
| `sleep(seconds)` | time | timer queue |
| `connect(host, port)` | the connection | name resolution on the worker, then a nonblocking connect (`EPOLLOUT`, `SO_ERROR`) |
| `Socket.read()` | the peer to close | socket read readiness, until EOF |
| `Socket.read_bytes(n)` | the bytes, or the peer to close | socket read readiness, until `n` or EOF |
| `Socket.readline()` | a line, or the peer to close | socket read readiness and the socket's input buffer |
| `Socket.readlines()` | the peer to close | socket read readiness and the socket's input buffer, until EOF |
| `Socket.write(text)` | room in the connection | socket write readiness; keep the string and an offset |
| `Socket.writeline(text)` | room in the connection | as `write`, plus a newline |
| `Server.accept()` | an inbound connection | listener read readiness |

**Calls that block their worker.** Each waits for standard input or for
another process: `input()`, `run(program, args)`, and every Process call in the
Blocking calls table (`read`, `read_bytes`, `readline`, `readlines`,
`read_err`, `err_read_bytes`, `err_readline`, `err_readlines`, `write`,
`writeline`, `wait`). They keep today's rule.

The set is the nine calls above. The property is: the call suspends only its
task, except in a context where the task cannot suspend, and except during the
name resolution of `connect`. The set and the property are equal by call
identity: the rule attaches to the call, not to the function that contains it,
so the dynamic call graph cannot add a call to the set or remove one. The
exceptions are stated by a dynamic predicate (the next section), not by a
syntactic selector.

The two classes together must cover the spec's Blocking calls table exactly.
A script checks this before any backend node starts.

## Semantics

### Suspension

A call in the first class suspends the calling task until its wait completes.
Every other task on the worker keeps running. The call is a suspend point, as
the table in `spec/04-semantics.md` says.

A suspended call suspends the whole task. Every frame of the task stays in
place, including the frames of a coroutine the task resumed. No deferred call
runs. A coroutine that holds a suspended call keeps the status `"running"`, so
a resume of it from another task raises `RunningCoroutineError`, as for a
suspended `receive`.

### Contexts where a task cannot suspend

The runtime calls some Lox++ code from a C++ frame, and needs the result
before it continues. A task cannot suspend through that frame. Today
`task.receive()` on an empty mailbox raises `YieldAcrossNativeError` there.

A call in the first class in such a context **blocks the worker**, as it does
today. It does not raise. The contexts, from a search of the native VM and
QBE on `main` at d19037d, checked with a probe for each:

| Context | The runtime calls Lox++ code for |
|---|---|
| `__str__` | `print`, `str()`, an element or key of a List or Map that is printed, a `__str__` inside a `__str__`, an uncaught throw of an instance that is not an `Error` |
| a map key's `__hash__` and `__eq__` | `m[k]`, `m[k] = v`, a map literal, `k in m`, `Map.has`, `Map.del`, and a hash collision |
| `reflect.callMethod` | the method, or a callable field |
| a deferred call | every drain: at a normal return, after a caught throw, after a run-time fault, after an uncaught throw, at the end of a task function, at the end of a coroutine function |
| a native entry | a task or coroutine whose entry is a native or a bound native (after #723) |
| a nested coroutine | each coroutine the task resumes inside one of the contexts above |

The predicate is `taskCanSuspend()`. The set and the predicate must be equal.
#723 is the one known difference today.

A task **can** suspend at the top level, in functions, in tasks, in `init`, in
every operator dunder and its reflected form, in `__lt__` and `__gt__`,
`__contains__`, `__call__`, `__index_get__`, `__index_set__`, `__len__`,
`__iter__`, `__slice__`, in `==` and in `match` patterns, and in a coroutine
resumed outside the contexts above.

Why the call blocks and does not raise: a raise would make the fault depend on
the network. The same deferred `conn.writeline("bye")` would succeed when the
socket has room and raise when it has none. A program with one task would get
a new fault from a feature it does not use. Blocking keeps the old behavior
exactly where suspension is not possible.

### `connect` and name resolution

`getaddrinfo` cannot be polled. When `host` is a host name, the worker blocks
while the name resolves; then the TCP connect suspends. When `host` is a
numeric address, there is no resolution, so `connect` suspends fully. The spec
states this exception on `connect`.

### Resume and repeated suspension

A resumed call returns the value it would have returned from a blocking call.
One call may suspend more than once: `read_bytes(n)` may park on every chunk;
`write` may park until the last byte is accepted. The pending call keeps its
state (the buffer, the target count, the string and the offset) on the heap,
not on a C++ stack frame.

### Ordering

`spec/04-semantics.md`, Scheduling, gains one rule:

> A task that suspends in a call that waits for the network or for time leaves
> the run queue. When its wait completes, it joins the back of the run queue.

When several waits complete in one wait of the worker, the tasks join the run
queue in this order: ready descriptors in the order the calls registered
their interest, then expired timers in deadline order. `epoll_wait` gives no
order, so the poller sorts the ready records.

The order of the tasks of one worker stays defined, except for one thing: an
event outside the program decides when a wait completes, and so in which wait
of the worker it is seen. The sentence of §Scheduling "The order in which the
tasks of one worker run is therefore defined" changes to state this limit, as
the spec already does for messages between workers.

### Errors

With the socket-fault prerequisite, a fault at a suspend point is the fault
the call raises today, now catchable, with the same kind and message. The
fault is raised in the parked task when it runs again.

### Close while parked

A task can close a Socket or Server while another task of the same worker is
parked on it. The close unregisters the descriptor, then wakes the parked
task. The parked call raises the fault of a call on a closed socket
("Cannot call 'readline' on a closed socket.", with the kind that the
prerequisite gives it). The woken task joins the back of the run queue at the
close.

### Liveness and deadlock

A task that waits for the network or for time is not deadlocked: the event can
arrive. The program is deadlocked only when the main task waits in `receive`,
no task on any worker is running or ready to run, no task is parked on a
descriptor or a timer, no task is in a blocking call, and no message is on its
way.

A wait that can never complete (a peer that never writes) hangs the program,
as a blocking call does today. This mission adds no timeout.

The deadlock report "every task waits in receive" must not name a parked task.

### Teardown

When the main task ends, `exit()` runs, or a fatal fault ends the program,
every poller wakes and the parked tasks are abandoned. Their deferred calls do
not run, as for every abandoned task. The runtime closes the descriptors of
parked tasks itself, because no deferred call will.

### Buffers and memory

A write does not copy the program's string. The pending call holds the string
and an offset; on `EAGAIN` it parks on write readiness and continues at the
offset.

A read uses one input buffer per socket. The buffer holds the bytes of the
result the call returns, plus at most one read chunk after the end of a line,
which the next `readline` uses. `read()` and `readlines()` return the whole
stream, as they do today.

### Timers

`sleep(seconds)` uses the timer queue. The timeout of `epoll_wait` is the
nearest internal deadline. It is not a user timeout.

## Constraints

Suspension is allowed in these cases, but the result changes because another
task of the worker runs during the wait. The decision holds them as
constraints on every node, and the spec states C1, C6 and C7.

- **C1. A `for`-in over a Map that another task changes.** A park inside
  `for (var k in m)` while another task adds a key to `m` ends the program with
  the fatal fault "Map changed size during iteration." (`spec/04-semantics.md`,
  Halting Faults). The fault stays fatal. The spec and the `for`-in
  section warn: a `for`-in over a Map that another task can change must not
  contain a call that can suspend. A probe shows the fatal result.
- **C2. A `for`-in over a List that another task changes.** The behavior is
  defined, but the loop can end early when another task removes elements.
- **C3. A park inside a generator that the task resumes.** The generator stays
  `"running"`; a resume from another task raises `RunningCoroutineError`.
- **C4. Stale operands and lost updates.** In `a + b` with a dunder that
  parks, the operands are read before the switch. In `m[k] = val()` where
  `val()` parks, the write of the parked task replaces a write that another
  task made during the park.
- **C5. A half-built instance.** If `init` parks after `this` escapes, other
  tasks can see the instance before `init` completes.
- **C6. Program end.** The runtime closes the descriptors of parked tasks
  (see "Teardown").
- **C7. Deadlock count.** A parked task is a task that can continue (see
  "Liveness and deadlock").

**Atomicity.** Today the spec says that no task of the worker runs until a
blocking call returns. A program can use this to make a critical section
atomic across a network call. After this change it cannot, and the language
gives no way to keep the old behavior for one call. The spec reservation
permits the change. A program that needs a critical section across a wait puts
the shared state in one task, which the other tasks reach with messages.

## What each consumer owes

| Consumer | What it owes | Evidence |
|---|---|---|
| Native VM | The whole model: poller, park and resume, readiness registry, socket transport, liveness, teardown, the contexts rule. | The reference implementation. |
| JVM | Oracle parity: a task that waits in a call of the first class lets the other tasks of its worker run, in the same order. | The handoff in `runtime/jvm/src/lox/LoxWorker.java` stops the worker; it needs a park and resume path in the scheduler. |
| Bootstrap interpreter | Its one-thread form: a blocking call blocks every worker. It cannot pass the same-worker probes, and they do not bind it, because scheduling is implementation-defined. | `spec/README.md`, Conformance; a bootstrap-only probe checks its choice. |
| QBE | Runs the same probes. It links the native runtime. Check the coroutine mode and its C frames; #723 also covers a native-entry defect on QBE. | `src/backend/qbe_emitter.cpp`. |
| Spec | The two-class rule, the markers, the tables, the contexts rule, the Scheduling rule, the `connect` exception, C1, the probes. | `spec/04-semantics.md`, `spec/05-stdlib.md`. |
| LSP docs | The text of the nine calls says "suspends the task until". No signature changes. | `src/lsp/stdlib_docs.cpp`. |
| Tree-sitter and editors | No change: there is no syntax change. | — |
| Gates | `tools/check_blocking_calls.py`, the probe runners, `tools/check_jvm_probes.sh`, the bootstrap checks, `tools/diff_runtimes.py`. | Each consumer that does not pass a probe lists it in an expected-failure file. |

## Design sketch

### Task state

`TaskState` gains `PARKED`, with a wait reason (`FD_READ`, `FD_WRITE`,
`TIMER`) and a pending value or pending fault. `WAITING` stays for `receive`.
`task.pending()` reads only the mailbox, so the pending result cannot live
there; a sentinel message would be observable. `deliverMessage` wakes a
`WAITING` task only; a `PARKED` task keeps the message in its mailbox.

### Park and resume

A native calls `Runtime::parkCurrentTask(reason)`. When `taskCanSuspend()` is
false, the native does a blocking wait on the descriptor instead, and returns
normally. Otherwise `callNative` suspends the task, and `resumeTask` supplies
the pending value as the value of the suspend point.

The parked tasks and their pending values are roots. `pushTempRoot` is
C++-stack-scoped and cannot span a suspension, so the runtime keeps a parked
set that `markRoots` traces.

### Poller

One `Poller` per worker, created lazily on the first park. It holds an `epoll`
descriptor, an `eventfd` wakeup, a timer heap, and a map from descriptor to
wait record. `waitForWork` waits on the epoll descriptor with the nearest
deadline; `postEnvelope` writes the eventfd. `EPOLL_CLOEXEC`, `EFD_CLOEXEC`,
and `SOCK_CLOEXEC` keep the descriptors out of a child that `run()` forks.

A worker that never parks never creates a poller.

### Readiness registry

A descriptor maps to its wait records: the task, the interest (read or
write), and the call state. Level-triggered. A close unregisters before it
closes the descriptor: `epoll` keeps a reference to the open file
description after `close`, and a reused descriptor number must not inherit a
stale record.

### Transport

`ObjSocket` and `ObjServer` drop the `FILE*` and keep the descriptor, in
nonblocking mode. Reads and writes use `read` and `write`. `EINTR` retries;
`EAGAIN` parks, or waits in place when the task cannot suspend. `SIGPIPE`
stays ignored for the call. The runtime sets `O_NONBLOCK` only on descriptors
it creates; standard input and the Process pipes keep their mode.

### Liveness accounting

`WorkerGroup::goIdle` learns the wait reason. A worker with a parked task can
make progress; it is not idle for the deadlock count. Program end wakes every
poller.

## Tables and checks

The spec's Blocking calls table stays the source of the set: 22 calls. It
gains a column that names the class of each call. The nine calls of the first
class carry `**Suspends:** yes`; the others keep `**Blocks:** yes`.
`tools/check_blocking_calls.py` checks each marker against the table and the
class column. A row with no marker, a marker with no row, or a marker that
disagrees with the class fails.

The decision holds the mechanism table of the first class. A new check
cross-references it with the spec table and fails when a call is missing or
extra. The decision also holds the table of contexts where a task cannot
suspend. The table and the scripts land with the spec node, before any
backend node starts (`AGENTS.md`, "Plans for a language change", rule 2).

The shared probes land with the spec node:

- `sleep` suspends one task;
- `accept`, `read`, `readline`, `read_bytes`, `write`, and `connect` each
  suspend one task;
- `connect` to a numeric address suspends; a host name blocks during
  resolution;
- `input()`, a Process read, and `run()` still block the worker;
- one probe per group of the contexts table: the call blocks, and does not
  raise;
- two ready descriptors wake in the defined order;
- a parked task is not a deadlock;
- a close while a task is parked raises the closed-socket fault in that task;
- C1: a `for`-in over a Map that another task changes ends with the fatal
  fault;
- a bootstrap-only probe for the one-thread exception.

Each probe carries `STATUS`, `TIMEOUT`, and `CHECK` lines in the existing
form, so the probe runner reads it.

## Rejected alternatives

**The whole Blocking calls table (F1-A).** Standard input and the Process
calls need `pidfd` or `SIGCHLD` (process-wide), a change to the stderr thread
of `run()`, and `fork()` in a process with threads. `epoll` refuses a regular
file or `/dev/null` as standard input with `EPERM`, and `O_NONBLOCK` on an
inherited standard input changes the descriptor for every process that shares
it. None of this serves the network goal.

**Raise `YieldAcrossNativeError` in a context where the task cannot suspend
(F2-A).** The fault would depend on the network, and a program with one task
would get it (see "Contexts where a task cannot suspend").

**Support suspension through a C++ frame.** It needs a C++ continuation or a
separate C stack per task. QBE's C frames make it harder.

**A resolver thread for `connect` (F3-A).** It removes the resolution
exception, at the cost of a thread per worker, a handoff into the heap, and
`fork()` with threads. It can come later with no change to the rule for a
numeric address.

**Worker per connection.** Closed: a `Socket` is not a message, and workers
cannot share a listener. It also costs one OS thread per connection.

**Sendable sockets.** A blocking read still stops the receiving worker, so a
server would still need one worker per connection.

**A poller library in Lox++.** It needs a readiness primitive that the
language does not have (see "The gap is not bootstrappable").

**A new async API.** `async`/`await`, futures, or a poller object would grow
the surface. Invisible suspension keeps it unchanged and is the design the
spec reserved.

**Fair-mode preemption.** Reserved by the concurrency decision and not
needed. It would add work at sites that do not name a suspend point.

## Compatibility and migration

The rule changes for the nine calls of the first class. A program that relied
on a network call or `sleep` to serialize its worker sees a different order.
The spec's reservation is the authorization; the decision must quote it.

There is no syntax change, no new global, and no new method. The JVM's output
changes on the same-worker probes; its expected-failure file lists them until
the JVM node lands. The bootstrap keeps its one-thread exception.

No program in the repository makes a blocking call in a context where the task
cannot suspend, and no program has a second task on a worker that does
network I/O. Probe 83 puts its second task on another worker.

The term "blocking call" stays for all 22 calls. It means a call that waits
for an outside party. The class column says whether it suspends its task or
blocks its worker.

## Non-goals

- Suspension for `input()`, `run()`, and the Process calls.
- A resolver thread for `connect`.
- True parallelism or multi-core scheduling.
- Suspending FFI calls.
- Socket timeouts, `read_some`, UDP, unix sockets, TLS.
- A user-facing poller API.
- Fair mode.
- A catchable fault for a Map that changes size during `for`-in.
- Changing `SendError`, `DeadlockError`, or the message rule.

## Residual hard edges

1. **Descriptor reuse.** Unregister before close; `epoll` holds the open file
   description, not the number.
2. **Lost wakeup.** Between the inbox check and `epoll_wait`, a send must
   write the eventfd.
3. **The blocking path.** In a context where the task cannot suspend, the call
   waits on a nonblocking descriptor with `poll`. It must not spin.
4. **`fork()` in `run()`.** Every new descriptor is `CLOEXEC`.
5. **JVM scheduler.** The handoff model is the blocker, not the socket call.
   The JVM node adds park and resume without a change to the FIFO order.

## References

- `VISION.md`, "What Lox++ is", "What counts as a language gap", "Design
  goals", "The backends", "Distribution".
- `spec/04-semantics.md`, Suspend Points, Scheduling, Deadlock, Ending, `for`
  statement, Halting Faults; `spec/README.md`, Conformance.
- `spec/05-stdlib.md`, Blocking calls, `sleep`, `connect`, Socket methods,
  Server methods.
- `docs/decisions/concurrency-model-decision.md`, "Scheduler" and "What this
  unblocks and in which order".
- `docs/decisions/expressiveness-roadmap.md`, item 5 and item 7.
- `docs/decisions/coroutines.md`.
- `docs/process/multi-agent-playbook.md`, "Mission lifecycle" and "Node
  specification structure".
- The review on #711; #519, #629, #722, #723, #724, #726.
