#!/usr/bin/env python3
"""Checks the two countable sets of docs/decisions/blocking-call-suspension.md
against the spec, and checks that each set has probes.

The rules:

  - The decision's mechanism table (first header cell `Call`) lists the calls
    that suspend their task. Its set of calls must equal the set of rows of
    the Blocking calls table in spec/05-stdlib.md whose `Class` cell is
    `suspends`.
  - The decision's table of contexts where a task cannot suspend (first header
    cell `Context`) must list the same contexts as the table of that name in
    spec/04-semantics.md, with the same text in the second column.
  - Every COVERS line must be backed by the probe: the probe must name the
    covered call or context in its code, outside comments (CODE_MARKERS and
    CONTEXT_MARKERS). A COVERS line for a call ends with `in <function>`, and
    the call must sit in the body of that function of the probe, so a call
    that the probe uses only to set up does not count. A label on a probe that
    does not use the call fails.
  - Every call of class `suspends` has a spec probe with the line
    `// COVERS: suspend-call <call>`, where <call> is the name that
    check_blocking_calls.py uses (for example `Socket.readline`).
  - Every context has a spec probe with `// COVERS: suspend-context <slug>`.
    CONTEXT_SLUGS gives the slug of each context.
  - Each call in BLOCKS_PROBED, which is of class `blocks`, has a spec probe
    with `// COVERS: blocks-call <call>`.

A row removed from either side, a context the script has no slug for, and a
row with no probe each fail.

Usage:
    tools/check_blocking_decision_tables.py [--decision PATH] [--semantics PATH]
                                            [--stdlib PATH] [--probes DIR]
"""

import argparse
import re
from pathlib import Path

from check_blocking_calls import call_name
from spec_tables import (
    PROBE_DIR,
    REPO_ROOT,
    SpecError,
    code,
    probe_covers,
    report,
    section,
    spec_path,
    table,
)

CONTEXT_SLUGS = {
    "`__str__`": "str",
    "a map key's `__hash__` and `__eq__`": "map-key",
    "`reflect.callMethod`": "call-method",
    "a receive condition": "receive-want",
    "a deferred call": "defer",
    "a native entry": "native-entry",
    "a nested coroutine": "nested-coroutine",
}

# The calls of class blocks that the decision requires a probe for.
BLOCKS_PROBED = ["input", "run", "Process.readline"]

# A regular expression that the code of a probe (comments removed) must match
# for a COVERS line to count, by the last part of the call name.
CODE_MARKERS = {
    "sleep": r"\bsleep\(",
    "connect": r"\bconnect\(",
    "input": r"\binput\(",
    "run": r"\brun\(",
    "read": r"\.read\(",
    "read_bytes": r"\.read_bytes\(",
    "readline": r"\.readline\(",
    "readlines": r"\.readlines\(",
    "write": r"\.write\(",
    "writeline": r"\.writeline\(",
    "accept": r"\.accept\(",
}
CONTEXT_MARKERS = {
    "str": r"__str__",
    "map-key": r"__hash__",
    "call-method": r"\bcallMethod\(",
    "receive-want": r"\btask\.receive_where(?:_within)?\(",
    "defer": r"\bdefer\b",
    "native-entry": r"task\.spawn\(\s*\w+\.\w+\s*\)",
    "nested-coroutine": r"coroutine\.create\(",
}

DECISION = REPO_ROOT / "docs" / "decisions" / "blocking-call-suspension.md"


def function_body(code_text: str, name: str) -> str | None:
    """Returns the body of `fun <name>(...) { ... }` in `code_text`, or None."""
    m = re.search(r"\bfun\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", code_text)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(code_text) and depth:
        depth += {"{": 1, "}": -1}.get(code_text[i], 0)
        i += 1
    return code_text[m.end() : i]


def has_call_cover(covers: dict, kind: str, name: str) -> bool:
    return any(words[:2] == (kind, name) for words in covers)


def names(rows: list[dict[str, str]], problems: list[str], where: str) -> list[str]:
    found: list[str] = []
    for row in rows:
        try:
            name = call_name(code(row["Call"]), None)
        except SpecError as exc:
            problems.append(f"{where}: {exc}")
            continue
        if name is None:
            problems.append(f"{where}: row {row['Call']!r} does not name a call")
        else:
            found.append(name)
    return found


def compare(left: list[str], right: list[str], left_name: str, right_name: str, problems: list[str]) -> None:
    for item in left:
        if item not in right:
            problems.append(f"{item!r} is in {left_name} but not in {right_name}")
    for item in right:
        if item not in left:
            problems.append(f"{item!r} is in {right_name} but not in {left_name}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--decision", help="path to a copy of the decision")
    parser.add_argument("--semantics", help="path to a copy of spec/04-semantics.md")
    parser.add_argument("--stdlib", help="path to a copy of spec/05-stdlib.md")
    parser.add_argument("--probes", default=str(PROBE_DIR), help="the spec probe directory")
    args = parser.parse_args()

    problems: list[str] = []
    try:
        decision = Path(args.decision or DECISION).read_text(encoding="utf-8")
        sem = spec_path("04-semantics.md", args.semantics).read_text(encoding="utf-8")
        std = spec_path("05-stdlib.md", args.stdlib).read_text(encoding="utf-8")
        mechanism = table(section(decision, "## Scope"), "Call")
        decision_contexts = table(decision, "Context")
        spec_contexts = table(
            section(section(sem, "## Workers and Tasks"), "### Contexts where a task cannot suspend"),
            "Context",
        )
        blocking_rows = table(section(std, "## Blocking calls"), "Call")
        covers = probe_covers(Path(args.probes))
    except SpecError as exc:
        report("check_blocking_decision_tables.py", [f"cannot read the inputs: {exc}"], "")
        return

    decided = names(mechanism, problems, "decision mechanism table")
    suspends = names([r for r in blocking_rows if r["Class"] == "suspends"], problems, "spec table")
    blocks = names([r for r in blocking_rows if r["Class"] == "blocks"], problems, "spec table")
    compare(decided, suspends, "the decision mechanism table", "the class-suspends rows of the spec", problems)

    spec_text = {r["Context"]: r["The runtime calls Lox++ code for"] for r in spec_contexts}
    for r in decision_contexts:
        other = spec_text.get(r["Context"])
        if other is not None and other != r["The runtime calls Lox++ code for"]:
            problems.append(f"context {r['Context']!r}: the text differs between the decision and the spec")
    in_decision = [r["Context"] for r in decision_contexts]
    in_spec = [r["Context"] for r in spec_contexts]
    compare(in_decision, in_spec, "the decision contexts table", "the spec contexts table", problems)

    for context in in_spec:
        slug = CONTEXT_SLUGS.get(context)
        if slug is None:
            problems.append(f"context {context!r} has no slug in CONTEXT_SLUGS")
        elif ("suspend-context", slug) not in covers:
            problems.append(f"context {context!r} has no probe with '// COVERS: suspend-context {slug}'")
    for context in CONTEXT_SLUGS:
        if context not in in_spec:
            problems.append(f"CONTEXT_SLUGS names {context!r}, which the spec table does not list")

    for name in suspends:
        if not has_call_cover(covers, "suspend-call", name):
            problems.append(f"call {name!r} has no probe with '// COVERS: suspend-call {name} in <function>'")
    for name in BLOCKS_PROBED:
        if name not in blocks:
            problems.append(f"BLOCKS_PROBED names {name!r}, which is not a class-blocks row of the spec")
        elif not has_call_cover(covers, "blocks-call", name):
            problems.append(f"call {name!r} has no probe with '// COVERS: blocks-call {name} in <function>'")

    for words, paths in covers.items():
        if words[0] not in ("suspend-call", "suspend-context", "blocks-call"):
            continue
        kind, key = words[0], words[1]
        function = None
        if kind != "suspend-context":
            if len(words) != 4 or words[2] != "in":
                problems.append(f"COVERS {' '.join(words)}: a call needs 'in <function>' at the end")
                continue
            function = words[3]
        elif len(words) != 2:
            problems.append(f"COVERS {' '.join(words)}: a context takes no more words")
            continue
        if kind == "suspend-context":
            pattern = CONTEXT_MARKERS.get(key)
        else:
            pattern = CODE_MARKERS.get(key.split(".")[-1])
        if pattern is None:
            problems.append(f"COVERS {kind} {key}: the script has no code marker for it")
            continue
        for path in paths:
            code_text = "\n".join(
                line for line in path.read_text(encoding="utf-8").splitlines()
                if not line.lstrip().startswith("//")
            )
            if function is not None:
                code_text = function_body(code_text, function)
                if code_text is None:
                    problems.append(f"{path.name}: COVERS {kind} {key} names function {function!r}, which the probe lacks")
                    continue
            if not re.search(pattern, code_text):
                where = f"function {function!r}" if function else "its code"
                problems.append(f"{path.name} has 'COVERS: {kind} {key}' but {where} has no {pattern}")

    report(
        "check_blocking_decision_tables.py",
        problems,
        f"{len(suspends)} calls that suspend, {len(in_spec)} contexts; "
        "decision and spec agree, and every row has a probe",
    )


if __name__ == "__main__":
    main()
