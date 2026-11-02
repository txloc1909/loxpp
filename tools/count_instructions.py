#!/usr/bin/env python3
"""Counts the user-space instructions a command runs.

The dev image has neither `perf` nor `valgrind`, so this uses `perf_event_open`
directly through `ctypes`. The event counts only the instructions that the
program itself runs (`exclude_kernel`), on the main thread of the process.

Usage:
  tools/count_instructions.py [--runs R] [--attempts N] -- <command> [args...]

Each run prints one line to stdout: `run <index> <instructions>`, and the last
line is `median <instructions>`.

Why one run is several attempts. On a loaded host the kernel does not always
keep a counting event on the CPU for the whole run; the time the event was
enabled can exceed the time it ran, and the count is then short by an amount
it cannot report. The loss only ever subtracts, so the tool repeats the run
and keeps the largest count: a short count cannot win the comparison. `lost`
reports how much of the best attempt's window the event spent off the CPU, and
`--max-lost` fails the run when even the best attempt lost more than a bound,
so a short count is never printed as the answer.

The default seccomp profile of podman refuses `perf_event_open`, so a
container that runs this tool needs `--security-opt seccomp=unconfined`. It is
a tool to run by hand, not part of `ctest`.
"""

import argparse
import ctypes
import ctypes.util
import os
import signal
import struct
import sys
from pathlib import Path

from runner_precondition import refuse_unrunnable, require_runner

PERF_TYPE_HARDWARE = 0
PERF_COUNT_HW_INSTRUCTIONS = 1

# struct perf_event_attr, the leading fields only. The kernel zero-checks the
# bytes past `size`, and ctypes zeroes the whole structure.
class PerfEventAttr(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("size", ctypes.c_uint32),
        ("config", ctypes.c_uint64),
        ("sample_period", ctypes.c_uint64),
        ("sample_type", ctypes.c_uint64),
        ("read_format", ctypes.c_uint64),
        ("flags", ctypes.c_uint64),
        ("wakeup_events", ctypes.c_uint32),
        ("bp_type", ctypes.c_uint32),
        ("config1", ctypes.c_uint64),
        ("config2", ctypes.c_uint64),
    ]


FLAG_DISABLED = 1 << 0
FLAG_EXCLUDE_KERNEL = 1 << 5
FLAG_EXCLUDE_HV = 1 << 6

# read_format: the count first, then the time the event was enabled and the
# time it was on a CPU. Equal times mean the whole run was counted.
READ_TIME_ENABLED = 1 << 0
READ_TIME_RUNNING = 1 << 1
READ_FORMAT = READ_TIME_ENABLED | READ_TIME_RUNNING

IOC_ENABLE = 0x2400
IOC_RESET = 0x2403

# __NR_perf_event_open, x86_64.
SYS_PERF_EVENT_OPEN = 298

LIBC = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)


def perf_event_open(pid: int) -> int:
    attr = PerfEventAttr()
    attr.type = PERF_TYPE_HARDWARE
    attr.size = ctypes.sizeof(attr)
    attr.config = PERF_COUNT_HW_INSTRUCTIONS
    attr.flags = FLAG_DISABLED | FLAG_EXCLUDE_KERNEL | FLAG_EXCLUDE_HV
    attr.read_format = READ_FORMAT
    fd = LIBC.syscall(
        ctypes.c_long(SYS_PERF_EVENT_OPEN),
        ctypes.byref(attr),
        ctypes.c_int(pid),
        ctypes.c_int(-1),  # the task's current CPU, not a pinned one
        ctypes.c_int(-1),  # no group
        ctypes.c_ulong(0),
    )
    if fd < 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))
    return fd


def ioctl(fd: int, request: int) -> None:
    if LIBC.ioctl(ctypes.c_int(fd), ctypes.c_ulong(request), ctypes.c_int(0)) < 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))


def require_startable(path: str) -> None:
    """Stops with exit 2 for a runner that the kernel would refuse to start.

    The child execs only after the counting event exists, so a refused exec
    would be reported after the event call, and on a container that refuses
    `perf_event_open` the count fails first and names the wrong problem. A
    script whose interpreter is missing is the common case, and it is visible
    here without a spawn.
    """
    require_runner(path)
    with open(path, "rb") as script:
        head = script.read(4096)
    if not head.startswith(b"#!"):
        return
    line = head[2:].split(b"\n", 1)[0]
    words = line.split()
    if not words:
        print(f"error: {path} cannot run: the #! line names no interpreter", file=sys.stderr)
        sys.exit(2)
    interpreter = Path(words[0].decode(errors="surrogateescape"))
    if not interpreter.is_file() or not os.access(interpreter, os.X_OK):
        print(
            f"error: {path} cannot run: its interpreter {interpreter} cannot run",
            file=sys.stderr,
        )
        sys.exit(2)


def attempt(command: list[str], core: int | None) -> tuple[int, int]:
    """Runs the command once. Returns (instructions, nanoseconds off the CPU)."""
    # The child waits for a byte before it execs, so the event exists and is
    # enabled before the program's first instruction. A signal handshake
    # instead lets the child run part of the program first, and the count is
    # then short by an amount no run reports.
    read_end, write_end = os.pipe()
    error_read, error_write = os.pipe()
    pid = os.fork()
    if pid == 0:
        try:
            os.close(write_end)
            os.close(error_read)
            if core is not None:
                os.sched_setaffinity(0, {core})
            # Only the byte this tool writes starts the command. End of file
            # means that the parent died before it could count anything, and
            # running the command then would leave a stray process behind.
            if os.read(read_end, 1) != b"x":
                os._exit(127)
            os.close(read_end)
            try:
                os.execv(command[0], command)
            except OSError as error:
                os.write(error_write, f"{error.errno} {error.strerror}".encode())
                os._exit(126)
        except BaseException:
            pass
        os._exit(127)
    os.close(read_end)
    os.close(error_write)
    try:
        fd = perf_event_open(pid)
    except BaseException:
        # Nothing will write the byte, so end the child here: closing the pipe
        # would let it run the command with no event, and leaving it blocked
        # would orphan it.
        os.close(write_end)
        os.close(error_read)
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        raise
    try:
        ioctl(fd, IOC_RESET)
        ioctl(fd, IOC_ENABLE)
        os.write(write_end, b"x")
        os.close(write_end)
        _, status = os.waitpid(pid, 0)
        count, enabled, running = struct.unpack("QQQ", os.read(fd, 24))
    finally:
        os.close(fd)
    error_text = os.read(error_read, 4096).decode(errors="replace")
    os.close(error_read)
    if not os.WIFEXITED(status):
        raise SystemExit(f"command died to signal {os.WTERMSIG(status)}")
    code = os.WEXITSTATUS(status)
    if code == 126 and error_text:
        number, _, message = error_text.partition(" ")
        refuse_unrunnable(command[0], OSError(int(number), message))
    if code != 0:
        raise SystemExit(f"command exited {code}")
    return count, max(0, enabled - running)


def measure(command: list[str], core: int | None, attempts: int) -> tuple[int, int]:
    best = None
    for _ in range(attempts):
        count, lost = attempt(command, core)
        if best is None or count > best[0]:
            best = (count, lost)
    assert best is not None
    return best


def count_at_least_one(text: str) -> int:
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError(f"{text} is not 1 or more")
    return value


def nanoseconds_at_least_zero(text: str) -> int:
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError(f"{text} is not 0 or more")
    return value


def median(values: list[int]) -> int:
    ordered = sorted(values)
    mid = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[mid]
    return (ordered[mid - 1] + ordered[mid]) // 2


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Count the user-space instructions a command runs."
    )
    parser.add_argument("--core", type=int, default=None, help="pin to this core")
    parser.add_argument(
        "--runs", type=count_at_least_one, default=1, help="number of runs"
    )
    parser.add_argument(
        "--attempts",
        type=count_at_least_one,
        default=15,
        help="attempts per run; the largest count wins (default 15)",
    )
    parser.add_argument(
        "--max-lost",
        type=nanoseconds_at_least_zero,
        default=50_000,
        help="nanoseconds the best attempt may spend off the CPU (default 50000)",
    )
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("no command given")
    require_startable(command[0])
    counts = []
    try:
        for index in range(args.runs):
            count, lost = measure(command, args.core, args.attempts)
            counts.append(count)
            print(f"run {index} {count} lost {lost}", flush=True)
            if lost > args.max_lost:
                print(
                    f"count_instructions: the best of {args.attempts} attempts "
                    f"spent {lost} ns off the CPU, over --max-lost "
                    f"{args.max_lost}; the count is short by an amount this "
                    "run cannot bound",
                    file=sys.stderr,
                )
                return 2
    except OSError as error:
        print(f"count_instructions: {error}", file=sys.stderr)
        return 2
    print(f"median {median(counts)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
