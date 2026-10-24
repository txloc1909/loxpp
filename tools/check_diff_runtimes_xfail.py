#!/usr/bin/env python3
"""Checks that tools/diff_runtimes.py refuses an expected-failure entry that no
spec probe of the run backs, and accepts one that a spec probe does.

Both flags grade a program by spec-probe criteria, so an entry that no spec
probe backs is never counted: it prints nothing where a listed probe the
consumer passes prints XPASS and exits 1. A rename therefore leaves an entry
that reads as satisfied, and a gate that runs only this script stays green over
the gap the list claims to record. tools/check_spec_probes.py already rejects
that shape; this pins the same rule on the sibling tool (#858).

Four cases, each on an explicit program list, so no corpus is walked:
  1. a name no program has, in --xfail, exits 2 and names the file and the
     entry, and prints no comparison first;
  2. the same name in --oracle-xfail exits 2 the same way;
  3. a real .lox program that is not a spec probe is refused in --xfail, so a
     name cannot be satisfied by a program the flags cannot grade;
  4. a real spec probe in --xfail gets past the check: the run reaches the
     comparison and reports that probe as XFAIL or XPASS.

Both runners are the native binary, so the comparison is native against
itself: it needs no managed toolchain and no runtime jar.

Usage:
    tools/check_diff_runtimes_xfail.py --native build/loxpp [--probes DIR]

Exits 0 when all four cases hold, 1 with a report per failed case otherwise,
and 2 when the probe this script needs is missing.
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DIFF_RUNTIMES = REPO_ROOT / "tools" / "diff_runtimes.py"

# A spec probe of test/translation-probes/ that runs in milliseconds. Case 4
# asserts only that the check let the run through, so whether this probe passes
# on the build under test does not matter: a trace build reports it XFAIL and a
# release build may report XPASS, and both are a comparison, which is the point.
REAL_PROBE = "185_int_truthiness.lox"
BOGUS_PROBE = "no_such_probe_at_all.lox"
# A program with no STATUS line: a real .lox file, and not a spec probe.
NOT_A_PROBE = "print 1;"


def run(native: str, paths: list[Path], flag: str, list_path: Path) -> tuple[int, str, str]:
    completed = subprocess.run(
        [sys.executable, str(DIFF_RUNTIMES), native, native, *map(str, paths), flag, str(list_path)],
        capture_output=True,
        text=True,
        timeout=300,
    )
    return completed.returncode, completed.stdout, completed.stderr


def refusal(list_path: Path, name: str) -> str:
    return f"{list_path} lists {name}, which is not a spec probe of this run"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", required=True, help="path to the native loxpp binary")
    parser.add_argument(
        "--probes",
        default=str(REPO_ROOT / "test" / "translation-probes"),
        help="directory holding the spec probes, default test/translation-probes",
    )
    args = parser.parse_args()

    probe_dir = Path(args.probes)
    real_probe = probe_dir / REAL_PROBE
    if not real_probe.is_file():
        print(f"error: no spec probe at {real_probe}", file=sys.stderr)
        return 2

    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)

        # Cases 1 and 2: a name that no program of the run has.
        for flag in ("--xfail", "--oracle-xfail"):
            list_path = work / f"bogus{flag.lstrip('-')}.txt"
            list_path.write_text(
                f"# a list whose only entry no program of this run backs\n"
                f"{BOGUS_PROBE}  a reason that does not save it\n"
            )
            code, out, err = run(args.native, [real_probe], flag, list_path)
            if code != 2:
                failures.append(
                    f"{flag}: exit {code}, expected 2 (the run must refuse, not compare)"
                )
            if refusal(list_path, BOGUS_PROBE) not in err:
                failures.append(
                    f"{flag}: stderr does not name the dead entry; "
                    f"expected {refusal(list_path, BOGUS_PROBE)!r}, got {err.strip()!r}"
                )
            if out.strip():
                failures.append(
                    f"{flag}: printed {out.strip()!r} before refusing, so a reader "
                    f"cannot tell the refusal from a comparison"
                )

        # Case 3: a real program that is not a spec probe. The flags grade a
        # program by spec-probe criteria, so naming this one would leave an
        # entry that is never counted.
        plain = work / "plain_program.lox"
        plain.write_text(NOT_A_PROBE + "\n")
        list_path = work / "nonprobe.txt"
        list_path.write_text(f"{plain.name}  a reason\n")
        code, out, err = run(args.native, [plain], "--xfail", list_path)
        if code != 2:
            failures.append(
                f"a non-probe program was accepted: exit {code}, expected 2. "
                f"Nothing grades it, so its entry would never be counted"
            )
        if refusal(list_path, plain.name) not in err:
            failures.append(
                f"a non-probe program: stderr does not name it; "
                f"expected {refusal(list_path, plain.name)!r}, got {err.strip()!r}"
            )

        # Case 4: a real spec probe of the run must get past the check.
        real_list = work / "real.txt"
        real_list.write_text(f"{REAL_PROBE}  a reason\n")
        code, out, err = run(args.native, [real_probe], "--xfail", real_list)
        if code == 2:
            failures.append(
                f"a real spec probe was refused: exit 2, stderr {err.strip()!r}. "
                f"The check must resolve against the probes of the run, not "
                f"against some other directory"
            )
        elif not any(mark in out for mark in ("XFAIL", "XPASS")):
            failures.append(
                f"a real spec probe did not reach the comparison: exit {code}, "
                f"stdout {out.strip()!r}"
            )

    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1

    print(
        "check_diff_runtimes_xfail.py: both flags refuse a dead entry, a program "
        "that is not a spec probe is refused, and a real probe reaches the comparison"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
