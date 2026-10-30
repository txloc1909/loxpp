#!/usr/bin/env python3
"""Checks that each tool which spawns a runner from its command line refuses a
runner that cannot run with exit 2 and a message, and never with a traceback.

Exit 1 is the code those tools use for a difference in the program under test.
A runner path with a typo, or a wrapper whose shebang names a missing
interpreter, is a setup mistake: it must not read as a failed case, and it
must not end as a Python traceback from inside subprocess.

Each row of TOOLS names a tool and the runner slots on its command line. For
every slot the checker runs the tool twice, with the other slots set to a
runner that starts:
  - the slot names a path that does not exist: exit 2, and stderr names it
    as missing;
  - the slot names an executable file whose interpreter is missing: exit 2,
    and stderr names it as a runner that cannot run.
In both runs stderr holds no "Traceback".

tools/diff_runtimes.py and tools/check_diff_runtimes_xfail.py hold their own
cases for the same rule (EXTERNAL). Every other tools/*.py that spawns a
process is in NOT_A_RUNNER_TOOL with the reason it has no runner on its
command line, so a new tool that spawns a runner cannot join the tree without
a row here.

Usage:
    tools/check_runner_precondition.py

Exit status: 0 when every case holds, 1 when a case fails, 2 when the table
and the tools disagree.
"""

import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

REPO_ROOT = Path(__file__).resolve().parent.parent
TOOLS_DIR = REPO_ROOT / "tools"
TIMEOUT_SECONDS = 120


@dataclass(frozen=True)
class Tool:
    script: str
    slots: int
    argv: Callable[[list[str]], list[str]]


def py(script: str) -> list[str]:
    return [sys.executable, str(TOOLS_DIR / script)]


TOOLS = (
    Tool("check_spec_probes.py", 1,
         lambda r: py("check_spec_probes.py") + [r[0], "test/translation-probes/"]),
    Tool("check_cr_bytes.py", 2,
         lambda r: py("check_cr_bytes.py") + [r[0], r[1]]),
    Tool("check_print_lines.py", 1,
         lambda r: py("check_print_lines.py") + [r[0]]),
    Tool("check_examples.py", 1,
         lambda r: py("check_examples.py") + [r[0], "examples/"]),
    Tool("check_gc_min_heap.py", 1,
         lambda r: py("check_gc_min_heap.py") + [r[0]]),
    Tool("check_gc_sweep_linear.py", 1,
         lambda r: py("check_gc_sweep_linear.py") + [r[0]]),
    Tool("check_gc_trace.py", 1,
         lambda r: py("check_gc_trace.py") + [r[0]]),
    Tool("check_repl_session.py", 1,
         lambda r: py("check_repl_session.py") + [r[0]]),
    Tool("check_bootstrap_map_iteration.py", 2,
         lambda r: py("check_bootstrap_map_iteration.py") + ["--native", r[0], "--bootstrap", r[1]]),
    Tool("check_bootstrap_operator_overload.py", 2,
         lambda r: py("check_bootstrap_operator_overload.py") + ["--native", r[0], "--bootstrap", r[1]]),
    Tool("check_bootstrap_match_arm_break.py", 2,
         lambda r: py("check_bootstrap_match_arm_break.py") + ["--native", r[0], "--bootstrap", r[1]]),
    Tool("check_bootstrap_stdin_exit.py", 1,
         lambda r: py("check_bootstrap_stdin_exit.py") + [r[0]]),
    Tool("check_bootstrap_park_state.py", 1,
         lambda r: py("check_bootstrap_park_state.py") + ["--native", r[0]]),
    Tool("check_compile_error_cases.py", 1,
         lambda r: py("check_compile_error_cases.py") + ["--native", r[0]]),
    Tool("lsp_smoke.py", 1,
         lambda r: py("lsp_smoke.py") + [r[0]]),
    Tool("check_fault_table.py", 3,
         lambda r: py("check_fault_table.py")
         + ["--native", r[0], "--jvm", r[1], "--bootstrap", r[2], "--only", "arithmetic_type_error"]),
    Tool("check_file_durability.sh", 1,
         lambda r: ["bash", str(TOOLS_DIR / "check_file_durability.sh"), r[0]]),
)

EXTERNAL = {
    "diff_runtimes.py": "tools/check_diff_runtimes_xfail.py, case 14",
    "check_diff_runtimes_xfail.py": "its own case 14",
}

NOT_A_RUNNER_TOOL = {
    "check_docs_placement.py": "spawns git",
    "check_nvim_syntax.py": "spawns nvim, a fixed tool",
    "check_bootstrap_stack_overflow.py": "spawns build/loxpp and the bootstrap wrapper by fixed repo path; no runner on the command line",
    "run_bootstrap_error_kind_causes.py": "spawns the bootstrap wrapper by fixed repo path; no runner on the command line",
    "check_runner_precondition.py": "this checker",
}

# Variables that make a tool skip (exit 125) before it reaches its spawn.
SKIP_VARIABLES = (
    "LOXPP_EXEC_TRACE_ENABLED",
    "LOXPP_BOOTSTRAP_TRACE_ENABLED",
)


def table_problems() -> list[str]:
    """The tools that spawn a process, against the rows above."""
    problems = []
    known = {t.script for t in TOOLS} | set(EXTERNAL) | set(NOT_A_RUNNER_TOOL)
    for path in sorted(TOOLS_DIR.glob("*.py")):
        text = path.read_text(encoding="utf-8")
        spawns = "subprocess." in text
        uses_helper = "runner_precondition" in text
        if (spawns or uses_helper) and path.name not in known and path.name != "runner_precondition.py":
            problems.append(
                f"{path.name} spawns a process and has no row in TOOLS, EXTERNAL or NOT_A_RUNNER_TOOL"
            )
    for tool in TOOLS:
        path = TOOLS_DIR / tool.script
        if not path.is_file():
            problems.append(f"TOOLS names {tool.script}, which does not exist")
        elif "runner_precondition" not in path.read_text(encoding="utf-8") and path.suffix == ".py":
            problems.append(f"{tool.script} has a row but does not import runner_precondition")
    for name in list(EXTERNAL) + list(NOT_A_RUNNER_TOOL):
        if not (TOOLS_DIR / name).is_file():
            problems.append(f"{name} is listed but does not exist")
    return problems


def run_case(argv: list[str]) -> tuple[int, str]:
    env = {k: v for k, v in os.environ.items() if k not in SKIP_VARIABLES}
    completed = subprocess.run(
        argv, capture_output=True, text=True, timeout=TIMEOUT_SECONDS, cwd=REPO_ROOT, env=env
    )
    return completed.returncode, completed.stderr


def main() -> int:
    problems = table_problems()
    if problems:
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        return 2

    failures: list[str] = []
    cases = 0
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        starts = work / "starts.sh"
        starts.write_text("#!/bin/sh\nexit 0\n")
        starts.chmod(0o755)
        cannot_start = work / "cannot_start.sh"
        cannot_start.write_text("#!/nonexistent/interpreter\necho never\n")
        cannot_start.chmod(0o755)
        missing = work / "no_such_runner"

        for tool in TOOLS:
            for slot in range(tool.slots):
                for label, bad, wanted in (
                    ("a path that does not exist", missing, f"{missing} does not exist"),
                    ("a runner that cannot start", cannot_start, f"{cannot_start} cannot run"),
                ):
                    runners = [str(starts)] * tool.slots
                    runners[slot] = str(bad)
                    code, stderr = run_case(tool.argv(runners))
                    cases += 1
                    where = f"{tool.script} slot {slot + 1} of {tool.slots}, {label}"
                    if code != 2:
                        failures.append(f"{where}: exit {code}, expected 2: {stderr.strip()[-300:]!r}")
                    if wanted not in stderr:
                        failures.append(f"{where}: stderr does not say {wanted!r}: {stderr.strip()[-300:]!r}")
                    if "Traceback" in stderr:
                        failures.append(f"{where}: stderr holds a Python traceback: {stderr.strip()[-300:]!r}")

    if failures:
        print(f"{len(failures)} problem(s) in {cases} case(s):", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(f"OK: {cases} case(s), {len(TOOLS)} tool(s) refuse a runner that cannot run with exit 2")
    return 0


if __name__ == "__main__":
    sys.exit(main())
