#!/usr/bin/env python3
# diff_http.py — differential oracle for the Lox++ HTTP server.
#
# Instead of re-asserting the transcribed stage expectations, this drives the
# server through Python's http.client (an independent HTTP/1.1 implementation)
# and checks every gzip body against Python's gzip + zlib, including the gzip
# trailer's CRC32 and ISIZE fields. Any framing or compression bug shows up as
# a mismatch with a second, unrelated implementation.
#
# Usage: tests/diff_http.py [your_http.sh]

import gzip
import http.client
import os
import random
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run_stages import free_port, start_server  # noqa: E402

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


def main():
    prog = os.path.abspath(
        sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "your_http.sh")
    )
    directory = tempfile.mkdtemp(prefix="loxpp-http-diff-")
    with open(os.path.join(directory, "payload"), "wb") as f:
        f.write(bytes(range(256)) * 4)

    port = free_port()
    proc, log_path, up = start_server(prog, directory, port)
    if not up:
        print("server did not start:\n%s" % open(log_path).read())
        return 1

    try:
        rng = random.Random(20261003)
        strings = [""] + [
            "".join(rng.choice("abcXYZ019-._~") for _ in range(rng.randint(1, 200)))
            for _ in range(40)
        ]

        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)

        # Plain echo: http.client frames the body from Content-Length.
        plain_ok = True
        for s in strings:
            conn.request("GET", "/echo/%s" % s)
            resp = conn.getresponse()
            body = resp.read()
            if (
                resp.status != 200
                or body != s.encode()
                or resp.getheader("Content-Length") != str(len(s))
                or resp.getheader("Content-Type") != "text/plain"
            ):
                plain_ok = False
                print("    mismatch for %r: %s %r" % (s, resp.status, body))
                break
        check("plain echo vs http.client", plain_ok)

        # A 1024-byte file round-trips byte-for-byte.
        conn.request("GET", "/files/payload")
        resp = conn.getresponse()
        body = resp.read()
        with open(os.path.join(directory, "payload"), "rb") as f:
            want = f.read()
        check(
            "binary file round-trip",
            resp.status == 200
            and body == want
            and resp.getheader("Content-Type") == "application/octet-stream",
            "%s len=%d want=%d" % (resp.status, len(body), len(want)),
        )

        # gzip: every body must decode with Python, and the trailer must carry
        # the true CRC32 and ISIZE of the uncompressed text.
        gzip_ok = True
        for s in strings:
            conn.request("GET", "/echo/%s" % s, headers={"Accept-Encoding": "gzip"})
            resp = conn.getresponse()
            body = resp.read()
            raw = s.encode()
            problems = []
            if resp.getheader("Content-Encoding") != "gzip":
                problems.append("no Content-Encoding")
            if resp.getheader("Content-Length") != str(len(body)):
                problems.append("Content-Length %r != %d" % (resp.getheader("Content-Length"), len(body)))
            try:
                if gzip.decompress(body) != raw:
                    problems.append("decompress mismatch")
            except Exception as exc:  # noqa: BLE001
                problems.append("decompress error: %s" % exc)
            if len(body) >= 8:
                crc, isize = struct.unpack("<II", body[-8:])
                if crc != zlib.crc32(raw):
                    problems.append("trailer CRC32 %d != %d" % (crc, zlib.crc32(raw)))
                if isize != len(raw):
                    problems.append("trailer ISIZE %d != %d" % (isize, len(raw)))
            else:
                problems.append("short stream")
            if problems:
                gzip_ok = False
                print("    %r: %s" % (s, "; ".join(problems)))
                break
        check("gzip round-trip vs Python gzip/zlib (%d inputs)" % len(strings), gzip_ok)

        conn.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except Exception:  # noqa: BLE001
            proc.kill()

    print("\n%d passed, %d failed" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
