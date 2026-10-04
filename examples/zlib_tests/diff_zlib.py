#!/usr/bin/env python3
# diff_zlib.py — differential oracle for examples/zlib.lox.
#
# This drives the example's file probe modes against Python's zlib, an
# independent RFC 1950/1951 implementation:
#
#   * encode interop: the example compresses; Python zlib.decompress must
#     recover the input.
#   * decode interop: Python zlib.compress produces streams at several levels
#     (forcing stored, fixed, and dynamic blocks); the example must recover
#     the input.
#
# Any framing, Huffman-table, LZ77, or Adler-32 bug shows up as a mismatch
# against the other implementation.
#
# Usage: examples/zlib_tests/diff_zlib.py [loxpp]

import os
import random
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

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


def run(prog, mode, data):
    """Run the example probe on `data`; return its output bytes."""
    d = tempfile.mkdtemp(prefix="loxpp-zlib-")
    inp = os.path.join(d, "in.bin")
    outp = os.path.join(d, "out.bin")
    with open(inp, "wb") as f:
        f.write(data)
    result = subprocess.run(
        [prog, os.path.join(REPO, "examples", "zlib.lox"), mode, inp, outp],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if result.returncode != 0:
        raise RuntimeError(
            "%s %s failed (rc=%d): %s"
            % (prog, mode, result.returncode, result.stderr.decode("utf-8", "replace"))
        )
    with open(outp, "rb") as f:
        return f.read()


def make_inputs():
    rng = random.Random(42)
    text = b"the quick brown fox jumps over the lazy dog. " * 40
    return [
        ("empty", b""),
        ("hello", b"hello"),
        ("repeated word", b"abracadabra" * 10),
        ("all byte values", bytes(range(256)) * 4),
        ("long run", b"A" * 5000),
        ("prose", text),
        ("incompressible", rng.randbytes(3000)),
        ("nul and high bytes", b"\x00" * 100 + b"\xff\xfe" * 50),
        # Over the 65535-byte single-stored-block limit. The encoder must not
        # emit an oversized stored block here; Python still has to decode it.
        ("over 64 KiB", rng.randbytes(66000)),
    ]


def main():
    prog = os.path.abspath(
        sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "loxpp")
    )
    if not os.path.exists(prog):
        print("error: loxpp not found at %s" % prog, file=sys.stderr)
        return 2

    inputs = make_inputs()

    print("encode interop (loxpp compress -> Python zlib.decompress):")
    for name, data in inputs:
        try:
            comp = run(prog, "--compress", data)
            out = zlib.decompress(comp)
            check(name, out == data, "round-trip mismatch")
        except Exception as e:  # noqa: BLE001
            check(name, False, repr(e))

    print("decode interop (Python zlib.compress -> loxpp decompress):")
    for name, data in inputs:
        for level in (0, 1, 6, 9):
            try:
                comp = zlib.compress(data, level)
                out = run(prog, "--decompress", comp)
                check("%s (level %d)" % (name, level), out == data, "mismatch")
            except Exception as e:  # noqa: BLE001
                check("%s (level %d)" % (name, level), False, repr(e))

    print("\n%d passed, %d failed" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
