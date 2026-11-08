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
| streams | 13 | pass |
| transactions | 11 | pass |
| optimistic-locking | 8 | pass |
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

Two commands have a step before them: the session parses the arguments of
`XRANGE` and `XREAD` (`PARSERS`) before the command runs, and yields every
1024 options or IDs, because one command can hold about 524287 of them. The
parse reads no key, so the command stays atomic.

### Lists and BLPOP

A list key holds a deque over two Lox++ Lists (`class Deque`). `LPUSH`
adds to the first List, which holds the left part from right to left;
`RPUSH` appends to the second. A pop moves a count of live elements in the
first List, or a head index in the second, so a pop of k elements costs one
slice of k. When the free slots outnumber the elements of a List, one slice
drops them. So no push copies the list, and each push and pop costs a
constant time on average. An empty list is never
stored: the key goes away with its last element, as in Redis.

`BLPOP` puts a `Waiter` for the session in a first-in first-out queue for
each of its keys, then waits in `task.receive_where_within` (or
`task.receive_where` for no time limit) for `Msg.Served` or `Msg.Closed`.
A push serves the oldest waiters at the end of the push command: it pops
one element for each and sends it in `Msg.Served(key, value)`. The pusher
does the pop, so the element is not given to two clients, and the waiter
gets it with no other command between the push and the pop. The commands
that the client sends during the wait stay in the mailbox and run after it,
as Redis queues the commands of a blocked client.

A key with one waiter maps to the `Waiter` itself; a `WaitQueue` comes with
the second waiter. So a `BLPOP` on many keys makes one object for each key
in its one step. When the waiter leaves, it yields between steps of 1024
keys: it is no longer active, so no pusher serves it meanwhile.

A pusher can serve a waiter whose client has gone, or whose time limit has
passed, before the session of the waiter runs. The element is then in the
mailbox of the session. The session finds it there (`w.servedBy` is set),
and puts it back at the head of its key for the next waiter. It does the
same when the write of the reply fails.

### Streams

A stream key holds a `Stream`: three Lists of the same length, the two
parts of each entry ID and the fields of each entry. `XADD` keeps the fields
as one slice of its arguments, so it costs no more for an entry with more
fields. The IDs only grow, so `XRANGE` and `XREAD` find their first entry
with a binary search, and an ID at or above the top costs one comparison. A
stream only grows at its end, so the reply keeps only the stream and the
bounds of its entries (`class Segments`); the session reads the entries
when it writes them.

`XREAD BLOCK` waits as `BLPOP` does (`waitForElement`), in a second map of
queues, `readers`. At the end of `XADD`, `wakeReaders` takes each reader of
the key that now has entries to read, puts its reply in its `Waiter` and
sends `Msg.Ready`. So the reply holds the entries up to this `XADD` and only
this stream, as in Redis. For a key that the reader named with `$`, the
wait stores the `Stream` that the key held, with no new object. Each `XADD`
on that same `Stream` wakes the reader at once, so its reply is the entry of
that `XADD`. When the key holds another `Stream` (the first one expired or
was replaced), the reader compares with the top of the first one, which
nothing appends to any more. A reader whose ID is still above the top joins the
queue again. `XREAD` takes nothing from the stream, so a reply that the
client cannot get is dropped.

### Transactions

After `MULTI`, the session keeps a `Transaction` in its `Conn` and queues
each command (`inTransaction`). It checks the name and the argument count
of the command first, as Redis does (`ARITY`): a rejected command makes
`EXEC` give `EXECABORT`. The parse step of `XRANGE` and `XREAD` runs when the
command is queued, with yields, because it reads no key.

`EXEC` runs the queue in one step (`execQueued`): each command gets its
parse, so no command yields, and no other client runs between two commands
of a transaction. A command that would wait (`BLPOP`, `XREAD BLOCK`) gives a
null array at once, as in Redis. In the step, a push or an `XADD` only notes
that its key is ready (`listReady`, `streamReady`), and `serveReady` serves
the waiting clients one time after the last command, from the state at that
time, as Redis does. So a waiter acts before or after a transaction, never
in it, and an `EXEC` of many `XADD` passes over the readers of a key once.

`GET`, `ECHO`, `PING` with a message, `LPOP` of one element and `SET ... GET`
give their value as a `Bulk`, with no copy, so `EXEC` holds only references.
The session writes the replies after the step, in steps of its own
(`writeReplies`), and writes a long value alone (`Output.addBulk`).

A transaction holds at most 65536 commands, 1048576 arguments and the query
limit of bytes (`--client-query-buffer-limit`, 1 GB by default). A command
over a bound gets an error and makes `EXEC` give `EXECABORT`.

In one `EXEC`, the replies of `LRANGE` and `LPOP` with a count may copy at
most 8388608 list elements in all (`spendElements`), because `EXEC` keeps
every reply until its step ends. A command over that budget gets an error
and does nothing, so the transaction stays atomic.

The probes T1-T7 measure large steps of `EXEC` under these bounds. They do
not prove a bound on every step: the cost of a command depends on the data
it reads, and T7 shows one case that grows with the number of readers.

### Optimistic locking

`WATCH` puts the client in `watchers` for each key: the `Conn` itself for
one client, a Map of client ids from the second one. Each command that
writes the keyspace calls `touch(key)`, also a write that keeps the value,
and a lazy expiry. `touch` marks each client that watches the key
(`watchDirty`). `EXEC` gives a null array when the mark is set, or when a
watched key that had an expiry at `WATCH` has expired since, also with no
read, as Redis 7.0 does. `EXECABORT` comes first.

`EXEC`, `DISCARD`, `UNWATCH` and the end of the connection forget the
watched keys (`unwatchAll`). The client watches nothing at once, and the
old entries go after, with a yield every 1024 keys. So it runs in the body
of `sessionLoop`, not in a deferred call: a deferred call cannot suspend.
A client may watch at most 1048576 keys at a time (rule 4).

### Large replies

`LRANGE` and `LPOP` with a count take their elements from the keyspace with
at most one slice of each List of the deque. The session then encodes and writes the reply
in steps of 1024 elements and yields between the steps (`class Bulks`,
`writeBulks`). The command stays atomic, because its keyspace work is the
one slice; the other clients go on while a long reply is written.

A stream reply (`class Segments`) is written the same way: `class Output`
writes at most 1024 parts and about 1 MB in each step, and yields between
the steps. A part of 1 MB or more goes to the socket alone, and a long field
goes as its header, the field and CRLF, so no step copies it.

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

- An integer argument, and each part of a stream ID, has at most 15
  digits, so that it is exact in a Number. Redis accepts the full 64-bit
  range.
- `INCR` stops at 999999999999999 (15 digits) with "increment or decrement
  would overflow". Redis goes on to 9223372036854775807.
- A client may watch at most 1048576 keys at a time. Redis has no such
  bound.
- A transaction holds at most 65536 commands, 1048576 arguments and the
  query limit of bytes, and its `LRANGE` and `LPOP` replies copy at most
  8388608 list elements. Redis has no such bound.
- `XADD` takes no options (`NOMKSTREAM`, `MAXLEN`, `MINID`, `LIMIT`): the
  word in the ID place must be an ID. `XRANGE` takes no exclusive bounds
  (`(1-1`).
- A stream ID part is digits with no leading zero and no sign. Redis also
  accepts `007-1` and `+8-1`.
- Some errors come in another order when a command has two of them: `XADD`
  on a key of another kind with a bad ID, `XRANGE` with a bad `COUNT` and a
  bad ID (here the `COUNT` error comes first), and `XREAD` with a key of
  another kind before a bad ID (here the ID error comes first).
- An `XREAD BLOCK` that names one key twice waits for entries above the
  second ID of that key.
- An inline command (a line with no `*` header) is split on spaces and tabs.
  Redis also reads quotes and escapes in it.
- A last inline command with no newline before the end of the stream gets a
  reply. Redis sends nothing.
- The server is stricter than the Redis 7.0 of the dev image in two places,
  and replies with a protocol error where Redis does not:
  - two bytes other than CRLF after a bulk string (Redis 7.0 does not check
    them);
  - an array header above 1048576 (Redis waits for the arguments).
- A client that sends more than 960 commands during one `BLPOP` or
  `XREAD BLOCK` is closed.
  Redis keeps them in its query buffer, up to `client-query-buffer-limit`.
- A malformed request during `BLPOP` or `XREAD BLOCK` closes the client at
  once, with no reply, and the `BLPOP` takes no element. Redis keeps the client blocked,
  serves it, then sends the error and closes it.
- The `BLPOP` time-out is a decimal number. Redis also accepts exponents
  (`1e3`) and hexadecimal (`0x10`), gives other error text for `inf`, gives
  "timeout is negative" for a value that overflows its milliseconds, and
  reads a small negative value such as `-0.0001` as 0 (no limit). This
  server gives an error for the first two forms, waits with no limit for a
  value whose milliseconds overflow, and gives "timeout is negative" for
  every negative value.

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
| P8: the first `LPUSH` on a list of 4194292 elements that `RPUSH` built | PING within 0.1 s (when the push copies the list: 0.32 s) | 0.001 s |
| P9: `BLPOP` on 1048574 keys, the most that one command can name | PING within 1.0 s while it joins the queue of each key in one step (before the change: 1.51 s; redis-server 7.0: 0.62 s) | 0.55–0.63 s |
| P9: an `RPUSH` serves it, and it leaves the other 1048573 queues | PING within 0.2 s (without the yield of `leaveQueues`: 0.48 s) | 0.002 s |
| Group `lists`: 150 waiters that reset the connection just before a push | each element stays in its list | as required (before the change: 249–283 of 300 lost) |
| Group `lists`: the same, with a second waiter behind each one | each element goes to the second waiter | as required |
| P10 (group `lists`): `LPOP k 4194291` of a list of 8388584 elements that `LPUSH` built | PING within 0.14 s (when the pop removes the elements one by one: 0.19–0.23 s) | 0.07–0.08 s |
| Not in the suite: `RPUSH` / `LPUSH` of 1048573 elements | PING within 1.0 s | 0.10–0.13 s / 0.17–0.19 s |
| Not in the suite: `LPOP k 2000000` of a list of 2097146 elements, from the right part / `LPOP k 1048000` from the left part | PING within 1.0 s | 0.08–0.09 s / 0.06–0.07 s |
| Not in the suite: `LRANGE 0 -1` of a list of 1049146 elements that `LPUSH` built | PING within 0.2 s | 0.06–0.08 s |
| S1 (group `streams`): `XADD` with 524286 field pairs | PING within 0.2 s (when `XADD` encodes the entry: 0.71 s) | 0.006–0.046 s |
| S2: `XRANGE` of that entry | every field; PING within 0.15 s (without the yield of `Output.add`: 0.27 s) | as required; 0.056–0.067 s |
| S3: `XREAD` of 524286 missing streams | PING within 0.5 s (when the IDs are parsed in the step of the command: 0.93 s) | 0.23–0.27 s |
| S4: `XREAD BLOCK 1000` on 524286 streams | PING within 1.0 s, while it joins the queue of each stream in one step | 0.45–0.49 s |
| S5 / S6 (on a server of their own): `XREAD` / `XRANGE` with 524000 `COUNT` options | PING within 0.2 s (without the yield of the parse: about 1.4 s) | 0.010 s / 0.04–0.09 s |
| S7: `XRANGE` of 64 entries of almost 1 MB | PING within 0.2 s (without the byte bound of `Output`: 1.38 s) | 0.08–0.09 s |
| S8 (on a new server): `XREAD` of 131072 streams that have entries | every stream; PING within 0.45 s (when the command copies the entries of each stream: 0.61–0.72 s) | 0.18–0.22 s |
| S9: `XRANGE` of one field of 64 MB | the field; PING within 0.25 s (when the field is copied into its bulk: 0.37 s) | 0.10–0.11 s |
| T1 (group `transactions`): `EXEC` of 65535 `RPUSH` of 14 elements, the most commands and arguments that a transaction holds | PING within 0.6 s | 0.28–0.30 s |
| T3 (on a server of their own, as T4 and T5): `EXEC` of 10000 `XADD` with 200 readers that wait above the top | PING within 0.5 s (when each `XADD` wakes the readers at once: 4.28 s) | 0.07 s |
| T4: `EXEC` of 8 `GET` of a 64 MB value | every byte; PING within 0.3 s (when `GET` copies its value: 1.88 s) | 0.002–0.04 s |
| T5: `EXEC` of 65535 `GET` of a 1 KB value | every byte; PING within 0.5 s (when the replies are written in one step: 1.94 s) | 0.10–0.11 s |
| W1 (group `optimistic-locking`, on a server of its own): `WATCH` of 1048575 keys | PING within 1.0 s, while it joins all keys in one step | 0.76–0.80 s |
| W2: the `EXEC` after it, which forgets the 1048575 keys | PING within 0.15 s (without the yield of `unwatchAll`: 0.27–0.31 s) | 0.010–0.015 s |
| T6: `EXEC` of 200 `LRANGE` of a list of 1048573 elements, then two `LPOP` with a count | 8 replies, 192 refused, the LPOP over the budget refused; PING within 0.5 s (with no budget: 0.96 s at 200 and 1.6 GB, and no bound in memory) | 0.07–0.09 s |
| T7: `EXEC` of 65535 `XADD` while 5 readers wait above the top on each key | PING within 1.0 s; the pass checks each reader on each key, as in Redis | 0.60–0.63 s. At 50 readers: 1.91 s, of which up to about 0.47 s is a GC pause (redis-server 7.0: 1.32 s). This is over the 1 s of rule 6 in Lox++ code; the owner accepted it, and #1169 is the follow-up. |
| T2: `EXEC` of one `XREAD` of 524286 streams | PING within 0.5 s (when the parse runs in the step of `EXEC`: 0.84 s) | 0.12–0.15 s |
| Group `transactions`: `EXEC` of 20000 `INCR` while another client reads the key | the other client sees the value before or after, never in between | as required |
| Not in the suite: `EXEC` of 65536 `INCR` / `SET` / `GET`, of 4 `LPUSH` of 262142 elements, of one `BLPOP` on 1048574 keys | PING within 1.0 s | 0.18 s / 0.12 s / 0.09 s / 0.16–0.18 s / 0.25 s |
| Not in the suite: `XREAD` of 524286 streams that have entries | PING within 1.0 s (redis-server 7.0: 0.76 s) | 0.72–0.78 s |
| Not in the suite: `XREAD BLOCK 0` with `$` on 524286 streams, by a first and a second client | PING within 1.0 s in Lox++ code; a GC pause is native (#1165). No probe holds the 1.0 s of the Lox++ part, and its margin is small. | first 0.58–0.63 s; second 1.70–1.76 s: one GC pause of 0.76–0.79 s, and 0.92–0.95 s of Lox++ code (redis-server 7.0: 0.56 s and 0.49 s) |
| Not in the suite: `XRANGE` of one field of 200 MB | PING within 1.0 s | 0.009–0.023 s |
| Group `streams`: `XREAD BLOCK 0` and 100 more commands, then the client closes | the tasks of the connection end | as required |
| Not in the suite: `XRANGE` / `XREAD` of 200000 entries | PING within 1.0 s | 0.07–0.13 s / 0.07–0.21 s |
| Group `query limit`: a command above `--client-query-buffer-limit`, as an array or inline | client closed, nothing printed, server serves | as required |
| Not in the suite (it sends 512 MB): `SET k` with a bulk of 512 MB, the bulk limit | over 1 s is allowed only in a native call | 1.30–1.50 s, nearly all in the native `read_bytes` after the last byte arrives (#1132, #1134); RSS 530 MB |
| Not in the suite (it sends 1.2 GB): a command of four 300 MB bulks at the default 1 GB query limit | client closed; PING within 1.0 s | 0.75 s, in the native `read_bytes` of the first three bulks (#1132) |

Measured in the dev container with the `release` preset, on 16 CPUs; a
range is the spread between runs on different days and hosts. Each
probe has a mutation in `tests/mutations/base.json`,
`tests/mutations/lists.json`, `tests/mutations/streams.json` or
`tests/mutations/transactions.json` or
`tests/mutations/optimistic-locking.json` that removes
its guard (S4, T1, T7 and W1 hold rule 6 only: no single change removes their
guard).

A large heap brings a second kind of stall: the native VM stops the worker
for each full collection, and the pause grows with the live heap. With
524286 streams, one pause was 0.76–0.79 s: 9.6 million objects at the
start of the pause, 5.8 million live after it (#1165). It can come in any command, so a probe on such a heap measures it
too; S8 uses 131072 streams so that it measures the command. Two guards have no
probe, because no client timing forces the order of events that they
handle:
- the give-back after a failed write of a `BLPOP` reply: the write fails
  only when the reset arrives after the session gets its element and
  before it writes, inside one step of the worker;
- the element that a pusher hands to a waiter after its time limit ended
  but before its session ran: the session must reply with it, not with a
  null array.

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
# the same with tests/mutations/lists.json, streams.json, transactions.json
# and optimistic-locking.json
```

The server binds `0.0.0.0` on port 6379 by default. Build `loxpp` with the
`release` preset: the `debug` preset prints a trace on standard output.
Binary resolution: `build/loxpp` beside this checkout first, then `loxpp` on
`PATH`.
