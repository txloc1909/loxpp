"""Shared readers for the countable tables in spec/.

The table check scripts (check_sendable_table.py, check_worker_error_kinds.py,
check_suspend_points.py, check_blocking_calls.py, and the check scripts of the
Int kind) import this module. It reads Markdown sections and pipe tables, and
the `// COVERS:` lines of the spec probes, and nothing else, so a check script
states its own rule and this module states none.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SPEC_DIR = REPO_ROOT / "spec"
PROBE_DIR = REPO_ROOT / "test" / "translation-probes"


class SpecError(Exception):
    """The spec text does not have the shape a check script reads."""


def spec_path(name: str, override: str | None = None) -> Path:
    """Returns spec/<name>, or the override path when a caller gives one.

    The override lets a reviewer run a check against a scratch copy of a
    spec file, for example to see the check fail when a row is removed.
    """
    return Path(override) if override else SPEC_DIR / name


def section(text: str, heading: str) -> str:
    """Returns the body of the section whose heading line is `heading`.

    The section ends at the next heading of the same level or higher.
    """
    level = len(heading) - len(heading.lstrip("#"))
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.rstrip() == heading:
            body = []
            for later in lines[i + 1 :]:
                m = re.match(r"^(#+) ", later)
                if m and len(m.group(1)) <= level:
                    break
                body.append(later)
            return "\n".join(body)
    raise SpecError(f"no heading {heading!r}")


def split_row(line: str) -> list[str]:
    """Splits one pipe-table row into its stripped cells.

    A backslash-escaped pipe (`\\|`) is part of a cell, as in the cell of
    the `|` operator.
    """
    inner = line.strip()
    if inner.startswith("|"):
        inner = inner[1:]
    if inner.endswith("|") and not inner.endswith("\\|"):
        inner = inner[:-1]
    cells = re.split(r"(?<!\\)\|", inner)
    return [cell.replace("\\|", "|").strip() for cell in cells]


def table(body: str, first_header: str) -> list[dict[str, str]]:
    """Returns the rows of the first table in `body` whose first header cell
    is `first_header`, as dicts keyed by header text."""
    lines = body.splitlines()
    for i, line in enumerate(lines):
        if not line.startswith("|"):
            continue
        headers = split_row(line)
        if headers[0] != first_header:
            continue
        if i + 1 >= len(lines) or not re.match(r"^\|[\s|:-]+\|$", lines[i + 1].strip()):
            raise SpecError(f"table {first_header!r} has no separator row")
        rows = []
        for row_line in lines[i + 2 :]:
            if not row_line.startswith("|"):
                break
            cells = split_row(row_line)
            if len(cells) != len(headers):
                raise SpecError(
                    f"table {first_header!r}: row has {len(cells)} cells, "
                    f"header has {len(headers)}: {row_line}"
                )
            rows.append(dict(zip(headers, cells)))
        return rows
    raise SpecError(f"no table with first header {first_header!r}")


def code(cell: str) -> str:
    """Returns the text inside the one backtick span a cell holds."""
    m = re.fullmatch(r"`([^`]+)`", cell)
    if not m:
        raise SpecError(f"cell is not one code span: {cell!r}")
    return m.group(1)


COVERS_RE = re.compile(r"^\s*//\s*COVERS:\s*(.+?)\s*$")


def probe_covers(probe_dir: Path) -> dict[tuple[str, ...], list[Path]]:
    """Returns the `// COVERS: <set> <id>...` lines of the spec probes of a
    directory: each line as a tuple of its whitespace-separated words, mapped
    to the probes that carry it.

    A probe says which row of a spec table it checks with a COVERS line, for
    example `// COVERS: operand-kind | int` or `// COVERS: fault shift_count_error`.
    The check script of a table reads these lines to learn which row has a
    probe. A COVERS line in a file that is not a spec probe (it has no STATUS
    line, see tools/check_spec_probes.py) is an error, because no gate runs
    that file.
    """
    from check_spec_probes import parse_probe

    found: dict[tuple[str, ...], list[Path]] = {}
    for path in sorted(probe_dir.glob("*.lox")):
        lines = [
            m.group(1)
            for m in map(COVERS_RE.match, path.read_text(encoding="utf-8").splitlines())
            if m
        ]
        if not lines:
            continue
        if parse_probe(path) is None:
            raise SpecError(f"{path}: a COVERS line in a file that has no STATUS line")
        for words in lines:
            found.setdefault(tuple(words.split()), []).append(path)
    return found


def report(script: str, problems: list[str], ok_line: str) -> None:
    """Prints the result and exits: 0 when there is no problem, 1 otherwise."""
    if problems:
        print(f"{script}: FAIL", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        sys.exit(1)
    print(f"{script}: {ok_line}")
