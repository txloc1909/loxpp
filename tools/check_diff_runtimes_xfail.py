#!/usr/bin/env python3
"""Checks that tools/diff_runtimes.py refuses an expected-failure entry that the
run cannot grade, and accepts one that it can.

Both flags grade a program by spec-probe criteria, so an entry that the run
never grades is never counted: it prints nothing where a listed probe the
consumer passes prints XPASS and exits 1. A rename therefore leaves an entry
that reads as satisfied, and a gate that runs only this script stays green over
the gap the list claims to record. tools/check_spec_probes.py already rejects
that shape; this pins the same rule on the sibling tool (#858, #862).

Nine cases, each on an explicit program list, so no corpus is walked:
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
     Python traceback, which would read as a divergence of the whole run;
  7. a probe on BOTH lists reports AHEAD when native's gap is the reason a
     reader needs, and states that the consumer line is now stale;
  8. a probe on both lists that both sides fail reports XFAIL with BOTH
     reasons, so neither list's reason is dropped;
  9. a probe on both lists that times out names the side that did not finish,
     with that side's reason only, and claims nothing about the other side.

Cases 1 to 6 run native against itself, so they need no managed toolchain and
no runtime jar. Cases 7 to 9 use a stub script as a runner, because they need
one side to pass, to fail, or to hang where the other side does not.

Usage:
    tools/check_diff_runtimes_xfail.py --native build/loxpp [--probes DIR]

Exits 0 when all nine cases hold, 1 with a report per failed case otherwise,
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


# A probe whose program fails on native and passes on a runner that only echoes
# the CHECK line, so case 7 can hold "native does not pass it" and "the other
# runner does" at once.
BOTH_LISTS_PROBE = """print "boom";
print undeclared_name;
// STATUS: ok
// CHECK: boom
"""


def run(
    native: str,
    paths: list[Path],
    lists: list[tuple[str, Path]],
    consumer: str | None = None,
    runner: str | None = None,
    timeout: float | None = None,
) -> tuple[int, str, str]:
    argv = [sys.executable, str(DIFF_RUNTIMES), native, runner or native, *map(str, paths)]
    if consumer is not None:
        argv += ["--consumer", consumer]
    for flag, list_path in lists:
        argv += [flag, str(list_path)]
    if timeout is not None:
        argv += ["--timeout", str(timeout)]
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
            code, out, err = run(args.native, [real_probe], [(flag, list_path)])
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
        code, out, err = run(args.native, [plain], [("--xfail", list_path)])
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
        code, out, err = run(args.native, [real_probe], [("--xfail", real_list)])
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
            args.native,
            [exempt_probe],
            [("--xfail", list_path)],
            consumer="bootstrap",
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
        code, out, err = run(args.native, [bad_probe], [("--xfail", list_path)])
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

        # Case 7: one probe on both lists. The consumer entry used to continue
        # first, so native's reason was never shown. A stub runner passes where
        # native fails, which is the state the overlap describes: native
        # regressed under a consumer that did not notice.
        both_dir = work / "both"
        both_dir.mkdir()
        both_probe = both_dir / "both_lists.lox"
        both_probe.write_text(BOTH_LISTS_PROBE)
        stub = work / "stub_runner.sh"
        stub.write_text('#!/bin/sh\n# Ignores its argument on purpose: it stands in for a consumer that\n# passes a probe native still fails.\necho boom\nexit 0\n')
        stub.chmod(0o755)
        consumer_list = work / "both_consumer.txt"
        consumer_list.write_text(f"{both_probe.name}  consumer is behind\n")
        oracle_list = work / "both_oracle.txt"
        oracle_list.write_text(f"{both_probe.name}  native regressed\n")

        code, out, err = run(
            args.native,
            [both_probe],
            [("--xfail", consumer_list), ("--oracle-xfail", oracle_list)],
            runner=str(stub),
        )
        expects(
            failures,
            "a probe on both lists",
            "AHEAD" in out and "native regressed" in out,
            f"did not report AHEAD with the oracle reason: exit {code}, "
            f"stdout {out.strip()!r}, stderr {err.strip()!r}",
        )
        expects(
            failures,
            "a probe on both lists",
            code == 0,
            f"exit {code}, expected 0: AHEAD is not a divergence",
        )

        # The same probe with the consumer list alone still reports XFAIL, so
        # case 7 measures the overlap rule and not the ordinary path.
        code, out, err = run(
            args.native, [both_probe], [("--xfail", consumer_list)], runner=str(stub)
        )
        expects(
            failures,
            "a probe on the consumer list alone",
            "XFAIL" in out and "consumer is behind" in out,
            f"did not report XFAIL with the consumer reason: exit {code}, "
            f"stdout {out.strip()!r}",
        )

        # Case 8: both lists, and both sides fail the probe. Neither list's
        # reason may be dropped: the line names the consumer's and native's.
        failing_stub = work / "failing_runner.sh"
        failing_stub.write_text('#!/bin/sh\n# Fails the probe on purpose.\nexit 1\n')
        failing_stub.chmod(0o755)
        code, out, err = run(
            args.native,
            [both_probe],
            [("--xfail", consumer_list), ("--oracle-xfail", oracle_list)],
            runner=str(failing_stub),
        )
        expects(
            failures,
            "both lists, both sides fail",
            "XFAIL" in out,
            f"did not report XFAIL: exit {code}, stdout {out.strip()!r}",
        )
        expects(
            failures,
            "both lists, both sides fail",
            "consumer is behind" in out and "native: native regressed" in out,
            f"dropped a graded reason: expected both on the line, got {out.strip()!r}",
        )

        # Case 9: both lists, and a side that never finishes. The run grades
        # nothing, so it must name the side that did not finish, carry only that
        # side's reason, and claim nothing about the other side.
        slow_stub = work / "slow_runner.sh"
        slow_stub.write_text('#!/bin/sh\n# Never finishes within the timeout below.\nsleep 30\n')
        slow_stub.chmod(0o755)
        code, out, err = run(
            args.native,
            [both_probe],
            [("--xfail", consumer_list), ("--oracle-xfail", oracle_list)],
            runner=str(slow_stub),
            timeout=1,
        )
        expects(
            failures,
            "the other runner times out",
            "XFAIL" in out and "the other runner did not finish" in out,
            f"did not name the side that timed out: exit {code}, stdout {out.strip()!r}, "
            f"stderr {err.strip()!r}",
        )
        expects(
            failures,
            "the other runner times out",
            "consumer is behind" in out and "native regressed" not in out,
            f"claimed a reason for the side that finished: {out.strip()!r}",
        )

        code, out, err = run(
            str(slow_stub),
            [both_probe],
            [("--xfail", consumer_list), ("--oracle-xfail", oracle_list)],
            runner=args.native,
            timeout=1,
        )
        expects(
            failures,
            "native times out",
            "XFAIL" in out and "native did not finish" in out,
            f"did not name native as the side that timed out: exit {code}, "
            f"stdout {out.strip()!r}, stderr {err.strip()!r}",
        )
        expects(
            failures,
            "native times out",
            "native regressed" in out and "consumer is behind" not in out,
            f"claimed a reason for the side that finished: {out.strip()!r}",
        )

    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1

    print(
        "check_diff_runtimes_xfail.py: both flags refuse an entry the run cannot "
        "grade (no probe, a non-probe program, an exempted probe), a malformed "
        "probe is a clean refusal, a real probe reaches the comparison, and a "
        "probe on both lists reports every reason the run graded"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
