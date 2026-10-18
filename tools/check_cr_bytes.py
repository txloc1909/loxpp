#!/usr/bin/env python3
"""
Byte-exact check for a printed carriage return (byte 13).

tools/diff_runtimes.py and tools/check_spec_probes.py read stdout in text
mode, and text mode turns each CR byte into a newline. A runner that drops or
changes a printed CR therefore passes both. This script runs each program on
native and on a second runner in binary mode, and compares the raw stdout
bytes and the exit status.

Native must print a CR byte itself. Without that rule, two outputs that both
lost the CR would be equal and the check would pass with nothing checked.

Usage:
    python3 tools/check_cr_bytes.py <native-loxpp> <other-runner> [program.lox...]

With no program, it runs
test/translation-probes/bootstrap-only/stdout_carriage_return.lox.
Exit status: 0 when every program matches, 1 on a difference, 2 on a usage
error.
"""

import subprocess
import sys
from pathlib import Path

DEFAULT_PROGRAM = (
    Path(__file__).resolve().parent.parent
    / "test/translation-probes/bootstrap-only/stdout_carriage_return.lox"
)
TIMEOUT_SECONDS = 120


def run(cmd: list[str]) -> tuple[bytes, int]:
    result = subprocess.run(cmd, capture_output=True, timeout=TIMEOUT_SECONDS)
    return result.stdout, result.returncode


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    native, other = sys.argv[1], sys.argv[2]
    programs = [Path(p) for p in sys.argv[3:]] or [DEFAULT_PROGRAM]

    failed = False
    for program in programs:
        if not program.is_file():
            print(f"error: not a file: {program}", file=sys.stderr)
            return 2
        native_out, native_status = run([native, str(program)])
        other_out, other_status = run([other, str(program)])
        if b"\r" not in native_out:
            print(f"FAIL  {program}  (native stdout has no CR byte: {native_out!r})")
            failed = True
        elif native_out != other_out or native_status != other_status:
            print(f"FAIL  {program}")
            print(f"      native: status {native_status}, stdout {native_out!r}")
            print(f"      {Path(other).name}: status {other_status}, stdout {other_out!r}")
            failed = True
        else:
            print(f"MATCH {program}  (stdout {native_out!r}, status {native_status})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
