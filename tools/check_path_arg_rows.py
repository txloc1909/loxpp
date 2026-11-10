#!/usr/bin/env python3
"""Checks the path-taking calls of spec/05-stdlib.md against their consumers.

A path that holds a NUL byte names a shorter path at the host's file-system
interface, so the program cannot name it back and a consumer that passes the
String through acts on a different path (issue #1158). `## Paths` decides the
case once, and every function that takes a `path` states it for itself, so the
set of functions is countable and a function left out of the rule is the
failure this script guards against.

The script derives the set from the spec headings, then compares it with three
things:

- The spec itself. Every heading whose signature takes a `path` must carry the
  rule sentence of `## Paths`, and `## Paths` must name every one of them.
- The probes. Each call needs a spec probe with `// COVERS: path <call>`, and a
  COVERS line that names a call no heading has fails.
- The consumers. Native checks the String with `pathHoldsNul(path, "<call>")`
  (src/stdlib/path_arg.h), and the JVM resolves it with
  `resolvePath("<call>", path)` (runtime/jvm/src/lox/LoxRuntime.java). Every
  call must do both, and neither may do it for a call the spec does not list.

Proving this check can fail: pass a copy of the spec, the probes, or the
sources with one rule sentence, COVERS line, or call site removed, and see it
fail.

Usage:
    tools/check_path_arg_rows.py [--spec PATH] [--probes DIR]
                                 [--stdlib DIR] [--jvm DIR]
"""

import argparse
import re
import sys
from pathlib import Path

from spec_tables import (
    PROBE_DIR,
    REPO_ROOT,
    SPEC_DIR,
    SpecError,
    probe_covers,
    report,
    section,
)

SCRIPT = "check_path_arg_rows.py"
STDLIB_DIR = REPO_ROOT / "src" / "stdlib"
JVM_DIR = REPO_ROOT / "runtime" / "jvm" / "src" / "lox"

HEADING_RE = re.compile(r"^(#{2,3}) (.*)$")
SIGNATURE_RE = re.compile(r"^([A-Za-z_]\w*)\(([^)]*)\)")
PATH_RULE = "A `path` that holds a NUL byte is a runtime error"
NATIVE_SITE_RE = re.compile(r'pathHoldsNul\([^;\n]*"([a-z_]+)"')
JVM_SITE_RE = re.compile(r'resolvePath\("([a-z_]+)"')


def path_calls(text: str) -> dict[str, str]:
    """Returns each call whose signature takes a `path`, mapped to its heading."""
    calls: dict[str, str] = {}
    for line in text.splitlines():
        heading = HEADING_RE.match(line)
        if not heading:
            continue
        for span in re.findall(r"`([^`]+)`", heading.group(2)):
            m = SIGNATURE_RE.match(span)
            if not m:
                continue
            params = [p.strip() for p in m.group(2).split(",") if p.strip()]
            if "path" in params:
                calls[m.group(1)] = line.rstrip()
    return calls


def sites(directory: Path, suffix: str, pattern: re.Pattern[str]) -> set[str]:
    """Returns the function names `pattern` captures across `directory`."""
    found: set[str] = set()
    for path in sorted(directory.glob("*" + suffix)):
        found.update(pattern.findall(path.read_text(encoding="utf-8")))
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--spec", help="path to a copy of spec/05-stdlib.md")
    parser.add_argument("--probes", default=str(PROBE_DIR),
                        help="a directory of spec probes")
    parser.add_argument("--stdlib", default=str(STDLIB_DIR),
                        help="a directory of native stdlib sources")
    parser.add_argument("--jvm", default=str(JVM_DIR),
                        help="a directory of JVM runtime sources")
    args = parser.parse_args()

    problems: list[str] = []
    try:
        text = (Path(args.spec) if args.spec else SPEC_DIR / "05-stdlib.md").read_text(
            encoding="utf-8")
        calls = path_calls(text)
        paths_body = section(text, "## Paths")
        bodies = {call: section(text, heading) for call, heading in calls.items()}
        covers = probe_covers(Path(args.probes))
        native = sites(Path(args.stdlib), ".cpp", NATIVE_SITE_RE)
        jvm = sites(Path(args.jvm), ".java", JVM_SITE_RE)
    except SpecError as exc:
        report(SCRIPT, [f"cannot read the spec: {exc}"], "")
        return 1

    if not calls:
        problems.append("no heading in the spec takes a `path` argument")

    for call in sorted(calls):
        if PATH_RULE not in bodies[call]:
            problems.append(
                f"{call}() takes a `path` but its section does not state "
                f"'{PATH_RULE}'")
        if not re.search(rf"`{re.escape(call)}`", paths_body):
            problems.append(f"the Paths section does not name {call}()")
        if ("path", call) not in covers:
            problems.append(f"{call}() has no spec probe with "
                            f"'// COVERS: path {call}'")
        if call not in native:
            problems.append(f"{call}() has no native 'pathHoldsNul(path, "
                            f"\"{call}\")' check")
        if call not in jvm:
            problems.append(f"{call}() has no JVM 'resolvePath(\"{call}\", "
                            "path)' call")

    for words in sorted(covers):
        if words[0] != "path":
            continue
        if len(words) != 2 or words[1] not in calls:
            problems.append("COVERS line names no path-taking call of the "
                            "spec: " + " ".join(words))
    for call in sorted(native):
        if call not in calls:
            problems.append(f"pathHoldsNul() checks {call}(), which the spec "
                            "does not list as taking a `path`")
    for call in sorted(jvm):
        if call not in calls:
            problems.append(f"resolvePath() serves {call}(), which the spec "
                            "does not list as taking a `path`")

    report(SCRIPT, problems,
           f"{len(calls)} path-taking calls, each with the rule sentence, a "
           "spec probe, a native check, and a JVM resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
