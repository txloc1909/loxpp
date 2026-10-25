#!/usr/bin/env python3
"""
Differential runner: native loxpp vs. another loxpp consumer, at compile time
and at run time.

Runs each Lox++ program on the native binary and on a second runner
(tools/loxpp_jvm.sh, tools/loxpp_qbe.sh, bootstrap/lox_wrapper.sh, or an
equivalent), then compares the two runs in two phases.

The comparison follows spec/README.md, Conformance: it compares what a
program can observe and never compares stderr, because every report and
message is implementation-defined. Native's exit status tells this script
which case a program is (`loxpp --help`, src/main.cpp): 65 with
empty stdout is a rejection, and 70 is a halt. The other runner may use any
non-zero status for either.

Rejection. When native rejects the program, the other runner must exit
non-zero with empty stdout, or the program reports DIVERGE (compile phase).
A match prints as "MATCH (rejected)". When the other runner rejects a
program that native runs, the exit status differs, and the run comparison
below reports DIVERGE.

--compile-errors says that every program is a compile-error case: a
program that native does not reject reports DIVERGE, so a corpus file that
stops being a compile error fails and does not pass as a quiet run-time
match.

Halt. When native halts, the other runner must exit non-zero, and one stdout
must be a prefix of the other: the output before the fault binds, and the
deferred calls of a halt may run on one runner and not on the other. A match
prints as "MATCH (halt)". A program that calls exit(70) on native is checked
the same way, which is weaker than the exact comparison it could have.

Run. Otherwise this script compares stdout and the exact exit status: a run
with the same stdout but another exit status reports DIVERGE. The probe
criteria that the --xfail account uses are those of
tools/check_spec_probes.py. Four outcomes per program:
  MATCH        stdout is byte-identical on both runtimes.
  STALE        stdout is byte-identical on both runtimes, but the program is
               on the exclusion list. The exclusion is no longer needed and
               must be removed. Exit code 1.
  PERMUTATION  stdout differs only in line order, and the program is on the
               exclusion list (tools/jvm_excluded_examples.txt). Map
               iteration order is unspecified (spec/03-types.md), so a
               reordering alone is not a defect. QBE needs no exclusion
               list: it reuses native's own Table, so its map iteration
               order matches native by construction.
  DIVERGE      a real difference: content differs (not only order), or the
               program is not on the exclusion list. Exit code 1.

A program on the exclusion list still reports DIVERGE if its stdout stops
being a permutation of the native stdout — the exclusion list excuses a
reordering, never a content change.

--xfail <file> names the spec probes (tools/check_spec_probes.py) that the
other runner does not pass yet: the consumer's expected-failure list, in the
form tools/check_spec_probes.py reads. A name that is not a spec probe of this
run is an error (exit 2), as it is in tools/check_spec_probes.py. A name that
no program backs says nothing at all, where a listed probe the consumer passes
prints XPASS and exits 1, so after a rename the list would look satisfied and
a gate that runs only this script would report green over the gap. A listed
program reports one of two outcomes:
  XFAIL        the other runner's run does not pass the probe, or native's
               does not. Not a divergence.
  XPASS        both runs pass the probe (tools/check_spec_probes.py). Exit
               code 1: the backend passes it now, so its line must be
               deleted.

--oracle-xfail <file> is native's own expected-failure list, in the
same form, with the same rule: a name that is not a spec probe of this run is
an error (exit 2). A listed spec probe that the other runner passes and native
does not reports one more outcome:
  AHEAD        the other runner passes the probe before native does. Not a
               divergence: native's own gate shows its gap. A listed probe
               that native passes, or that the other runner fails, is
               compared as usual.

--consumer <name> names the consumer the other runner is. A spec probe whose
`// NOT-BINDING:` line names it (see tools/check_spec_probes.py) reports
EXEMPT and is not run: the probe depends on implementation-defined behavior
that the consumer chooses differently. An --xfail or --oracle-xfail entry that
names such a probe is an error (exit 2), as it is in tools/check_spec_probes.py:
this run never grades it, so the entry would read as satisfied. A list path that
does not exist is no gap: it is reported as a NOTE and grades every probe.

--only-spec-probes keeps only the spec probes (the .lox files with a
`// STATUS:` line) of the program list, so a walk of a directory runs the
shared probes of the spec and no other program in it.

Two silent runs are not proof that both ran: a runner that crashes before
writing anything looks identical, on stdout alone, to a program that
legitimately prints nothing. The exact exit status comparison above rejects
such a run. A match of two silent runs prints as "MATCH (empty)".

--skip-on-trace exits 125 (ctest's SKIP_RETURN_CODE) when
LOXPP_BOOTSTRAP_TRACE_ENABLED=1 is in the environment. A build that traces
execution prints an instruction trace on stdout, so no stdout comparison can
pass. This flag is opt-in, for a ctest entry that runs one probe on a build
that may carry the debug trace macros.

On a DIVERGE, this script prints a short diff excerpt and a best-effort guess
at which opcode family (P1-P8, see docs/reference/bytecode-translation-problems.md) is
involved, from a syntax scan of the program. This is a hint, not a proof: a
faithful answer needs a real bytecode disassembly, which needs a debug-preset
build; a syntax scan needs no extra build and runs on every program, so it is
this script's default. State that trade-off, do not hide it.

Usage:
    python3 tools/diff_runtimes.py <native-loxpp> <other-runner> <path>...
        [--exclude <file>] [--xfail <file>] [--oracle-xfail <file>] [--consumer <name>]
        [--only-spec-probes] [--compile-errors] [--timeout <seconds>]
        [--skip-on-trace]
    python3 tools/diff_runtimes.py <native-loxpp> <other-runner>
        --exclude <file> --only-excluded <dir>

<path> is a .lox file, or a directory (every *.lox file inside it, sorted).
A path that is neither a file nor a directory, a program list that ends up
empty, or an --xfail or --oracle-xfail entry that is not a spec probe of the
list, is an error (exit 2), not a silent zero-file pass.

--only-excluded <dir> ignores <path> and runs only the programs named in
--exclude, resolved under <dir>. This wires the permutation guard: an
excluded program that starts to fail for a reason other than reordering
turns DIVERGE, and the run exits 1.

If <name>.input exists next to a .lox file, it is piped in as stdin, on both
runtimes; otherwise the program gets empty stdin, so a run does not depend on
what the caller's stdin holds. Each runtime gets --timeout seconds (default
30); a runtime that does not finish in time reports DIVERGE for that program,
with a timeout reason, rather than hanging the whole run.
"""

import argparse
import difflib
import os
import re
import resource
import subprocess
import sys
from pathlib import Path

from check_spec_probes import parse_probe, parse_xfail_file, passes

# native's own stringifyObj (src/object.cpp) recurses once per level of
# list/map nesting with no depth guard, so a host whose ambient soft stack
# limit is already below this floor can fail native's OWN run on a deeply
# nested value before either backend is even compared. Applied only to
# native's own child process (see _raise_native_stack_limit), never to this
# parent process or to the other runner's child: raising it here, instead of
# ambient for the whole run, keeps the other runner's own stack-sizing
# mechanism (if it has one) the only thing standing between it and the same
# native failure.
_NATIVE_STACK_FLOOR_BYTES = 64 * 1024 * 1024


def _raise_native_stack_limit() -> None:
    """Runs inside a subprocess's own child, after fork and before exec —
    never in this parent process. Raises that one child's RLIMIT_STACK soft
    limit toward _NATIVE_STACK_FLOOR_BYTES, capped at the process's own hard
    limit, and does nothing if the platform has no RLIMIT_STACK, the
    current soft limit is already RLIM_INFINITY (unlimited), or the current
    soft limit already meets the floor.

    This is a raise, never a lowering, and the RLIM_INFINITY case is why
    that distinction needs its own check: RLIM_INFINITY is -1, which a bare
    numeric comparison treats as smaller than any finite floor, so an
    unguarded "raise toward the floor" would replace an unlimited soft
    limit with the finite floor — a lowering. A host whose hard limit is
    already capped below the floor still
    gets raised, but only up to that hard ceiling, not to the full floor;
    the child then fails only if the ceiling itself is too small for the
    depth being probed, not because the raise did nothing. The raise truly
    does nothing only when the current soft limit already meets or exceeds
    the hard limit, leaving no room to raise into.
    """
    try:
        soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
        if soft == resource.RLIM_INFINITY:
            return
        ceiling = _NATIVE_STACK_FLOOR_BYTES if hard == resource.RLIM_INFINITY else hard
        target = min(_NATIVE_STACK_FLOOR_BYTES, ceiling)
        if target > soft:
            resource.setrlimit(resource.RLIMIT_STACK, (target, hard))
    except (AttributeError, ValueError, OSError):
        pass

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


# Native's exit status for a rejected program and for a halt
# (`loxpp --help`, src/main.cpp). A program can also end with
# exit(65), so a rejection also needs empty stdout.
NATIVE_REJECT_STATUS = 65
NATIVE_HALT_STATUS = 70


class RunTimeout(Exception):
    """A runtime did not finish inside the allowed time."""


def run_program(
    cmd: list[str], input_file: Path, timeout: float, *, raise_native_stack: bool = False
) -> tuple[str, str, int]:
    """Runs one program and returns (stdout, stderr, exit code).

    stderr is returned for a diagnostic print only; no comparison reads it.

    raise_native_stack scopes _raise_native_stack_limit to this one child via
    preexec_fn — never this parent process, never the other runner's child.
    """
    stdin_data = input_file.read_text(encoding="utf-8") if input_file.exists() else ""
    try:
        result = subprocess.run(
            cmd,
            input=stdin_data,
            capture_output=True,
            text=True,
            timeout=timeout,
            preexec_fn=_raise_native_stack_limit if raise_native_stack else None,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunTimeout(f"{cmd[0]} did not finish in {timeout:.0f}s") from exc
    return result.stdout, result.stderr, result.returncode


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


def parse_probe_or_exit(path: Path):
    """parse_probe, with a malformed probe reported as exit 2, not a traceback.

    A run that carries an expected-failure list parses every program it walks, so
    one bad probe file would otherwise abort a corpus of hundreds with a Python
    traceback and exit 1, which reads as a divergence of the whole run.
    """
    try:
        return parse_probe(path)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(2)


def require_gradeable_names(
    list_path: Path,
    entries: dict[str, str],
    probes: list,
    consumer: str | None,
) -> None:
    """Exits when an expected-failure list names something this run cannot grade.

    Both flags grade a program by spec-probe criteria, so only a spec probe this
    run actually grades can be listed. A name that no spec probe backs is worse
    than a stale one: the lookup misses, no outcome is ever counted for it, and
    the entry reads as satisfied where a listed probe the consumer passes would
    print XPASS and exit 1. A probe the consumer exempts is the same shape for a
    different reason: the run skips it before either flag is consulted.
    """
    known = {p.path.name: p for p in probes}
    for name in entries:
        probe = known.get(name)
        if probe is None:
            print(
                f"error: {list_path} lists {name}, which is not a spec probe of "
                f"this run ({len(probes)} spec probes compared)",
                file=sys.stderr,
            )
            sys.exit(2)
        if consumer is not None and consumer in probe.not_binding:
            print(
                f"error: {list_path} lists {name}, which does not bind {consumer}",
                file=sys.stderr,
            )
            sys.exit(2)


def resolve_excluded_programs(excluded: dict[str, str], base_dir: Path) -> list[Path]:
    """Resolves every name in the exclusion file under base_dir.

    Used to run the permutation guard over exactly the excluded programs,
    without scanning the rest of the corpus (test/translation-probes, or
    any example not on this list).
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
        description="Differential runner: native loxpp vs another loxpp consumer, compile phase and run phase."
    )
    parser.add_argument("native", help="path to the native loxpp binary")
    parser.add_argument(
        "other_runner",
        help="path to tools/loxpp_jvm.sh, tools/loxpp_qbe.sh, bootstrap/lox_wrapper.sh, or an equivalent runner",
    )
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
        "--xfail",
        type=Path,
        default=None,
        help="the consumer's expected-failure list of spec probes, in the form tools/check_spec_probes.py reads",
    )
    parser.add_argument(
        "--oracle-xfail",
        type=Path,
        default=None,
        help="native's expected-failure list of spec probes, in the form tools/check_spec_probes.py reads",
    )
    parser.add_argument(
        "--consumer",
        default=None,
        help="the consumer the other runner is, for the NOT-BINDING lines of spec probes",
    )
    parser.add_argument(
        "--only-spec-probes",
        action="store_true",
        help="run only the spec probes (the files with a STATUS line) of the program list",
    )
    parser.add_argument(
        "--compile-errors",
        action="store_true",
        help="every program must be a compile error on native; one that is not reports DIVERGE",
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
    parser.add_argument(
        "--skip-on-trace",
        action="store_true",
        help=(
            "exit 125 (ctest's SKIP_RETURN_CODE) when "
            "LOXPP_BOOTSTRAP_TRACE_ENABLED=1 is in the environment"
        ),
    )
    args = parser.parse_args()

    if args.skip_on_trace and os.environ.get("LOXPP_BOOTSTRAP_TRACE_ENABLED") == "1":
        print("SKIP: the build under test traces execution (release preset required)")
        sys.exit(125)

    excluded = parse_exclude_file(args.exclude) if args.exclude else {}
    xfail = parse_xfail_file(args.xfail) if args.xfail else {}
    oracle_xfail = parse_xfail_file(args.oracle_xfail) if args.oracle_xfail else {}

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

    if args.only_spec_probes:
        programs = [p for p in programs if parse_probe_or_exit(p) is not None]

    if not programs:
        print(
            "error: no programs to check (empty directory, an empty exclusion list, "
            "or no spec probe with --only-spec-probes)",
            file=sys.stderr,
        )
        sys.exit(2)

    if args.xfail or args.oracle_xfail:
        # One parse of the run's programs serves both lists: it decides which
        # names this run can grade, and a malformed probe fails here rather than
        # inside the loop.
        probes = [p for p in map(parse_probe_or_exit, programs) if p is not None]
        if args.xfail:
            require_gradeable_names(args.xfail, xfail, probes, args.consumer)
        if args.oracle_xfail:
            require_gradeable_names(args.oracle_xfail, oracle_xfail, probes, args.consumer)

    matched = permuted = diverged = staled = xfailed = xpassed = exempted = ahead = 0

    for lox_file in programs:
        if args.consumer is not None:
            probe = parse_probe_or_exit(lox_file)
            if probe is not None and args.consumer in probe.not_binding:
                print(f"EXEMPT      {lox_file}  (does not bind {args.consumer})")
                exempted += 1
                continue
        input_file = lox_file.with_suffix(".input")
        try:
            native_out, native_err, native_status = run_program(
                [args.native, str(lox_file)], input_file, args.timeout, raise_native_stack=True
            )
            other_out, other_err, other_status = run_program(
                [args.other_runner, str(lox_file)], input_file, args.timeout
            )
        except RunTimeout as exc:
            if lox_file.name in xfail:
                print(f"XFAIL       {lox_file}  (timeout: {exc}; {xfail[lox_file.name]})")
                xfailed += 1
                continue
            print(f"DIVERGE     {lox_file}  (timeout: {exc})")
            diverged += 1
            continue

        if lox_file.name in xfail:
            probe = parse_probe_or_exit(lox_file)
            if (
                probe is not None
                and passes(probe, native_out, native_status)
                and passes(probe, other_out, other_status)
            ):
                print(f"XPASS       {lox_file}  (both runs pass it now: delete its line from {args.xfail})")
                xpassed += 1
            else:
                print(f"XFAIL       {lox_file}  ({xfail[lox_file.name]})")
                xfailed += 1
            continue

        if lox_file.name in oracle_xfail:
            probe = parse_probe_or_exit(lox_file)
            if (
                probe is not None
                and passes(probe, other_out, other_status)
                and not passes(probe, native_out, native_status)
            ):
                print(f"AHEAD       {lox_file}  (native: {oracle_xfail[lox_file.name]})")
                ahead += 1
                continue

        native_rejects = native_status == NATIVE_REJECT_STATUS and native_out == ""
        if args.compile_errors and not native_rejects:
            print(
                f"DIVERGE     {lox_file}  (not a compile-error case: native exited "
                f"{native_status} with {len(native_out)} byte(s) of stdout)"
            )
            diverged += 1
            continue
        if native_rejects:
            if other_status != 0 and other_out == "":
                print(f"MATCH       {lox_file}  (rejected)")
                matched += 1
                continue
            print(f"DIVERGE     {lox_file}  (compile phase: native rejects the program)")
            print(f"            other exited {other_status}, with {len(other_out)} byte(s) of stdout")
            for line in other_out.splitlines()[:20]:
                print(f"            | {line}")
            diverged += 1
            continue

        if native_status == NATIVE_HALT_STATUS:
            if other_status == 0:
                print(f"DIVERGE     {lox_file}  (native halts, but other exited 0)")
                diverged += 1
                continue
            if native_out.startswith(other_out) or other_out.startswith(native_out):
                print(f"MATCH       {lox_file}  (halt)")
                matched += 1
                continue
            print(f"DIVERGE     {lox_file}  (halt: neither stdout is a prefix of the other)")
            for line in difflib.unified_diff(
                native_out.splitlines(),
                other_out.splitlines(),
                fromfile="native stdout",
                tofile=f"{Path(args.other_runner).name} stdout",
                lineterm="",
            ):
                print(f"            {line}")
            diverged += 1
            continue

        if native_out == other_out:
            if native_status != other_status:
                print(
                    f"DIVERGE     {lox_file}  (stdout is equal, but exit status "
                    f"disagrees: native exited {native_status}, other exited {other_status})"
                )
                diverged += 1
                continue
            if lox_file.name in excluded:
                print(f"STALE       {lox_file}  (byte-identical, but marked as excluded: {excluded[lox_file.name]})")
                staled += 1
                continue
            if native_out == "":
                print(f"MATCH       {lox_file}  (empty, exit status agrees)")
            else:
                print(f"MATCH       {lox_file}")
            matched += 1
            continue

        native_lines = native_out.splitlines()
        other_lines = other_out.splitlines()
        is_permutation = sorted(native_lines) == sorted(other_lines)

        if lox_file.name in excluded and is_permutation:
            if native_status != other_status:
                print(
                    f"DIVERGE     {lox_file}  (excluded for map order, but exit status "
                    f"disagrees: native exited {native_status}, other exited {other_status})"
                )
                diverged += 1
                continue
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
            difflib.unified_diff(
                native_lines, other_lines, fromfile="native", tofile=Path(args.other_runner).name, lineterm=""
            )
        )
        for line in diff_lines[:20]:
            print(f"            {line}")
        if len(diff_lines) > 20:
            print(f"            ... ({len(diff_lines) - 20} more diff line(s))")
        diverged += 1

    print()
    print(
        f"{matched} matched, {staled} stale, {permuted} permutation-excused, {diverged} diverged, "
        f"{xfailed} expected to fail, {xpassed} passing but listed as expected to fail, "
        f"{exempted} exempt, {ahead} ahead of native"
    )
    sys.exit(1 if (diverged or staled or xpassed) else 0)


if __name__ == "__main__":
    main()
