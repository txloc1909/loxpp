#!/usr/bin/env python3
"""
Byte-exact check for a printed carriage return (byte 13).

tools/diff_runtimes.py and tools/check_spec_probes.py read stdout in text
mode, and text mode turns each CR byte into a newline. A lone CR that a runner
drops still shows as a difference. Two cases do not show: a runner that
changes a CR into a newline, and a runner that drops a CR before a newline.
This script runs each program on native and on a second runner in binary
mode, and compares the raw stdout bytes and the exit status. The default
probes hold a lone CR and a CR directly before a newline. The first covers a
runner that changes a CR into a newline; the second covers a runner that
drops a CR before a newline, which the text-mode gates cannot see.

Native must print a CR byte itself. Without that rule, two outputs that both
lost the CR would be equal and the check would pass with nothing checked.

Usage:
    python3 tools/check_cr_bytes.py <native-loxpp> <other-runner> [program.lox...]

With no program, it runs both probes that DEFAULT_PROGRAMS names.
Exit status: 0 when every program matches, 1 on a difference, 2 on a usage
error.
"""

import subprocess
import sys
from pathlib import Path

from runner_precondition import refuse_unrunnable, require_runner

PROBE_DIR = (
    Path(__file__).resolve().parent.parent / "test/translation-probes/bootstrap-only"
)
DEFAULT_PROGRAMS = (
    PROBE_DIR / "stdout_carriage_return.lox",
    PROBE_DIR / "stdout_carriage_return_before_newline.lox",
)
TIMEOUT_SECONDS = 120


def run(cmd: list[str]) -> tuple[bytes, int]:
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=TIMEOUT_SECONDS)
    except OSError as exc:
        refuse_unrunnable(cmd[0], exc)
    return result.stdout, result.returncode


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    native, other = sys.argv[1], sys.argv[2]
    require_runner(native)
    require_runner(other)
    programs = [Path(p) for p in sys.argv[3:]] or list(DEFAULT_PROGRAMS)

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
