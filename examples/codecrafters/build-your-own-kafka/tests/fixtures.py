#!/usr/bin/env python3
"""fixtures.py — an independent encoder for the Kafka formats the broker reads.

Nothing here is shared with kafka.lox: this module writes the bytes, and the
broker parses them. The two implementations come from the same published
format, so a mistake in one shows up as a failing case in the other.

The pieces that the broker reads but never writes are here:

  * the CRC32C (Castagnoli) table, which Kafka uses for every RecordBatch;
  * the RecordBatch v2 layout (kafka.apache.org/42/implementation/message-format);
  * the `__cluster_metadata` records, from the Kafka source at the commit the
    stage descriptions name:
    metadata/src/main/resources/common/metadata/{Topic,Partition,FeatureLevel}Record.json
"""

import os

# ---------------------------------------------------------------------------
# CRC32C: reflected Castagnoli polynomial 0x82F63B78.
# ---------------------------------------------------------------------------

_POLY = 0x82F63B78


def _make_table():
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ _POLY
            else:
                crc >>= 1
        table.append(crc)
    return table


_CRC_TABLE = _make_table()


def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc = _CRC_TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Primitive writers.
# ---------------------------------------------------------------------------


class W:
    def __init__(self):
        self.b = bytearray()

    def raw(self, data):
        self.b.extend(data)
        return self

    def i8(self, v):
        return self.raw(int(v).to_bytes(1, "big", signed=True))

    def i16(self, v):
        return self.raw(int(v).to_bytes(2, "big", signed=True))

    def i32(self, v):
        return self.raw(int(v).to_bytes(4, "big", signed=True))

    def i64(self, v):
        return self.raw(int(v).to_bytes(8, "big", signed=True))

    def uvarint(self, v):
        v = int(v)
        if v < 0:
            raise ValueError("uvarint of a negative value")
        while True:
            if v < 0x80:
                self.b.append(v)
                return self
            self.b.append((v & 0x7F) | 0x80)
            v >>= 7

    def varint(self, v):
        # zigzag: the low bit is the sign
        return self.uvarint((v << 1) ^ (v >> 63))

    def str_(self, s):
        if s is None:
            return self.i16(-1)
        return self.i16(len(s)).raw(s)

    def compact_str(self, s):
        if s is None:
            return self.uvarint(0)
        return self.uvarint(len(s) + 1).raw(s)

    def compact_bytes(self, b):
        if b is None:
            return self.uvarint(0)
        return self.uvarint(len(b) + 1).raw(b)

    def compact_len(self, n):
        return self.uvarint(n + 1)

    def tags(self):
        return self.uvarint(0)

    def uuid(self, u):
        assert len(u) == 16
        return self.raw(u)

    def buf(self):
        return bytes(self.b)


# ---------------------------------------------------------------------------
# Records and RecordBatches.
# ---------------------------------------------------------------------------


def record(value, key=None, offset_delta=0, timestamp_delta=0, headers=()):
    body = W()
    body.i8(0)  # attributes
    body.varint(timestamp_delta)
    body.varint(offset_delta)
    if key is None:
        body.varint(-1)
    else:
        body.varint(len(key)).raw(key)
    if value is None:
        body.varint(-1)
    else:
        body.varint(len(value)).raw(value)
    body.varint(len(headers))
    for hk, hv in headers:
        body.varint(len(hk)).raw(hk)
        if hv is None:
            body.varint(-1)
        else:
            body.varint(len(hv)).raw(hv)
    payload = body.buf()
    out = W()
    out.varint(len(payload)).raw(payload)
    return out.buf()


def record_batch(values, base_offset=0, base_timestamp=1700000000000, attributes=0):
    """A RecordBatch v2 whose records carry the given values, in order."""
    records = b"".join(
        record(value, offset_delta=i) for i, value in enumerate(values)
    )
    tail = W()
    tail.i16(attributes)
    tail.i32(len(values) - 1 if values else 0)  # lastOffsetDelta
    tail.i64(base_timestamp)
    tail.i64(base_timestamp)
    tail.i64(-1)  # producerId
    tail.i16(-1)  # producerEpoch
    tail.i32(-1)  # baseSequence
    tail.i32(len(values))
    tail.raw(records)
    after_crc = tail.buf()

    body = W()
    body.i32(-1)  # partitionLeaderEpoch
    body.i8(2)  # magic
    body.raw(crc32c(after_crc).to_bytes(4, "big"))
    body.raw(after_crc)

    batch = body.buf()
    out = W()
    out.i64(base_offset)
    out.i32(len(batch))
    out.raw(batch)
    return out.buf()


def batch_fields(batch):
    """The header fields a broker needs, decoded without trusting a record body."""
    return {
        "base_offset": int.from_bytes(batch[0:8], "big", signed=True),
        "batch_length": int.from_bytes(batch[8:12], "big", signed=True),
        "last_offset_delta": int.from_bytes(batch[23:27], "big", signed=True),
    }


# ---------------------------------------------------------------------------
# __cluster_metadata records. A record value is a 3-byte frame (frame version,
# record type, field version) and then the body in the flexible encoding.
# ---------------------------------------------------------------------------

TOPIC_RECORD = 2
PARTITION_RECORD = 3
FEATURE_LEVEL_RECORD = 12


def _framed(record_type, version, body):
    out = W()
    out.i8(0)
    out.i8(record_type)
    out.i8(version)
    out.raw(body)
    return out.buf()


def topic_record(name, topic_id):
    body = W()
    body.compact_str(name)
    body.uuid(topic_id)
    body.tags()
    return _framed(TOPIC_RECORD, 0, body.buf())


def partition_record(partition_id, topic_id, replicas, isr, leader,
                     leader_epoch=0, partition_epoch=0, version=2):
    body = W()
    body.i32(partition_id)
    body.uuid(topic_id)
    body.compact_len(len(replicas))
    for r in replicas:
        body.i32(r)
    body.compact_len(len(isr))
    for r in isr:
        body.i32(r)
    body.compact_len(0)  # removingReplicas
    body.compact_len(0)  # addingReplicas
    body.i32(leader)
    body.i32(leader_epoch)
    body.i32(partition_epoch)
    if version >= 1:
        body.compact_len(0)  # directories
    body.tags()
    return _framed(PARTITION_RECORD, version, body.buf())


def feature_level_record(name=b"metadata.version", level=28):
    body = W()
    body.compact_str(name)
    body.i16(level)
    body.tags()
    return _framed(FEATURE_LEVEL_RECORD, 0, body.buf())


def metadata_log(topics, partition_version=2):
    """topics: {name: (uuid_bytes, [partition ids])} -> a metadata log file."""
    values = [feature_level_record()]
    for name in sorted(topics):
        topic_id, partitions = topics[name]
        values.append(topic_record(name.encode() if isinstance(name, str) else name, topic_id))
        for pid in partitions:
            values.append(partition_record(pid, topic_id, [1], [1], 1,
                                           version=partition_version))
    return record_batch(values)


def write_metadata_log(log_dir, topics, partition_version=2):
    path = os.path.join(log_dir, "__cluster_metadata-0", "00000000000000000000.log")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(metadata_log(topics, partition_version))
    return path


def write_properties(path, log_dir, port):
    with open(path, "w") as f:
        f.write("broker.id=1\n")
        f.write("node.id=1\n")
        f.write("listeners=PLAINTEXT://:%d\n" % port)
        f.write("log.dirs=%s\n" % log_dir)
    return path


def topic_uuid(name):
    """A stable, recognisable UUID per topic name (not a hash of the name)."""
    seed = sum(name.encode()) & 0xFF
    raw = bytearray(range(16))
    raw[0] = seed
    raw[15] = (seed * 7 + 3) & 0xFF
    return bytes(raw)
