#!/usr/bin/env python3
"""Checks that test/test_labels.txt labels every CTest test, and nothing else.

test/test_labels.txt gives each test the consumers it can fail for, so CI can
run a subset with `ctest -L <label>` (issue #1258). This script reads
test/CMakeLists.txt and the table as text, so it needs no build. It fails when:

- a GTest target in LOXPP_GTEST_TARGETS, or a test that add_test(NAME ...)
  registers, has no line in the table;
- a line names no such test;
- a name has two lines;
- a line has no label, or a label that the header of the table does not define.

Exit status 0 when the table is complete, 1 when it is not.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CMAKE = ROOT / "test" / "CMakeLists.txt"
TABLE = ROOT / "test" / "test_labels.txt"


def parse_table(text):
    """Returns (vocabulary, {name: [labels]}, errors)."""
    vocabulary = set()
    rows = {}
    errors = []
    for number, line in enumerate(text.splitlines(), 1):
        defined = re.match(r"#   ([a-z][a-z-]*)\s{2,}\S", line)
        if defined:
            vocabulary.add(defined.group(1))
            continue
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 2:
            errors.append(f"{TABLE.name}:{number}: want '<name> <label>[,<label>...]', got {line!r}")
            continue
        name, labels = fields
        if name in rows:
            errors.append(f"{TABLE.name}:{number}: {name} has a second line")
        rows[name] = labels.split(",")
    return vocabulary, rows, errors


def registered_tests(text):
    """Returns the GTest targets and the add_test() names of test/CMakeLists.txt."""
    no_comments = re.sub(r"(?m)#.*$", "", text)
    targets = []
    block = re.search(r"set\(LOXPP_GTEST_TARGETS(.*?)\)", no_comments, re.S)
    if block:
        targets += block.group(1).split()
    for appended in re.finditer(r"list\(APPEND LOXPP_GTEST_TARGETS\s+([^)]*)\)", no_comments):
        targets += appended.group(1).split()
    named = re.findall(r"add_test\(\s*NAME\s+(\S+)", no_comments)
    return targets, named


def check(cmake_text, table_text):
    vocabulary, rows, errors = parse_table(table_text)
    if not vocabulary:
        errors.append(f"{TABLE.name}: the header defines no label")
    targets, named = registered_tests(cmake_text)
    if not targets:
        errors.append("test/CMakeLists.txt: no LOXPP_GTEST_TARGETS found")
    registered = set(targets) | set(named)
    for name in sorted(registered - rows.keys()):
        errors.append(f"{TABLE.name}: no line for the test {name}")
    for name in sorted(rows.keys() - registered):
        errors.append(f"{TABLE.name}: the line for {name} names no test")
    for name, labels in sorted(rows.items()):
        for label in labels:
            if label not in vocabulary:
                errors.append(f"{TABLE.name}: {name} has the label {label!r}, which the header does not define")
    return errors


def main():
    errors = check(CMAKE.read_text(), TABLE.read_text())
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 1
    print("every test has labels")
    return 0


if __name__ == "__main__":
    sys.exit(main())
