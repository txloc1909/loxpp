#!/usr/bin/env python3
"""run_stages.py — public-source test oracle for the Lox++ Kafka broker.

The stage cases are transcribed from
codecrafters-io/build-your-own-kafka stage_descriptions/*.md (public, no
membership needed); local regression cases are marked "local:". It drives the
real challenge entrypoint, your_program.sh, over raw TCP, so every response
field is decoded and compared here.

Where a stage description gives the literal bytes of a request, the case uses
those bytes. Two of the published hex strings declare a `message_size` that
does not match their length (base-05 and listing-partitions-01 repeat the
39-byte frame of base-03 but declare 26); the cases build those two requests
from the encoder instead and say so in the README.

Usage: tests/run_stages.py [your_program.sh]
"""

import os
import pty
import select
import socket
import subprocess
import sys
import tempfile
import time

import fixtures
from fixtures import W

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


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# ---------------------------------------------------------------------------
# Decoder. Every field is read in the order the Kafka schemas give it.
# ---------------------------------------------------------------------------


class R:
    def __init__(self, data):
        self.b = data
        self.p = 0

    def bytes(self, n):
        if self.p + n > len(self.b):
            raise ValueError("decode past end")
        out = self.b[self.p:self.p + n]
        self.p += n
        return out

    def i8(self):
        return int.from_bytes(self.bytes(1), "big", signed=True)

    def i16(self):
        return int.from_bytes(self.bytes(2), "big", signed=True)

    def i32(self):
        return int.from_bytes(self.bytes(4), "big", signed=True)

    def i64(self):
        return int.from_bytes(self.bytes(8), "big", signed=True)

    def uvarint(self):
        x = 0
        mult = 1
        while True:
            b = self.bytes(1)[0]
            x += (b & 0x7F) * mult
            if not b & 0x80:
                return x
            mult *= 128

    def compact_str(self):
        n = self.uvarint()
        if n == 0:
            return None
        return self.bytes(n - 1)

    def compact_bytes(self):
        n = self.uvarint()
        if n == 0:
            return None
        return self.bytes(n - 1)

    def compact_len(self):
        n = self.uvarint()
        return -1 if n == 0 else n - 1

    def uuid(self):
        return self.bytes(16)

    def tags(self):
        count = self.uvarint()
        for _ in range(count):
            self.uvarint()
            self.bytes(self.uvarint())

    def int32_array(self):
        n = self.compact_len()
        return [self.i32() for _ in range(max(n, 0))]


# ---------------------------------------------------------------------------
# Request encoders.
# ---------------------------------------------------------------------------


def request(api_key, api_version, correlation, body, client_id=b"kafka-cli",
            flexible=True):
    h = W()
    h.i16(api_key)
    h.i16(api_version)
    h.i32(correlation)
    h.str_(client_id)
    if flexible:
        h.tags()
    payload = h.buf() + body
    return len(payload).to_bytes(4, "big") + payload


def api_versions_v4(correlation=1):
    body = W().compact_str(b"kafka-cli").compact_str(b"0.1").tags().buf()
    return request(18, 4, correlation, body)


def produce_v11(correlation, topics, acks=1, timeout=1000):
    body = W()
    body.compact_str(None)
    body.i16(acks)
    body.i32(timeout)
    body.compact_len(len(topics))
    for name, parts in topics:
        body.compact_str(name)
        body.compact_len(len(parts))
        for index, records in parts:
            body.i32(index)
            body.compact_bytes(records)
            body.tags()
        body.tags()
    body.tags()
    return request(0, 11, correlation, body.buf())


def fetch_v16(correlation, topics, session_id=0):
    body = W()
    body.i32(500)
    body.i32(1)
    body.i32(0x7FFFFFFF)
    body.i8(0)
    body.i32(session_id)
    body.i32(0)
    body.compact_len(len(topics))
    for topic_id, parts in topics:
        body.uuid(topic_id)
        body.compact_len(len(parts))
        for index in parts:
            body.i32(index)
            body.i32(-1)
            body.i64(0)
            body.i32(-1)
            body.i64(-1)
            body.i32(0x100000)
            body.tags()
        body.tags()
    body.compact_len(0)
    body.compact_str(b"")
    body.tags()
    return request(1, 16, correlation, body.buf())


def describe_v0(correlation, names, limit=2000):
    body = W()
    body.compact_len(len(names))
    for name in names:
        body.compact_str(name)
        body.tags()
    body.i32(limit)
    body.i8(-1)
    body.tags()
    return request(75, 0, correlation, body.buf())


# ---------------------------------------------------------------------------
# Response decoders. Each returns the decoded fields and asserts that no byte
# is left over: the challenge calls leftover bytes an error.
# ---------------------------------------------------------------------------


def decode_api_versions(frame, version=4):
    """ApiVersions always uses response header v0: the correlation id alone."""
    flexible = version >= 3
    r = R(frame)
    size = r.i32()
    corr = r.i32()
    error = r.i16()
    count = r.compact_len() if flexible else r.i32()
    keys = []
    for _ in range(max(count, 0)):
        entry = {"key": r.i16(), "min": r.i16(), "max": r.i16()}
        if flexible:
            r.tags()
        keys.append(entry)
    throttle = r.i32() if version >= 1 else 0
    if flexible:
        r.tags()
    return {"corr": corr, "error": error, "keys": keys,
            "throttle": throttle, "size_ok": size == len(frame) - 4,
            "left": len(frame) - r.p}


def decode_produce(frame):
    r = R(frame)
    size = r.i32()
    corr = r.i32()
    r.tags()
    topics = []
    for _ in range(r.compact_len()):
        name = r.compact_str()
        parts = []
        for _ in range(r.compact_len()):
            p = {
                "index": r.i32(),
                "error": r.i16(),
                "base_offset": r.i64(),
                "log_append_time": r.i64(),
                "log_start_offset": r.i64(),
            }
            for _ in range(max(r.compact_len(), 0)):
                r.i32()
                r.compact_str()
                r.tags()
            r.compact_str()
            r.tags()
            parts.append(p)
        r.tags()
        topics.append({"name": name, "partitions": parts})
    throttle = r.i32()
    r.tags()
    return {"corr": corr, "topics": topics, "throttle": throttle,
            "size_ok": size == len(frame) - 4, "left": len(frame) - r.p}


def decode_fetch(frame):
    r = R(frame)
    size = r.i32()
    corr = r.i32()
    r.tags()
    throttle = r.i32()
    error = r.i16()
    session = r.i32()
    responses = []
    for _ in range(r.compact_len()):
        topic_id = r.uuid()
        parts = []
        for _ in range(r.compact_len()):
            p = {
                "index": r.i32(),
                "error": r.i16(),
                "high_watermark": r.i64(),
                "last_stable": r.i64(),
                "log_start_offset": r.i64(),
            }
            for _ in range(max(r.compact_len(), 0)):
                r.i64()
                r.i64()
                r.tags()
            p["preferred"] = r.i32()
            p["records"] = r.compact_bytes()
            r.tags()
            parts.append(p)
        r.tags()
        responses.append({"topic_id": topic_id, "partitions": parts})
    r.tags()
    return {"corr": corr, "throttle": throttle, "error": error,
            "session": session, "responses": responses,
            "size_ok": size == len(frame) - 4, "left": len(frame) - r.p}


def decode_describe(frame):
    r = R(frame)
    size = r.i32()
    corr = r.i32()
    r.tags()
    throttle = r.i32()
    topics = []
    for _ in range(r.compact_len()):
        t = {
            "error": r.i16(),
            "name": r.compact_str(),
            "id": r.uuid(),
            "internal": r.i8(),
        }
        parts = []
        for _ in range(r.compact_len()):
            p = {
                "error": r.i16(),
                "index": r.i32(),
                "leader": r.i32(),
                "leader_epoch": r.i32(),
                "replicas": r.int32_array(),
                "isr": r.int32_array(),
                "eligible": r.int32_array(),
                "last_known_elr": r.int32_array(),
                "offline": r.int32_array(),
            }
            r.tags()
            parts.append(p)
        t["partitions"] = parts
        t["authorized"] = r.i32()
        r.tags()
        topics.append(t)
    cursor = r.i8()
    if cursor >= 0:
        r.compact_str()
        r.i32()
        r.tags()
    r.tags()
    return {"corr": corr, "throttle": throttle, "topics": topics,
            "cursor": cursor, "size_ok": size == len(frame) - 4,
            "left": len(frame) - r.p}


def split_batches(data):
    """Walk a log file's RecordBatches, checking each batch length."""
    batches = []
    pos = 0
    while pos + 12 <= len(data):
        length = int.from_bytes(data[pos + 8:pos + 12], "big", signed=True)
        if length < 0 or pos + 12 + length > len(data):
            raise ValueError("batch length %d at %d is not valid" % (length, pos))
        batches.append(data[pos:pos + 12 + length])
        pos += 12 + length
    return batches


# ---------------------------------------------------------------------------
# Process and connection plumbing.
# ---------------------------------------------------------------------------


def command(program, props):
    """The challenge entrypoint, or the binary named by LOXPP_KAFKA_BIN."""
    binary = os.environ.get("LOXPP_KAFKA_BIN")
    if binary:
        return [binary, os.path.join(HERE, "..", "kafka.lox"), props]
    return [program, props]


class Broker:
    def __init__(self, program, topics=None):
        self.program = program
        self.dir = tempfile.mkdtemp(prefix="loxpp-kafka-")
        self.log_dir = os.path.join(self.dir, "kraft-combined-logs")
        os.makedirs(self.log_dir, exist_ok=True)
        if topics:
            fixtures.write_metadata_log(self.log_dir, topics)
        self.port = free_port()
        self.props = fixtures.write_properties(
            os.path.join(self.dir, "server.properties"), self.log_dir, self.port)
        self.proc = None
        self.out = None
        self.pipe = ""

    def __enter__(self):
        env = dict(os.environ)
        env["LOXPP_KAFKA_PORT"] = str(self.port)
        # A pseudo-terminal, so the broker's stdout is line-buffered: a fault
        # message must be visible while the broker runs, not only at exit.
        self.out, slave = pty.openpty()
        self.proc = subprocess.Popen(command(self.program, self.props), env=env,
                                     stdout=slave, stderr=subprocess.STDOUT)
        os.close(slave)
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("broker exited: " + self.tail())
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return self
            except OSError:
                time.sleep(0.02)
        raise RuntimeError("broker did not listen: " + self.tail())

    def __exit__(self, *exc):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self.out is not None:
            os.close(self.out)
            self.out = None
        return False

    def tail(self):
        """Everything the broker printed so far, read from the pty."""
        if self.out is not None:
            while True:
                ready, _, _ = select.select([self.out], [], [], 0)
                if not ready:
                    break
                try:
                    chunk = os.read(self.out, 4096)
                except OSError:
                    break
                if not chunk:
                    break
                self.pipe += chunk.decode("latin-1")
        return self.pipe[-2000:]

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def client(self, timeout=10):
        return Client(self.port, timeout)

    def log_path(self, topic, partition):
        return os.path.join(self.log_dir, "%s-%d" % (topic, partition),
                            "00000000000000000000.log")

    def read_log(self, topic, partition):
        path = self.log_path(topic, partition)
        if not os.path.exists(path):
            return b""
        with open(path, "rb") as f:
            return f.read()

    def call(self, payload, timeout=10):
        c = self.client(timeout)
        try:
            c.send(payload)
            return c.frame()
        finally:
            c.close()


class Client:
    def __init__(self, port, timeout=10):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)

    def send(self, data):
        self.sock.sendall(data)

    def _exact(self, n):
        out = b""
        while len(out) < n:
            try:
                chunk = self.sock.recv(n - len(out))
            except socket.timeout:
                return out
            if not chunk:
                return out
            out += chunk
        return out

    def frame(self):
        head = self._exact(4)
        if len(head) < 4:
            return None
        n = int.from_bytes(head, "big")
        return head + self._exact(n)

    def raw(self, n):
        return self._exact(n)

    def closed(self):
        self.sock.settimeout(2)
        return self.sock.recv(1) == b""

    def close(self):
        self.sock.close()


# Fixture shapes shared by several cases.
ONE_TOPIC = {"foo": (fixtures.topic_uuid("foo"), [0])}
TWO_PARTITIONS = {"foo": (fixtures.topic_uuid("foo"), [0, 1])}
TWO_TOPICS = {
    "zebra": (fixtures.topic_uuid("zebra"), [0]),
    "apple": (fixtures.topic_uuid("apple"), [0, 1]),
}


def uuid_of(name):
    return fixtures.topic_uuid(name)


def value(size, seed=0):
    return bytes((seed + i) & 0xFF for i in range(size))


def batch_of_values(values):
    return fixtures.record_batch(values)


# ---------------------------------------------------------------------------
# base stages
# ---------------------------------------------------------------------------


def case_base(prog):
    print("base")
    with Broker(prog) as b:
        # base-01: a client reaches a broker that listens.
        c = b.client()
        c.send(api_versions_v4(1))
        check("base-01 bind to a port", c.frame() is not None)
        c.close()

    with Broker(prog) as b:
        # base-02: the published placeholder request, answered with
        # correlation_id 7 in eight bytes.
        c = b.client()
        c.send(b"Placeholder request")
        data = c.raw(8)
        check("base-02 response is 8 bytes", data is not None and len(data) == 8,
              repr(data))
        check("base-02 correlation_id is 7",
              data is not None and data[4:8] == (7).to_bytes(4, "big"), repr(data))

    with Broker(prog) as b:
        # base-03: the published request header v2, correlation echoed.
        frame = bytes.fromhex(
            "00000023001200046f7fc66100096b61666b612d636c69"
            "000a6b61666b612d636c6904302e3100")
        data = b.call(frame)
        check("base-03 message_size field is 4 bytes",
              data is not None and int.from_bytes(data[0:4], "big") == len(data) - 4)
        check("base-03 correlation_id echoed",
              data is not None and data[4:8] == bytes.fromhex("6f7fc661"), repr(data))

    with Broker(prog) as b:
        # base-04: ApiVersions version 26442 is unsupported: error 35.
        frame = bytes.fromhex(
            "000000230012674a4f74d28b00096b61666b612d636c69"
            "000a6b61666b612d636c6904302e3100")
        data = b.call(frame)
        check("base-04 correlation_id echoed",
              data is not None and data[4:8] == bytes.fromhex("4f74d28b"), repr(data))
        if data is not None:
            resp = decode_api_versions(data)
            check("base-04 error_code is 35", resp["error"] == 35, str(resp))
            check("base-04 no extra bytes", resp["left"] == 0, str(resp["left"]))

    with Broker(prog) as b:
        # base-05: the full ApiVersions v4 body.
        data = b.call(api_versions_v4(1737034428))
        resp = decode_api_versions(data)
        check("base-05 message_size is the header and body",
              int.from_bytes(data[0:4], "big") == len(data) - 4)
        check("base-05 correlation_id", resp["corr"] == 1737034428, str(resp))
        check("base-05 error_code is 0", resp["error"] == 0, str(resp))
        entry = [k for k in resp["keys"] if k["key"] == 18]
        check("base-05 api_keys has 18 (ApiVersions)", len(entry) == 1, str(resp["keys"]))
        check("base-05 ApiVersions is 0..4",
              len(entry) == 1 and entry[0]["min"] == 0 and entry[0]["max"] >= 4,
              str(entry))
        check("base-05 no extra bytes", resp["left"] == 0, str(resp["left"]))
        # Each range is exactly the range this broker serves, so no client is
        # invited to ask for a version it would refuse.
        for key, low, high in ((0, 11, 11), (1, 16, 16), (18, 0, 4), (75, 0, 0)):
            entry = [k for k in resp["keys"] if k["key"] == key]
            check("base-05 API %d advertises exactly %d-%d" % (key, low, high),
                  len(entry) == 1 and entry[0]["min"] == low
                  and entry[0]["max"] == high, str(entry))


# ---------------------------------------------------------------------------
# producing-messages stages
# ---------------------------------------------------------------------------


def case_producing(prog):
    print("producing-messages")
    with Broker(prog, ONE_TOPIC) as b:
        resp = decode_api_versions(b.call(api_versions_v4(1)))
        entry = [k for k in resp["keys"] if k["key"] == 0]
        check("producing-01 ApiVersions lists Produce with max >= 11",
              len(entry) == 1 and entry[0]["max"] >= 11, str(entry))

    with Broker(prog, ONE_TOPIC) as b:
        # producing-02: a valid topic with an invalid partition, and an
        # unknown topic, both answer error 3 and echo the request fields.
        records = batch_of_values([b"k"])
        bad_partition = decode_produce(
            b.call(produce_v11(11, [(b"foo", [(9, records)])])))
        unknown_topic = decode_produce(
            b.call(produce_v11(12, [(b"missing", [(0, records)])])))
        for label, resp, name, index in (
                ("valid topic, invalid partition", bad_partition, b"foo", 9),
                ("unknown topic", unknown_topic, b"missing", 0)):
            check("producing-02 (%s) throttle is 0" % label,
                  resp["throttle"] == 0, str(resp["throttle"]))
            check("producing-02 (%s) one topic with the request name" % label,
                  len(resp["topics"]) == 1 and resp["topics"][0]["name"] == name,
                  str(resp["topics"]))
            part = resp["topics"][0]["partitions"][0] if resp["topics"] else {}
            check("producing-02 (%s) error_code is 3" % label,
                  part.get("error") == 3, str(part))
            check("producing-02 (%s) index echoed" % label,
                  part.get("index") == index, str(part))
            check("producing-02 (%s) base_offset is -1" % label,
                  part.get("base_offset") == -1, str(part))
            check("producing-02 (%s) log_append_time_ms is -1" % label,
                  part.get("log_append_time") == -1, str(part))
            check("producing-02 (%s) log_start_offset is -1" % label,
                  part.get("log_start_offset") == -1, str(part))
            check("producing-02 (%s) no extra bytes" % label,
                  resp["left"] == 0, str(resp["left"]))
        check("producing-02 nothing written for an unknown topic",
              b.read_log("missing", 0) == b"")
        check("producing-02 nothing written for an invalid partition",
              b.read_log("foo", 9) == b"")

    with Broker(prog, ONE_TOPIC) as b:
        # producing-03: a valid topic and partition answers NO_ERROR.
        records = batch_of_values([b"hello"])
        resp = decode_produce(b.call(produce_v11(21, [(b"foo", [(0, records)])])))
        part = resp["topics"][0]["partitions"][0] if resp["topics"] else {}
        check("producing-03 throttle is 0", resp["throttle"] == 0)
        check("producing-03 error_code is 0", part.get("error") == 0, str(part))
        check("producing-03 index echoes 0", part.get("index") == 0, str(part))
        check("producing-03 base_offset is 0", part.get("base_offset") == 0, str(part))
        check("producing-03 log_append_time_ms is -1",
              part.get("log_append_time") == -1, str(part))
        check("producing-03 log_start_offset is 0",
              part.get("log_start_offset") == 0, str(part))
        check("producing-03 no extra bytes", resp["left"] == 0)

    with Broker(prog, ONE_TOPIC) as b:
        # producing-04: one record, one batch, written as it arrived.
        records = batch_of_values([b"first record"])
        resp = decode_produce(b.call(produce_v11(31, [(b"foo", [(0, records)])])))
        part = resp["topics"][0]["partitions"][0]
        check("producing-04 base_offset is 0", part["base_offset"] == 0, str(part))
        check("producing-04 the batch is on disk byte for byte",
              b.read_log("foo", 0) == records,
              "%d bytes on disk, %d sent" % (len(b.read_log("foo", 0)), len(records)))

    with Broker(prog, ONE_TOPIC) as b:
        # producing-05: one batch that holds several records stays one batch.
        records = batch_of_values([b"one", b"two", b"three"])
        resp = decode_produce(b.call(produce_v11(41, [(b"foo", [(0, records)])])))
        part = resp["topics"][0]["partitions"][0]
        check("producing-05 base_offset is 0", part["base_offset"] == 0, str(part))
        check("producing-05 one batch on disk",
              len(split_batches(b.read_log("foo", 0))) == 1,
              str(len(split_batches(b.read_log("foo", 0)))))
        check("producing-05 the batch is on disk byte for byte",
              b.read_log("foo", 0) == records)

    with Broker(prog, TWO_PARTITIONS) as b:
        # producing-06: two partitions of one topic, one batch each.
        r0 = batch_of_values([b"p0"])
        r1 = batch_of_values([b"p1"])
        resp = decode_produce(
            b.call(produce_v11(51, [(b"foo", [(0, r0), (1, r1)])])))
        parts = resp["topics"][0]["partitions"]
        check("producing-06 one entry per partition",
              [p["index"] for p in parts] == [0, 1], str(parts))
        check("producing-06 both partitions have no error",
              all(p["error"] == 0 for p in parts), str(parts))
        check("producing-06 each base_offset is 0",
              all(p["base_offset"] == 0 for p in parts), str(parts))
        check("producing-06 partition 0 log", b.read_log("foo", 0) == r0)
        check("producing-06 partition 1 log", b.read_log("foo", 1) == r1)

    both_topics = {
        "zebra": (fixtures.topic_uuid("zebra"), [0, 1]),
        "apple": (fixtures.topic_uuid("apple"), [0, 1]),
    }
    with Broker(prog, both_topics) as b:
        # producing-07: several topics, several partitions each.
        batches = {}
        topics = []
        for name in (b"zebra", b"apple"):
            parts = []
            for index in (0, 1):
                records = batch_of_values([name + b"/" + str(index).encode()])
                batches[(name, index)] = records
                parts.append((index, records))
            topics.append((name, parts))
        resp = decode_produce(b.call(produce_v11(61, topics)))
        check("producing-07 one response entry per topic",
              [t["name"] for t in resp["topics"]] == [b"zebra", b"apple"],
              str([t["name"] for t in resp["topics"]]))
        ok = all(p["error"] == 0 and p["base_offset"] == 0
                 for t in resp["topics"] for p in t["partitions"])
        check("producing-07 every partition has no error and base_offset 0", ok)
        for (name, index), records in batches.items():
            check("producing-07 log for %s-%d" % (name.decode(), index),
                  b.read_log(name.decode(), index) == records)


# ---------------------------------------------------------------------------
# consuming-messages stages
# ---------------------------------------------------------------------------


def case_consuming(prog):
    print("consuming-messages")
    with Broker(prog, ONE_TOPIC) as b:
        resp = decode_api_versions(b.call(api_versions_v4(1)))
        entry = [k for k in resp["keys"] if k["key"] == 1]
        check("consuming-01 ApiVersions lists Fetch with max >= 16",
              len(entry) == 1 and entry[0]["max"] >= 16, str(entry))

    with Broker(prog, ONE_TOPIC) as b:
        # consuming-02: no topics asked for, no topics answered.
        resp = decode_fetch(b.call(fetch_v16(12, [])))
        check("consuming-02 correlation_id", resp["corr"] == 12, str(resp["corr"]))
        check("consuming-02 error_code is 0", resp["error"] == 0, str(resp))
        check("consuming-02 throttle is 0", resp["throttle"] == 0)
        check("consuming-02 responses has 0 elements",
              len(resp["responses"]) == 0, str(resp["responses"]))
        check("consuming-02 no extra bytes", resp["left"] == 0)

    with Broker(prog, ONE_TOPIC) as b:
        # consuming-03: an unknown topic id answers 100.
        unknown = bytes(range(200, 216))
        resp = decode_fetch(b.call(fetch_v16(13, [(unknown, [0])])))
        check("consuming-03 error_code is 0", resp["error"] == 0)
        check("consuming-03 throttle is 0", resp["throttle"] == 0)
        check("consuming-03 one topic response", len(resp["responses"]) == 1)
        topic = resp["responses"][0]
        check("consuming-03 topic_id echoed", topic["topic_id"] == unknown)
        part = topic["partitions"][0]
        check("consuming-03 partition_index is 0", part["index"] == 0, str(part))
        check("consuming-03 error_code is 100", part["error"] == 100, str(part))
        check("consuming-03 no extra bytes", resp["left"] == 0)

    with Broker(prog, ONE_TOPIC) as b:
        # consuming-04: a topic with an empty log.
        resp = decode_fetch(b.call(fetch_v16(14, [(uuid_of("foo"), [0])])))
        part = resp["responses"][0]["partitions"][0]
        check("consuming-04 error_code is 0", part["error"] == 0, str(part))
        check("consuming-04 records decode to 0 batches",
              part["records"] in (None, b"") or split_batches(part["records"]) == [],
              repr(part["records"]))
        check("consuming-04 no extra bytes", resp["left"] == 0)

    with Broker(prog, ONE_TOPIC) as b:
        # consuming-05: one message, read back from disk.
        records = batch_of_values([b"single"])
        b.call(produce_v11(15, [(b"foo", [(0, records)])]))
        resp = decode_fetch(b.call(fetch_v16(16, [(uuid_of("foo"), [0])])))
        part = resp["responses"][0]["partitions"][0]
        check("consuming-05 error_code is 0", part["error"] == 0, str(part))
        check("consuming-05 records has 1 batch",
              len(split_batches(part["records"])) == 1,
              str(len(split_batches(part["records"] or b""))))
        check("consuming-05 the batch matches the log file",
              part["records"] == b.read_log("foo", 0))

    with Broker(prog, ONE_TOPIC) as b:
        # consuming-06: several messages, several batches, one partition.
        all_batches = b""
        for i in range(3):
            records = batch_of_values([b"m%d" % i])
            b.call(produce_v11(20 + i, [(b"foo", [(0, records)])]))
            all_batches += records
        resp = decode_fetch(b.call(fetch_v16(30, [(uuid_of("foo"), [0])])))
        part = resp["responses"][0]["partitions"][0]
        check("consuming-06 error_code is 0", part["error"] == 0)
        check("consuming-06 records has the batches the log holds",
              len(split_batches(part["records"] or b"")) == 3,
              str(len(split_batches(part["records"] or b""))))
        check("consuming-06 the records match the log file",
              part["records"] == all_batches and b.read_log("foo", 0) == all_batches,
              "%d bytes" % len(part["records"] or b""))


# ---------------------------------------------------------------------------
# listing-partitions stages
# ---------------------------------------------------------------------------


def case_listing(prog):
    print("listing-partitions")
    with Broker(prog, ONE_TOPIC) as b:
        resp = decode_api_versions(b.call(api_versions_v4(1)))
        entry = [k for k in resp["keys"] if k["key"] == 75]
        check("listing-01 ApiVersions lists DescribeTopicPartitions 0..0",
              len(entry) == 1 and entry[0]["min"] == 0 and entry[0]["max"] >= 0,
              str(entry))

    with Broker(prog, ONE_TOPIC) as b:
        # listing-02: an unknown topic, with response header v1.
        data = b.call(describe_v0(41, [b"nope"]))
        resp = decode_describe(data)
        check("listing-02 correlation_id", resp["corr"] == 41)
        check("listing-02 error_code is 3",
              len(resp["topics"]) == 1 and resp["topics"][0]["error"] == 3,
              str(resp["topics"]))
        topic = resp["topics"][0]
        check("listing-02 topic_name echoed", topic["name"] == b"nope", str(topic))
        check("listing-02 topic_id is all zeros", topic["id"] == bytes(16), str(topic["id"]))
        check("listing-02 partitions is empty", topic["partitions"] == [])
        check("listing-02 next_cursor is null", resp["cursor"] == -1, str(resp["cursor"]))
        check("listing-02 no extra bytes", resp["left"] == 0)

    with Broker(prog, ONE_TOPIC) as b:
        # listing-03: one existing topic with one partition.
        resp = decode_describe(b.call(describe_v0(42, [b"foo"])))
        topic = resp["topics"][0]
        check("listing-03 error_code is 0", topic["error"] == 0, str(topic))
        check("listing-03 topic_name", topic["name"] == b"foo", str(topic))
        check("listing-03 topic_id from the metadata log",
              topic["id"] == uuid_of("foo"), str(topic["id"]))
        check("listing-03 one partition", len(topic["partitions"]) == 1, str(topic))
        part = topic["partitions"][0]
        check("listing-03 partition error_code is 0", part["error"] == 0, str(part))
        check("listing-03 partition_index is 0", part["index"] == 0, str(part))
        check("listing-03 next_cursor is null", resp["cursor"] == -1)
        check("listing-03 no extra bytes", resp["left"] == 0)

    with Broker(prog, TWO_PARTITIONS) as b:
        # listing-04: one topic with two partitions.
        resp = decode_describe(b.call(describe_v0(43, [b"foo"])))
        topic = resp["topics"][0]
        check("listing-04 two partition entries", len(topic["partitions"]) == 2,
              str(topic["partitions"]))
        check("listing-04 partition indexes 0 and 1",
              [p["index"] for p in topic["partitions"]] == [0, 1],
              str([p["index"] for p in topic["partitions"]]))
        check("listing-04 both partitions have no error",
              all(p["error"] == 0 for p in topic["partitions"]))
        check("listing-04 no extra bytes", resp["left"] == 0)

    with Broker(prog, TWO_TOPICS) as b:
        # listing-05: two topics asked in reverse order, answered sorted.
        resp = decode_describe(b.call(describe_v0(44, [b"zebra", b"apple"])))
        names = [t["name"] for t in resp["topics"]]
        check("listing-05 topics sorted by name",
              names == [b"apple", b"zebra"], str(names))
        by_name = {t["name"]: t for t in resp["topics"]}
        check("listing-05 apple has 2 partitions",
              len(by_name[b"apple"]["partitions"]) == 2,
              str(by_name[b"apple"]["partitions"]))
        check("listing-05 zebra has 1 partition",
              len(by_name[b"zebra"]["partitions"]) == 1)
        check("listing-05 topic ids come from the metadata log",
              by_name[b"apple"]["id"] == uuid_of("apple")
              and by_name[b"zebra"]["id"] == uuid_of("zebra"))
        check("listing-05 every partition has no error",
              all(p["error"] == 0 for t in resp["topics"] for p in t["partitions"]))
        check("listing-05 next_cursor is null", resp["cursor"] == -1)
        check("listing-05 no extra bytes", resp["left"] == 0)


# ---------------------------------------------------------------------------
# concurrent-clients stages
# ---------------------------------------------------------------------------


def case_concurrent(prog):
    print("concurrent-clients")
    with Broker(prog, ONE_TOPIC) as b:
        # concurrent-01: many requests on one connection.
        c = b.client()
        ok = True
        for i in range(5):
            c.send(api_versions_v4(100 + i))
            data = c.frame()
            if data is None:
                ok = False
                break
            resp = decode_api_versions(data)
            ok = ok and resp["corr"] == 100 + i and resp["error"] == 0 \
                and resp["left"] == 0
        c.close()
        check("concurrent-01 five requests on one connection", ok)

    with Broker(prog, ONE_TOPIC) as b:
        # concurrent-02: three connections, requests interleaved.
        clients = [b.client() for _ in range(3)]
        for i, c in enumerate(clients):
            c.send(api_versions_v4(200 + i))
            c.send(api_versions_v4(210 + i))
        ok = True
        for i, c in enumerate(clients):
            for corr in (200 + i, 210 + i):
                data = c.frame()
                if data is None:
                    ok = False
                    continue
                resp = decode_api_versions(data)
                ok = ok and resp["corr"] == corr and resp["error"] == 0 \
                    and resp["left"] == 0
        for c in clients:
            c.close()
        check("concurrent-02 three connections interleaved", ok)


# ---------------------------------------------------------------------------
# local cases: the traps that the published stages do not reach
# ---------------------------------------------------------------------------


def case_local(prog):
    print("local")

    with Broker(prog, ONE_TOPIC) as b:
        # local: a request split across two TCP segments.
        frame = api_versions_v4(301)
        c = b.client()
        c.send(frame[:3])
        time.sleep(0.05)
        c.send(frame[3:])
        resp = decode_api_versions(c.frame())
        c.close()
        check("local: a request split across two segments",
              resp["corr"] == 301 and resp["error"] == 0)

    with Broker(prog, ONE_TOPIC) as b:
        # local: two requests in one segment, answered in order.
        c = b.client()
        c.send(api_versions_v4(302) + api_versions_v4(303))
        first = decode_api_versions(c.frame())
        second = decode_api_versions(c.frame())
        c.close()
        check("local: two requests in one segment",
              first["corr"] == 302 and second["corr"] == 303)

    with Broker(prog, ONE_TOPIC) as b:
        # local: every version the broker advertises for ApiVersions is served,
        # and one past the end is refused. This is the property behind the
        # advertised range, not the range's literal value.
        first = decode_api_versions(b.call(api_versions_v4(317)))
        entry = [k for k in first["keys"] if k["key"] == 18][0]
        served = []
        for version in range(entry["min"], entry["max"] + 1):
            if version >= 3:
                body = W().compact_str(b"kafka-cli").compact_str(b"0.1").tags().buf()
                req = request(18, version, 400 + version, body)
            else:
                req = request(18, version, 400 + version, b"", flexible=False)
            resp = decode_api_versions(b.call(req), version)
            served.append((version, resp["error"]))
        check("local: every advertised ApiVersions version is served",
              all(error == 0 for _, error in served), str(served))
        past = entry["max"] + 1
        body = W().compact_str(b"kafka-cli").compact_str(b"0.1").tags().buf()
        resp = decode_api_versions(b.call(request(18, past, 419, body)))
        check("local: one version past the advertised end is refused",
              resp["error"] == 35, str(resp["error"]))

    with Broker(prog, ONE_TOPIC) as b:
        # local: a topic created while the broker runs is visible at once.
        resp = decode_describe(b.call(describe_v0(304, [b"late"])))
        check("local: topic unknown before the metadata log is written",
              resp["topics"][0]["error"] == 3, str(resp["topics"]))
        fixtures.write_metadata_log(
            b.log_dir, {"late": (fixtures.topic_uuid("late"), [0])})
        resp = decode_describe(b.call(describe_v0(305, [b"late"])))
        check("local: topic visible after the metadata log is written",
              resp["topics"][0]["error"] == 0
              and resp["topics"][0]["id"] == fixtures.topic_uuid("late"),
              str(resp["topics"]))

    with Broker(prog, ONE_TOPIC) as b:
        # local: every byte value, NUL included, survives produce and fetch.
        payload = bytes(range(256)) * 4
        records = batch_of_values([payload, b"\x00\x00", b""])
        resp = decode_produce(b.call(produce_v11(306, [(b"foo", [(0, records)])])))
        check("local: a record of every byte value is accepted",
              resp["topics"][0]["partitions"][0]["error"] == 0)
        fetched = decode_fetch(b.call(fetch_v16(307, [(uuid_of("foo"), [0])])))
        served = fetched["responses"][0]["partitions"][0]["records"]
        check("local: every byte value round-trips",
              served == records == b.read_log("foo", 0),
              "%d served, %d sent" % (len(served or b""), len(records)))

    with Broker(prog, ONE_TOPIC) as b:
        # local: a 1 MB produce on one connection does not stop another.
        big = batch_of_values([value(1024 * 1024)])
        big_client = b.client(timeout=20)
        big_client.send(produce_v11(308, [(b"foo", [(0, big)])]))
        other = b.client()
        other.send(api_versions_v4(309))
        resp = decode_api_versions(other.frame())
        other.close()
        check("local: a second client is served during a 1 MB produce",
              resp["corr"] == 309 and resp["error"] == 0)
        produced = decode_produce(big_client.frame())
        big_client.close()
        check("local: the 1 MB produce is accepted",
              produced["topics"][0]["partitions"][0]["error"] == 0,
              str(produced["topics"]))
        check("local: the 1 MB batch is on disk",
              b.read_log("foo", 0) == big)

    with Broker(prog, ONE_TOPIC) as b:
        # local: an unknown API key is refused loudly and the server lives.
        c = b.client()
        c.send(request(9999, 1, 310, b""))
        check("local: an unknown api key closes the connection", c.closed(),
              repr(c.frame()))
        c.close()
        check("local: unknown api key is reported", "9999" in b.tail(), b.tail()[-300:])
        check("local: the server survives an unknown api key", b.alive())
        resp = decode_api_versions(b.call(api_versions_v4(311)))
        check("local: a later client is served", resp["corr"] == 311)

    with Broker(prog, ONE_TOPIC) as b:
        # local: an API this broker does not serve at that version.
        c = b.client()
        c.send(request(1, 11, 312, b""))
        check("local: an unsupported Fetch version closes the connection",
              c.closed(), repr(c.frame()))
        c.close()
        check("local: the server survives an unsupported version", b.alive())

    with Broker(prog, ONE_TOPIC) as b:
        # local: a truncated request does not stop the server.
        c = b.client()
        c.send((100).to_bytes(4, "big") + b"short")
        c.close()
        time.sleep(0.05)
        check("local: the server survives a truncated request", b.alive())
        resp = decode_api_versions(b.call(api_versions_v4(313)))
        check("local: a later client is served after a truncated request",
              resp["corr"] == 313)

    with Broker(prog, ONE_TOPIC) as b:
        # local: message_size 0 reaches no handler.
        c = b.client()
        c.send((0).to_bytes(4, "big"))
        check("local: message_size 0 closes the connection", c.closed())
        c.close()
        check("local: the server survives message_size 0", b.alive())

    with Broker(prog, ONE_TOPIC) as b:
        # local: a produce for a partition the metadata does not have.
        records = batch_of_values([b"x"])
        resp = decode_produce(
            b.call(produce_v11(314, [(b"foo", [(7, records)])])))
        part = resp["topics"][0]["partitions"][0]
        check("local: unknown partition answers error 3 and index 7",
              part["error"] == 3 and part["index"] == 7, str(part))
        check("local: unknown partition writes nothing", b.read_log("foo", 7) == b"")

    # local: a metadata log that names a topic with a path separator. The
    # name reaches the filesystem only through produce, so a broker without
    # the name check creates a directory outside the log directory.
    escape = os.path.join(tempfile.gettempdir(), "loxpp-kafka-escape-%d-0"
                          % os.getpid())
    if os.path.isdir(escape):
        os.rmdir(escape)
    relative = "../../" + os.path.basename(escape)[:-2]
    topics = {relative: (fixtures.topic_uuid("escape"), [0])}
    with Broker(prog, topics) as b:
        records = batch_of_values([b"x"])
        name = relative.encode()
        resp = decode_produce(b.call(produce_v11(315, [(name, [(0, records)])])))
        part = resp["topics"][0]["partitions"][0]
        check("local: a traversal topic name answers error 3", part["error"] == 3,
              str(part))
        check("local: a traversal topic name writes nothing",
              not os.path.exists(escape), escape)
        if os.path.isdir(escape):
            os.rmdir(escape)


# ---------------------------------------------------------------------------


CASES = [
    ("base", case_base),
    ("producing-messages", case_producing),
    ("consuming-messages", case_consuming),
    ("listing-partitions", case_listing),
    ("concurrent-clients", case_concurrent),
    ("local", case_local),
]


def main():
    program = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PROG
    if not os.path.exists(program):
        print("run_stages.py: no program at %s" % program)
        return 2
    only = os.environ.get("LOXPP_KAFKA_ONLY")
    for name, fn in CASES:
        if only and name != only:
            continue
        try:
            fn(program)
        except Exception as exc:  # a crash of a case is a failure, not a stop
            global FAIL
            FAIL += 1
            print("  FAIL %s raised %s: %s" % (name, type(exc).__name__, exc))
    print()
    print("%d passed, %d failed" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
