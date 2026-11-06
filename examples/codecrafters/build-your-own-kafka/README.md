# Build your own Kafka (Codecrafters) in Lox++

A complete implementation of the public
[Build your own Kafka](https://github.com/codecrafters-io/build-your-own-kafka)
challenge in pure Lox++. No membership is needed: the stage cases are
transcribed from the public `stage_descriptions/*.md` files, and local
regression cases are marked `local:`.

The Kafka wire protocol (INT8/16/32/64, unsigned and zigzag varints, compact
and plain strings, nullable bytes and structs, tagged fields, UUIDs), the
`__cluster_metadata` log reader, and the partition log files all run **inside
the VM**. Every byte a client sends is decoded by `kafka.lox`; every byte the
broker writes is built by it.

## Layout

| File | Role |
|---|---|
| `kafka.lox` | the broker: codec, four APIs, metadata reader, log store |
| `your_program.sh` | challenge entrypoint: resolves `loxpp`, passes argv through |
| `tests/run_stages.py` | the 25 stage cases and `local:` cases, over raw TCP |
| `tests/check_format.py` | a second battery: an independent decoder of the log file and of the metadata record versions |
| `tests/fixtures.py` | an independent encoder: CRC32C, RecordBatch v2, `__cluster_metadata` records, `server.properties` |

## Stage coverage

All twenty-five stages of the challenge pass.

| Group | Stage | Status |
|---|---|---|
| base | 01 bind to a port | pass |
| base | 02 correlation ID | pass |
| base | 03 parse the request header | pass |
| base | 04 unsupported version (error 35) | pass |
| base | 05 ApiVersions v4 body | pass |
| producing-messages | 01 Produce in ApiVersions | pass |
| producing-messages | 02 unknown topic or partition (error 3) | pass |
| producing-messages | 03 valid topic and partition | pass |
| producing-messages | 04 one record to disk | pass |
| producing-messages | 05 several records in one batch | pass |
| producing-messages | 06 several partitions | pass |
| producing-messages | 07 several topics | pass |
| consuming-messages | 01 Fetch in ApiVersions | pass |
| consuming-messages | 02 no topics | pass |
| consuming-messages | 03 unknown topic id (error 100) | pass |
| consuming-messages | 04 empty log | pass |
| consuming-messages | 05 one batch from disk | pass |
| consuming-messages | 06 several batches from disk | pass |
| listing-partitions | 01 DescribeTopicPartitions in ApiVersions | pass |
| listing-partitions | 02 unknown topic (error 3) | pass |
| listing-partitions | 03 one partition | pass |
| listing-partitions | 04 several partitions | pass |
| listing-partitions | 05 several topics, sorted | pass |
| concurrent-clients | 01 several requests on one connection | pass |
| concurrent-clients | 02 several connections | pass |

Two published request examples are internally inconsistent: base-05 and
listing-partitions-01 repeat the 39-byte frame of base-03 but declare a
`message_size` of 26. A broker that trusts the declared size reads 10 bytes
of the next frame. Those two cases build the request from the encoder
instead, with the correlation ids the stage descriptions name. Every other
published hex string is used as written.

## Run

```sh
./examples/codecrafters/build-your-own-kafka/your_program.sh /tmp/server.properties
python3 examples/codecrafters/build-your-own-kafka/tests/run_stages.py
python3 examples/codecrafters/build-your-own-kafka/tests/check_format.py
```

The broker reads `listeners` (for the port) and `log.dirs` (for the log
root) from the properties file. Without a properties file it binds
`0.0.0.0:9092` and uses `/tmp/kraft-combined-logs`, as the challenge does.
`--port` and `--log-dir` override the file, and `LOXPP_KAFKA_PORT` and
`LOXPP_KAFKA_LOG_DIR` override both, so several brokers can run at once.
`LOXPP_KAFKA_BIN=<path>` runs a specific `loxpp` binary directly instead of
`your_program.sh`; `LOXPP_KAFKA_ONLY=<group>` runs one group of cases.

Binary resolution: `build/loxpp` beside this checkout first, then `loxpp` on
`PATH`.

Build with the `release` preset before comparing output: the `debug` preset
prints a full per-instruction trace on stdout.

## Design

One worker, one task per connection. `main()` reads the configuration, calls
`listen`, and starts a task for each `accept()`. A fault in one connection
ends only that connection: the task throws instances of `Fault` with `kind`
and `message`, and reads a caught value only through `faultText`, so no
client input can end the program for the other clients.

A connection loops on `read_bytes(4)` for `message_size` and `read_bytes(n)`
for the header and body. Both suspend only their own task, so two clients
interleave at every request boundary, and a connection stays open for as many
requests as the client sends.

| API | Key | Version | Request header | Response header |
|---|---|---|---|---|
| ApiVersions | 18 | 0-4 | v2 from v3, v1 below | v0 |
| Produce | 0 | 11 | v2 | v1 |
| Fetch | 1 | 16 | v2 | v1 |
| DescribeTopicPartitions | 75 | 0 | v2 | v1 |

ApiVersions advertises exactly these four entries, with the ranges above.
A version outside a served range is refused: ApiVersions answers error 35 as
its stage requires, and the other three print a line naming the API key and
version and close the connection.

### Metadata

The broker's only source of topics, partitions, and topic ids is the
`__cluster_metadata` topic's log,
`<log.dirs>/__cluster_metadata-0/00000000000000000000.log`. It is read for
every request, because the challenge creates a topic while the broker runs.

The log is a sequence of RecordBatches. Each record is framed by its own
varint length, so the record types this broker does not need are skipped
without being understood. A record value starts with a 3-byte frame (frame
version, record type, field version) and then the flexible body:

| Type | Record | Fields used |
|---|---|---|
| 2 | TopicRecord v0 | `name`, `topic_id` |
| 3 | PartitionRecord v0-v2 | `partition_id`, `topic_id`, `replicas`, `isr`, `leader`, `leader_epoch` |

PartitionRecord v1 adds `directories`, which is skipped by its compact array
length; v0 has neither field nor the tag buffer's later additions. All three
versions are exercised by `tests/check_format.py`.

### Partition logs

Produce appends the request's RecordBatch bytes to
`<log.dirs>/<topic>-<partition>/00000000000000000000.log`, unchanged. The
challenge says to write each batch "directly", and re-encoding it would change
its CRC32C.

One consequence is deliberate: a client always sends base offset 0 in a batch,
so the second batch in a file repeats the offsets of the first. Real Kafka
rewrites that field on append and recomputes the CRC. This broker instead
assigns the next offset from the number of records the log holds, and reports
the assigned offset in the Produce response; the stored bytes stay the
client's. `tests/check_format.py` pins both halves of that trade.

The partition directory is created on the first Produce for it, with
`run("mkdir", ["-p", path])`. The stdlib has no `mkdir` (#1147), and `run` is
the process interface the spec already binds. `ensureDir` calls `is_dir`
first, so `mkdir` runs only when the directory is absent.

Fetch returns the whole log file as the `records` field, which is a sequence of
RecordBatches, and the topic is matched by the `topic_id` in the request.

### ApiVersions and the base-02 stage

The base-02 stage sends the literal bytes `Placeholder request` and expects
eight bytes back with `correlation_id` 7. Those first four bytes read as a
`message_size` of 1.35 GB, which is above the largest request Kafka accepts
(`socket.request.max.bytes`, 100 MB). A `message_size` above that bound takes
the compatibility path: the broker answers with the eight bytes the stage
wants and closes the connection. No valid request can reach it.

### Limits

- A request is at most 100 MB, the Kafka default; a larger `message_size`
  takes the base-02 path.
- The metadata log is re-read for each request. A large metadata log costs
  its size per request; the challenge's fixtures are a few KB.
- Records are not compressed and not validated: the `records` field is
  written and served as it arrives, so batches with the `compressed` attribute
  are stored and returned as opaque bytes.
- `acks=0` asks for no response. The challenge client always asks for one;
  this broker sends none when `acks` is 0.
- A record set is never decoded, so a batch with a broken CRC or a broken
  record length is stored and served as it arrives.
- `max_bytes` and `fetch_offset` in a Fetch request are parsed and ignored:
  the whole log is served.

### Output

Fault lines go to stdout, one per broken connection, through the pty that the
test harness gives the broker. Like the HTTP example, the output is
block-buffered when it is a pipe or a file, so a line can appear late there.

## The two batteries

`tests/run_stages.py` drives the real entrypoint over raw TCP and decodes every
response field, checking also that no byte is left over after the last field.
It covers the 25 stages plus these `local:` cases:

| Case | What it proves |
|---|---|
| a request split across two TCP segments | framing does not depend on segmentation |
| two requests in one segment | the loop reads the second request from the buffer |
| a topic created while the broker runs | the metadata log is not cached at start |
| every byte value in a record, NUL included | the log and Fetch are byte-transparent |
| a 1 MB Produce while another client asks for ApiVersions | one big request does not stop the others |
| an unknown API key | the connection closes, a line is printed, the server lives |
| an unsupported API version | the same, without a crash |
| a truncated request, and `message_size` 0 | no handler runs on a short frame |
| a Produce for a partition the metadata lacks | error 3, nothing written |
| a metadata topic name with `../` | nothing is created outside the log root |

`tests/check_format.py` decodes what the broker wrote, from the published
layout rather than from `kafka.lox`:

- the RecordBatch header: base offset, batch length, magic, record count,
  last offset delta, and the CRC32C over the bytes from `attributes` to the
  end, with the harness's own CRC32C table (checked against the `123456789`
  vector);
- the record framing: varint lengths that end exactly at the batch end, and
  offset deltas that step by one;
- the offset the Produce response claims against the record count on disk;
- the PartitionRecord versions 0, 1 and 2.

Each check was proved able to fail: five mutations of `kafka.lox` were run,
each failed the case that names it, and each was restored. The five: the
offset formula, the PartitionRecord v1 directory field, the topic name check,
the base-02 compatibility path, and the Produce request header's tag buffer.

## One gap this example found

`mkdir`. See #1147. The broker uses `run("mkdir", ["-p", ...])` once per
partition directory, and the README of the issue explains why the gap belongs
in the stdlib and not in a library.
