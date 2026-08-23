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
    python3 tools/check_examples.py <loxpp-binary> <examples-dir> [--exclude <file>]

--exclude <file> names a file with one excluded example per line: the file
name, then a reason. tools/jvm_excluded_examples.txt and
tools/clr_excluded_examples.txt are two such files — the map-order-sensitive
examples each managed backend legitimately reorders (spec leaves map
iteration order unspecified). An excluded file is skipped, not run, and
reported as SKIP with its reason.
"""

import re
import subprocess
import sys
from pathlib import Path


def parse_exclude_file(path: Path) -> dict[str, str]:
    """Reads a 'name  reason' exclusion list. Blank lines and lines starting
    with '#' are comments."""
    reasons: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        name, _, reason = stripped.partition(" ")
        reasons[name] = reason.strip()
    return reasons


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
    args = sys.argv[1:]
    exclude_path: Path | None = None
    if "--exclude" in args:
        i = args.index("--exclude")
        if i + 1 >= len(args):
            print(f"Usage: {sys.argv[0]} <loxpp> <examples-dir> [--exclude <file>]", file=sys.stderr)
            sys.exit(2)
        exclude_path = Path(args[i + 1])
        del args[i : i + 2]

    if len(args) != 2:
        print(f"Usage: {sys.argv[0]} <loxpp> <examples-dir> [--exclude <file>]", file=sys.stderr)
        sys.exit(2)

    loxpp, examples_dir = args[0], Path(args[1])
    excluded = parse_exclude_file(exclude_path) if exclude_path else {}

    passed = failed = skipped = 0

    for lox_file in sorted(examples_dir.glob("*.lox")):
        if lox_file.name in excluded:
            print(f"SKIP  {lox_file.name}  (excluded: {excluded[lox_file.name]})")
            skipped += 1
            continue

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
