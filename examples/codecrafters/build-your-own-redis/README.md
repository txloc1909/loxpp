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
  mailbox with `task.receive()`, runs each command, and writes the reply.

Why two: a session must also take messages from other tasks while its
client sends nothing (a blocked list pop, a published message, a replica
acknowledgement). A task that waits in `readline` cannot receive. Only the
session writes the socket, because two tasks that wait on one side of a
socket raise `BusyError` (`spec/05-stdlib.md`, Socket methods).

When the session ends, its deferred `close()` wakes the reader, whose
`readline` then raises `SocketClosedError`. When the reader ends first, it
sends `Msg.Closed`.

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

### Commands do not stop other clients for long

A command has no suspend point, so while it runs, no other client gets a
reply. So the work of a command must stay linear in its input. A helper that
grows a String one byte at a time takes time that grows with the square of
the length, and one long argument then stops every client for minutes.
`upper` gives a word longer than any keyword back unchanged, and
`splitWords` slices. The `local:` cases with a 256 KB name and a 256 KB
inline word hold this.

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
