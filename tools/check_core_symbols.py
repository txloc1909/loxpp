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
    if not test_dir.is_dir():
        print(f"not a directory: {test_dir}", file=sys.stderr)
        return 1
    exes = sorted(
        p for p in test_dir.glob("test_*") if p.is_file() and os.access(p, os.X_OK)
    )
    linking = {exe: core_libraries(exe) for exe in exes}
    linking = {exe: libs for exe, libs in linking.items() if libs}
    # A build that has no core-linked test (for example test_scanner alone) has
    # nothing to compare. ctest reports SKIP as Skipped. A core library that some
    # executable needs, or that exists without one, is a failure, not a skip.
    if not linking and not list(test_dir.glob("libloxpp_test_core*.so")):
        print(f"no core-linked test under {test_dir}: nothing to compare", file=sys.stderr)
        return SKIP
    lib_defs = {}
    bad = []
    for exe, libs in linking.items():
        exe_defs = strong_defs(exe, dynamic=False)
        for lib in libs:
            if not (test_dir / lib).is_file():
                bad.append(f"{exe.name} needs {lib}, which is not under {test_dir}")
                continue
            if lib not in lib_defs:
                lib_defs[lib] = strong_defs(test_dir / lib, dynamic=True)
            for sym in sorted(exe_defs & lib_defs[lib]):
                bad.append(f"duplicate strong symbol: {exe.name} {lib} {sym}")
    # Zero linking executables with a core library present would pass silently.
    if not linking:
        print(f"core library under {test_dir} but no test executable links it", file=sys.stderr)
        return 1
    if bad:
        for line in bad:
            print(line, file=sys.stderr)
        return 1
    print(f"checked {len(linking)} test executables against their core libraries: no duplicates")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
