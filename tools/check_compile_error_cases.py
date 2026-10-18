#!/usr/bin/env python3
"""
Check that every compile message native can report has a test/compile-errors/
case, or a recorded reason why it has none (issue #642).

Native's compile messages live as string literals in src/parser.cpp,
src/compiler.cpp, and src/scanner.cpp. Nothing in the build or the test suite
enumerates them, so a new message can land with no case and no gate notices.
This script makes the set countable: it extracts every message template from
those three files and compares it with the corpus, case by case.

Each case starts with a marker line that names the message it must reach:

    // DIAGNOSTIC: Cannot use 'break' outside a loop or match.
    break;

`<...>` stands for a part the compiler interpolates at run time, such as a
name. A case whose marker has no such part must produce exactly that text.
A message may have more than one case: two code paths that report the same
message are both worth pinning.

A message with no case must be in one of three tables below.

  DEFERRED    A consumer reports a different message, so the case belongs to
              the issue that fixes that consumer (see AGENTS.md and #642).
              The issue body carries the repro. The checker cannot see a fix
              land, because no repro is in the tree; this is the accepted
              limit of recording only the issue number.

  GENERATED   The smallest program that reaches the message is too large to
              keep in the tree. The script builds it, runs it, and checks the
              message. The three size-limit messages are here.

  UNREACHABLE No program reaches the message. The reason must name the code
              that makes it unreachable, so a reviewer can check it.

The script fails when a source message is in no table, when a table row no
longer matches a source message, when a case names a message the source does
not have, when native does not reject a case at compile time, or when a case's
first diagnostic differs from its marker. It prints the complete table with
--list.

This script checks native only. The bootstrap, JVM, and QBE differential
suites run every case in the directory with --compile-errors, so a consumer
that stops matching a case fails those suites, not this one.

A build that traces every instruction (LOXPP_DEBUG_TRACE_EXECUTION or
LOXPP_DEBUG_PRINT_CODE) skips the stdout-empty check on the cases, because the
trace writes that disassembly to stdout. The marker, stderr, exit status, and
GENERATED checks still run.

Usage:
    tools/check_compile_error_cases.py [--native PATH] [--list] [--timeout SECONDS]

The default native binary is build/loxpp. Exits 0 when every check passes and
1 otherwise.
"""

from __future__ import annotations

import argparse
import ast
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORPUS = REPO_ROOT / "test" / "compile-errors"
SOURCE_FILES = ("src/parser.cpp", "src/compiler.cpp", "src/scanner.cpp")
COMPILE_ERROR_STATUS = 65
PLACEHOLDER = "<...>"

MARKER = re.compile(r"^\s*//\s*DIAGNOSTIC:\s*(.+?)\s*$", re.MULTILINE)
ERROR_LINE = re.compile(r"^\[line \d+\] Error(?: at (?:end|.*?))?: (.*)$")

# Calls that only forward a message another function built. They carry no
# literal text, so the extractor would otherwise report them as unreadable.
FORWARDED_CALLS = {
    ("error", "message"),
    ("errorAtCurrent", "message"),
    ("errorAt", "message"),
    # Parser::advance forwards the scanner's error token text; the scanner's
    # own three messages are extracted from createErrorToken calls.
    ("errorAtCurrent", "m_current.lexeme.data()"),
}


class ExtractionError(Exception):
    """A compile-message call site the extractor cannot read."""


@dataclass(frozen=True)
class Deferred:
    issue: int
    note: str


@dataclass(frozen=True)
class Generated:
    builder: str
    issue: int | None
    note: str


@dataclass(frozen=True)
class Unreachable:
    reason: str


# Message templates with no case. See the module docstring for the rule.
DEFERRED: dict[str, Deferred] = {
    "'_' cannot be used as an @-binding name.":
        Deferred(680, 'the bootstrap misparses the @-binding'),
    'A binding pattern must be the sole arm pattern.':
        Deferred(650, 'the bootstrap accepts the comma chain'),
    "Can't have more than 255 arguments.":
        Deferred(681, 'different token and a second error'),
    "Can't have more than 255 parameters.":
        Deferred(681, 'different token and a second error'),
    'Cannot have more than 255 elements in a list literal.':
        Deferred(681, 'different message and a second error'),
    'Cannot have more than 255 entries in a map literal.':
        Deferred(681, 'different message and a second error'),
    "Class patterns only support named-field syntax (e.g., 'ClassName{field1, field2}').":
        Deferred(680, 'the bootstrap runs the program'),
    "Expect '(' after 'catch'.":
        Deferred(641, 'the bootstrap adds a second error'),
    "Expect '(' after 'for'.":
        Deferred(641, 'the bootstrap adds a second error'),
    "Expect ')' after catch identifier.":
        Deferred(679, 'different message text'),
    "Expect ')' after field list.":
        Deferred(679, 'different message text'),
    "Expect ')' after field pattern.":
        Deferred(679, 'different message text and recovery'),
    "Expect ')' after sequence.":
        Deferred(679, 'different message text'),
    "Expect '.' after 'super'.":
        Deferred(641, 'the bootstrap adds a second error'),
    "Expect ';' after 'break'.":
        Deferred(641, 'different line at end of file'),
    "Expect ';' after 'continue'.":
        Deferred(641, 'different line at end of file'),
    "Expect ';' after defer statement.":
        Deferred(641, 'different line at end of file'),
    "Expect ';' after loop condition.":
        Deferred(641, 'error recovery differs'),
    "Expect ';' after return value.":
        Deferred(641, 'different line at end of file'),
    "Expect ';' after throw expression.":
        Deferred(679, 'different message text'),
    "Expect '=' after destructuring pattern.":
        Deferred(679, 'different message text'),
    "Expect '=>' after pattern.":
        Deferred(679, 'different message text'),
    "Expect ']' after element names.":
        Deferred(679, 'different message text'),
    "Expect ']' after sequence pattern.":
        Deferred(679, 'different message text'),
    "Expect 'case' in match body.":
        Deferred(679, 'different message text and token'),
    "Expect 'catch' after try block.":
        Deferred(641, 'different line at end of file'),
    "Expect '{' after 'try'.":
        Deferred(641, 'different line at end of file'),
    "Expect '{' before class body.":
        Deferred(641, 'different line at end of file'),
    "Expect '{' before function body.":
        Deferred(641, 'different line at end of file'),
    "Expect '{' before match body.":
        Deferred(679, 'different message text'),
    "Expect '{' for catch block.":
        Deferred(641, 'different line at end of file'),
    "Expect '}' after block.":
        Deferred(641, 'different line at end of file'),
    "Expect '}' after class body.":
        Deferred(641, 'different line at end of file'),
    "Expect '}' after enum body.":
        Deferred(641, 'different line at end of file'),
    "Expect '}' after field names.":
        Deferred(679, 'different message text'),
    "Expect '}' after field pattern.":
        Deferred(679, 'different message text and recovery'),
    "Expect '}' after match arm body.":
        Deferred(641, 'different line at end of file'),
    "Expect '}' after match body.":
        Deferred(679, 'different message text and line'),
    "Expect a call expression after 'defer'.":
        Deferred(676, 'different message and token'),
    'Expect binding name.':
        Deferred(680, 'the bootstrap runs the program'),
    'Expect element name in sequence pattern.':
        Deferred(679, 'different message text'),
    'Expect element name.':
        Deferred(679, 'different message text'),
    "Expect ',' between arguments.":
        Deferred(679, 'different message text'),
    'Expect expression.':
        Deferred(640, 'different token'),
    'Expect identifier in catch clause.':
        Deferred(679, 'different message text'),
    'Expect method name.':
        Deferred(641, 'the bootstrap omits a second error'),
    "Expect name after '...' in sequence pattern.":
        Deferred(680, 'the bootstrap misparses the rest name'),
    'Expect parameter name.':
        Deferred(641, 'the bootstrap omits a second error'),
    'Expect superclass method name.':
        Deferred(641, 'the bootstrap adds a second error'),
    "Expected a structural pattern (constructor, class, or '[') after '@'.":
        Deferred(680, 'the bootstrap misparses the sub-pattern'),
    'Or-pattern alternatives must bind the same names in the same order.':
        Deferred(680, 'the bootstrap accepts the program'),
    'Or-patterns are not supported for sequence patterns.':
        Deferred(680, 'the bootstrap misparses it'),
    "Sub-pattern after '@' must be a structural pattern (constructor or class), not a plain binding.":
        Deferred(680, 'the bootstrap misparses the sub-pattern'),
    'Too many closure variables in function.':
        Deferred(681, 'the bootstrap accepts the program'),
    'Too many elements in sequence destructuring.':
        Deferred(681, 'the bootstrap has a different limit'),
    'Too many elements in sequence pattern.':
        Deferred(681, 'the bootstrap has a different limit'),
    'Too many fields in constructor pattern.':
        Deferred(681, 'the bootstrap accepts the program'),
    'Too many fields in destructuring pattern.':
        Deferred(681, 'the bootstrap has a different limit'),
    'Too many local variables in function.':
        Deferred(681, 'the bootstrap accepts the program'),
    'Unexpected character.':
        Deferred(641, 'the bootstrap adds a second error'),
    "Unknown constructor or class '<...>'.":
        Deferred(678, 'the bootstrap does not check the name'),
    "Unknown field '<...>' for '<...>'.":
        Deferred(678, 'the bootstrap does not check the field'),
    'Unreachable arm after catch-all.':
        Deferred(639, 'the bootstrap does not check it'),
    'Unterminated string.':
        Deferred(641, 'different line and a second error'),
}

GENERATED: dict[str, Generated] = {
    'Too many constants in one chunk.':
        Generated('too_many_constants', 681, 'a 65,536-constant function; the bootstrap accepts it'),
    'Too much code to jump over.':
        Generated('too_much_code_to_jump_over', 681, 'an if body over 65,535 bytes of bytecode; the bootstrap accepts it'),
    'Loop body too large.':
        Generated('loop_body_too_large', 681, 'a loop body over 65,535 bytes of bytecode; the bootstrap accepts it'),
}

UNREACHABLE: dict[str, Unreachable] = {
    "Expect '{' in destructuring pattern.":
        Unreachable('varDeclaration checks LEFT_BRACE before it calls varDestructure (src/compiler.cpp:462), so the consume cannot fail'),
    "Expect '[' in sequence destructuring pattern.":
        Unreachable('varDeclaration checks LEFT_BRACKET before it calls varDestructureSeq (src/compiler.cpp:458), so the consume cannot fail'),
    "Expect '[' in sequence pattern.":
        Unreachable('both call sites check LEFT_BRACKET before compileListPattern (src/compiler.cpp:1086, 1213), so the consume cannot fail'),
}


def mask_source(text: str) -> str:
    """Returns text with comments and string contents blanked out.

    Quotes stay, and newlines stay, so offsets and line numbers match the
    original. The mask lets the extractor find balanced parentheses without
    being fooled by a '(' inside a message.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == '"':
            out.append('"')
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append("  ")
                    i += 2
                elif text[i] == '"':
                    out.append('"')
                    i += 1
                    break
                else:
                    out.append("\n" if text[i] == "\n" else " ")
                    i += 1
        elif c == "'":
            # A character literal such as '"' must not be read as the start
            # of a string, or the scanner code after it disappears from the
            # mask and its messages go uncounted.
            out.append("'")
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append("  ")
                    i += 2
                elif text[i] == "'":
                    out.append("'")
                    i += 1
                    break
                else:
                    out.append("\n" if text[i] == "\n" else " ")
                    i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def split_top(expr: str, separator: str) -> list[str]:
    """Splits expr on separator at paren depth zero, outside string literals."""
    parts: list[str] = []
    depth = 0
    in_string = False
    current: list[str] = []
    i = 0
    while i < len(expr):
        c = expr[i]
        if in_string:
            current.append(c)
            if c == "\\":
                current.append(expr[i + 1])
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue
        if c == '"':
            in_string = True
            current.append(c)
            i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == separator and depth == 0:
            parts.append("".join(current))
            current = []
        else:
            current.append(c)
        i += 1
    parts.append("".join(current))
    return parts


def literal_terms(term: str) -> list[str] | None:
    """Returns the decoded strings of an all-literal term, or None.

    A term is all-literal when it is one or more adjacent C++ string literals,
    which is how a message is written across several source lines.
    """
    pieces: list[str] = []
    i = 0
    saw_literal = False
    while i < len(term):
        if term[i].isspace():
            i += 1
            continue
        if term[i] != '"':
            return None
        j = i + 1
        while j < len(term):
            if term[j] == "\\":
                j += 2
                continue
            if term[j] == '"':
                break
            j += 1
        if j >= len(term):
            return None
        pieces.append(ast.literal_eval(term[i : j + 1]))
        saw_literal = True
        i = j + 1
    return pieces if saw_literal else None


def template_from_expr(expr: str, where: str) -> str:
    """Turns a message argument into its template.

    Literal parts are kept and each interpolated expression becomes one
    PLACEHOLDER. A call the extractor cannot read raises ExtractionError: a
    message it cannot count must fail the check, never pass unseen.
    """
    e = expr.strip()
    e = re.sub(r"\.c_str\(\)\s*$", "", e).strip()
    if e.startswith("(") and e.endswith(")"):
        e = e[1:-1].strip()
    out: list[str] = []
    for term in split_top(e, "+"):
        literals = literal_terms(term)
        if literals is None:
            if term.strip():
                out.append(PLACEHOLDER)
            continue
        out.extend(literals)
    template = "".join(out)
    if not template or template == PLACEHOLDER:
        raise ExtractionError(
            f"{where}: cannot read the message expression {expr.strip()!r}; "
            "write it as one concatenation of string literals and values"
        )
    return template


def is_definition(source: str, start: int) -> bool:
    """True for a function definition such as `void Parser::error(...)`."""
    prefix = source[max(0, start - 80) : start]
    return re.search(r"(?:void|Token|bool|int)\s+\w+::\s*$", prefix) is not None


def extract_messages(source: str, filename: str) -> dict[str, list[str]]:
    """Returns every message template in source, with its call sites."""
    masked = mask_source(source)
    found: dict[str, list[str]] = {}
    pattern = re.compile(r"\b(errorAtCurrent|errorAt|error|consume|createErrorToken)\s*\(")
    for match in pattern.finditer(masked):
        callee = match.group(1)
        start = match.end()
        depth = 1
        i = start
        while i < len(masked) and depth:
            if masked[i] == "(":
                depth += 1
            elif masked[i] == ")":
                depth -= 1
            i += 1
        if depth:
            raise ExtractionError(f"{filename}: unbalanced parentheses at offset {match.start()}")
        line = source[: match.start()].count("\n") + 1
        where = f"{filename}:{line}"
        if is_definition(source, match.start()):
            continue
        args = source[start : i - 1]
        # The message is the last argument of every call in this set.
        message_expr = split_top(args, ",")[-1].strip()
        if (callee, message_expr) in FORWARDED_CALLS:
            continue
        template = template_from_expr(message_expr, where)
        found.setdefault(template, []).append(where)
    return found


def source_messages() -> dict[str, list[str]]:
    messages: dict[str, list[str]] = {}
    for filename in SOURCE_FILES:
        text = (REPO_ROOT / filename).read_text(encoding="utf-8")
        for template, sites in extract_messages(text, filename).items():
            messages.setdefault(template, []).extend(sites)
    return messages


def template_pattern(template: str) -> re.Pattern[str]:
    parts = [re.escape(part) for part in template.split(PLACEHOLDER)]
    return re.compile("^" + ".+".join(parts) + "$")


def first_diagnostic(stderr: str) -> str | None:
    for line in stderr.splitlines():
        match = ERROR_LINE.match(line)
        if match:
            return match.group(1)
    return None


@dataclass
class Run:
    status: int
    stdout: str
    stderr: str


def run_native(native: Path, program: Path, timeout: float) -> Run:
    result = subprocess.run(
        [str(native), str(program)],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    return Run(result.returncode, result.stdout, result.stderr)


def build_too_many_constants() -> str:
    """A function with 65536 distinct constants in one chunk.

    `makeConstant` deduplicates, so the values must be distinct. One distinct
    number per expression statement is the smallest source that reaches the
    limit.
    """
    statements = "\n".join(f"{value};" for value in range(70000))
    return f"fun f() {{\n{statements}\n}}\n"


def build_too_much_code_to_jump_over() -> str:
    """An `if` body whose jump offset exceeds UINT16_MAX.

    Each `1;` is a constant load and a pop, repeated with one deduplicated
    constant. Repeated literals keep the source small and the constant pool
    flat.
    """
    statements = "\n".join("1;" for _ in range(30000))
    return f"fun f() {{\nif (true) {{\n{statements}\n}}\n}}\n"


def build_loop_body_too_large() -> str:
    """A loop body whose back-jump offset exceeds UINT16_MAX."""
    statements = "\n".join("1;" for _ in range(30000))
    return f"fun f() {{\nwhile (true) {{\n{statements}\n}}\n}}\n"


BUILDERS = {
    "too_many_constants": build_too_many_constants,
    "too_much_code_to_jump_over": build_too_much_code_to_jump_over,
    "loop_body_too_large": build_loop_body_too_large,
}


def parse_case(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    markers = MARKER.findall(text)
    if len(markers) != 1:
        raise ValueError(
            f"{path.relative_to(REPO_ROOT)}: expected one // DIAGNOSTIC: line, found {len(markers)}"
        )
    return markers[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, default=REPO_ROOT / "build" / "loxpp")
    parser.add_argument("--list", action="store_true", help="print every message and its status")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    problems: list[str] = []
    if not args.native.is_file():
        print(f"error: no native binary at {args.native}", file=sys.stderr)
        sys.exit(2)

    messages = source_messages()
    trace_build = os.environ.get("LOXPP_EXEC_TRACE_ENABLED") == "1"
    cases: dict[str, list[Path]] = {}
    for path in sorted(CORPUS.glob("*.lox")):
        try:
            marker = parse_case(path)
        except ValueError as exc:
            problems.append(str(exc))
            continue
        cases.setdefault(marker, []).append(path)

    exceptions: dict[str, str] = {}
    for table_name, table in (
        ("DEFERRED", DEFERRED),
        ("GENERATED", GENERATED),
        ("UNREACHABLE", UNREACHABLE),
    ):
        for template, row in table.items():
            if template in exceptions:
                problems.append(
                    f"message is in both {exceptions[template]} and {table_name}: {template!r}"
                )
            exceptions[template] = table_name
            if isinstance(row, Deferred) and row.issue <= 0:
                problems.append(f"DEFERRED row has no issue number: {template!r}")
            if isinstance(row, Generated) and row.builder not in BUILDERS:
                problems.append(f"GENERATED row names an unknown builder: {template!r}")

    for template, sites in sorted(messages.items()):
        if template in cases or template in exceptions:
            continue
        problems.append(
            f"no case and no reason for {template!r} ({', '.join(sites)})"
        )
    for template in sorted(set(cases) | set(exceptions)):
        if template not in messages:
            problems.append(
                f"row or case names a message the source does not have: {template!r}"
            )
    for template in sorted(set(cases) & set(exceptions)):
        problems.append(
            f"message has both a case and a {exceptions[template]} row: {template!r}"
        )

    for template, paths in sorted(cases.items()):
        pattern = template_pattern(template)
        for path in paths:
            try:
                run = run_native(args.native, path, args.timeout)
            except subprocess.TimeoutExpired:
                problems.append(f"{path.relative_to(REPO_ROOT)}: timed out")
                continue
            rel = path.relative_to(REPO_ROOT)
            if run.status != COMPILE_ERROR_STATUS:
                problems.append(
                    f"{rel}: native exited {run.status}, not {COMPILE_ERROR_STATUS}"
                )
                continue
            if run.stdout and not trace_build:
                problems.append(f"{rel}: compile error wrote to stdout")
                continue
            diagnostic = first_diagnostic(run.stderr)
            if diagnostic is None:
                problems.append(f"{rel}: no diagnostic on stderr")
                continue
            if not pattern.match(diagnostic):
                problems.append(
                    f"{rel}: first diagnostic is {diagnostic!r}, marker says {template!r}"
                )

    for template, row in sorted(GENERATED.items()):
        builder = BUILDERS.get(row.builder)
        if builder is None:
            continue
        program = builder()
        with tempfile.NamedTemporaryFile(
            "w", suffix=".lox", delete=False, encoding="utf-8"
        ) as handle:
            handle.write(program)
            temp = Path(handle.name)
        try:
            run = run_native(args.native, temp, args.timeout)
        except subprocess.TimeoutExpired:
            problems.append(f"GENERATED {template!r}: timed out")
            continue
        finally:
            temp.unlink(missing_ok=True)
        if run.status != COMPILE_ERROR_STATUS:
            problems.append(
                f"GENERATED {template!r}: native exited {run.status}, not {COMPILE_ERROR_STATUS}"
            )
            continue
        diagnostic = first_diagnostic(run.stderr)
        if diagnostic is None or not template_pattern(template).match(diagnostic):
            problems.append(
                f"GENERATED {template!r}: first diagnostic is {diagnostic!r}"
            )

    if args.list:
        print(f"{len(messages)} messages")
        for template in sorted(messages):
            if template in cases:
                status = "CASE      " + ", ".join(
                    str(p.relative_to(REPO_ROOT)) for p in cases[template]
                )
            elif template in DEFERRED:
                row = DEFERRED[template]
                status = f"DEFERRED  #{row.issue}: {row.note}"
            elif template in GENERATED:
                row = GENERATED[template]
                issue = f" #{row.issue}" if row.issue else ""
                status = f"GENERATED {row.builder}{issue}: {row.note}"
            elif template in UNREACHABLE:
                status = f"UNREACHABLE {UNREACHABLE[template].reason}"
            else:
                status = "MISSING"
            print(f"  {status:<60} {template}")

    if problems:
        print("check_compile_error_cases: FAIL", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        sys.exit(1)
    print(
        f"check_compile_error_cases: OK "
        f"({len(messages)} messages, {len(cases)} with a case, "
        f"{len(DEFERRED)} deferred, {len(GENERATED)} generated, "
        f"{len(UNREACHABLE)} unreachable)"
    )


if __name__ == "__main__":
    main()
