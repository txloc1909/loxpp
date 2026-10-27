#!/usr/bin/env python3
"""Checks the fault sites of the Socket and Server natives.

spec/05-stdlib.md says which Socket and Server faults are catchable and
which halt the program. This script lists every place in the native and the
JVM sources that raises a fault for a socket, and fails when a place is not
in the table below, or when a table entry has no place. So a new fault site
cannot be fatal by accident, and a catchable one cannot go back to fatal.

Native (src/stdlib/net_api.cpp):
  - `nativeRuntimeError(` is a halting fault. Only argument checks use it.
  - `raiseNet(KIND, ...)` is a catchable fault of the given kind.
  - `setFault(out, KIND, ...)` records a catchable fault in the outcome of a
    read-side step. The caller raises it with `raiseNet(out.faultKind, ...)`
    where the task runs, or `failPark(task, out.faultKind, ...)` where the task
    was parked. Those two pass a kind through and are not sites.
  - `failPark(task, KIND, ...)` with a constant kind ends a parked call with a
    catchable fault of that kind.
  - `writeStreamOrError(` must not appear: it is a halting fault, and the
    Socket write path uses `stepWrite` and `raiseNet`.
  - `FILE`, `fdopen`, `fread`, `fgetc`, `fgets`, `fwrite`, `fflush` and other
    stdio calls, and `writeStream(`, must not appear:
    the Socket and Server natives use raw descriptors.
  - `raiseFromNative(` outside `raiseNet`, and any other `native...Error(`
    helper, are not in the table, so they fail.
  - A fatal exit fails: `abort(`, `runtimeError(`, `exit(` and its kin, a C++
    `throw`, and `std::terminate`. A Socket or Server fault is a catchable one
    from the list above or an argument check by `nativeRuntimeError(`, which
    the table counts. No other halting form has a row, so a halt in a socket
    function that is not an argument check cannot pass.
  - A fault kind assigned in two lines (`out.faultKind` then `= "X"`) is
    still `faultKind =` outside `setFault`, so it fails.
  - Comments are blanked before anything is counted, so a site inside a
    comment does not count.

JVM (runtime/jvm/src/lox/LoxSocket.java and LoxServer.java):
  - `new LoxError(` is a halting fault. Only argument checks use it, and
    `readBytesFrom`, which Process shares.
  - `makeError("KIND"` is a catchable fault of the given kind.
  - Any other `throw` fails, so a fault cannot use another exception.
  - `LoxRuntime.fatal`, `System.exit` and `Runtime.getRuntime().halt` fail.
    No table row lists them.

`--self-test` edits the real sources in memory, one defect at a time, and
fails unless each defect changes the result. It proves the rules above can fail.

Usage:
    tools/check_net_faults.py [--native PATH] [--socket PATH] [--server PATH]
    tools/check_net_faults.py --self-test
"""

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
NATIVE = REPO_ROOT / "src" / "stdlib" / "net_api.cpp"
SOCKET = REPO_ROOT / "runtime" / "jvm" / "src" / "lox" / "LoxSocket.java"
SERVER = REPO_ROOT / "runtime" / "jvm" / "src" / "lox" / "LoxServer.java"

NETWORK = "NetworkError"
CLOSED = "SocketClosedError"
HALT = "halt"

# (function, kind or HALT) -> number of sites.
NATIVE_SITES = {
    ("asStringArg", HALT): 1,
    ("asPort", HALT): 2,
    ("asByteCount", HALT): 2,
    ("socketWriteCall", HALT): 1,
    ("connectNative", NETWORK): 1,
    ("stepConnect", NETWORK): 1,
    ("listenNative", NETWORK): 2,
    ("fillInput", NETWORK): 1,
    ("stepReadBytes", NETWORK): 1,
    ("stepAccept", NETWORK): 1,
    ("step", CLOSED): 2,
    ("runIo", NETWORK): 1,
    ("closeDescriptor", CLOSED): 1,
    ("checkSocketRead", CLOSED): 1,
    ("checkSocketWrite", CLOSED): 1,
    ("stepWrite", NETWORK): 1,
    ("runWrite", NETWORK): 1,
    ("socketCloseWriteNative", CLOSED): 1,
    ("serverAcceptNative", CLOSED): 1,
}

SOCKET_SITES = {
    ("checkStringArg", HALT): 1,
    ("checkByteCountArg", HALT): 2,
    ("readBytesFrom", HALT): 1,
    ("checkOpen", CLOSED): 1,
    ("checkWritable", CLOSED): 1,
    ("fill", NETWORK): 1,
    ("writeBytes", NETWORK): 1,
    ("connect", NETWORK): 1,
}

SERVER_SITES = {
    ("listen", NETWORK): 1,
    ("accept", CLOSED): 1,
    ("accept", NETWORK): 1,
}

NATIVE_FUNCTION = re.compile(r"^(?:static |inline )?[\w:<>*& ]+?[ *&](\w+)\([^;]*$")
JAVA_METHOD = re.compile(r"^    (?:public |private )?(?:static )?(?:final )?[\w<>\[\]]+ (\w+)\(.*\{\s*$")


# A fatal exit that the table has no row for. Every one fails, with no
# allowed site: a Socket or Server fault is either catchable (raiseNet,
# setFault, failPark, makeError) or an argument check (nativeRuntimeError,
# new LoxError), and those two are counted by the table.
FATAL_NATIVE = re.compile(
    r"(?<![\w.])(abort|runtimeError|exit|_exit|_Exit|quick_exit|terminate|throw)\b"
)
FATAL_JAVA = re.compile(r"\b(LoxRuntime\.fatal|System\.exit|Runtime\.getRuntime\(\)\.halt)\b")


def strip_comments(text: str) -> str:
    """Blanks `//` and `/* */` comments, keeping string and char literals.

    Each newline survives, so a line number in the result is a line number in
    the source. A comment is not code: a fault site inside one must not count.
    """
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        pair = text[i : i + 2]
        if pair == "//":
            while i < n and text[i] != "\n":
                i += 1
        elif pair == "/*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif c == '"' or (c == "'" and not (i and text[i - 1].isalnum())):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i : j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def native_sites(text: str) -> tuple[Counter, list[str]]:
    sites: Counter = Counter()
    problems: list[str] = []
    function = None
    function_at: dict[int, str | None] = {}
    lines = strip_comments(text).splitlines()
    for number, line in enumerate(lines, 1):
        if line and not line[0].isspace() and not line.startswith(("}", "#")):
            m = NATIVE_FUNCTION.match(line)
            if m:
                function = m.group(1)
        function_at[number] = function
        fatal = FATAL_NATIVE.search(line)
        if fatal:
            problems.append(f"line {number}: {fatal.group(1)} is a fatal exit outside the table")
        if "writeStreamOrError(" in line:
            problems.append(f"line {number}: writeStreamOrError is a halting write")
        stdio = re.search(r"\b(FILE|fdopen|fread|fgetc|fgets|fscanf|getline|getchar|fwrite|fputs|fputc|fflush|fclose|writeStream)\b", line)
        if stdio:
            problems.append(f"line {number}: {stdio.group(1)} is stdio, the natives use raw descriptors")
        if function != "raiseNet" and re.search(r"\braiseFromNative\(", line):
            problems.append(f"line {number}: raiseFromNative outside raiseNet")
        for helper in re.findall(r"\b(native\w*Error)\(", line):
            if helper != "nativeRuntimeError":
                problems.append(f"line {number}: {helper} is not a known fault helper")
        if re.search(r"\bnativeRuntimeError\(", line) and function != "raiseNet":
            sites[(function, HALT)] += 1
        if function not in ("raiseNet", "setFault"):
            # A formatter can split a call after its open parenthesis or after
            # the first argument, so the kind is read from this line and the
            # next one.
            window = line
            if line.rstrip().endswith(("(", ",")) and number < len(lines):
                window += " " + lines[number].strip()
            for head in re.finditer(r"\b(raiseNet|setFault|failPark)\(", line):
                pattern = {
                    "raiseNet": r"raiseNet\(\s*([\w.]+)",
                    "setFault": r"setFault\(\s*out,\s*([\w.]+)",
                    "failPark": r"failPark\(\s*task,\s*([\w.]+)",
                }[head.group(1)]
                arg = re.match(pattern, window[head.start():])
                if arg is None:
                    problems.append(f"line {number}: cannot read the kind of {head.group(1)}")
                elif arg.group(1) == "out.faultKind":
                    continue
                elif not re.fullmatch(r"k\w+", arg.group(1)):
                    problems.append(f"line {number}: a fault kind that is not a constant: {arg.group(1)}")
                else:
                    kind = {"kNetworkError": NETWORK, "kSocketClosedError": CLOSED}.get(arg.group(1))
                    if kind is None:
                        problems.append(f"line {number}: a fault names an unknown kind {arg.group(1)}")
                    else:
                        sites[(function, kind)] += 1
    # A formatter can put `=` on the line after `faultKind`, so the text is
    # scanned whole.
    code = "\n".join(lines)
    for m in re.finditer(r"\bfaultKind\s*=(?!=)", code):
        number = code.count("\n", 0, m.start()) + 1
        if function_at[number] != "setFault":
            problems.append(f"line {number}: faultKind is set outside setFault")
    return sites, problems


def java_sites(text: str) -> tuple[Counter, list[str]]:
    sites: Counter = Counter()
    problems: list[str] = []
    method = None
    lines = strip_comments(text).splitlines()
    for number, line in enumerate(lines, 1):
        m = JAVA_METHOD.match(line)
        if m:
            method = m.group(1)
        # A statement can wrap, so read the line and the next one.
        window = line + (lines[number] if number < len(lines) else "")
        fatal = FATAL_JAVA.search(line)
        if fatal:
            problems.append(f"line {number}: {fatal.group(1)} is a fatal exit outside the table")
        if re.search(r"\bthrow\b", line) and not re.search(
            r"\bthrow (new LoxError\(|LoxOps\.makeError\()", line
        ):
            problems.append(f"line {number}: a throw other than LoxError or makeError")
        if "new LoxError(" in line:
            sites[(method, HALT)] += 1
        m = re.search(r'makeError\("(\w+)"', window)
        if m and "LoxOps.makeError" in line:
            if m.group(1) not in (NETWORK, CLOSED):
                problems.append(f"line {number}: makeError names an unknown kind {m.group(1)}")
            else:
                sites[(method, m.group(1))] += 1
    return sites, problems


def compare(label: str, found: Counter, expected: dict) -> list[str]:
    problems = []
    for key in sorted(set(found) | set(expected), key=str):
        if found.get(key, 0) != expected.get(key, 0):
            problems.append(
                f"{label}: {key[0]} has {found.get(key, 0)} {key[1]} site(s), the table says "
                f"{expected.get(key, 0)}"
            )
    return problems


def self_test(native: str, socket: str) -> int:
    """Each defect below must change the result of the parser it targets."""
    def inject(text: str, anchor: str, extra: str, before: bool = False) -> str:
        assert text.count(anchor) >= 1, anchor
        return text.replace(anchor, extra + anchor if before else anchor + extra, 1)

    def sites_and_table(parse, text: str, table: dict) -> list[str]:
        found, extra = parse(text)
        return extra + compare("t", found, table)

    read_fn = "static Value socketReadlineNative(int /*argc*/, Value* args) {\n"
    java_fn = "    private void checkOpen("
    cases = [
        ("native: a fault site in a // comment is not counted", native_sites, NATIVE_SITES, inject(native, read_fn, '    // setFault(out, kNetworkError, "x");\n    /* raiseNet(kNetworkError, "y"); */\n'),
         False),
        ("native: a // inside a string is not a comment", native_sites, NATIVE_SITES, inject(native, read_fn, '    const char* u = "http://x"; raiseNet(kNetworkError, u);\n'), True),
        ("native: a commented-out live site is a missing site", native_sites, NATIVE_SITES, native.replace("raiseNet(kNetworkError", "// raiseNet(kNetworkError", 1), True),
        ("native: faultKind assigned over two lines", native_sites, NATIVE_SITES, inject(native, read_fn, '    out.faultKind\n\n        = "BogusKind";\n'), True),
        ("native: abort()", native_sites, NATIVE_SITES, inject(native, read_fn, "    abort();\n"), True),
        ("native: runtimeError(", native_sites, NATIVE_SITES, inject(native, read_fn, '    runtimeError("x");\n'), True),
        ("native: throw", native_sites, NATIVE_SITES, inject(native, read_fn, "    throw 1;\n"), True),
        ("native: exit()", native_sites, NATIVE_SITES, inject(native, read_fn, "    exit(1);\n"), True),
        ("java: LoxRuntime.fatal", java_sites, SOCKET_SITES, inject(socket, java_fn, '    static void f() { LoxRuntime.fatal("x"); }\n', True), True),
        ("java: System.exit", java_sites, SOCKET_SITES, inject(socket, java_fn, "    static void f() { System.exit(1); }\n", True), True),
        ("java: a makeError in a comment is not counted", java_sites, SOCKET_SITES, inject(socket, java_fn, '    // throw LoxOps.makeError("NetworkError", "x");\n', True), False),
    ]
    failed = False
    for name, parse, table, mutated, must_fail in cases:
        after = sites_and_table(parse, mutated, table)
        ok = bool(after) if must_fail else not after
        print(f"{'ok  ' if ok else 'FAIL'} {name}")
        failed |= not ok
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--native", type=Path, default=NATIVE)
    parser.add_argument("--socket", type=Path, default=SOCKET)
    parser.add_argument("--server", type=Path, default=SERVER)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test(NATIVE.read_text(encoding="utf-8"), SOCKET.read_text(encoding="utf-8"))

    problems: list[str] = []
    for label, path, parse, expected in (
        ("native", args.native, native_sites, NATIVE_SITES),
        ("LoxSocket", args.socket, java_sites, SOCKET_SITES),
        ("LoxServer", args.server, java_sites, SERVER_SITES),
    ):
        found, extra = parse(path.read_text(encoding="utf-8"))
        problems += [f"{label}: {p}" for p in extra]
        problems += compare(label, found, expected)

    if problems:
        print("check_net_faults.py: the fault sites differ from the table", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1
    total = sum(NATIVE_SITES.values()) + sum(SOCKET_SITES.values()) + sum(SERVER_SITES.values())
    print(f"check_net_faults.py: {total} fault sites match the table")
    return 0


if __name__ == "__main__":
    sys.exit(main())
