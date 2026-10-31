# Waiting for the first of several events

**Status:** proposal. Waits for the owner to choose an option. It covers #632 and #937.

## Question

Two open issues ask for a task to wait on more than one thing:

- #632: a task that holds two channel ends gets the items of one channel and
  the credits of the other in one mailbox. `task.receive()` takes the next
  message, so the task cannot ask for the next message of one channel.
- #937: a socket call waits with no limit. A limit means "the call ends, or
  the time passes, whichever is first".

Both are "wait for the first of several events". The question is how much of
that the language must add, and how much a library can do. The test is the
litmus test of `docs/decisions/expressiveness-roadmap.md`: a feature is a
gap only if a program cannot build it from the primitives that exist.

## What a program can build today

The prototypes below were run on the native VM at `main`.

**A receive with a limit.** A timer task sleeps, then sends a tick that names
the call. The receiving task drops the ticks of earlier calls.

```
enum Wait { Tick(id) }
// receiveWithin(seconds): spawn a task that runs sleep(seconds) and then
// task.send(me, Wait.Tick(id)); then loop on task.receive(), return a message
// that is not a Tick, return nil on the Tick with this id, drop other Ticks.
```

Result: nothing arrives, so the call returns `nil` after the limit. A message
that arrives in time is returned. A tick from an earlier call is dropped.

**A selective receive.** A stash holds the messages that the caller did not
ask for. `receiveIf(want)` looks in the stash, then loops on `task.receive()`
and stashes each message that `want` rejects. Result: with `A(1)` then `B(2)`
in the mailbox, `receiveIf` for `B` returns `B(2)` and `A(1)` stays in the
stash. This is the library that #632 names.

**A limit on a read, write or accept.** A timer task calls `close()` on the
Socket. The spec says `close()` wakes the waiting task, and its call raises
`SocketClosedError`. Result: a `readline()` on a peer that never writes ends
after the limit with `SocketClosedError`.

## What a program cannot build today

1. **A limit on `connect`.** No Socket exists before `connect` returns, so no
   task can close it. A `connect` to a host that never answers parks the task
   for about two minutes (#937).
2. **A limit that keeps the Socket usable.** The `close()` method ends the
   Socket. A program that wants "wait 5 seconds for a line, then try again" on
   the same Socket cannot do it, because a call that is in flight cannot be
   cancelled by itself.
3. **A wait on a Socket and a mailbox together.** One task cannot park on both.
   A program can use a reader task that sends each line to the mailbox of the
   main task, but the reader task is in the same position as point 2 when the
   main task stops caring.

Point 1 is a real gap. Points 2 and 3 have a workaround that is correct but
heavy: one reader task for each Socket.

## Options

**A. A library, and a limit on `connect`.** Ship `receiveWithin` and
`receiveIf` as a library in `examples/`, as `examples/credit_channel.lox`
does, with a probe. Change the spec in one place only: `connect(host, port)`
takes an optional third argument, a limit in seconds. When the limit passes,
the call raises a catchable `NetworkError`. No new primitive. Points 2 and 3
stay as they are.

**B. A receive with a limit as a primitive.** `task.receive(seconds)` returns
`nil` when the limit passes. It saves one task for each call, and it removes
the stale tick. It still needs the limit on `connect`, and it does not help
points 2 and 3.

**C. A `select` over mailboxes, sockets and timers.** It closes points 2 and
3. It is the largest change: the poller and the mailbox wake-up are separate
paths today, and `select` joins them. The decision for the concurrency model
keeps spawn, send and receive as the whole primitive, so C needs a decision
that changes that line. The bootstrap interpreter cannot implement it.

## Recommendation

Take A. The litmus test names `connect` as the one real gap. A selective
receive and a receive with a limit are libraries. The cost of A is one
spawned task for each limited call, and a heavier form of points 2 and 3.
Move to B or C only when a program shows that this cost is the problem.

## If the owner takes A

- The library: a file in `examples/` with the copies in
  `test/translation-probes/`, as `tools/check_credit_channel_copies.py`
  checks for the credit channel.
- The `connect` limit is a language change. The consumers are the native VM,
  the JVM backend, the QBE backend, the bootstrap interpreter, and the
  built-in signature table of the resolver and the LSP. A mission plan lists
  each of them, and a decision under `docs/decisions/` comes first.
- Open question for the decision: does the limit cover the name lookup too?
  #946 says the worker blocks while it resolves a host name, so the limit
  cannot cover it until #946 is done.
