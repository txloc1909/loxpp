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
| persistence-rdb | 6 | pass |
| replication | 18 | pass |
| persistence-aof | 10 | pass |
| pub-sub | 7 | pass |
| sorted-sets | 8 | pass |
| geospatial | 8 | pass |
| bitmaps | 9 | pass |
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
one client, a Map of client ids from the second one. It yields every 1024
keys: a write while it runs is concurrent with it, so a write of a key that
it has not joined yet comes before it, and no write after its reply is
missed. Each command that writes the keyspace calls `touch(key)`, also a
write that keeps the value, and a lazy expiry. `touch` marks each client
that watches the key (`watchDirty`), and drops the marked clients from
`watchers`: a marked client stays marked until its next `EXEC`, so each
later write of the key costs one Map read. On a replica, the `DEL` from the
master of a key that had expired at `WATCH` marks no client, and `touch`
keeps that client in `watchers`. `EXEC` gives a null array when the mark is set, or when a
watched key that had an expiry at `WATCH` has expired since, also with no
read, as Redis 7.0 does. `EXECABORT` comes first.

`EXEC`, `DISCARD`, `UNWATCH` and the end of the connection forget the
watched keys (`unwatchAll`). The client watches nothing at once, and the
old entries go after, with a yield every 1024 keys. So it runs in the body
of `sessionLoop`, not in a deferred call: a deferred call cannot suspend.
A client may watch at most 1048576 keys at a time (rule 4).

### RDB persistence

At start, before it listens, the server loads `<--dir>/<--dbfilename>`
(`loadRdb`). It reads the string keys of database 0: the length forms of
6, 14, 32 and 64 bits, integer strings of 1, 2 and 4 bytes, LZF strings,
expiries in seconds and in milliseconds (a negative one has passed, as
Redis reads it), and it skips the LRU and LFU fields. A key that has
expired already is not loaded, as Redis does when it loads as a master. A missing file is
an empty keyspace. A file that the server cannot load stops the start with
a message, as in Redis. The load has no yield: no client is connected then.

`KEYS` takes the keys and the entries of the keyspace with two native calls
in its step, and the time of that step. The session filters them with the
glob pattern and writes them in steps (`writeKeys`): the reply is the keys
that were live at that time. In `EXEC`, each `KEYS` spends twice the size
of the keyspace from the element budget. The glob match (`globMatch`) has
the rules of Redis 7.0 (`stringmatchlen`), also `[a-]` and an empty key. It
counts each byte of the pattern that it reads as a step, in a set too, so
it takes at most len(key) x len(pattern) steps, and it yields every 65536
steps.

### AOF persistence

With `--appendonly yes`, at start, before it listens, the server makes
`<dir>/<appenddirname>` if it is not there (`startAof`). When the manifest
`<appendfilename>.manifest` is there, the server replays the files that
it names: the base file (type `b`, an RDB file or an AOF file), then each
incremental file (type `i`) in its order. It skips a history file (type
`h`). The writes go to the last incremental file. With no manifest, the
server makes an empty `<appendfilename>.1.incr.aof` and a manifest with
the line `file <appendfilename>.1.incr.aof seq 1 type i`. With appendonly
yes, the RDB file is not loaded, as in Redis. `CONFIG GET dir` gives the
directory where the server started when `--dir` is not given.

The replay parses the RESP commands of each file and runs each as a client
that gets no replies. It runs a `MULTI` ... `EXEC` as one step. It accepts
`SELECT 0` and the annotation lines (`#...`) of Redis, also one that ends
the file with no newline, and it goes on after an `EXEC` with no `MULTI`,
as Redis does. As in Redis, no key expires while the files load, and an
RDB base keeps a key that has expired for the commands after it. A last file that ends inside a command, or
inside `MULTI`, is cut back to its last whole command (as Redis does with
`aof-load-truncated yes`) and the server starts. A file that is not RESP,
an unknown command, `SELECT` of another database, a manifest line that is
not `file <name> seq <n> type <t>` (with a seq from 1; the keys have no
case), an empty manifest, a blank line, two base files, incremental files
out of the order of their seq (an equal seq too), a name with a `/`, and a
missing file stop the start with a message. A manifest with comments only
names no file: the server makes a first incremental file. Redis does the
same for each of these forms, except some forms that Redis itself does not
write, which the known differences list (`seq 1x`, a base line with
`seq -1` and `type incr` among them). The values of `--appendonly` and `--appendfsync` have no case.

The AOF is one more reader of the replication log: each write appends its
effective form in its own step (`replicate`), and a writer task (`writeAof`)
appends the entries to the file in steps, as a replica session writes them
to its socket. A served `BLPOP` goes as `LPOP`, `XADD *` with its ID,
an `EXEC` with more than one write in `MULTI` and `EXEC` (with one write,
the write alone, as Redis 7.0 writes it), and a lazy expiry as `DEL`.
`SET` with `EX`, `PX` or `EXAT` goes with `PXAT` and the absolute time, as
Redis 7.0 writes it, so a replay later does not give the key a new life;
the replicas get the same form. A time of 16 digits or more goes as it
came, because `parseInteger` reads 15 digits; such a key lives for tens of
thousands of years either way. The `GETACK` of `WAIT` does not go to the file.
The writer closes the file after each batch: the standard library has no
flush and no fsync, and `close()` is what makes the bytes visible to other
processes (#1190).

A command that wrote replies only when the file has its entries
(`aofSync`), whatever `appendfsync` says. So an acknowledged write is in
the file, and each client has at most one command in the log that the
file has not got (rule 4). A command that wrote nothing does not wait,
except a `BLPOP` that times out and an `XREAD BLOCK` that is served: they
wait for the writes that other clients made while they were blocked. The
log drops the entries that every reader has got when they hold 1024
entries or 1 MB of cost (`trimReplLog`), so a long run of large writes
does not keep them (AOF3). A
write to the file that fails stops the server with a message, as Redis
does with `appendfsync always`.

### Sorted sets

A sorted set is a skip list with spans, as in Redis, and a Map of member
to score (`ZSet`). The order is by score, then by member. `ZADD`, `ZREM`
and `ZRANK` take O(log N) steps, and `ZRANGE` O(log N + M); `ZRANGE` takes
its members in its step (about 0.2 microseconds for each), and the session
writes them in steps. The level of a node comes from the high bits of a
linear congruential generator, with a chance of 1/4 for each level, because
the standard library has no random numbers. `ZADD` reads its scores before
it runs (`parseZadd`, with yields), so a bad score changes nothing; a
replica parses a `ZADD` of its master with yields too. A score has the
forms of `strtod` that Redis takes, and a value out of range or one that
rounds to zero is refused, as in Redis. The parse is linear, and it checks
the range from the first digit that is not zero and the exponent before it
makes any text, so `1e-999999` costs no more than `1e-9`. A `ZADD` or `ZREM` that changes
the set marks the watchers and is propagated; one that changes nothing is
not, as in Redis.

An insert costs about 9 microseconds of Lox++ code, and a command runs in
one step. So one `ZADD` or `ZREM` names at most 16384 members, a score has
at most 64 bytes, and one `EXEC` does at most 32768 inserts and deletes of
sorted sets, where a new score of a member is a delete and an insert, also
for a member that comes again in the same `ZADD`, and a pair that changes
nothing is one unit (rule 6); `ZRANGE` in
`EXEC` spends the element budget, and parses its options before it runs,
with yields.

### Geospatial

A geo key is a sorted set whose scores are 52-bit geohashes, as in Redis:
26 bits of the latitude offset and 26 of the longitude offset, interleaved,
with Int bit operators. The code follows `geohash.c`, `geohash_helper.c`
and `geo.c` of Redis 7.0. `GEOADD` reads its places before it runs, with
yields, and adds through the same code as `ZADD`; it goes to the replicas
and the AOF as that `ZADD`, as in Redis. `GEOPOS` gives the centre of the
cell of the score, and `GEODIST` the haversine distance on a sphere of
6372797.560856 m, with four decimals.

`GEOSEARCH` reads the boxes that Redis reads: the geohash of the centre at
a step from the radius (one less near the edge of its box), and its 8
neighbours, with the boxes outside the search area left out, in the same
order. In its step it takes the members and scores of those boxes, a walk
from the first score of each box; the session keeps the members within the
radius in steps (`writeGeoSearch`), so the command stays atomic and the
other clients go on. It answers every command read before the end of the
stream, also after a half-close (rule 7). The order of the reply is the order of Redis: a fuzz
of 2400 searches and distances against redis-server 7.0 gave no
difference. As in Redis 7.0, it reads the key first, decodes a
`FROMMEMBER` member while it parses, and takes the last of an option that
comes again. In `EXEC`, `GEOSEARCH` spends the element budget for each
member of its boxes, and one `EXEC` walks at most 2097152 members of boxes
(rule 6). `GEOPOS` names at most 16384 members, and spends the sorted set
work of `EXEC`.

### Bitmaps

A String cannot be changed in place, so a string that `SETBIT` writes is
kept as `Bytes`: chunks of 4096 bytes and the length. `SETBIT` copies one
chunk, so it costs O(1) as in Redis; the growth appends a shared chunk of
zero bytes, so `SETBIT` at the largest offset (2^32 - 1, 512 MB) is fast.
The type stays `string`, and `GET`, `SET ... GET`, `STRLEN`, `INCR` and
`GETBIT` read both forms: `GET` writes the chunks in steps, from a copy of
their List taken in its step (`ChunkedBulk`), with no join. The bits of a
byte go from the most significant, as in Redis.

The key of a string value keeps a count cache (`CountCache` on its
`Entry`): the number of 1 bits of each chunk of 4096 bytes, and a total
when each chunk has a count. A String and its `Bytes` have the same chunk
borders, so the counts stay valid when `SETBIT` splits a String. `SETBIT`
changes the count of its chunk and the total in O(1), and only when the bit
changes; a chunk of zero bytes that growth adds counts 0, and a key that
`SETBIT` makes starts with counts. A count of the whole value is then O(1).
Any other count takes a snapshot in its step (the String, or a copy of the
List of chunks), and the session counts from it in steps
(`writeBitcount`), so the command stays atomic and the other clients go
on. A count of a range adds the counts of its whole chunks and scans at
most its two end chunks. A chunk that no count has read since it was
written (a large String that `SET` wrote, the result of `BITOP`) is
scanned at about 0.15 microseconds a byte, one chunk a step, and its count
is written back if the live value still holds the same chunk. So each
byte is scanned at most one time after each write, and counts at the same
time share the scan. A whole chunk of zero bytes is one native compare.
`SET`, `BITOP` and the RDB load make a new `Entry` with no counts, and
`INCR` drops them, so no count can reach a new value.

As in Redis, the session answers each command that it read before the end
of the stream, also after a half-close or a protocol error (rule 7). So a
client that sends a `BITCOUNT` of data that has no count and then closes
leaves the server counting until that scan ends: at most one scan of each
byte that a client wrote (8 MB: about 1.2 s of CPU; 512 MB: about
@@512@@ s, measured). The other clients go on. A native count of the 1 bits
of a String (#1214) would remove that first scan. As in Redis 7.0, it reads the
key before its arguments, gives 0 for two negative indexes with the start
after the end, takes `BYTE` and `BIT` ranges, and takes an index of up to
19 digits.

`BITOP AND | OR | XOR | NOT` runs in one step with tables of 65536 entries
(each built from an earlier one at start). Each source byte is Lox++ code,
so one `BITOP` reads at most 1048576 source bytes (the length of the result
times the number of sources), and one `EXEC` does at most 1048576 units of
bitmap work: the chunks that `SETBIT` adds or splits, the source bytes of
`BITOP`, and the chunks that `GET` and `BITCOUNT` copy (rule 6).

### Pub/Sub

`subscribers` maps each channel to the Conns that subscribe to it, and each
Conn keeps its channels (`conn.channels`). `SUBSCRIBE` and `UNSUBSCRIBE`
give one reply for each channel, and yield every 1024 channels (in
`Output`, one reply each); a
`PUBLISH` while they run is concurrent with them, as a write is with
`WATCH`. A client with a channel is in subscribed mode: it may send only
`SUBSCRIBE`, `UNSUBSCRIBE` and `PING` (which replies `["pong", <arg>]`).
As in Redis, an unknown command, a wrong number of arguments, an unknown
`CONFIG` subcommand and, on a replica, `READONLY` come before the check of
the context; the error names a subcommand as `config|get`, and gives `EXEC`
the `EXECABORT` prefix. A session that ends leaves its channels,
with yields (`unsubscribeAll`).

`PUBLISH` sends a `Published` message to the session of each subscriber
in its own step, so each subscriber gets the messages in the order of the
`PUBLISH` commands; the session writes the message when it takes it. The
reader sends commands ahead of the session, so a message can be behind a
command in the mailbox although it came before that command ran. So the
session counts its messages (`pubQueued`) and writes them before it runs
a command, and `UNSUBSCRIBE` writes them before each reply: a message
that counted a client comes before the reply that leaves its channel. It
costs one step for each subscriber, as in Redis, and spends one element of
the budget of `EXEC` for each. A subscriber that does not read is closed
at the pubsub output buffer limits of Redis (rule 4): over 32 MB of
pending messages at once, or over 8 MB for 60 s (the test flag
`--pubsub-soft-seconds` sets the time). The cost of a message is its
bytes and 200 more, the memory of one message in a mailbox (measured).
A channel with no subscriber left is dropped. A master propagates `PUBLISH` to its replicas, which
deliver it to their subscribers; the AOF does not get it, as in Redis.

### Replication

A master keeps a replication log (`replLog`). Each write command appends
its effective form to the log in its own step (`replicate`), at the point
where it writes: a push comes before the pop of the client that it serves,
`XADD *` goes with the ID that the master gave, the pop of `BLPOP` goes as
`LPOP`, a lazy expiry goes as `DEL`, and `EXEC` wraps its writes in `MULTI`
and `EXEC` when it has more than one (one write goes alone, as in Redis
7.0); the pops of the waiters that `EXEC` serves come after it. So the order of
the log is the order of the writes, and the offset (`replOffset`) counts the
RESP bytes of each entry when it is appended. Each replica session writes
the log from its own position, in steps (`writeReplLog`). A replica that is
falls behind is dropped as Redis drops it at its output buffer limits: at
once over 256 MB, or after 60 s over 64 MB (`--replica-soft-seconds`). The
lag counts the RESP bytes of each entry and 64 bytes for each argument,
because the log holds Lox++ objects, which are larger than their bytes. The
entries that every replica has got are dropped (rule 4). A master keeps no
log while it has no replica, so its offset does not grow then; Redis keeps
its backlog and its offset goes on.

`PSYNC` gives `FULLRESYNC` and an empty RDB file of version 10, the version
of Redis 7.0, so a Redis 7.0 replica loads it. `REPLCONF ACK` keeps the
offset that a replica acknowledged and wakes the clients in `WAIT`. `WAIT`
sends `REPLCONF GETACK *` and waits, as `BLPOP` does, until enough replicas
have acknowledged the offset at its start, or until its time limit.

A replica (`--replicaof "host port"`) runs one task (`masterLink`): it
connects, does the handshake, and loads the RDB file of `FULLRESYNC` into a
new Map, with yields; then, in one step, it puts the new keyspace in place
and marks each client that watches a key. It applies each command of the
stream with no reply, and counts the bytes from the offset of `FULLRESYNC`.
A second task writes to the master: the answer to `REPLCONF GETACK`, with
the offset before the GETACK, and an ACK of its own every second, as Redis
replicas send. As in Redis, a replica does not remove an expired key: its
clients read it as missing, and the commands of its master see it until the
`DEL` of the master comes. This also holds for an expired key in the RDB
of a full resync. A client that watches a key that has expired sees no
change when the `DEL` of the master comes, as in Redis 7.0. A full resync marks each client that watches a
key, one step for each such client. It applies a
`MULTI` ... `EXEC` of the master in one step, yields every 64 commands, and
connects again one second after the link breaks. It refuses a write from a
client with `READONLY`. The cross checks of `diff_redis.py` run a Lox++
master with a redis-server replica, and a redis-server master with a Lox++
replica.

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
- The RDB load reads strings only, in database 0. A file with another
  value kind or another database stops the start. The load does not check
  the checksum. `CONFIG GET` knows `dir`, `dbfilename`, `appendonly`,
  `appenddirname`, `appendfilename` and `appendfsync` only, and takes
  no glob pattern, and its order of names is one order that Redis can
  give (Redis has none that it promises). A glob pattern with more than 1000 stars matches in
  Redis only to a depth of 1000; here it has no such limit.
- A full resync sends an empty RDB file, so a replica gets only the writes
  after it connects. A master has no backlog while it has no replica, so
  its offset does not grow then, unless appendonly is yes: then the log is
  kept for the AOF, and the offset grows. `WAIT`, `REPLCONF` and `PSYNC` cannot be
  queued in `MULTI`. A Lox++ replica of a master whose RDB file has a list
  or a stream key cannot load it, so it connects again every second (and a
  redis-server master makes a new RDB file each time).
- Sorted sets: a score is written as the shortest text that reads back as
  the same double (`30.1`), as the stages ask; Redis 7.0 gives 17
  significant digits (`30.100000000000001`). A hexadecimal score (`0x10`)
  is refused; Redis reads it. `ZADD` takes no options (`NX`, `XX`, `GT`,
  `LT`, `CH`, `INCR`), and `ZRANGE` takes only `WITHSCORES` (not `BYSCORE`,
  `BYLEX`, `REV` or `LIMIT`). One `ZADD` or `ZREM` names at most 16384
  members, a score has at most 64 bytes, and one `EXEC` does at most 32768
  inserts and deletes of sorted sets; Redis has no such limits. A `ZRANGE`
  index of more than 15 digits is "not an integer" (as for `LRANGE`); Redis
  gives an empty range. The RDB load does not read a sorted set.
- Geospatial: `GEOPOS` writes a coordinate as the shortest text, as the
  stages ask (`2.294471561908722`); Redis 7.0 gives 17 decimals.
  `GEOADD` takes no options (`NX`, `XX`, `CH`). `GEOSEARCH` takes only
  `FROMLONLAT` or `FROMMEMBER` with `BYRADIUS` (not `BYBOX`, `ASC`, `DESC`,
  `COUNT`, `WITHCOORD`, `WITHDIST` or `WITHHASH`). `GEOPOS` of a member
  whose score is negative or at least 2^63 gives a null array; Redis
  converts it to an unsigned 64-bit value. `GEOPOS` names at most 16384
  members, and one `EXEC` walks at most 2097152 members of `GEOSEARCH`
  boxes. As for `ZADD`, the limit on the names of `GEOPOS` comes before
  `WRONGTYPE`. The budgets of `EXEC` (elements, sorted set work, `GEOSEARCH`
  walk) are separate, so one `EXEC` can spend each of them (#1202). The coordinates in the error of `GEOADD`, and a distance, are
  rounded from the product by 10^d, so a value within one unit of the last
  place of a tie can round the other way from `printf`, and a coordinate
  of 1e15 or more is not written with all its digits. There is no
  `GEOHASH`, `GEORADIUS` or `GEOSEARCHSTORE`.
- Bitmaps: one `BITOP` reads at most 1048576 source bytes, and one `EXEC`
  does at most 1048576 units of bitmap work; Redis has no such limits.
  The first `BITCOUNT` of data that has no count is much slower than in
  Redis (about 0.15 microseconds a byte, in steps of one chunk, with no
  stall); a later count of the same data is fast (#1214). There is no `BITPOS`, `BITFIELD` or `SETRANGE`.
- Pub/Sub: `SUBSCRIBE` and `UNSUBSCRIBE` cannot be queued in `MULTI`.
  `UNSUBSCRIBE` with no channel leaves the channels in the order of the
  client's Map; Redis gives the order of its hash table. A `PUBLISH` that
  runs after a subscriber sent `UNSUBSCRIBE`, but before its session ran
  it (the session is behind when the client does not read), counts that
  subscriber, and the message comes before the reply; Redis has already
  run the `UNSUBSCRIBE` then. There is no `PSUBSCRIBE`,
  `PUNSUBSCRIBE`, `SSUBSCRIBE`, `QUIT`, `RESET` or `PUBSUB`.
- The manifest forms that Redis does not write: Redis refuses a comment
  that starts with a space and a last line with no newline, and this
  server reads both. Redis reads `seq 1x`, a base line with `seq -1` and
  `type incr`, and this server refuses them. With comments only or
  history lines only, Redis writes the manifest again without them and
  deletes the history files; this server keeps them.
- The AOF: every `appendfsync` policy waits for the file before the reply,
  as `always` does, and no policy forces the bytes to the disk (#1190). A
  new incremental file has no `SELECT 0` at its start, and there is no
  base file and no rewrite (`BGREWRITEAOF`), so the file only grows. A
  replica refuses `--appendonly yes`. A manifest name in quotes is not
  read. A write error stops the server under each policy.
- On a replica, a write from the master of a key that had expired at
  `WATCH` marks the client, and a full resync marks every client that
  watches a key. Redis 7.0 marks neither: it does not mark a client when the
  master overwrites such a key (nor the later clients of that key), and a
  full resync does not mark a client whose watched key is missing before and
  after it.
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
long pipeline, or by input that it never ends. Seven rules prevent this.
Rules 1 to 6 come from the referee ruling on PR #1125, and rule 7 from the
ruling on PR #1204. Every stage group keeps them.

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
7. **Work after the step of the command.** Work that a command gives to the
   session (a count, a filter, the write of a large reply) reads only a
   snapshot that the step of the command took, so the command stays
   atomic. It yields after at most about 2 ms of Lox++ work: 4096 bytes of
   work for each byte, or `ELEMENTS_PER_YIELD` elements. It always ends
   with its reply, in order: the end of the stream and a protocol error do
   not stop it; only a write fault or the end of the session does, and then
   no later reply is written. It does Lox++ work for each byte only on data
   that the 65536-byte limit of rule 3 bounds, or at most one time after
   each write of that data (a cache, as the counts of `BITCOUNT`).

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
| S7: `XRANGE` of 64 entries of almost 1 MB | PING within 0.5 s (without the byte bound of `Output`: 1.38 s) | 0.08–0.09 s |
| S8 (on a new server): `XREAD` of 131072 streams that have entries | every stream; PING within 0.45 s (when the command copies the entries of each stream: 0.61–0.72 s) | 0.18–0.22 s |
| S9: `XRANGE` of one field of 64 MB | the field; PING within 0.25 s (when the field is copied into its bulk: 0.37 s) | 0.10–0.11 s |
| T1 (group `transactions`): `EXEC` of 65535 `RPUSH` of 14 elements, the most commands and arguments that a transaction holds | PING within 0.6 s | 0.28–0.30 s |
| T3 (on a server of their own, as T4 and T5): `EXEC` of 10000 `XADD` with 200 readers that wait above the top | PING within 0.5 s (when each `XADD` wakes the readers at once: 4.28 s) | 0.07 s |
| T4: `EXEC` of 8 `GET` of a 64 MB value | every byte; PING within 0.3 s (when `GET` copies its value: 1.88 s) | 0.002–0.04 s |
| T5: `EXEC` of 65535 `GET` of a 1 KB value | every byte; PING within 0.5 s (when the replies are written in one step: 1.94 s) | 0.10–0.11 s |
| W1 (group `optimistic-locking`, on a server of its own): `WATCH` of 1048575 keys | PING within 0.3 s (without the yield of `WATCH`: 0.76–1.13 s) | 0.10 s |
| W3: a second client that watches the same 1048575 keys | PING within 1.0 s (rule 6 only) | 0.19 s; 0.31 s in CI |
| W2: the `EXEC` after it, which forgets the 1048575 keys | PING within 0.15 s (without the yield of `unwatchAll`: 0.27–0.31 s) | 0.010–0.016 s |
| W4: 100 clients watch one key, and one `EXEC` writes it 65535 times | PING within 0.5 s (when `touch` keeps the marked clients: 3.6 s) | 0.24 s |
| K1 (group `persistence-rdb`): `KEYS k*` of 524288 keys | every key; PING within 1.0 s (rule 6 only) | 0.30–0.32 s; 0.55 s in CI |
| K4: `KEYS nomatch` of 524288 keys, one glob step for each key | PING within 0.1 s (without the yield of the filter loop: 0.21 s) | 0.013 s |
| K5 (on the K3 server): `KEYS ?` and 262144 stars on 256 keys of one byte; each key ends at once and the rest of the pattern is stars | PING within 0.3 s (when the stars at the end are not steps: 5.13 s) | 0.04–0.06 s |
| K3 (on a server of its own): `KEYS *[` and 500 bytes `]z` on a key of 20000 bytes, which reads the set again at each byte (about 10^7 steps) | PING within 0.3 s (when the bytes of a set are not steps: 2.04 s) | 0.09 s |
| K2: `KEYS` with a star and 100 bytes that never match at the end, on a key of 100000 bytes (about 10^7 steps) | PING within 0.3 s (without the yield of the glob work: 4.18 s) | 0.10 s |
| RP1 (group `replication`): a replica that never reads, then 300 writes of 1 MB | the replica is dropped over the hard limit of 256 MB | as required |
| RP3 (with `--replica-soft-seconds 3`): a replica that never reads, then 80 writes of 1 MB, then one more write after 3.5 s | the replica stays while the soft time runs, then goes | as required |
| Not in the suite: `DEL` of 1048575 keys, live or expired | PING within 1.0 s (rule 5: one pass, as in Redis) | 0.53–0.56 s |
| Group `replication`: a full resync of 524288 keys on a live replica | the keys are loaded; PING within 1.0 s | 0.009–0.010 s |
| AOF1 (group `persistence-aof`, on a server of its own): `SET` of 64 MB with the AOF | the file has the command; PING within 1.0 s (rule 6 only) | 0.20 s |
| PS1 (group `pub-sub`): a subscriber that never reads, then 40 messages of 1 MB | the subscriber is closed over the hard limit of 32 MB | as required |
| PS2 (with `--pubsub-soft-seconds 2`): a subscriber that never reads, then 12 messages of 1 MB, then one more after 2.5 s | the subscriber stays while the soft time runs, then goes | as required |
| PS3 (on a server of its own): `SUBSCRIBE` of 1048575 channels | the last reply counts every channel; PING within 1.0 s | 0.40–0.50 s |
| PS4: the end of that client | it leaves every channel; PING within 1.0 s | 0.014–0.028 s |
| PS5: a subscriber that reads 40 messages of 1 MB | not closed (when the session does not give back the cost: closed) | as required |
| PS6: 600000 empty messages to a subscriber that does not read | closed (when a message costs only its bytes, or 64 more: not closed; 500000 such messages took 104 MB of RSS) | as required |
| PS7 (with `--pubsub-soft-seconds 2`): 12 MB, read, 2.5 s, 12 MB more | not closed: the soft time starts again | as required |
| PS8: 5 rounds of a client that joins 200000 new channels and goes | RSS grows less than 300 MB from round 2 to 5 (without the drop of empty channels: 380–470 MB). The growth with the drop moves with the baseline heap (#1206) | 241, 290, 394, 433, 498 MB |
| Z1 (group `sorted-sets`, on a server of its own): `ZADD` of 16384 members to a set of 524288 | PING within 1.0 s (when every node has level 1: far over) | 0.24–0.28 s |
| Z2: `ZRANGE 0 -1` of 524288 members | every member; PING within 1.0 s (rule 6 only) | 0.22–0.26 s |
| Z3: `EXEC` of two `ZADD` of 16384 members | PING within 1.0 s (rule 6 only) | 0.25–0.31 s |
| Z4: `ZADD` of 16384 scores of 64 bytes in the costliest form (`0.` and 52 zeros, 4 digits, `e-256`), on a master | PING within 1.0 s (rule 6 only) | 0.13 s |
| Z4 on a replica that applies it from its master | PING within 1.0 s (rule 6 only) | 0.18 s |
| Z6: `ZRANGE` with 1048572 `WITHSCORES` | PING within 0.5 s (when the options are parsed in one step: 1.75 s) | 0.04 s |
| Z5: the same `ZADD` again on that replica, which changes nothing, so only the parse is work | PING within 0.15 s (when the replica parses with no yield: 0.24–0.25 s). Its margin is small: on a host with every CPU busy it can fail. | 0.06–0.07 s |
| G1 (group `geospatial`, on a server of its own): `GEOSEARCH` whose boxes hold 524288 members, all in the radius | every member; PING within 1.0 s (rule 6 only) | 0.38 s |
| G2: `GEOADD` of 16384 members to that set | PING within 1.0 s (rule 6 only) | 0.30 s |
| G3: `EXEC` of 64 `GEOSEARCH` of that set with one member more (524289, which does not divide the limit) | 3 run and 61 are refused; the walk of the fourth stops as soon as it passes the limit and spends what is left, so the others walk no more; PING within 1.0 s (when the limit is checked after the walk: 5.3 s; when a refused walk spends nothing: 6.9–7.1 s). On a host with every CPU busy it can fail. | 0.45–0.50 s |
| G4: the same `GEOADD` again, which changes nothing, so only the parse is work | PING within 0.1 s (when the parse has no yield: 0.18–0.20 s). Its margin is small: on a host with every CPU busy it can fail. | 0.04–0.05 s |
| B1 (group `bitmaps`, on a server of its own): `SETBIT` at offset 2^32 - 1 | a string of 512 MB with the bit set; PING within 1.0 s (rule 6 only) | 0.01–0.02 s |
| B2: `BITCOUNT` of a dense 8 MB | PING within 1.0 s; the scan yields after each chunk of 4096 bytes (rule 6 only) | 0.003–0.027 s (1.2 s of CPU in all) |
| B3: `BITCOUNT` of the sparse 512 MB of B1 | a reply within 2 s (the counts of a key that `SETBIT` makes: O(1)) | @@B3@@ s |
| B3b: `BITCOUNT` of a String that `SET` wrote and `SETBIT` grew to 512 MB, which has no counts | a reply within 2 s (when a zero chunk is scanned: minutes) | @@B3b@@ s |
| S1 (on a server of its own): 32 connections each send `BITCOUNT` of its own new dense 1 MB at the same time | PING within 0.5 s (when the scan yields every 65536 bytes: @@S1M@@ s) | 0.061–0.066 s |
| C6 (on a server of its own): 10 connections each send `BITCOUNT` of a new dense 8 MB at the same time and close | server CPU at most 2 times one count; the connections end (when each scan reads the counts only at its start: @@C6M@@) | @@C6@@ |
| C7: one connection sends 200 `BITCOUNT` of a new dense 8 MB and closes | server CPU at most 1.5 times one count; the connection ends; a second run that half-closes gets 200 replies (when the scan keeps no count: @@C7M@@) | @@C7@@ |
| B4: `BITOP AND` of two sources of 512 KB (the limit) | PING within 1.0 s (rule 6 only) | 0.30–0.48 s |
| B5: `GET` of a bitmap of 64 MB | every byte; PING within 1.0 s (rule 6 only) | 0.12–0.19 s |
| B6: `INCR` of the 512 MB of B1 | an error; PING within 1.0 s (when the value is joined first: far over) | 0.00 s |
| Not in the suite: `SETBIT` on a String of 512 MB, which splits it into chunks | PING within 1.0 s; the split is once for each String | 0.62–0.63 s |
| AOF3: 40 writes of 16 MB to one key with the AOF | RSS under 300 MB (without the trim by cost: 632 MB) | 86 MB |
| AOF2: `RPUSH` of 1048573 elements with the AOF | the file has every element; PING within 1.0 s (when a command that wrote nothing waits for the file: 2.1 s) | 0.22–0.27 s |
| RP2: `RPUSH` of 1048573 elements with a replica that reads | every byte reaches the replica; PING within 1.0 s (rule 6 only) | 0.22–0.25 s |
| Not in the suite: `XADD` with 524286 field pairs with a replica | PING within 1.0 s | 0.11 s |
| Group `replication`: `WAIT 1 0` and 100 more commands, then the client closes | the tasks of the connection end | as required |
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
`tests/mutations/optimistic-locking.json` or
`tests/mutations/persistence-rdb.json` or
`tests/mutations/replication.json` or
`tests/mutations/persistence-aof.json` or `tests/mutations/pub-sub.json` or
`tests/mutations/sorted-sets.json` or `tests/mutations/geospatial.json` or
`tests/mutations/bitmaps.json` that removes
its guard (S4, T1, T7, W3, K1, RP2, AOF1, PS3, Z2, Z3, G1, G2, B1, B2, B4 and B5 hold rule 6 only: no single change removes their
guard). Three guards of replication have no case, because no client timing
forces the order of events that they handle: the `LPUSH` that a give-back
propagates (the same race as the give-back itself), the step in which a
replica applies a `MULTI` ... `EXEC` of the master, and the wake of a
replica session for an entry that comes while its last write of a call
suspends (an entry that comes before that is written by the same call). Two parts of the RDB load have no mutation, because no reply can
show them: the skip of a key that expired before the load (GET and KEYS
skip an expired key too), and the check of an early end in `need`
(the next read fails too). The `touch` in the lazy expiry of `lookup` has no case: the check
of expiring keys in `watchBroken` already makes `EXEC` fail for a watched
key that expires, so no reply shows it. It stays for a client that watches
the key after the read and before the removal.

The element budget that a `PUBLISH` in `EXEC` spends has no case: it
needs 8388608 deliveries in one `EXEC`, such as 65536 `PUBLISH` to 128
subscribers, which is too long for the suite. Two guards of the AOF have no case. The
wait of `WATCH` for the file (`aofSync`) holds the `DEL` of a lazy expiry,
which the writer writes in one step before the next client reads, so no
timing shows a reply before it. A client that goes while it waits for the
file ends its session at the wake (`isClosedMsg` in `aofSync`); no client
timing makes it go inside that short wait.

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
# and optimistic-locking.json, persistence-rdb.json, replication.json,
# persistence-aof.json, pub-sub.json, sorted-sets.json, geospatial.json,
# bitmaps.json
REDIS_STAGE_GROUPS=persistence-aof python3 examples/codecrafters/build-your-own-redis/tests/run_stages.py
```

`REDIS_STAGE_GROUPS` (names split by commas) runs only those groups, also
under `mutate.py`. A mutation is killed only when its named cases fail, so
it needs only the group of those cases. A mutation is one `old`/`new` replacement, or an
`edits` list of such pairs when it must change more than one site.

The server binds `0.0.0.0` on port 6379 by default. Build `loxpp` with the
`release` preset: the `debug` preset prints a trace on standard output.
Binary resolution: `build/loxpp` beside this checkout first, then `loxpp` on
`PATH`.
