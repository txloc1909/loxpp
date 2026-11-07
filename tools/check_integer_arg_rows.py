#!/usr/bin/env python3
"""Checks the table of integer arguments in spec/05-stdlib.md.

spec/05-stdlib.md, "Integer arguments", lists every standard-library parameter
that must be an integer: each one accepts an Int or an integer-valued Number
(the decision in #1135). The set is countable, and the failure it guards
against is a parameter left out of the change, so the table has a row per
parameter and this script fails when a row, its probe, or its native call site
is missing.

The script reads three things and compares them:

- The table. Every `Row` cell is an id.
- The negative side, from `// COVERS: integer-arg <row>` lines in
  test/translation-probes/. Each row needs at least one spec probe, and a
  COVERS line that names a row the table does not have fails.
- The native side. `readIntegerArg` (src/stdlib/integer_arg.h) is the one place
  that accepts both kinds, so a parameter that must be an integer calls it.
  Each row maps to the enclosing native function, and the function must call
  `readIntegerArg`. Every call site of `readIntegerArg` in src/stdlib/ must
  belong to a function that a row names, so a new integer parameter with no row
  fails here. Four rows may share one function, as the byte-count rows share
  `asByteCount`.

The JVM runtime is not read here. It has its own copy of the check
(runtime/jvm/src/lox/LoxRuntime.java, `integerArg`), and the spec probes that
check the rows run on the JVM in CI, so a JVM that rejects the Int form fails
there rather than here.

Proving this check can fail: pass a copy of a spec file, probe directory, or
stdlib directory with one row, probe, or call site removed, and see it fail.

Usage:
    tools/check_integer_arg_rows.py [--stdlib PATH] [--probes DIR] [--spec PATH]
"""

import argparse
import re
import sys
from pathlib import Path

from spec_tables import (
    PROBE_DIR,
    SPEC_DIR,
    SpecError,
    code,
    probe_covers,
    report,
    section,
    table,
)

SCRIPT = "check_integer_arg_rows.py"
STDLIB_DIR = SPEC_DIR.parent / "src" / "stdlib"
HELPER = "readIntegerArg"

# The native function that reads each row's argument, by (file, function). Each
# entry must call the helper, and the helper must be called nowhere else.
SITES = {
    "chr": ("globals.cpp", "chrNative"),
    "connect-port": ("net_api.cpp", "asPort"),
    "listen-port": ("net_api.cpp", "asPort"),
    "socket-read-bytes": ("net_api.cpp", "asByteCount"),
    "socket-read-some": ("net_api.cpp", "asByteCount"),
    "process-read-bytes": ("process_api.cpp", "asByteCount"),
    "process-err-read-bytes": ("process_api.cpp", "asByteCount"),
}

FUNCTION = re.compile(r"^(?:static |inline )?[\w:<>*& ]+?[ *&](\w+)\([^;]*$")


def call_sites(stdlib_dir: Path) -> dict[tuple[str, str], int]:
    """Returns (file, enclosing function) -> number of `readIntegerArg` calls.

    The helper's own definition is not a call site, so its file is skipped.
    """
    sites: dict[tuple[str, str], int] = {}
    for path in sorted(stdlib_dir.glob("*.cpp")):
        if path.name == "integer_arg.cpp":
            continue
        function = None
        for line in path.read_text(encoding="utf-8").splitlines():
            if line and not line[0].isspace() and not line.startswith(("}", "#")):
                match = FUNCTION.match(line)
                if match:
                    function = match.group(1)
            if HELPER + "(" in line and function is not None:
                key = (path.name, function)
                sites[key] = sites.get(key, 0) + line.count(HELPER + "(")
    return sites


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--spec", help="path to a copy of spec/05-stdlib.md")
    parser.add_argument("--probes", default=str(PROBE_DIR),
                        help="a directory of spec probes")
    parser.add_argument("--stdlib", default=str(STDLIB_DIR),
                        help="a directory of native stdlib sources")
    args = parser.parse_args()

    problems: list[str] = []
    try:
        text = (Path(args.spec) if args.spec else SPEC_DIR / "05-stdlib.md").read_text(
            encoding="utf-8")
        rows = table(section(text, "## Integer arguments"), "Argument")
        covers = probe_covers(Path(args.probes))
        sites = call_sites(Path(args.stdlib))
    except SpecError as exc:
        report(SCRIPT, [f"cannot read the spec: {exc}"], "")
        return

    row_ids: list[str] = []
    for row in rows:
        try:
            row_id = code(row["Row"])
            code(row["Argument"])
            row["Range"]
        except SpecError as exc:
            problems.append(str(exc))
            continue
        if row_id in row_ids:
            problems.append(f"row {row_id!r} appears more than once")
        row_ids.append(row_id)

    for row_id in row_ids:
        if ("integer-arg", row_id) not in covers:
            problems.append(f"row {row_id!r} has no spec probe with "
                            f"'// COVERS: integer-arg {row_id}'")
        if row_id not in SITES:
            problems.append(f"row {row_id!r} names no native site")
    for words in sorted(covers):
        if words[0] != "integer-arg":
            continue
        if len(words) != 2 or words[1] not in row_ids:
            problems.append("COVERS line names no row of the table: "
                            + " ".join(words))

    needed = {SITES[row_id] for row_id in row_ids if row_id in SITES}
    for key in sorted(needed):
        if key not in sites:
            problems.append(f"{key[0]}: {key[1]}() has a row but does not call "
                            f"{HELPER}")
    for key in sorted(sites):
        if key not in needed:
            problems.append(f"{key[0]}: {key[1]}() calls {HELPER} but no row "
                            "names it")

    report(SCRIPT, problems,
           f"{len(row_ids)} integer-argument rows, each with a native call "
           "site and a spec probe")


if __name__ == "__main__":
    sys.exit(main())
