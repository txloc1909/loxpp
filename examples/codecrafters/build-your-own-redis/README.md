# Build your own Redis (Codecrafters) in Lox++

An implementation of the public
[Build your own Redis](https://github.com/codecrafters-io/build-your-own-redis)
challenge in pure Lox++. No membership is needed: the stage cases are
transcribed from the public `stage_descriptions/*.md` files, and local
regression cases are marked `local:`. A second battery compares the replies
with `redis-server` as an independent oracle.

The work runs one stage group at a time. Tracking issue: #1123.

## Layout

| File | Role |
|---|---|
| `redis.lox` | The server: RESP parser, keyspace, commands. Pure Lox++. |
| `your_program.sh` | Challenge entrypoint: resolves `loxpp`, passes the flags through |
| `tests/run_stages.py` | Transcribed stage cases and `local:` cases, over raw TCP |
| `tests/diff_redis.py` | Differential test against `redis-server` (Redis 7.0 in the dev image) |
| `tests/mutate.py`, `tests/mutations/*.json` | Proof that the stage cases can fail: one change to `redis.lox` per mutation, and the cases it must fail |

## Stage coverage

| Group | Stages | Status |
|---|---|---|
| base | 7 | pass |
| lists | 11 | not started |
| streams | 13 | not started |
| transactions | 11 | not started |
| optimistic-locking | 8 | not started |
| persistence-rdb | 6 | not started |
| replication | 18 | not started |
| persistence-aof | 10 | not started |
| pub-sub | 7 | not started |
| sorted-sets | 8 | not started |
| geospatial | 8 | not started |
| bitmaps | 9 | not started |
| authentication | 8 | not started |

## Design

### Two tasks per connection

All tasks run on the one main worker.

- The **reader** task owns the read side of the socket. It parses RESP with
  `readline` and `read_bytes`, and sends one `Msg.Cmd(args, nbytes)` to the
  session task for each command. At the end of the stream it sends
  `Msg.Closed`.
- The **session** task owns the write side. It takes messages from its
  mailbox with `task.receive()`, runs each command, writes the reply, and
  sends `Msg.Credit` back to the reader (see rule 4 below).

Why two: a session must also take messages from other tasks while its
client sends nothing (a blocked list pop, a published message, a replica
acknowledgement). A task that waits in `readline` cannot receive. Only the
session writes the socket, because two tasks that wait on one side of a
socket raise `BusyError` (`spec/05-stdlib.md`, Socket methods).

When the session ends, it closes the socket and sends `Msg.Stop` to the
reader. The close wakes a reader that waits in a read (its call raises
`SocketClosedError`), and `Stop` wakes a reader that waits for credit. When
the reader ends first, it sends `Msg.Closed`. So neither task outlives the
connection.

### Messages

A List is not a message (`spec/03-types.md`, Messages), also between two
tasks of one worker. So the arguments of a command travel as an enum list,
`Args.Cons(head, tail)` / `Args.Empty`.

`nbytes` counts every byte of the command on the wire. `readline()` drops
the `\n`, so each line adds one. Replication needs this count for its
offsets.

### Keyspace and atomic commands

The keyspace is a global Map that every task of the worker shares. The code
that runs one command has no suspend point: the socket write comes after the
command. So each command is atomic, as in Redis.

### Expiry

Lazy, as in Redis: a read compares the deadline of the key with
`time() * 1000` and removes an expired key. This needs a `time()` finer than
whole seconds, which the native VM has since #1124.

### Faults

An uncaught fault in any task ends the whole program, and a property read on
a value that is not an Instance or an Error is a halting fault, also inside a
`catch` block. So the server throws only `ProtocolError` instances, and every
`catch` reads a fault through `faultKind` and `faultText`, which check the
kind of the value first.

A malformed request gets `-ERR Protocol error: ...`, and the server closes
that connection, as Redis does. A reset by a client is normal and is not
printed.

### Known differences from Redis

- An integer argument has at most 15 digits, so that it is exact in a
  Number. Redis accepts the full 64-bit range.
- An inline command (a line with no `*` header) is split on spaces and tabs.
  Redis also reads quotes and escapes in it.
- A last inline command with no newline before the end of the stream gets a
  reply. Redis sends nothing.
- The server is stricter than the Redis 7.0 of the dev image in two places,
  and replies with a protocol error where Redis does not:
  - two bytes other than CRLF after a bulk string (Redis 7.0 does not check
    them);
  - an array header above 1048576 (Redis waits for the arguments).

### One client must not stop the other clients

All tasks run on one worker, and a command has no suspend point. So one
client could stop every other client by a fault, by a long command, by a
long pipeline, or by input that it never ends. Six rules prevent this. They
come from the referee ruling on PR #1125, and every stage group keeps them.

1. **No halting fault from client input.** See "Faults" above.
2. **Suspend points in the reader.** The reader yields after every 64
   commands (`WINDOW`), after every 64 empty lines, and after every 1024
   elements of one array (`ELEMENTS_PER_YIELD`). The hand-over of the
   arguments (`toArgs` in the reader, `fromArgs` in the session before the
   command starts) also yields every 1024 elements. The native VM looks at
   the network at each task switch, so the other connections then get
   their turn.
3. **The Redis 7.0 limits on sizes.** An inline line has at most 65536 bytes
   (`INLINE_MAX`; above it: `-ERR Protocol error: too big inline request`),
   an array at most 1048576 elements (`ARRAY_MAX`), and a bulk at most
   512 MB (`BULK_MAX`), and one command at most 1 GB in all (`QUERY_MAX`,
   Redis's `client-query-buffer-limit`; the flag
   `--client-query-buffer-limit` sets it). Above the last one the server
   closes the client with no reply, as Redis does. Work for each byte in
   Lox++ code happens only under the inline limit.
4. **A bound on each queue that a client fills.** The reader sends at most
   `WINDOW` (64) commands that the session has not done. After each
   command, the session gives one credit back. A client that sends without
   reading its replies therefore fills at most 64 messages of the session
   mailbox; then the reader stops reading, and TCP makes the client wait.
   When the session ends, it closes the socket and sends `Stop` to the
   reader, so a reader that waits for credit does not outlive its
   connection.
5. **A command is atomic, and its work is linear**, as in the Redis
   documentation of the command. `upper` gives a word longer than any
   keyword back unchanged, and `splitWords` slices.
6. **Each limit and its measured stall** are in the table below.

| Probe (`tests/run_stages.py`, group `fairness`) | Limit | Measured |
|---|---|---|
| P1: 4 clients send PING pipelines for 5 s and read the replies | another client's PING within 0.5 s | 0.006–0.014 s |
| P2: the same, but the clients never read | PING within 0.5 s; server RSS at most 64 MB | 0.006–0.011 s; 7.5 MB |
| P3: an inline line of 65537 bytes | protocol error, connection closed | as required |
| P3: an inline line of 64 MB | PING within 1.0 s | 0.43–0.69 s |
| P4: an array of 1048576 empty bulks | PING within 1.0 s; also within 0.2 s, which holds the yields of `toArgs` and `fromArgs` | 0.016–0.024 s |
| P5: an inline line of 65536 bytes (the limit) | PING within 0.5 s | 0.016–0.020 s |
| P6: 4 clients send only empty lines for 3 s | PING within 0.5 s | 0.002 s |
| Group `query limit`: a command above `--client-query-buffer-limit` | client closed, nothing printed, server serves | as required |

Measured in the dev container with the `release` preset, on 16 CPUs; a
range is the spread between runs on two days. Each
probe has a mutation in `tests/mutations/base.json` that removes its guard.

Known residuals, each with its own issue:

- The spec does not promise that a task switch lets the worker see ended
  socket waits; rule 2 depends on the native VM doing so (#1131).
- One socket read of a stream that keeps arriving does not switch tasks, so
  a 512 MB bulk keeps the other clients waiting while it arrives (#1132).
- `readline` has no length limit, so the server reads a long inline line
  before it can reject it: the 64 MB line of P3 raises the RSS to about
  140 MB for a moment, and most of the P3 stall is that read (#1133).
- A 512 MB `ECHO` took 5.96–10.09 s against 0.19 s in `redis-server`,
  mostly in native String work (#1134).
- The `*` and `$` header lines have no length limit for the same reason as
  the inline line: a 64 MB header line stalls the other clients for about
  0.8–0.9 s, in the native `readline` (#1133).
- The number of connections has no limit (Redis has `maxclients`) (#1144).
- One `SET` with 1048573 options stalls the other clients for 0.94–0.98 s
  (the command takes 3.9 s in all, with yields during the hand-over of the
  arguments). The stall is the command itself, which rule 5 allows; it is
  close to the 1 s of rule 6, and no probe holds it.

## Run

```sh
./examples/codecrafters/build-your-own-redis/your_program.sh --port 6380
redis-cli -p 6380 PING
python3 examples/codecrafters/build-your-own-redis/tests/run_stages.py
python3 examples/codecrafters/build-your-own-redis/tests/diff_redis.py
python3 examples/codecrafters/build-your-own-redis/tests/mutate.py build/loxpp \
    examples/codecrafters/build-your-own-redis/redis.lox \
    examples/codecrafters/build-your-own-redis/tests/run_stages.py \
    examples/codecrafters/build-your-own-redis/tests/mutations/base.json
```

The server binds `0.0.0.0` on port 6379 by default. Build `loxpp` with the
`release` preset: the `debug` preset prints a trace on standard output.
Binary resolution: `build/loxpp` beside this checkout first, then `loxpp` on
`PATH`.
