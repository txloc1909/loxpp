#!/usr/bin/env python3
"""Checks that tools/diff_runtimes.py refuses an expected-failure entry that the
run cannot grade, and accepts one that it can.

Both flags grade a program by spec-probe criteria, so an entry that the run
never grades is never counted: it prints nothing where a listed probe the
consumer passes prints XPASS and exits 1. A rename therefore leaves an entry
that reads as satisfied, and a gate that runs only this script stays green over
the gap the list claims to record. tools/check_spec_probes.py already rejects
that shape; this pins the same rule on the sibling tool (#858, #862).

Six cases, each on an explicit program list, so no corpus is walked:
  1. a name no program has, in --xfail, exits 2 and names the file and the
     entry, and prints no comparison first;
  2. the same name in --oracle-xfail exits 2 the same way;
  3. a real .lox program that is not a spec probe is refused, so a name cannot
     be satisfied by a program the flags cannot grade;
  4. a real spec probe gets past the check: the run reaches the comparison and
     reports that probe as XFAIL or XPASS;
  5. a listed probe that --consumer exempts is refused: the run skips it before
     either flag is consulted;
  6. a malformed probe in the run is a clean exit 2 with a message, not a
     Python traceback, which would read as a divergence of the whole run.

Both runners are the native binary, so the comparison is native against
itself: it needs no managed toolchain and no runtime jar.

Usage:
    tools/check_diff_runtimes_xfail.py --native build/loxpp [--probes DIR]

Exits 0 when all six cases hold, 1 with a report per failed case otherwise,
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
# A well-formed probe that one consumer does not bind, in this run's own
# directory, so case 5 needs no corpus file and no replacement probe behind it.
EXEMPT_PROBE = """print 1;
// NOT-BINDING: bootstrap bootstrap-only/one_thread_blocking_call.lox
// STATUS: ok
// CHECK: 1
"""
# A probe whose directives contradict each other: the spec binds no stderr text,
# so tools/check_spec_probes.py's parse raises for it.
MALFORMED_PROBE = """print 1;
// STATUS: ok
// CHECK: 1
// STDERR: the spec binds no stderr text
"""


def run(
    native: str,
    paths: list[Path],
    flag: str,
    list_path: Path,
    consumer: str | None = None,
) -> tuple[int, str, str]:
    argv = [sys.executable, str(DIFF_RUNTIMES), native, native, *map(str, paths)]
    if consumer is not None:
        argv += ["--consumer", consumer]
    argv += [flag, str(list_path)]
    completed = subprocess.run(argv, capture_output=True, text=True, timeout=300)
    return completed.returncode, completed.stdout, completed.stderr


def refusal(list_path: Path, name: str) -> str:
    return f"{list_path} lists {name}, which is not a spec probe of this run"


def expects(failures: list[str], case: str, condition: bool, detail: str) -> None:
    if not condition:
        failures.append(f"{case}: {detail}")


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
            expects(
                failures,
                flag,
                code == 2,
                f"exit {code}, expected 2 (the run must refuse, not compare)",
            )
            expects(
                failures,
                flag,
                refusal(list_path, BOGUS_PROBE) in err,
                f"stderr does not name the dead entry; expected "
                f"{refusal(list_path, BOGUS_PROBE)!r}, got {err.strip()!r}",
            )
            expects(
                failures,
                flag,
                not out.strip(),
                f"printed {out.strip()!r} before refusing, so a reader cannot "
                f"tell the refusal from a comparison",
            )

        # Case 3: a real program that is not a spec probe. The flags grade a
        # program by spec-probe criteria, so naming this one would leave an
        # entry that is never counted.
        plain = work / "plain_program.lox"
        plain.write_text(NOT_A_PROBE + "\n")
        list_path = work / "nonprobe.txt"
        list_path.write_text(f"{plain.name}  a reason\n")
        code, out, err = run(args.native, [plain], "--xfail", list_path)
        expects(
            failures,
            "a non-probe program",
            code == 2,
            f"exit {code}, expected 2. Nothing grades it, so its entry would "
            f"never be counted",
        )
        expects(
            failures,
            "a non-probe program",
            refusal(list_path, plain.name) in err,
            f"stderr does not name it; expected {refusal(list_path, plain.name)!r}, "
            f"got {err.strip()!r}",
        )

        # Case 4: a real spec probe of the run must get past the check.
        real_list = work / "real.txt"
        real_list.write_text(f"{REAL_PROBE}  a reason\n")
        code, out, err = run(args.native, [real_probe], "--xfail", real_list)
        expects(
            failures,
            "a real spec probe",
            code != 2,
            f"refused: exit 2, stderr {err.strip()!r}. The check must resolve "
            f"against the probes of the run, not against some other directory",
        )
        expects(
            failures,
            "a real spec probe",
            any(mark in out for mark in ("XFAIL", "XPASS")),
            f"did not reach the comparison: exit {code}, stdout {out.strip()!r}",
        )

        # Case 5: a listed probe that --consumer exempts. The run skips it
        # before either flag is consulted, so its entry would never be counted.
        exempt_dir = work / "exempt"
        exempt_dir.mkdir()
        exempt_probe = exempt_dir / "exempt_probe.lox"
        exempt_probe.write_text(EXEMPT_PROBE)
        list_path = work / "exempt.txt"
        list_path.write_text(f"{exempt_probe.name}  a reason\n")
        code, out, err = run(
            args.native, [exempt_probe], "--xfail", list_path, consumer="bootstrap"
        )
        expected = f"{list_path} lists {exempt_probe.name}, which does not bind bootstrap"
        expects(
            failures,
            "an exempted listed probe",
            code == 2,
            f"exit {code}, expected 2. --consumer skips the probe before the "
            f"list is consulted, so the entry would read as satisfied",
        )
        expects(
            failures,
            "an exempted listed probe",
            expected in err,
            f"stderr does not say the probe is exempt; expected {expected!r}, "
            f"got {err.strip()!r}",
        )

        # Case 6: a malformed probe in the run. A traceback and exit 1 read as a
        # divergence of the whole run, which is not what a bad probe file is.
        bad_dir = work / "malformed"
        bad_dir.mkdir()
        bad_probe = bad_dir / "malformed_probe.lox"
        bad_probe.write_text(MALFORMED_PROBE)
        list_path = work / "malformed.txt"
        list_path.write_text(f"{bad_probe.name}  a reason\n")
        code, out, err = run(args.native, [bad_probe], "--xfail", list_path)
        expects(
            failures,
            "a malformed probe",
            code == 2,
            f"exit {code}, expected 2 (a clean refusal, not a crash)",
        )
        expects(
            failures,
            "a malformed probe",
            "Traceback" not in err,
            f"stderr holds a Python traceback: {err.strip()!r}",
        )
        expects(
            failures,
            "a malformed probe",
            "the spec binds no stderr text" in err,
            f"stderr does not name the malformed directive: {err.strip()!r}",
        )

    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1

    print(
        "check_diff_runtimes_xfail.py: both flags refuse an entry the run cannot "
        "grade (no probe, a non-probe program, an exempted probe), a malformed "
        "probe is a clean refusal, and a real probe reaches the comparison"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
