#!/usr/bin/env python3
"""Parse clang-tidy output and emit a categorized markdown report to stdout."""

import re
import sys
from collections import defaultdict

CATEGORY_ORDER = [
    "clang-diagnostic",
    "clang-analyzer",
    "modernize",
    "readability",
    "performance",
]


def categorize(check: str) -> str:
    for prefix in CATEGORY_ORDER:
        if check.startswith(prefix):
            return prefix
    return "other"


def main() -> None:
    path = sys.argv[1] if len(sys.argv) > 1 else "-"
    content = sys.stdin.read() if path == "-" else open(path).read()

    pattern = re.compile(
        r"^.+:\d+:\d+:\s+(warning|error):\s+.+\[([^\]]+)\]$", re.MULTILINE
    )

    # {category: {severity: int, checks: {name: int}}}
    cats: dict = defaultdict(lambda: {"warning": 0, "error": 0, "checks": defaultdict(int)})

    for m in pattern.finditer(content):
        severity, check = m.group(1), m.group(2)
        cat = categorize(check)
        cats[cat][severity] += 1
        cats[cat]["checks"][check] += 1

    total_e = sum(v["error"] for v in cats.values())
    total_w = sum(v["warning"] for v in cats.values())

    print("# clang-tidy Report\n")
    print(f"**{total_e} errors, {total_w} warnings** across all checks\n")

    print("## Summary\n")
    print("| Category | Errors | Warnings | Total |")
    print("|---|---|---|---|")
    ordered = sorted(cats.keys(), key=lambda c: CATEGORY_ORDER.index(c) if c in CATEGORY_ORDER else 99)
    for cat in ordered:
        d = cats[cat]
        e, w = d["error"], d["warning"]
        e_str = f"**{e}**" if e else str(e)
        print(f"| `{cat}` | {e_str} | {w} | {e + w} |")

    print("\n## Details\n")
    for cat in ordered:
        d = cats[cat]
        print(f"### `{cat}` — {d['error']} errors, {d['warning']} warnings\n")
        print("| Check | Count |")
        print("|---|---|")
        for check, count in sorted(d["checks"].items(), key=lambda x: -x[1]):
            print(f"| `{check}` | {count} |")
        print()


if __name__ == "__main__":
    main()
