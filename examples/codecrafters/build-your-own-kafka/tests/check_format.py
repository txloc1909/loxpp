#!/usr/bin/env python3
"""check_format.py — an independent decoder of what the broker writes.

`run_stages.py` compares the bytes a client asked for with the bytes a client
gets back. This battery looks at the two places where a round trip cannot
see a defect: the partition log file on disk, and the `__cluster_metadata`
records the broker reads.

It checks the parts of the Kafka format that a broker can get wrong while
every client stays happy:

  * the RecordBatch header: base offset, batch length, magic, last offset
    delta, and the CRC32C over the bytes from `attributes` to the end;
  * the record framing inside the batch: varint lengths that end exactly at
    the batch end, and per-record offset deltas that step by one;
  * the offset the Produce response claims, against the offset the file
    actually ends at;
  * the metadata record versions 0, 1 and 2 of PartitionRecord, which take a
    different field list.

Usage: tests/check_format.py [your_program.sh]
"""

import os
import sys

import fixtures
import run_stages as rs

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PROG = os.path.join(HERE, "..", "your_program.sh")

PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  ok   %s" % name)
    else:
        FAIL += 1
        print("  FAIL %s  %s" % (name, detail))


# ---------------------------------------------------------------------------
# Record decoding, written from the published layout, not from kafka.lox.
# ---------------------------------------------------------------------------


def read_record(data, pos, end):
    """Decode one record; returns (fields, next position)."""
    def varint():
        nonlocal pos
        shift = 0
        value = 0
        while True:
            if pos >= end:
                raise ValueError("varint runs past the batch")
            b = data[pos]
            pos += 1
            value |= (b & 0x7F) << shift
            if not b & 0x80:
                break
            shift += 7
        return (value >> 1) ^ -(value & 1)

    length = varint()
    body_end = pos + length
    attributes = data[pos]
    pos += 1
    timestamp_delta = varint()
    offset_delta = varint()
    key_len = varint()
    key = None
    if key_len >= 0:
        key = data[pos:pos + key_len]
        pos += key_len
    value_len = varint()
    value = None
    if value_len >= 0:
        value = data[pos:pos + value_len]
        pos += value_len
    header_count = varint()
    headers = []
    for _ in range(header_count):
        hk = varint()
        hkey = data[pos:pos + hk] if hk >= 0 else None
        pos += max(hk, 0)
        hv = varint()
        hval = data[pos:pos + hv] if hv >= 0 else None
        pos += max(hv, 0)
        headers.append((hkey, hval))
    if pos != body_end:
        raise ValueError("record length does not match its fields")
    return {
        "attributes": attributes,
        "timestamp_delta": timestamp_delta,
        "offset_delta": offset_delta,
        "key": key,
        "value": value,
        "headers": headers,
    }, body_end


def decode_batch(batch):
    """Decode one RecordBatch and check every field the format fixes."""
    if len(batch) < 61:
        raise ValueError("batch is shorter than its header")
    base_offset = int.from_bytes(batch[0:8], "big", signed=True)
    batch_length = int.from_bytes(batch[8:12], "big", signed=True)
    if 12 + batch_length != len(batch):
        raise ValueError("batch length %d does not match %d bytes"
                         % (batch_length, len(batch)))
    magic = batch[16]
    stored_crc = int.from_bytes(batch[17:21], "big")
    computed_crc = fixtures.crc32c(batch[21:])
    attributes = int.from_bytes(batch[21:23], "big", signed=True)
    last_offset_delta = int.from_bytes(batch[23:27], "big", signed=True)
    record_count = int.from_bytes(batch[57:61], "big", signed=True)
    records = []
    pos = 61
    while pos < len(batch):
        record, pos = read_record(batch, pos, len(batch))
        records.append(record)
    return {
        "base_offset": base_offset,
        "magic": magic,
        "stored_crc": stored_crc,
        "computed_crc": computed_crc,
        "attributes": attributes,
        "last_offset_delta": last_offset_delta,
        "record_count": record_count,
        "records": records,
    }


def check_batches(label, data, expected_values, expected_base):
    try:
        batches = rs.split_batches(data)
    except ValueError as exc:
        check("%s batches frame exactly" % label, False, str(exc))
        return
    check("%s is one batch" % label, len(batches) == 1, str(len(batches)))
    if not batches:
        return
    try:
        decoded = decode_batch(batches[0])
    except ValueError as exc:
        check("%s decodes" % label, False, str(exc))
        return
    check("%s base offset is %d" % (label, expected_base),
          decoded["base_offset"] == expected_base, str(decoded["base_offset"]))
    check("%s magic is 2" % label, decoded["magic"] == 2, str(decoded["magic"]))
    check("%s CRC32C matches the bytes it covers" % label,
          decoded["stored_crc"] == decoded["computed_crc"],
          "stored %08x computed %08x" % (decoded["stored_crc"],
                                         decoded["computed_crc"]))
    check("%s record count is %d" % (label, len(expected_values)),
          decoded["record_count"] == len(expected_values),
          str(decoded["record_count"]))
    check("%s last offset delta is count - 1" % label,
          decoded["last_offset_delta"] == len(expected_values) - 1,
          str(decoded["last_offset_delta"]))
    check("%s record offset deltas step by one" % label,
          [r["offset_delta"] for r in decoded["records"]]
          == list(range(len(expected_values))),
          str([r["offset_delta"] for r in decoded["records"]]))
    check("%s record values are exactly what was produced" % label,
          [r["value"] for r in decoded["records"]] == list(expected_values),
          str([r["value"] for r in decoded["records"]]))
    check("%s records carry no headers and no keys" % label,
          all(r["headers"] == [] and r["key"] is None for r in decoded["records"]))


def main():
    program = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PROG
    if not os.path.exists(program):
        print("check_format.py: no program at %s" % program)
        return 2

    print("record format")

    # The harness's own CRC32C must be right before it can judge the broker's.
    check("CRC32C of \"123456789\" is 0xE3069283",
          fixtures.crc32c(b"123456789") == 0xE3069283,
          "%08x" % fixtures.crc32c(b"123456789"))

    topics = rs.ONE_TOPIC
    with rs.Broker(program, topics) as b:
        values = [b"alpha", b"\x00\x01\x02", b""]
        records = fixtures.record_batch(values)
        resp = rs.decode_produce(
            b.call(rs.produce_v11(71, [(b"foo", [(0, records)])])))
        part = resp["topics"][0]["partitions"][0]
        path = b.log_path("foo", 0)
        check("the log file is at the documented path", os.path.exists(path), path)
        on_disk = b.read_log("foo", 0)
        check("the file holds exactly the bytes the client sent",
              on_disk == records, "%d bytes" % len(on_disk))
        check_batches("the first produce", on_disk, values, 0)
        check("Produce reports the base offset the file starts at",
              part["base_offset"] == 0, str(part["base_offset"]))

        # A second batch continues the offset sequence.
        more = [b"delta"]
        records2 = fixtures.record_batch(more)
        resp = rs.decode_produce(
            b.call(rs.produce_v11(72, [(b"foo", [(0, records2)])])))
        part = resp["topics"][0]["partitions"][0]
        check("the second produce reports base offset 3",
              part["base_offset"] == 3, str(part["base_offset"]))
        on_disk = b.read_log("foo", 0)
        check("the file holds both batches", on_disk == records + records2,
              "%d bytes" % len(on_disk))
        batches = rs.split_batches(on_disk)
        check("the file holds two batches", len(batches) == 2, str(len(batches)))
        if len(batches) == 2:
            second = decode_batch(batches[1])
            check("the second batch is stored as the client sent it",
                  second["base_offset"] == 0 and batches[1] == records2,
                  str(second["base_offset"]))
            check("the second batch's CRC32C matches",
                  second["stored_crc"] == second["computed_crc"])

        # The offset sequence continues from the record count, not from the
        # stored base offset, which a client always sends as 0.
        third = fixtures.record_batch([b"epsilon"])
        resp = rs.decode_produce(
            b.call(rs.produce_v11(73, [(b"foo", [(0, third)])])))
        part = resp["topics"][0]["partitions"][0]
        check("the third produce reports base offset 4",
              part["base_offset"] == 4, str(part["base_offset"]))

        # Fetch must hand back the same batch sequence.
        final = b.read_log("foo", 0)
        fetched = rs.decode_fetch(
            b.call(rs.fetch_v16(74, [(rs.uuid_of("foo"), [0])])))
        served = fetched["responses"][0]["partitions"][0]["records"]
        check("Fetch serves the file byte for byte", served == final,
              "%d served, %d on disk" % (len(served or b""), len(final)))
        check("Fetch's records frame exactly",
              rs.split_batches(served or b"") == rs.split_batches(final))
        check("the log holds three batches after three produces",
              len(rs.split_batches(final)) == 3,
              str(len(rs.split_batches(final))))

    print("metadata records")
    for version in (0, 1, 2):
        fixture = {"foo": (fixtures.topic_uuid("foo"), [0, 1])}
        with rs.Broker(program, None) as b:
            fixtures.write_metadata_log(
                b.log_dir, fixture, partition_version=version)
            resp = rs.decode_describe(b.call(rs.describe_v0(74 + version,
                                                            [b"foo"])))
            topic = resp["topics"][0]
            check("PartitionRecord version %d: topic found" % version,
                  topic["error"] == 0 and topic["id"] == fixtures.topic_uuid("foo"),
                  str(topic))
            check("PartitionRecord version %d: both partitions" % version,
                  [p["index"] for p in topic["partitions"]] == [0, 1],
                  str([p["index"] for p in topic["partitions"]]))
            check("PartitionRecord version %d: replicas and isr" % version,
                  all(p["replicas"] == [1] and p["isr"] == [1]
                      for p in topic["partitions"]),
                  str(topic["partitions"]))

    print()
    print("%d passed, %d failed" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
