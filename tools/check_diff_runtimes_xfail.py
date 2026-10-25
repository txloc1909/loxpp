#!/usr/bin/env python3
"""Checks that tools/diff_runtimes.py refuses an expected-failure entry that the
run cannot grade, and accepts one that it can.

Both flags grade a program by spec-probe criteria, so an entry that the run
never grades is never counted: it prints nothing where a listed probe the
consumer passes prints XPASS and exits 1. A rename therefore leaves an entry
that reads as satisfied, and a gate that runs only this script stays green over
the gap the list claims to record. tools/check_spec_probes.py already rejects
that shape; this pins the same rule on the sibling tool (#858, #862).

Thirteen cases, each on an explicit program list, so no corpus is walked:
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
     with that side's reason only, and claims nothing about the other side;
  10. a probe on both lists whose consumer fails while native passes reports
     the consumer reason and says native's entry is answered, never that
     native regressed;
  11. a probe on both lists that both runs pass reports XPASS citing BOTH
     files, since both entries are stale;
  12. a list path that does not exist is no gap: exit 0, a NOTE that names the
     path, and both flags grade every program. No expected-failure list exists
     in the tree today (test/translation-probes/README.md), so a call site that
     still names one must not turn a gate red on its own.
  13. a probe on the consumer list alone that the consumer passes and native
     fails is a divergence with exit 1, which says the consumer's line is
     stale and names no consumer reason: the consumer has no gap here.

Cases 1 to 6 and case 12 run native against itself, so they need no managed
toolchain and no runtime jar. Cases 7 to 11 and 13 use a stub script as a runner,
because they need one side to pass, to fail, or to hang where the other side
does not.

Cases 10 and 11 need a probe that the build under test PASSES. A build that
prints its disassembly or its execution trace can never pass one: the trace
lands in stdout, where the probe's CHECK lines are read. On such a build the
two cases are not judged, and this script exits 125 (ctest's SKIP_RETURN_CODE)
after the other ten pass, so a trace preset reports a skip instead of a false
red or a false green.

Usage:
    tools/check_diff_runtimes_xfail.py --native build/loxpp [--probes DIR]

Exits 0 when all thirteen cases hold, 1 with a report per failed case otherwise,
2 when a precondition fails (the probe this script needs is missing, or the
binary does not run a trivial program and print its line), and 125 when the
build traces execution so that cases 10 and 11 cannot be judged.
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


def build_traces(native: str, work: Path) -> bool | None:
    """True when the build prints a trace, so no probe can pass on it.

    The debug preset can leave the disassembly and the execution trace on, and
    both write to stdout, where a probe's CHECK lines are read. One trivial
    program settles it: a build that is not traced prints exactly its one line,
    and a traced one prints that line among others.

    Returns None when the output is not a probe's output at all: a non-zero
    exit, or a run that never prints the program's own line. That is a different
    fault — a missing toolchain, a binary that is not loxpp — and must not read
    as "traces", which would turn a broken CI leg into a quiet skip.
    """
    probe = work / "trace_check.lox"
    probe.write_text('print "trace check";\n')
    completed = subprocess.run(
        [native, str(probe)], capture_output=True, text=True, timeout=120
    )
    if completed.returncode != 0:
        return None
    if completed.stdout == 'trace check\n':
        return False
    if "trace check" in completed.stdout:
        return True
    return None


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
    traced = False
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        traced_state = build_traces(args.native, work)
        if traced_state is None:
            print(
                f"error: {args.native} does not run a trivial program and print "
                f"its line, so this checker cannot read a probe's output",
                file=sys.stderr,
            )
            return 2
        traced = traced_state

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
        expects(
            failures,
            "a probe on both lists",
            "can drop its line" in out and consumer_list.name in out,
            f"did not say the consumer line is stale, so that clause has no "
            f"case that can fail: {out.strip()!r}",
        )

        # Case 13: the same state with the consumer list ALONE, so case 7
        # measures the overlap rule and this case measures the single-list one.
        # The consumer passes a probe its own list calls a gap, and native fails
        # it with no entry to explain that: the consumer's line is stale, and
        # native's failure is the reason a reader needs. Printing the consumer's
        # reason would state a gap the consumer does not have, and exit 0 would
        # hide both facts.
        consumer_only = work / "consumer_only.txt"
        consumer_only.write_text(f"{both_probe.name}  consumer is behind\n")
        code, out, err = run(
            args.native, [both_probe], [("--xfail", consumer_only)], runner=str(stub)
        )
        expects(
            failures,
            "the consumer list alone, consumer ahead of native",
            "DIVERGE" in out,
            f"did not report a divergence: exit {code}, stdout {out.strip()!r}",
        )
        expects(
            failures,
            "the consumer list alone, consumer ahead of native",
            code == 1,
            f"exit {code}, expected 1 (a stale entry is not green): "
            f"stdout {out.strip()!r}",
        )
        expects(
            failures,
            "the consumer list alone, consumer ahead of native",
            "consumer is behind" not in out,
            f"printed the consumer's reason for a probe the consumer passes: "
            f"{out.strip()!r}",
        )
        expects(
            failures,
            "the consumer list alone, consumer ahead of native",
            "stale" in out and consumer_only.name in out,
            f"did not name the stale line: {out.strip()!r}",
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

        if not traced:
            # Case 10: both lists, the consumer fails the probe and native passes
            # it. Native's entry is answered here, not confirmed, so the line must
            # not claim native regressed. The probe is written here so native passes
            # it on any build of the tree.
            native_passes_probe = work / "native_passes.lox"
            native_passes_probe.write_text('print "ok";\n// STATUS: ok\n// CHECK: ok\n')
            consumer_list_2 = work / "consumer2.txt"
            consumer_list_2.write_text(f"{native_passes_probe.name}  consumer is behind\n")
            oracle_list_2 = work / "oracle2.txt"
            oracle_list_2.write_text(f"{native_passes_probe.name}  native regressed\n")
            code, out, err = run(
                args.native,
                [native_passes_probe],
                [("--xfail", consumer_list_2), ("--oracle-xfail", oracle_list_2)],
                runner=str(failing_stub),
            )
            expects(
                failures,
                "consumer fails, native passes, both lists",
                "XFAIL" in out and "consumer is behind" in out,
                f"did not report XFAIL with the consumer reason: exit {code}, "
                f"stdout {out.strip()!r}, stderr {err.strip()!r}",
            )
            expects(
                failures,
                "consumer fails, native passes, both lists",
                "native regressed" not in out,
                f"claimed native regressed while native passes the probe: {out.strip()!r}",
            )
            expects(
                failures,
                "consumer fails, native passes, both lists",
                "native passes it now" in out,
                f"did not say native's entry is answered: {out.strip()!r}",
            )

            # Case 11: both lists, and both runs pass the probe. The line must name
            # both files, because both entries are stale; without that clause a
            # reader would edit one list and leave the other.
            passing_stub = work / "passing_runner.sh"
            passing_stub.write_text('#!/bin/sh\necho ok\nexit 0\n')
            passing_stub.chmod(0o755)
            passing_probe = work / "both_pass.lox"
            passing_probe.write_text('print "ok";\n// STATUS: ok\n// CHECK: ok\n')
            consumer_list_3 = work / "consumer3.txt"
            consumer_list_3.write_text(f"{passing_probe.name}  consumer reason\n")
            oracle_list_3 = work / "oracle3.txt"
            oracle_list_3.write_text(f"{passing_probe.name}  oracle reason\n")
            code, out, err = run(
                args.native,
                [passing_probe],
                [("--xfail", consumer_list_3), ("--oracle-xfail", oracle_list_3)],
                runner=str(passing_stub),
            )
            expects(
                failures,
                "both lists, both runs pass",
                "XPASS" in out and code == 1,
                f"exit {code}, expected 1: XPASS fails the run. stdout {out.strip()!r}, "
                f"stderr {err.strip()!r}",
            )
            expects(
                failures,
                "both lists, both runs pass",
                consumer_list_3.name in out and oracle_list_3.name in out,
                f"named one file only, so that clause has no case that can fail: "
                f"{out.strip()!r}",
            )

        # Case 12: a list path that does not exist. No consumer lists a gap
        # today, so no expected-failure file is in the tree, and a call site
        # that still names one must not turn the gate red on its own. An absent
        # path is a NOTE and grades every program, in both flags.
        for flag in ("--xfail", "--oracle-xfail"):
            absent = work / f"absent{flag.lstrip('-')}.txt"
            code, out, err = run(args.native, [real_probe], [(flag, absent)])
            expects(
                failures,
                f"an absent list path ({flag})",
                code == 0,
                f"exit {code}, expected 0 (an absent list is no gap)",
            )
            expects(
                failures,
                f"an absent list path ({flag})",
                f"NOTE: {absent} does not exist" in out,
                f"no NOTE names the absent path; stdout was {out.strip()!r}",
            )
            expects(
                failures,
                f"an absent list path ({flag})",
                "MATCH" in out,
                f"the run did not grade the program; stdout was {out.strip()!r}",
            )

    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1

    if traced:
        print(
            f"SKIP: {args.native} prints a disassembly or an execution trace, so "
            f"no probe can pass on it and cases 10 and 11 cannot be judged. The "
            f"other eleven cases pass. Use the release preset for the full check."
        )
        return 125

    print(
        "check_diff_runtimes_xfail.py: both flags refuse an entry the run cannot "
        "grade (no probe, a non-probe program, an exempted probe), a malformed "
        "probe is a clean refusal, a real probe reaches the comparison, a probe "
        "on both lists reports every reason the run graded, and a consumer entry "
        "the consumer passes is a divergence, not an XFAIL"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
