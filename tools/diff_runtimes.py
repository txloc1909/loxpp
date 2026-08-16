#!/usr/bin/env python3
"""
Differential runner: native loxpp vs. the JVM backend, stdout only.

Runs each Lox++ program on the native binary and on tools/loxpp_jvm.sh, then
compares stdout. It does not compare stderr and does not compare the exit
code (notes/backend-implementation-dag.md, N11 row: "Differential scope ->
stdout only").

Three outcomes per program:
  MATCH        stdout is byte-identical on both runtimes.
  PERMUTATION  stdout differs only in line order, and the program is on the
               exclusion list (tools/jvm_excluded_examples.txt). Map
               iteration order is unspecified (spec/03-types.md), so a
               reordering alone is not a defect.
  DIVERGE      a real difference: content differs (not only order), or the
               program is not on the exclusion list. Exit code 1.

A program on the exclusion list still reports DIVERGE if its stdout stops
being a permutation of the native stdout — the exclusion list excuses a
reordering, never a content change.

On a DIVERGE, this script prints a short diff excerpt and a best-effort guess
at which opcode family (P1-P8, see notes/bytecode-translation-problems.md) is
involved, from a syntax scan of the program. This is a hint, not a proof: a
faithful answer needs a real bytecode disassembly, which needs a debug-preset
build; a syntax scan needs no extra build and runs on every program, so it is
this script's default. State that trade-off, do not hide it.

Usage:
    python3 tools/diff_runtimes.py <native-loxpp> <jvm-runner> <path>...
        [--exclude <file>] [--timeout <seconds>]
    python3 tools/diff_runtimes.py <native-loxpp> <jvm-runner>
        --exclude <file> --only-excluded <dir>

<path> is a .lox file, or a directory (every *.lox file inside it, sorted).
A path that is neither a file nor a directory, or a program list that ends
up empty, is an error (exit 2), not a silent zero-file pass.

--only-excluded <dir> ignores <path> and runs only the programs named in
--exclude, resolved under <dir>. This wires the permutation guard: an
excluded program that starts to fail for a reason other than reordering
turns DIVERGE, and the run exits 1.

If <name>.input exists next to a .lox file, it is piped in as stdin, on both
runtimes. Each runtime gets --timeout seconds (default 30); a runtime that
does not finish in time reports DIVERGE for that program, with a timeout
reason, rather than hanging the whole run.
"""

import argparse
import difflib
import re
import subprocess
import sys
from pathlib import Path

FAMILY_RULES: list[tuple[str, re.Pattern[str]]] = [
    (
        "P8 match/enum dispatch (GET_TAG, JUMP_TABLE, MATCH_ERROR, INSTANCEOF, IS_SEQ)",
        re.compile(r"\b(match|enum)\b"),
    ),
    (
        "P5+P4 classes (CLASS, GET_PROPERTY, SET_PROPERTY, DEFINE_METHOD, INVOKE, INHERIT, GET_SUPER, SUPER_INVOKE)",
        re.compile(r"\b(class|super)\b"),
    ),
    (
        "P4 closures & upvalues (CLOSURE, GET_UPVALUE, SET_UPVALUE, CLOSE_UPVALUE)",
        re.compile(r"\bfun\b"),
    ),
    (
        "P7 aggregates & iteration (BUILD_LIST, BUILD_MAP, GET_INDEX, SET_INDEX, SLICE, IN, GET_ITER, ITER_HAS_NEXT, ITER_NEXT)",
        re.compile(r"\bin\b|\[[^\]\n]*\]|\{[^{}\n]*:"),
    ),
    (
        "P3 control flow (JUMP, JUMP_IF_FALSE, LOOP)",
        re.compile(r"\b(if|while|for|and|or)\b"),
    ),
    (
        "P6 stdlib / runtime polymorphism (globals, math_module, map_api, file_api)",
        re.compile(r"\b(clock|input|File)\s*\(|\.(has|del|keys|values|entries)\s*\("),
    ),
]


def strip_comments_and_strings(source: str) -> str:
    """Removes '// ...' line comments and blanks string literal contents.

    classify_opcode_family scans only code with this. A file header comment
    such as "// Demonstrates: ... class-free ... in one file" must not name
    the P5, P7, or P3 families; only real syntax may.
    """
    out: list[str] = []
    in_string = False
    i = 0
    n = len(source)
    while i < n:
        ch = source[i]
        if in_string:
            if ch == "\\" and i + 1 < n:
                out.append("xx")
                i += 2
                continue
            if ch == '"':
                in_string = False
                out.append(ch)
                i += 1
                continue
            out.append("x")
            i += 1
            continue
        if ch == '"':
            in_string = True
            out.append(ch)
            i += 1
            continue
        if ch == "/" and i + 1 < n and source[i + 1] == "/":
            while i < n and source[i] != "\n":
                i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def classify_opcode_family(source: str) -> list[str]:
    code_only = strip_comments_and_strings(source)
    return [label for label, pattern in FAMILY_RULES if pattern.search(code_only)]


def parse_exclude_file(path: Path) -> dict[str, str]:
    """Reads a 'name  reason' exclusion list. Blank lines and lines starting
    with '#' are comments."""
    reasons: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        name, _, reason = stripped.partition(" ")
        reasons[name] = reason.strip()
    return reasons


class RunTimeout(Exception):
    """A runtime did not finish inside the allowed time."""


def run_stdout(cmd: list[str], input_file: Path, timeout: float) -> str:
    stdin_data = input_file.read_text(encoding="utf-8") if input_file.exists() else None
    try:
        result = subprocess.run(
            cmd, input=stdin_data, capture_output=True, text=True, timeout=timeout
        )
    except subprocess.TimeoutExpired as exc:
        raise RunTimeout(f"{cmd[0]} did not finish in {timeout:.0f}s") from exc
    return result.stdout


def collect_programs(paths: list[str]) -> list[Path]:
    """Resolves each path to a .lox file or every .lox file in a directory.

    Exits with an error if a path is neither a file nor a directory, so a
    rename or a typo fails the run instead of silently checking nothing.
    """
    files: list[Path] = []
    for raw in paths:
        p = Path(raw)
        if p.is_dir():
            files.extend(sorted(p.glob("*.lox")))
        elif p.is_file():
            files.append(p)
        else:
            print(f"error: path is not a file or a directory: {p}", file=sys.stderr)
            sys.exit(2)
    return files


def resolve_excluded_programs(excluded: dict[str, str], base_dir: Path) -> list[Path]:
    """Resolves every name in the exclusion file under base_dir.

    Used to run the permutation guard over exactly the excluded programs,
    without scanning the rest of the corpus (notes/translation-probes, or
    examples/huffman.lox, which diverges for a documented, non-permutation
    reason and is not on this list).
    """
    files: list[Path] = []
    for name in excluded:
        p = base_dir / name
        if not p.is_file():
            print(f"error: excluded program not found: {p}", file=sys.stderr)
            sys.exit(2)
        files.append(p)
    return files


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Differential runner: native loxpp vs the JVM backend, stdout only."
    )
    parser.add_argument("native", help="path to the native loxpp binary")
    parser.add_argument("jvm_runner", help="path to tools/loxpp_jvm.sh (or an equivalent runner)")
    parser.add_argument(
        "paths", nargs="*", default=[], help="a .lox file, or a directory of .lox files"
    )
    parser.add_argument(
        "--exclude",
        type=Path,
        default=None,
        help="tools/jvm_excluded_examples.txt: permutation-only exclusions",
    )
    parser.add_argument(
        "--only-excluded",
        metavar="DIR",
        default=None,
        help=(
            "ignore <paths>; run only the programs named in --exclude, resolved "
            "under DIR. Wires the permutation guard to CI without scanning the "
            "rest of the corpus."
        ),
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="seconds to allow each runtime per program, default 30",
    )
    args = parser.parse_args()

    excluded = parse_exclude_file(args.exclude) if args.exclude else {}

    if args.only_excluded is not None:
        if not args.exclude:
            parser.error("--only-excluded requires --exclude")
        if args.paths:
            parser.error("--only-excluded and explicit <paths> are mutually exclusive")
        programs = resolve_excluded_programs(excluded, Path(args.only_excluded))
    else:
        if not args.paths:
            parser.error("provide at least one <path>, or use --only-excluded")
        programs = collect_programs(args.paths)

    if not programs:
        print(
            "error: no programs to check (empty directory, or an empty exclusion list)",
            file=sys.stderr,
        )
        sys.exit(2)

    matched = permuted = diverged = 0

    for lox_file in programs:
        input_file = lox_file.with_suffix(".input")
        try:
            native_out = run_stdout([args.native, str(lox_file)], input_file, args.timeout)
            jvm_out = run_stdout([args.jvm_runner, str(lox_file)], input_file, args.timeout)
        except RunTimeout as exc:
            print(f"DIVERGE     {lox_file}  (timeout: {exc})")
            diverged += 1
            continue

        if native_out == jvm_out:
            print(f"MATCH       {lox_file}")
            matched += 1
            continue

        native_lines = native_out.splitlines()
        jvm_lines = jvm_out.splitlines()
        is_permutation = sorted(native_lines) == sorted(jvm_lines)

        if lox_file.name in excluded and is_permutation:
            print(f"PERMUTATION {lox_file}  (excluded: {excluded[lox_file.name]})")
            permuted += 1
            continue

        if lox_file.name in excluded:
            print(f"DIVERGE     {lox_file}  (excluded for map order, but this is NOT a permutation)")
        else:
            print(f"DIVERGE     {lox_file}")

        families = classify_opcode_family(lox_file.read_text(encoding="utf-8"))
        family_text = ", ".join(families) if families else "none matched (P2 straight-line only)"
        print(f"            likely opcode family (heuristic, not a bytecode decode): {family_text}")
        diff_lines = list(
            difflib.unified_diff(native_lines, jvm_lines, fromfile="native", tofile="jvm", lineterm="")
        )
        for line in diff_lines[:20]:
            print(f"            {line}")
        if len(diff_lines) > 20:
            print(f"            ... ({len(diff_lines) - 20} more diff line(s))")
        diverged += 1

    print()
    print(f"{matched} matched, {permuted} permutation-excused, {diverged} diverged")
    sys.exit(1 if diverged else 0)


if __name__ == "__main__":
    main()
