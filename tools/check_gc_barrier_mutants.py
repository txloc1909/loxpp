#!/usr/bin/env python3
"""Mutants of the heap headers for tools/check_gc_barrier_table.py.

Usage: check_gc_barrier_mutants.py

Each mutant copies the checked files of the repository to a temporary tree,
adds one public function to a heap object type, and runs the barrier check on
the copy. The check must fail and must name the function. A mutant that the
check accepts is a hole in the check.

Exit status: 0 pass, 1 fail.
"""

import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CHECK = os.path.join(ROOT, "tools", "check_gc_barrier_table.py")

# (name, file, text after which the function goes, function, name in the message)
MUTANTS = [
    (
        "double pointer to a slot",
        "src/container_objects.h",
        "[[nodiscard]] ObjTask* readHolder() const { return m_readHolder; }",
        "ObjTask** readHolderSlot() { return &m_readHolder; }",
        "ObjSocket::readHolderSlot",
    ),
]


def run_check(root):
    p = subprocess.run(
        [sys.executable, CHECK, "--root", root], capture_output=True, text=True
    )
    return p.returncode, p.stdout + p.stderr


def main():
    bad = []
    for name, rel, anchor, func, expect in MUTANTS:
        with tempfile.TemporaryDirectory() as tmp:
            root = os.path.join(tmp, "tree")
            for d in ("src", "docs/reference"):
                shutil.copytree(os.path.join(ROOT, d), os.path.join(root, d),
                                ignore=shutil.ignore_patterns("*.lox"))
            path = os.path.join(root, rel)
            text = open(path).read()
            if text.count(anchor) != 1:
                print(f"FAIL: mutant '{name}': the anchor is not in {rel} once")
                sys.exit(1)
            open(path, "w").write(text.replace(anchor, anchor + "\n    " + func))
            code, out = run_check(root)
            if code != 1 or expect not in out:
                bad.append(name)
                print(f"FAIL: mutant '{name}' was not refused (exit {code}): {out.strip()}")
            else:
                print(f"ok: mutant '{name}' is refused")
    if bad:
        sys.exit(1)
    print("OK: every mutant is refused")


if __name__ == "__main__":
    main()
