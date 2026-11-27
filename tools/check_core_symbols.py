#!/usr/bin/env python3
"""Fail when a test executable defines a strong symbol that its core library also defines.

Each test executable links one shared core library (libloxpp_test_core.so or
libloxpp_test_core_profile.so). A symbol defined in both is not a link error:
the executable's copy replaces the library's copy at run time. The object
library that these libraries replaced rejected such a duplicate at link time,
so this check puts that guard back.

Usage: check_core_symbols.py <build-test-dir>
"""

import os
import pathlib
import subprocess
import sys

# nm type letters for a defined, global symbol that the linker must not merge:
# text, data, bss, read-only data, small data. Weak (W, V) symbols are excluded
# because the linker merges inline and template code by design.
STRONG = {"T", "D", "B", "R", "S"}

# ctest SKIP_RETURN_CODE for a build with nothing to compare.
SKIP = 125


def core_libraries(exe):
    out = subprocess.run(
        ["readelf", "-d", str(exe)], capture_output=True, text=True, check=True
    ).stdout
    libs = []
    for line in out.splitlines():
        if "(NEEDED)" in line and "libloxpp_test_core" in line:
            libs.append(line.split("[", 1)[1].split("]", 1)[0])
    return libs


def strong_defs(path, dynamic):
    cmd = ["nm", "--defined-only", "-g"]
    if dynamic:
        cmd.append("-D")
    out = subprocess.run(cmd + [str(path)], capture_output=True, text=True, check=True).stdout
    defs = set()
    for line in out.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] in STRONG:
            defs.add(fields[2])
    return defs


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    test_dir = pathlib.Path(argv[1])
    # A build that targets no core-linked test (for example test_scanner alone)
    # has no core library to compare with. ctest reports SKIP_RETURN_CODE as
    # Skipped; the run that builds the core libraries still fails on a duplicate.
    if not list(test_dir.glob("libloxpp_test_core*.so")):
        print(f"no core library under {test_dir}: nothing to compare", file=sys.stderr)
        return SKIP
    exes = sorted(
        p for p in test_dir.glob("test_*") if p.is_file() and os.access(p, os.X_OK)
    )
    lib_defs = {}
    checked = 0
    bad = []
    for exe in exes:
        libs = core_libraries(exe)
        if not libs:
            continue
        checked += 1
        exe_defs = strong_defs(exe, dynamic=False)
        for lib in libs:
            if lib not in lib_defs:
                lib_defs[lib] = strong_defs(test_dir / lib, dynamic=True)
            for sym in sorted(exe_defs & lib_defs[lib]):
                bad.append(f"{exe.name} {lib} {sym}")
    # Zero checked executables would pass silently, so the check must see some.
    if checked == 0:
        print(f"no test executable under {test_dir} links a core library", file=sys.stderr)
        return 1
    if bad:
        for line in bad:
            print(f"duplicate strong symbol: {line}", file=sys.stderr)
        return 1
    print(f"checked {checked} test executables against their core libraries: no duplicates")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
