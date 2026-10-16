"""Shared readers for the countable tables in spec/.

The table check scripts (check_sendable_table.py, check_worker_error_kinds.py,
check_suspend_points.py, check_blocking_calls.py) import this module. It
reads Markdown sections and pipe tables, and nothing else, so a check script
states its own rule and this module states none.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SPEC_DIR = REPO_ROOT / "spec"


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
    """Splits one pipe-table row into its stripped cells."""
    inner = line.strip()
    if inner.startswith("|"):
        inner = inner[1:]
    if inner.endswith("|"):
        inner = inner[:-1]
    return [cell.strip() for cell in inner.split("|")]


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


def report(script: str, problems: list[str], ok_line: str) -> None:
    """Prints the result and exits: 0 when there is no problem, 1 otherwise."""
    if problems:
        print(f"{script}: FAIL", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        sys.exit(1)
    print(f"{script}: {ok_line}")
