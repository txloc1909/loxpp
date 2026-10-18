#!/usr/bin/env python3
"""
Differential check: every row of the Fatal Runtime Errors table in
spec/04-semantics.md is fatal on the bootstrap interpreter, as on native.

The spec says a fatal fault is never delivered to a `catch` block, and it
skips every pending deferred call, including the ones of the function where
the fault happened. Native follows the table. The bootstrap still raises some
of these faults through a catchable path (issue #666).

The script reads the table rows from the spec, so a new spec row gets a case
with no edit here. For each row it runs the Example three ways, on native and
on the bootstrap (LANGUAGE=LOXPP bootstrap/lox_wrapper.sh):

  bare   the Example as a program;
  try    the Example inside `try`/`catch` (the catch prints CAUGHT);
  defer  the Example in a function with a pending `defer` (prints DEFER).

Native is the reference. It must itself be fatal in each way: exit 70, empty
stdout, and the spec's Message column as the first stderr line. The bootstrap
must give the same stdout, exit status and first stderr line as native. Native
appends a stack trace to stderr that the bootstrap does not, so stderr is
compared on its first line only.

tools/fatal_table_xfail.txt lists the rows the bootstrap does not pass yet.
The check passes only when every mismatching row is on the list. It fails on
a mismatch that is not listed, and it fails on a listed row that passes, so
the list shrinks as the fix groups of issue #666 land.

A release build is required (the same --trace-enabled skip rule as the other
bootstrap ctest entries): a debug build's instruction trace floods native's
stdout.

Usage:
    tools/check_fatal_table.py [--native PATH] [--bootstrap PATH]
        [--xfail PATH] [--timeout SECONDS] [--jobs N]

Exits 0 when the mismatching rows are exactly the listed rows. Exits 1 and
prints every discrepancy otherwise. Exits 125 (ctest's SKIP_RETURN_CODE) when
the binary under test was built with the debug trace macros on.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SPEC_PATH = REPO_ROOT / "spec" / "04-semantics.md"
XFAIL_PATH = REPO_ROOT / "tools" / "fatal_table_xfail.txt"

SKIP_RETURN_CODE = 125
FATAL_EXIT = 70

SECTION_HEADING = "### Fatal Runtime Errors"
TABLE_HEADER = "| Cause | Example | Message |"
ROW_RE = re.compile(r"^\| (?P<cause>.+?) \| (?P<example>`.+?) \| `(?P<message>[^`]+)` \|$")
CODE_RE = re.compile(r"`([^`]+)`")
# `enum` is legal only at global scope, so a leading enum declaration stays
# outside the `try` block and the function of the wrapped variants.
ENUM_RE = re.compile(r"^((?:enum \w+ \{[^}]*\}\s*)+)(.*)$", re.DOTALL)
SPEC_TMP_FILE = "/tmp/f.txt"

VARIANTS = ("bare", "try", "defer")


@dataclass
class Row:
    cause: str
    example: str
    message: str


def parse_spec_rows(spec_text: str) -> list[Row]:
    start = spec_text.index(SECTION_HEADING)
    lines = spec_text[start:].splitlines()
    header = lines.index(TABLE_HEADER)
    rows = []
    for line in lines[header + 2:]:
        if not line.startswith("|"):
            break
        match = ROW_RE.match(line)
        if not match:
            raise SystemExit(f"cannot parse Fatal Runtime Errors row: {line!r}")
        code = CODE_RE.search(match["example"])
        rows.append(Row(match["cause"], code[1], match["message"]))
    return rows


def build_program(row: Row, variant: str, tmp_file: str) -> str:
    example = row.example.replace(SPEC_TMP_FILE, tmp_file)
    prelude, body = "", example
    enum = ENUM_RE.match(example)
    if enum:
        prelude, body = enum[1] + "\n", enum[2]
    if variant == "bare":
        return prelude + body + "\n"
    if variant == "try":
        return (prelude + "try {\n" + body + "\n} catch (e) {\n"
                '    print "CAUGHT";\n}\nprint "AFTER";\n')
    return (prelude + 'fun note(s) { print s; }\n'
            'fun main() {\n    defer note("DEFER");\n' + body + "\n}\nmain();\n"
            'print "AFTER";\n')


@dataclass
class RunResult:
    exit_code: int
    stdout: list[str]
    stderr_first: str
    timed_out: bool = False


def run_program(cmd: list[str], program: str, path: Path, timeout: float,
                env: dict | None) -> RunResult:
    path.write_text(program)
    try:
        proc = subprocess.run([*cmd, str(path)], capture_output=True, text=True,
                              timeout=timeout, env=env, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return RunResult(-1, [], "", timed_out=True)
    stderr = proc.stderr.splitlines()
    return RunResult(proc.returncode, proc.stdout.splitlines(),
                     stderr[0] if stderr else "")


def read_xfail(path: Path, rows: list[Row]) -> dict[str, tuple[str, str]]:
    causes = {row.cause for row in rows}
    listed: dict[str, tuple[str, str]] = {}
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        if not raw.strip() or raw.startswith("#"):
            continue
        parts = raw.split(" | ", 2)
        if len(parts) != 3 or not re.fullmatch(r"G[1-9]", parts[1]) or not parts[2].strip():
            raise SystemExit(f"{path}:{number}: expected 'cause | G1..G9 | reason'")
        cause, group, reason = parts
        if cause not in causes:
            raise SystemExit(f"{path}:{number}: no Fatal Runtime Errors row has the cause {cause!r}")
        if cause in listed:
            raise SystemExit(f"{path}:{number}: the cause {cause!r} is listed twice")
        listed[cause] = (group, reason)
    return listed


def check_row(row: Row, index: int, args, workdir: Path,
              bootstrap_env: dict) -> tuple[list[str], list[str]]:
    """Return (native problems, bootstrap mismatches) for one row."""
    native_problems: list[str] = []
    mismatches: list[str] = []
    for variant in VARIANTS:
        tag = f"row{index:02d}_{variant}"
        tmp_file = str(workdir / f"{tag}.txt")
        program = build_program(row, variant, tmp_file)
        native = run_program([args.native], program, workdir / f"{tag}_n.lox",
                             args.timeout, None)
        bootstrap = run_program([args.bootstrap], program, workdir / f"{tag}_b.lox",
                                args.timeout, bootstrap_env)
        if native.timed_out:
            native_problems.append(f"{variant}: native timed out")
            continue
        if native.exit_code != FATAL_EXIT:
            native_problems.append(f"{variant}: native exit {native.exit_code}, expected {FATAL_EXIT}")
        if native.stdout:
            native_problems.append(f"{variant}: native stdout {native.stdout!r}, expected none")
        if native.stderr_first != row.message:
            native_problems.append(
                f"{variant}: native first stderr line {native.stderr_first!r}, "
                f"spec message {row.message!r}")
        if bootstrap.timed_out:
            mismatches.append(f"{variant}: bootstrap timed out")
            continue
        if bootstrap.exit_code != native.exit_code:
            mismatches.append(f"{variant}: exit {bootstrap.exit_code}, native {native.exit_code}")
        if bootstrap.stdout != native.stdout:
            mismatches.append(f"{variant}: stdout {bootstrap.stdout!r}, native {native.stdout!r}")
        if bootstrap.stderr_first != native.stderr_first:
            mismatches.append(
                f"{variant}: first stderr line {bootstrap.stderr_first!r}, "
                f"native {native.stderr_first!r}")
    return native_problems, mismatches


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--native", default=str(REPO_ROOT / "build" / "loxpp"))
    parser.add_argument("--bootstrap",
                        default=str(REPO_ROOT / "bootstrap" / "lox_wrapper.sh"))
    parser.add_argument("--xfail", default=str(XFAIL_PATH))
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument(
        "--trace-enabled",
        action="store_true",
        default=os.environ.get("LOXPP_BOOTSTRAP_TRACE_ENABLED") == "1",
        help="skip (ctest SKIP_RETURN_CODE) instead of running: the native binary "
        "under test was built with LOXPP_DEBUG_TRACE_EXECUTION/LOXPP_DEBUG_PRINT_CODE on",
    )
    args = parser.parse_args()

    if args.trace_enabled:
        print("SKIP: native binary was built with debug trace macros on (release preset required)")
        return SKIP_RETURN_CODE

    rows = parse_spec_rows(SPEC_PATH.read_text())
    listed = read_xfail(Path(args.xfail), rows)
    bootstrap_env = {**os.environ, "LANGUAGE": "LOXPP"}

    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(
                lambda item: check_row(item[1], item[0], args, workdir, bootstrap_env),
                enumerate(rows, 1)))

    failures: list[str] = []
    passing = xfailing = 0
    for number, (row, (native_problems, mismatches)) in enumerate(zip(rows, results), 1):
        label = f"row {number}: {row.cause}"
        if native_problems:
            failures.append(f"{label}\n  native does not follow the spec:\n    "
                            + "\n    ".join(native_problems))
        elif mismatches and row.cause not in listed:
            failures.append(f"{label}\n  the bootstrap differs from native and the row is not listed:\n    "
                            + "\n    ".join(mismatches))
        elif not mismatches and row.cause in listed:
            failures.append(f"{label}\n  the row passes but is still listed in "
                            f"{Path(args.xfail).name}; remove the line")
        elif mismatches:
            xfailing += 1
        else:
            passing += 1

    if failures:
        print(f"{len(failures)}/{len(rows)} row(s) fail:\n")
        print("\n\n".join(failures))
        return 1

    print(f"OK: {len(rows)} Fatal Runtime Errors rows: {passing} match native, "
          f"{xfailing} are listed as expected failures.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
