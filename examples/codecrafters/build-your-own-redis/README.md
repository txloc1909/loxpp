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
| lists | 11 | pass |
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
that runs one command on the keyspace has no suspend point: the session
takes the arguments of the command out of its message first (this step
yields, see rule 2), and the socket write comes after the command. So each
command is atomic, as in Redis.

### Lists and BLPOP

A list key holds a deque over a Lox++ List (`class Deque`): a pop from the
left moves a head index, and a push to the left fills free slots before it,
so each push and pop costs a constant time on average. An empty list is
never stored: the key goes away with its last element, as in Redis.

`BLPOP` puts a `Waiter` for the session in a first-in first-out queue for
each of its keys, then waits in `task.receive_where_within` (or
`task.receive_where` for no time limit) for `Msg.Served` or `Msg.Closed`.
A push serves the oldest waiters at the end of the push command: it pops
one element for each and sends it in `Msg.Served(key, value)`. The pusher
does the pop, so the element is not given to two clients, and the waiter
gets it with no other command between the push and the pop. The commands
that the client sends during the wait stay in the mailbox and run after it,
as Redis queues the commands of a blocked client.

### Large replies

`LRANGE` and `LPOP` with a count take their elements from the keyspace with
one slice, into a new List. The session then encodes and writes the reply
in steps of 1024 elements and yields between the steps (`class Bulks`,
`writeBulks`). The command stays atomic, because its keyspace work is the
one slice; the other clients go on while a long reply is written.

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
- A client that sends more than 960 commands during one `BLPOP` is closed.
  Redis keeps them in its query buffer, up to `client-query-buffer-limit`.

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
   While the session waits in `BLPOP`, it gives no credit, but the reader
   must read on to see the end of the stream: a wait with no time limit
   would otherwise outlive a client that has gone. So during a wait the
   reader may send `HOLD` (960) commands past its credit, and the mailbox
   holds at most 1024 messages. The session sets `conn.blocked` and sends
   `Msg.Blocked`, which wakes a reader that already waits for credit. A
   client that sends more than `HOLD` commands during one wait is closed.
5. **A command is atomic, and its work is linear** (atomic: no suspend point
   between the start of its keyspace work and its reply), as in the Redis
   documentation of the command. `upper` gives a word longer than any
   keyword back unchanged, and `splitWords` slices.
6. **Each limit and its measured stall** are in the table below.

| Probe (`tests/run_stages.py`, group `fairness`) | Limit | Measured |
|---|---|---|
| P1: 4 clients send PING pipelines for 5 s and read the replies | another client's PING within 0.5 s | 0.006–0.014 s |
| P2: the same, but the clients never read | PING within 0.5 s; server RSS at most 64 MB | 0.006–0.011 s; 7.5 MB |
| P2: one client that never reads sends until the server stops taking bytes | at most 32 MB taken | 7.2 MB (the socket buffers) |
| P3: an inline line of 65537 bytes | protocol error, connection closed | as required |
| P3: an inline line of 64 MB | PING within 1.0 s | 0.43–0.69 s |
| P4: an array of 1048576 empty bulks | PING within 1.0 s | 0.016–0.024 s |
| P4: four such arrays at the same time | PING within 0.2 s, which holds the yield of `toArgs` and the yield of `fromArgs` one by one (without one of them: 0.38–0.68 s) | 0.054–0.074 s |
| P5: an inline line of 65536 bytes (the limit) | PING within 0.5 s | 0.016–0.020 s |
| P6: 4 clients send only empty lines for 3 s | PING within 0.5 s | 0.002 s |
| P7: `LRANGE 0 -1` of a list of 2097146 elements | every byte of the reply; PING within 0.2 s (without the yield of `writeBulks`: about 1.1 s) | as required; 0.004–0.005 s |
| Group `lists`: `BLPOP k 0` and 100 more commands, then the client closes (also with 1100 keys, where the reader already waits for credit) | the tasks of the connection end (`connected_clients` back) | as required |
| Group `lists`: `BLPOP k 0` and 2000 more commands | the client is closed, its tasks end | as required |
| Group `lists`: `BLPOP k 5` and 900 more commands, then a push | the 900 replies come after the `BLPOP` reply, in order | as required |
| Not in the suite: `RPUSH` / `LPUSH` of 1048573 elements | PING within 1.0 s | 0.10–0.11 s / 0.25 s |
| Not in the suite: `LPOP k 2000000` of a list of 2097146 elements | PING within 1.0 s | 0.05 s |
| Group `query limit`: a command above `--client-query-buffer-limit`, as an array or inline | client closed, nothing printed, server serves | as required |
| Not in the suite (it sends 512 MB): `SET k` with a bulk of 512 MB, the bulk limit | over 1 s is allowed only in a native call | 1.30–1.50 s, nearly all in the native `read_bytes` after the last byte arrives (#1132, #1134); RSS 530 MB |
| Not in the suite (it sends 1.2 GB): a command of four 300 MB bulks at the default 1 GB query limit | client closed; PING within 1.0 s | 0.75 s, in the native `read_bytes` of the first three bulks (#1132) |

Measured in the dev container with the `release` preset, on 16 CPUs; a
range is the spread between runs on different days and hosts. Each
probe has a mutation in `tests/mutations/base.json` or
`tests/mutations/lists.json` that removes its guard.

Known residuals, each with its own issue:

- The spec does not promise that a task switch lets the worker see ended
  socket waits; rule 2 depends on the native VM doing so (#1131).
- One socket read of a stream that keeps arriving does not switch tasks, so
  a 512 MB bulk keeps the other clients waiting while it arrives and while
  the native read makes its String: 1.30–1.50 s in all (#1132, #1134). The
  reader reads the bulk and its CRLF in two calls, so it does not copy the
  bulk with a slice (a slice of 512 MB takes 0.79 s).
- `readline` has no length limit, so the server reads a long inline line
  before it can reject it: the 64 MB line of P3 raises the RSS to about
  140 MB for a moment, and most of the P3 stall is that read (#1133).
- A 512 MB `ECHO` took 5.96–10.09 s against 0.19 s in `redis-server`,
  mostly in native String work (#1134).
- The `*` and `$` header lines have no length limit for the same reason as
  the inline line: a 64 MB header line stalls the other clients for about
  0.8–0.9 s, in the native `readline` (#1133).
- The number of connections has no limit (Redis has `maxclients`) (#1144).
- One `SET` with 1048573 options stalls the other clients for 0.70–0.98 s
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
# the same with tests/mutations/lists.json for the lists group
```

The server binds `0.0.0.0` on port 6379 by default. Build `loxpp` with the
`release` preset: the `debug` preset prints a trace on standard output.
Binary resolution: `build/loxpp` beside this checkout first, then `loxpp` on
`PATH`.
