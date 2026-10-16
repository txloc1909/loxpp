#!/usr/bin/env python3
"""
docs/README.md defines the document tiers. Nothing here is hand-kept: every
check is derived from the tree.

  1. docs/ holds README.md and the known tiers only. Every file under a tier
     is a markdown file.
  2. A decision carries a line that starts with `**Status:** accepted` and a
     proposal a line that starts with `**Status:** proposal`, so the status
     line and the directory cannot disagree.
  3. No tracked text file refers to the old notes directory. It is gone, so
     such a reference points at nothing.
  4. Every path reference to a markdown file resolves. A markdown link
     `[..](x.md)` resolves relative to the referring file; a backticked or
     bare `docs/.../x.md` or `spec/x.md` resolves from the repository root.

Pure Python, no build. Exits 1 and lists every finding otherwise.
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
TIERS = ("decisions", "proposals", "reference", "process", "records")
STATUS = {"decisions": "**Status:** accepted", "proposals": "**Status:** proposal"}
SELF = Path(__file__).resolve().relative_to(ROOT).as_posix()

OLD_DIR = re.compile(r"(?<![\w-])" + "notes" + "/")
ROOT_PATH = re.compile(r"(?<![\w./-])((?:docs|spec)/[\w./-]+?\.md)(?![\w-])")
MD_LINK = re.compile(r"\]\(([^)\s]+?\.md)(?:#[^)]*)?\)")


def tracked_text_files():
    out = subprocess.run(["git", "ls-files", "-z", "--cached", "--others",
                          "--exclude-standard"], cwd=ROOT, check=True,
                         capture_output=True).stdout
    for rel in out.decode("utf-8").split("\0"):
        if not rel:
            continue
        path = ROOT / rel
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        yield rel, path, text


def main():
    findings = []

    # 1. Only README.md and the known tiers under docs/.
    for entry in sorted(DOCS.iterdir()):
        if entry.name == "README.md":
            continue
        if entry.is_dir() and entry.name in TIERS:
            for file in sorted(entry.rglob("*")):
                if file.is_file() and file.suffix != ".md":
                    findings.append(f"{file.relative_to(ROOT)}: not a markdown file")
            continue
        findings.append(f"{entry.relative_to(ROOT)}: outside the known tiers {TIERS}")

    # 2. Status line agrees with the directory.
    for tier, prefix in STATUS.items():
        for file in sorted((DOCS / tier).glob("*.md")):
            lines = file.read_text(encoding="utf-8").split("\n")
            if not any(line.startswith(prefix) for line in lines):
                findings.append(
                    f"{file.relative_to(ROOT)}: no line starts with `{prefix}`")

    # 3 and 4. References in every tracked text file.
    for rel, path, text in tracked_text_files():
        if rel == SELF:
            continue
        for lineno, line in enumerate(text.split("\n"), 1):
            if OLD_DIR.search(line):
                findings.append(f"{rel}:{lineno}: refers to the removed notes directory")
            targets = [(t, ROOT / t) for t in ROOT_PATH.findall(line)]
            if rel.endswith(".md"):
                targets += [(t, path.parent / t) for t in MD_LINK.findall(line)
                            if not t.startswith(("http://", "https://"))]
            for ref, target in targets:
                if not target.is_file():
                    findings.append(f"{rel}:{lineno}: `{ref}` names no file")

    if findings:
        print(f"check_docs_placement.py: {len(findings)} finding(s):", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print("check_docs_placement.py: every document is in its tier with a "
          "matching status, and every path reference resolves")
    return 0


if __name__ == "__main__":
    sys.exit(main())
