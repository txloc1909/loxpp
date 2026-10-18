# Blocking calls suspend only their own task

**Status:** proposal. Waits for the owner's approval. On acceptance it moves to
`docs/decisions/` and names its tracking issue.

## Why

This proposal serves two goals in `VISION.md`.

- **"What Lox++ is"**: the language adds "access to files, the network, and
  processes" for real programs. A program can reach the network today, but it
  serves one connection at a time: a blocking call stops its worker and every
  task on it.
- **"What counts as a language gap"**: a feature is a real gap only when a
  program cannot compose it from the exposed primitives. Asynchronous I/O
  cannot be composed today. It is a language change, not a library.

The spec already reserved this change:

> "A blocking call is in the table so that a later version can let other tasks
> of the worker run while the call waits, with no change to the rule above."
> — `spec/04-semantics.md`, Suspend Points

This proposal is that later version. It adds no construct; it changes the
effect of calls the spec already names. That is why the change does not remove
a control: the serialization was a documented limitation with a reserved
change, not a promised control.

The proposal is bound by these controls in `VISION.md`.

- **suspension**: "a program pays for suspension only where it uses it". The
  poller is created lazily, and no fair-mode preemption is added.
- **cost**: "no construct ... adds work at a site that does not name it". The
  added work sits at the blocking call and at an idle worker's wait. No
  ordinary call or loop pays.
- **evaluation order**: "the order of evaluation is defined, not left open".
  The wakeup order is specified in "Semantics".
- **memory**: "the program can observe and bound the memory it uses". Buffers
  are bounded, and a write keeps the program's string instead of copying it.
- **The backends**: the native VM decides; the JVM is a semantic oracle; the
  bootstrap matches native except under its one-thread exception; QBE owes
  what the current experiment needs.
- **Distribution**: the poller stays behind the Linux platform boundary.

It does not serve true parallelism (roadmap item 7), FFI, or the dynamic
functional direction. Do not sell it as a step toward multi-core.

## Today

One `accept()` or `readline()` blocks its worker and every task on it. Probe
83 (`test/translation-probes/83_blocking_call_blocks_one_worker.lox`) covers
the cross-worker case: a task on another worker keeps running. The same-worker
case has no probe because it cannot pass.

The gap is reproducible in five lines. A reader task and a ticker task on one
worker; the reader waits three seconds in `readline`; the ticker should print
at once:

```lox
fun reader() {
    var c = connect("127.0.0.1", PORT);
    var line = c.readline();
    print "reader: " + line;
}
fun ticker() {
    print "ticker ran";
}
task.spawn(reader);
task.spawn(ticker);
```

On `build/loxpp` and on the JVM backend, `ticker ran` appears only after the
read returns. The JVM does not already have the behaviour: `LoxWorker` is a
cooperative handoff, and a blocking native never calls `switchTo`, so the
worker stops. Both backends need this mission; the JVM cannot be the oracle
before its own node lands.

## The gap is not bootstrappable

A pure-Lox++ netpoller needs four things the language does not expose:

1. a descriptor and a nonblocking mode;
2. a readiness primitive (`poll`, `select`, `epoll`);
3. a timeout on a socket call;
4. a way to wake a task when a descriptor becomes ready.

None exists. `src/stdlib/net_api.cpp` hands the descriptor to `fdopen` and
reads with `fread`/`fgetc`; there is no `fcntl`, no `O_NONBLOCK`, and no
`setsockopt` surface.

Two workarounds are also closed.

- A `Socket` is not a message (`src/container_objects.h`, `isMessage`), so a
  worker cannot accept a connection and hand it to another worker.
- `listen` sets `SO_REUSEADDR`, not `SO_REUSEPORT` (`src/stdlib/net_api.cpp`),
  so workers cannot share one listener.

A worker-per-connection server therefore cannot be written in pure Lox++.
#519 depends on this work, and #629 is its first step.

## Scope

The mission's end state is **every row of the Blocking calls table** in
`spec/05-stdlib.md`. The staging below is a sequence of capability nodes, not
a narrowing of the rule. Each stage handles a named set of call rows; the
property is attached to the call, not to the function that contains it, so the
set and the property are equal by call identity. A call in a later stage keeps
the old rule and stays in the expected-failure list until its stage lands.

| Call | Waits for | Mechanism |
|---|---|---|
| `input()` | a line on standard input | fd 0 read readiness and a line buffer |
| `sleep(seconds)` | time | timer queue |
| `connect(host, port)` | the connection | name resolution, then nonblocking connect (`POLLOUT`, `SO_ERROR`) |
| `run(program, args)` | the child to exit | child pipes and child exit |
| `Socket.read()` | the peer to close | socket read readiness, until EOF |
| `Socket.read_bytes(n)` | the bytes, or the peer to close | socket read readiness, until `n` or EOF |
| `Socket.readline()` | a line, or the peer to close | socket read readiness and a line buffer |
| `Socket.readlines()` | the peer to close | socket read readiness and a line buffer, until EOF |
| `Socket.write(text)` | room in the connection | socket write readiness; keep the string and an offset |
| `Socket.writeline(text)` | room in the connection | as `write`, plus a newline |
| `Server.accept()` | an inbound connection | listener read readiness |
| `Process.read()` | the child to close its standard output | pipe read readiness |
| `Process.read_bytes(n)` | the bytes, or the end of the stream | pipe read readiness, until `n` or EOF |
| `Process.readline()` | a line, or the end of the stream | pipe read readiness and a line buffer |
| `Process.readlines()` | the end of the stream | pipe read readiness and a line buffer, until EOF |
| `Process.read_err()` | the child to close its standard error | pipe read readiness |
| `Process.err_read_bytes(n)` | the bytes, or the end of the stream | pipe read readiness, until `n` or EOF |
| `Process.err_readline()` | a line, or the end of the stream | pipe read readiness and a line buffer |
| `Process.err_readlines()` | the end of the stream | pipe read readiness and a line buffer, until EOF |
| `Process.write(text)` | room in the pipe | pipe write readiness; keep the string and an offset |
| `Process.writeline(text)` | room in the pipe | as `write`, plus a newline |
| `Process.wait()` | the child to exit | child exit |

The table must cover the spec's Blocking calls table exactly. The decision
holds this table, and a script checks it against `spec/05-stdlib.md` before
any backend node starts.

The staging order is: timer loop and park/resume; the socket read side; the
socket write side; `connect`; stdin and process pipes; child wait; JVM parity;
bootstrap and QBE parity. The tracking issue holds the waves and the nodes.

## Semantics

### Suspension

A blocking call suspends the calling task until its wait completes. Every
other task on the worker keeps running, including tasks that were behind it in
the run queue. The call is a suspend point exactly as the table in
`spec/04-semantics.md` says; the table's rows do not change, only their
effect.

A blocked call suspends the whole task. Every frame of the task stays in
place, including the frames of a coroutine the task resumed. No deferred call
runs. A coroutine that holds a suspended blocking call keeps the status
`"running"`, so a resume of it from another task raises
`RunningCoroutineError`, exactly as a coroutine that holds a suspended
`receive` does today.

### Resume and repeated suspension

A resumed call returns the value it would have returned from a blocking call:
the bytes, the line, the accepted socket, or `nil`. One call may suspend more
than once. `read_bytes(n)` may park on every chunk until `n` bytes arrive;
`write` may park until the last byte is accepted. The pending call keeps its
own state (the buffer, the target count, the string and offset) on the heap,
not on a C++ stack frame.

### Ordering

A task that parks leaves the run queue. When its wait completes, it joins the
back of the run queue, like a task woken by `send` or a `yield`. If several
waits complete in one poll, the worker handles them in this order:

1. ready descriptors, in the order the calls registered their interest;
2. expired timers, in deadline order;
3. child exits, in registration order;
4. resolution results, in registration order.

`epoll_wait` does not define an order, so the poller sorts the ready records
by the registration sequence. The order is observable and must not depend on
the host.

### Liveness and deadlock

A task that waits for an outside party is not deadlocked: the event can
arrive. The deadlock rule becomes: the program is deadlocked only when every
task waits in `receive`, no worker has a runnable task, no descriptor is
registered, no timer is pending, no child wait is pending, and nothing is
inbound.

An I/O wait that can never complete (a peer that never writes, a pipe whose
writer is gone without closing) hangs the program, as a blocking call does
today. The runtime cannot know that the event will never arrive. No timeout is
added by this mission.

The existing deadlock report, "every task waits in receive", stays accurate
for its case. It must not name a task that waits on I/O, because that case is
not a deadlock.

### Errors

A read or write error at the suspend point follows the current rule: the same
message, the same fatal or catchable path. This mission does not change
whether a socket error is catchable. That is a separate gap.

### Buffers and memory

A write does not copy the program's string. The pending call holds the string
value and an offset; on `EAGAIN` it parks on write readiness and resumes at
the offset. No user-space output queue can grow without bound.

A read buffer holds only the bytes the call's result needs: the line for
`readline`, the `n` bytes for `read_bytes`, the whole stream for `read`. It
grows only with what the program asked for and already receives today.

### Re-entrant natives

`task.receive` raises `YieldAcrossNativeError` when a native callback is on the
call chain, because the C++ frame cannot be suspended through. A blocking call
in that position has three options:

1. raise `YieldAcrossNativeError` with a message that names the call
   (recommended: one rule, consistent with `receive`, and catchable);
2. fall back to blocking. This breaks the mission's property and the
   `AGENTS.md` rule about fallbacks; reject;
3. support suspension through a native callback. This needs a C++ continuation
   or a separate stack per task; reject for this mission.

The decision must pick one. A probe covers the case either way.

### Teardown

When the main task ends, `exit()` runs, or an uncaught fault ends the program,
every poller wakes, parked tasks are abandoned, descriptors are unregistered
and closed, and `endTasks` includes parked tasks. An abandoned task's deferred
calls do not run, as today.

### Timers and timeouts

`sleep(seconds)` uses the timer queue. Socket timeouts, a `read_some` call, and
a user-facing poller API are not part of this mission. The poller's
`epoll_wait` timeout is the nearest internal deadline, not a user timeout.

## What each consumer owes

| Consumer | What it owes | Evidence |
|---|---|---|
| Native VM | The whole model: poller, park/resume, readiness registry, transport, liveness, teardown. | The reference implementation. |
| JVM | Oracle parity: a task that waits in a blocking call must let the other tasks of its worker run. | The handoff in `runtime/jvm/src/lox/LoxWorker.java` stops the worker; it needs a park/resume path in the scheduler. |
| Bootstrap interpreter | Matches native except under the implementation-gated one-thread exception: "A blocking call blocks every worker, not only its own." | The exception is already in `spec/04-semantics.md`; a bootstrap-only probe binds it. |
| QBE | Runs the same probes. It links the native runtime, so it inherits the model; check the coroutine mode and its real C frames. | `src/backend/qbe_emitter.cpp`. |
| Spec | The rule, the `Suspends` marker, the table, the probes. | `spec/04-semantics.md`, `spec/05-stdlib.md`. |
| LSP docs | The text that says a read blocks becomes "suspends until". No signature changes. | `src/lsp/stdlib_docs.cpp`. |
| Tree-sitter and editors | No change: there is no syntax change. | — |
| Gates | `tools/check_blocking_calls.py`, the probe runners, `tools/check_jvm_probes.sh`, the bootstrap checks, `tools/diff_runtimes.py`. | Each consumer that does not pass a probe lists it in an expected-failure file. |

## Design sketch

### Task state

`TaskState` gains a wait reason. `WAITING` stays for `receive`; a new `PARKED`
state carries one of `FD_READ`, `FD_WRITE`, `TIMER`, `CHILD`, `RESOLVER`, and a
pending value or pending error. `task.pending()` still reads only the mailbox,
so the pending result cannot live there; a sentinel message would be
observable.

`deliverMessage` wakes a `WAITING` task only. A `PARKED` task receives the
message in its mailbox and stays parked; the message is read when the task
runs again.

### Park and resume

A native calls `Runtime::parkCurrentTask(reason)`. It checks `taskCanSuspend`,
the same predicate `receive` uses; when the chain is not clean it raises
`YieldAcrossNativeError`. `callNative` sees the park and suspends the task
instead of pushing a placeholder result. `resumeTask` supplies the pending
value as the value of the suspend point, as it supplies the mailbox head for
`receive`.

The parked task and its pending value are roots. `pushTempRoot` is
C++-stack-scoped and cannot span a suspension, so the runtime keeps a parked
set that `markRoots` traces, like `m_waitingTasks`.

### Poller

One `Poller` per worker, created lazily on the first wait. It holds an
`epoll` descriptor, an `eventfd` wakeup, a timer heap, and a map from
descriptor to wait record. `waitForWork` waits on the epoll descriptor with
the nearest deadline; `postEnvelope` writes the eventfd, so a send wakes a
worker that is parked in `epoll_wait`. `EPOLL_CLOEXEC`, `EFD_CLOEXEC`, and
`SOCK_CLOEXEC` keep the descriptors out of a child that `run()` forks.

A worker that never makes a blocking call never creates a poller, so it pays
nothing for this feature.

### Readiness registry

A descriptor maps to one or more wait records: the task, the interest
(`read`/`write`), and the call state. Level-triggered; on readiness the task
becomes ready and the record is re-armed unless the call completes. A close
unregisters before the descriptor is closed: `epoll` keeps a reference to the
open file description after `close`, and a reused descriptor number must not
inherit a stale record.

### Transport

`ObjSocket` and `ObjServer` drop the `FILE*` and keep the descriptor. Reads and
writes use raw `read`/`write` on a nonblocking descriptor. `EINTR` retries;
`EAGAIN` parks. `SIGPIPE` stays ignored for the call, as `stream_io.h` does
today.

### Liveness accounting

`WorkerGroup::goIdle` learns the wait reason. A worker parked on a descriptor,
a timer, a child, or a resolver can make progress; it is not idle for the
deadlock count. A worker that waits only in `receive` is. Program end wakes
every poller.

### Teardown

`endTasks` includes the parked set. Abandoning a parked task unregisters its
waits and clears its pending value. Closing a socket with a parked reader or
writer on the same worker unparks it with the defined error and removes the
record.

## Tables and checks

The spec's Blocking calls table stays the source of the set: 22 calls today.
Each section that carries the marker changes it from `**Blocks:** yes` to
`**Suspends:** yes`, and `tools/check_blocking_calls.py` checks the marker
against the table, as it checks `Blocks` today. A row with no marker, or a
marker with no row, fails.

The decision holds the mechanism table above. A new check cross-references it
with the spec table and fails when a call is missing or extra. The table and
the script land with the decision, before any backend node starts, per
`AGENTS.md` rule 2.

The shared probes land with the spec section:

- `sleep` suspends one task;
- `accept`, `read`, `write`, and `connect` each suspend one task;
- `Process.*` and `input()` suspend one task;
- `run()` and `Process.wait()` suspend one task;
- two ready descriptors wake in the defined order;
- an I/O wait is not a deadlock;
- a close while a task is parked follows the defined rule;
- a blocking call inside a native callback follows the decided rule;
- a bootstrap-only probe for the one-thread exception.

Each probe carries `STATUS`, `TIMEOUT`, and `CHECK` lines in the existing
form. The probe runner reads them, so a probe that nothing runs is impossible.

## Rejected alternatives

**Worker per connection.** Closed: a `Socket` is not a message, and workers
cannot share a listener. It also costs one OS thread per connection, which is
the scaling wall this mission removes.

**Sendable sockets.** A different design. It would let a worker hand off an
accepted connection, but a blocking read still stops that worker, so a server
would still need one worker per connection. Cross-heap descriptor ownership is
also a larger change. Keep it as a possible follow-up, not a substitute.

**A poller library in Lox++.** Impossible for the reasons above. This is the
gap, not a library.

**A new async API.** `async`/`await`, futures, or a poller object would grow
the surface and conflict with the "simple" goal. Invisible suspension keeps
the surface unchanged and is the design the spec reserved.

**A separate timer thread per worker.** It would give two wait mechanisms and
would not serve socket readiness. The poller's `epoll_wait` timeout is enough.

**Fair-mode preemption.** Reserved by the concurrency decision and not needed.
It would add work at sites that do not name a suspend point.

**Blocking as a fallback for re-entrant natives.** Tempting for compatibility,
but it breaks the property and the `AGENTS.md` fallback rule. Decide the case
instead of hiding it.

## Compatibility and migration

The rule changes for every row of the Blocking calls table. A program that
relied on a blocking call to serialize its worker sees a different order. The
spec's reservation is the authorization; the decision must quote it.

There is no syntax change, no new global, and no new method. The JVM's
observable output changes on the same-worker probes; its expected-failure file
lists them until the JVM node lands. The bootstrap keeps its one-thread
exception and gets a bootstrap-only probe.

The term "blocking call" stays. It means a call that waits for an outside
party; it no longer means that the call blocks the worker.

## Non-goals

- True parallelism or multi-core scheduling.
- Suspending FFI calls.
- Socket timeouts, `read_some`, UDP, unix sockets, TLS.
- A user-facing poller API.
- Fair mode.
- Changing the error kind or catchability of socket failures.
- Changing `SendError`, `DeadlockError`, or the message rule.

## Residual hard edges

1. **Re-entrant natives.** Raise, block, or support. The decision must pick;
   the probe makes the choice visible.
2. **Name resolution.** `getaddrinfo` cannot be polled. A lazy resolver thread
   per worker, which posts its result through a pipe, is the recommended
   option; a documented blocking exception is the alternative.
3. **JVM scheduler.** The handoff model is the blocker, not the socket call.
   The JVM node must add park/resume without changing the defined FIFO order.
4. **Bootstrap exception.** The one-thread form cannot pass the same-worker
   probes. Name the exception and bind it with a bootstrap-only probe; do not
   let the gate hide it.
5. **Child wait.** `pidfd_open` is the recommended Linux mechanism; a
   `SIGCHLD` self-pipe is the fallback. Signal disposition is process-wide.
6. **`run()`'s stderr drain.** Today it uses a `std::thread`. A suspended
   `run()` must not read a `std::string` that a thread appends to without a
   GC root or a lock. Move the drain into the poller, or keep the thread with
   an explicit handoff.
7. **`fork()` with threads.** `run()` already forks a multi-threaded process.
   A resolver thread adds another thread. The child must only use
   async-signal-safe calls, and every new descriptor must be `CLOEXEC`.
8. **Descriptor reuse.** Unregister before close; `epoll` holds the open file
   description, not the number.
9. **Close while parked.** Define the error and the order; a second task on
   the same worker can close a socket another task waits on.
10. **Memory.** `read()` and `readlines()` return the whole stream, as they do
    today. The buffer is as large as the result the program asked for. Do not
    add a second unbounded queue.

## Open questions for the owner

1. Does the mission cover the whole Blocking calls table, or stop at sockets
   and `sleep`? The title says the whole table; the staging above assumes it.
2. Re-entrant natives: raise `YieldAcrossNativeError`, or keep blocking as a
   named fallback?
3. `connect`: resolver thread, or a documented blocking name-resolution
   exception?
4. Write buffering: keep the string and offset (recommended, no copy), or a
   fixed byte buffer?
5. Socket errors: keep the current fatal path, or make them catchable in this
   mission? (Recommend: keep, separate gap.)
6. `sleep(0)` and a negative duration: return without suspending, or suspend
   for one scheduler turn? (Recommend: return at once.)
7. JVM mechanism: park/resume in the handoff scheduler, or free virtual
   threads behind a one-at-a-time lock? The second changes the defined order.

## References

- `VISION.md`, "What Lox++ is", "What counts as a language gap", "Design
  goals", "The backends", "Distribution".
- `spec/04-semantics.md`, Suspend Points, Workers and Tasks, Ending.
- `spec/05-stdlib.md`, Blocking calls, Socket methods, Server methods,
  Process methods, `task`, `sleep`, `input`, `run`.
- `docs/decisions/concurrency-model-decision.md`, "Scheduler" and "What this
  unblocks and in which order".
- `docs/decisions/expressiveness-roadmap.md`, item 5 and item 7.
- `docs/reference/concurrency_in_bytecode_vms.md`.
- `docs/reference/profiler-concurrency-notes.md`.
- `docs/records/concurrency-model-next-steps.md`.
- `docs/decisions/coroutines.md`.
- `docs/process/multi-agent-playbook.md`, "Mission lifecycle" and "Node
  specification structure".
- #519, #629, #595, #602.
