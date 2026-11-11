#!/usr/bin/env python3
"""Checks the strings-the-host-reads set of spec/05-stdlib.md against its consumers.

A String that a host reads to find something outside the program — a path, an
environment variable, a program, or a host — is read only up to its first NUL
byte, so a String that holds one names a shorter, different thing (issue
#1158, issues #1217 and #1220). `## Strings the host reads` decides the case
once, in a table, and every call in the table states the rule for itself. The
set is countable, and a call left out of the rule is the failure this script
guards against.

The script reads four things and compares them:

- The table. Each row names one argument of one or more calls.
- The spec. Every heading whose signature takes an argument of the table must
  be a call of the table, and its section must carry the rule sentence for that
  argument. Every call of the table must have such a heading. The reverse check
  skips an argument name that other headings take too — `name` is also a
  reflection parameter — because only a distinctive name can identify a new
  host-read call on its own.
- The probes. Each call needs a spec probe with `// COVERS: host-string
  <call>`, and a COVERS line that names a call no row has fails.
- The consumers. Native checks the String with `stringHoldsNul(value,
  "<call>")` (src/stdlib/host_string_arg.h), and the JVM refuses it with
  `requireName("<call>", …)` or `resolvePath("<call>", …)`
  (runtime/jvm/src/lox/LoxRuntime.java). Every call must do both, and neither
  may do it for a call the table does not list.

Proving this check can fail: pass a copy of the spec, the probes, or the
sources with one row, rule sentence, COVERS line, or call site removed, and see
it fail.

Usage:
    tools/check_host_string_rows.py [--spec PATH] [--probes DIR]
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
    code,
    probe_covers,
    report,
    section,
    table,
)

SCRIPT = "check_host_string_rows.py"
PATH_SECTION = "## Strings the host reads"
STDLIB_DIR = REPO_ROOT / "src" / "stdlib"
JVM_DIR = REPO_ROOT / "runtime" / "jvm" / "src" / "lox"

HEADING_RE = re.compile(r"^(#{2,3}) (.*)$")
SIGNATURE_RE = re.compile(r"^([A-Za-z_]\w*)\(([^)]*)\)")
NATIVE_SITE_RE = re.compile(r'stringHoldsNul\([^;\n]*"([a-z_]+)"')
JVM_SITE_RE = re.compile(r'(?:requireName|resolvePath)\("([a-z_]+)"')
RULE = "A `{}` that holds a NUL byte is a runtime error"


def headings(text: str) -> dict[str, tuple[str, set[str]]]:
    """Returns each call the spec gives a section to, mapped to its heading
    line and the parameter names its signature takes."""
    found: dict[str, tuple[str, set[str]]] = {}
    for line in text.splitlines():
        heading = HEADING_RE.match(line)
        if not heading:
            continue
        for span in re.findall(r"`([^`]+)`", heading.group(2)):
            m = SIGNATURE_RE.match(span)
            if not m:
                continue
            params = {p.strip() for p in m.group(2).split(",") if p.strip()}
            found[m.group(1)] = (line.rstrip(), {p.split()[0] for p in params})
    return found


def sites(directory: Path, suffix: str, pattern: re.Pattern[str]) -> set[str]:
    """Returns the call names `pattern` captures across `directory`."""
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
        rows = table(section(text, PATH_SECTION), "Call")
        calls = headings(text)
        covers = probe_covers(Path(args.probes))
        native = sites(Path(args.stdlib), ".cpp", NATIVE_SITE_RE)
        jvm = sites(Path(args.jvm), ".java", JVM_SITE_RE)
    except SpecError as exc:
        report(SCRIPT, [f"cannot read the spec: {exc}"], "")
        return 1

    listed: dict[str, str] = {}  # call -> the argument the table gives it
    arguments: set[str] = set()
    for row in rows:
        try:
            argument = code(row["Argument"])
            spans = re.findall(r"`([^`]+)`", row["Call"])
        except SpecError as exc:
            problems.append(str(exc))
            continue
        if not spans:
            problems.append(f"a row names no call: {row['Call']!r}")
        arguments.add(argument)
        for span in spans:
            m = SIGNATURE_RE.match(span)
            if not m:
                problems.append(f"a row cell is not a call: {span!r}")
                continue
            call = m.group(1)
            if call in listed:
                problems.append(f"{call}() appears in two rows")
            listed[call] = argument

    if not listed:
        problems.append("no call takes a String that the host reads")
    if PATH_SECTION not in text:
        problems.append(f"the spec has no {PATH_SECTION!r} section")

    # A parameter name that only one heading takes identifies a host-read call
    # on its own; `name` is also a reflection parameter, so it cannot.
    taken: dict[str, int] = {}
    for _, params in calls.values():
        for param in params:
            taken[param] = taken.get(param, 0) + 1
    distinctive = {param for param, count in taken.items() if count == 1}

    for call, (heading, params) in sorted(calls.items()):
        named = (params & arguments) & distinctive
        if named and call not in listed:
            problems.append(
                f"{heading} takes {', '.join(sorted(named))}, which the table "
                "of Strings the host reads does not list")
        if call not in listed:
            continue
        argument = listed[call]
        if argument not in params:
            problems.append(
                f"{call}() is a row of the table, but {heading} takes no "
                f"{argument!r} argument")
        # The rule may wrap across lines, so compare on joined white space.
        body = re.sub(r"\s+", " ", section(text, heading))
        rule = RULE.format(argument)
        if rule not in body:
            problems.append(
                f"{call}() takes a `{argument}` but its section does not state "
                f"'{rule}'")
        if ("host-string", call) not in covers:
            problems.append(f"{call}() has no spec probe with "
                            f"'// COVERS: host-string {call}'")
        if call not in native:
            problems.append(f"{call}() has no native 'stringHoldsNul(value, "
                            f"\"{call}\")' check")
        if call not in jvm:
            problems.append(f"{call}() has no JVM 'requireName(\"{call}\", …)' "
                            f"or 'resolvePath(\"{call}\", …)' call")

    for call in sorted(listed):
        if call not in calls:
            problems.append(f"the table names {call}(), which has no section")

    for words in sorted(covers):
        if words[0] != "host-string":
            continue
        if len(words) != 2 or words[1] not in listed:
            problems.append("COVERS line names no call of the table: "
                            + " ".join(words))
    for call in sorted(native):
        if call not in listed:
            problems.append(f"stringHoldsNul() checks {call}(), which the table "
                            "does not list")
    for call in sorted(jvm):
        if call not in listed:
            problems.append(f"requireName() or resolvePath() serves {call}(), "
                            "which the table does not list")

    report(SCRIPT, problems,
           f"{len(listed)} calls take a String the host reads, each with the "
           "rule sentence, a spec probe, a native check, and a JVM refusal")
    return 0


if __name__ == "__main__":
    sys.exit(main())
