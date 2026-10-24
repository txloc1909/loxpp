#!/usr/bin/env python3
"""Checks that tools/diff_runtimes.py refuses an expected-failure entry that
names no program of the run, and accepts one that does.

A dead entry is silent. A listed probe that the consumer passes reports XPASS,
but a listed name that no longer exists reports nothing at all, so a run stays
green over a gap that the list claims to record. tools/check_spec_probes.py
already rejects that shape; this script pins the same rule on the sibling tool,
which is the only differential gate `loxpp_gate`'s backend scope runs (#858).

Three cases, each on one explicit program, so no corpus is walked:
  1. a bogus name in --xfail exits 2 and names the file and the entry;
  2. a bogus name in --oracle-xfail exits 2 the same way;
  3. a real probe name in --xfail gets past the check: the run reaches the
     comparison and reports that probe as XFAIL or XPASS, never as an error.

Both runners are the native binary, so the comparison is native against
itself: it needs no managed toolchain and no runtime jar.

Usage:
    tools/check_diff_runtimes_xfail.py --native build/loxpp [--probes DIR]

Exits 0 when all three cases hold, 1 with a report per failed case otherwise.
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DIFF_RUNTIMES = REPO_ROOT / "tools" / "diff_runtimes.py"

# A spec probe that passes quickly on any build, and whose file name is used as
# the "real entry" of case 3. Case 3 asserts only that the check let the run
# through, so it does not matter whether the probe itself passes here.
REAL_PROBE = "185_int_truthiness.lox"
BOGUS_PROBE = "no_such_probe_at_all.lox"


def run(native: str, probe: Path, flag: str, list_path: Path) -> tuple[int, str, str]:
    completed = subprocess.run(
        [sys.executable, str(DIFF_RUNTIMES), native, native, str(probe), flag, str(list_path)],
        capture_output=True,
        text=True,
        timeout=300,
    )
    return completed.returncode, completed.stdout, completed.stderr


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

        for flag in ("--xfail", "--oracle-xfail"):
            list_path = work / f"bogus{flag.lstrip('-')}.txt"
            list_path.write_text(
                f"# a list whose only entry names no program of this run\n"
                f"{BOGUS_PROBE}  a reason that does not save it\n"
            )
            code, out, err = run(args.native, real_probe, flag, list_path)
            expected = (
                f"{list_path} lists {BOGUS_PROBE}, which is not a program of this run"
            )
            if code != 2:
                failures.append(
                    f"{flag}: exit {code}, expected 2 (the run must refuse, not compare)"
                )
            if expected not in err:
                failures.append(
                    f"{flag}: stderr does not name the dead entry; "
                    f"expected {expected!r}, got {err.strip()!r}"
                )
            if out.strip():
                failures.append(
                    f"{flag}: printed {out.strip()!r} before refusing, so a reader "
                    f"cannot tell the refusal from a comparison"
                )

        real_list = work / "real.txt"
        real_list.write_text(f"{REAL_PROBE}  a reason\n")
        code, out, err = run(args.native, real_probe, "--xfail", real_list)
        if code == 2:
            failures.append(
                f"a real entry was refused: exit 2, stderr {err.strip()!r}. "
                f"The check must resolve against the programs of the run, not "
                f"against the whole directory"
            )
        elif not any(mark in out for mark in ("XFAIL", "XPASS")):
            failures.append(
                f"a real entry did not reach the comparison: exit {code}, "
                f"stdout {out.strip()!r}"
            )

    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1

    print(
        "check_diff_runtimes_xfail.py: both flags refuse a dead entry, and a real "
        "entry reaches the comparison"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
