#!/usr/bin/env python3
"""
Lightweight FileCheck-style tester for Lox++ example programs.

For each examples/*.lox file (alphabetical order):
  1. Extract all '// CHECK: <text>' directives in file order.
  2. If no CHECK directives are found, skip the file.
  3. If examples/<name>.input exists, pipe it as stdin.
  4. Run loxpp and capture stdout.
  5. For each CHECK directive, scan forward through the remaining output
     lines until a stripped line matches the CHECK text exactly.
  6. Report PASS / FAIL / SKIP per file, then exit 1 if any failed.

Usage:
    python3 tools/check_examples.py <loxpp-binary> <examples-dir>
"""

import re
import subprocess
import sys
from pathlib import Path


def extract_checks(lox_file: Path) -> list[str]:
    checks = []
    for line in lox_file.read_text(encoding="utf-8").splitlines():
        m = re.match(r"\s*//\s*CHECK:\s*(.*)", line)
        if m:
            checks.append(m.group(1).rstrip())
    return checks


def run_example(loxpp: str, lox_file: Path, input_file: Path) -> tuple[int, list[str]]:
    stdin_data = input_file.read_text(encoding="utf-8") if input_file.exists() else None
    result = subprocess.run(
        [loxpp, str(lox_file)],
        input=stdin_data,
        capture_output=True,
        text=True,
    )
    return result.returncode, result.stdout.splitlines()


def match_checks(checks: list[str], lines: list[str]) -> tuple[bool, str | None]:
    """Match each CHECK against output lines in order (non-adjacent ok)."""
    cursor = 0
    for check in checks:
        matched = False
        while cursor < len(lines):
            if lines[cursor].strip() == check:
                cursor += 1
                matched = True
                break
            cursor += 1
        if not matched:
            return False, check
    return True, None


def main() -> None:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <loxpp> <examples-dir>", file=sys.stderr)
        sys.exit(2)

    loxpp = sys.argv[1]
    examples_dir = Path(sys.argv[2])

    passed = failed = skipped = 0

    for lox_file in sorted(examples_dir.glob("*.lox")):
        checks = extract_checks(lox_file)
        if not checks:
            print(f"SKIP  {lox_file.name}  (no CHECK directives)")
            skipped += 1
            continue

        input_file = lox_file.with_suffix(".input")
        rc, lines = run_example(loxpp, lox_file, input_file)

        if rc != 0:
            print(f"FAIL  {lox_file.name}  (exit code {rc})")
            failed += 1
            continue

        ok, bad_check = match_checks(checks, lines)
        if ok:
            print(f"PASS  {lox_file.name}  ({len(checks)} checks)")
            passed += 1
        else:
            print(f"FAIL  {lox_file.name}  (unmatched CHECK: '{bad_check}')")
            print(f"      actual output:")
            for line in lines:
                print(f"        {line!r}")
            failed += 1

    print(f"\n{passed} passed, {failed} failed, {skipped} skipped")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
