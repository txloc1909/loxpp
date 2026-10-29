"""Exit 2 for a runner path that cannot run, shared by the tools that spawn one.

A runner comes from the command line, so a typo or a wrapper whose interpreter
is missing is a setup mistake. Exit 1 is the code those tools use for a
difference in the program under test, so a setup mistake must not reach it, and
it must not end as a Python traceback from inside subprocess.

This module imports nothing from tools/, so any tool can import it without an
import cycle.
"""

import os
import sys
from pathlib import Path


def refuse_unrunnable(path: str, exc: OSError) -> None:
    """Stops with exit 2 for a runner that exec cannot start.

    require_runner cannot see this shape: the file exists, it is a file, and it
    is executable, but the kernel still refuses to start it. A wrapper whose
    shebang names a missing interpreter is the common case. That is a
    precondition like a missing path, so it must not read as a comparison that
    failed.
    """
    print(f"error: {path} cannot run: {exc}", file=sys.stderr)
    sys.exit(2)


def require_runner(path: str) -> None:
    """Stops with exit 2 unless path names an executable file.

    A runner comes from the command line, so a typo is a precondition of the
    run, not a comparison that failed: it must fail here, before the first
    program runs, and not as a traceback from inside subprocess or as a
    divergence of the whole walk.
    """
    runner = Path(path)
    if not runner.exists():
        print(f"error: {path} does not exist", file=sys.stderr)
        sys.exit(2)
    if not runner.is_file():
        print(f"error: {path} is not a file", file=sys.stderr)
        sys.exit(2)
    if not os.access(runner, os.X_OK):
        print(f"error: {path} is not executable", file=sys.stderr)
        sys.exit(2)
