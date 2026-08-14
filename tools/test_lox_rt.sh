#!/usr/bin/env bash
#
# Builds the runtime library, compiles the plain (no-JUnit — the image has
# none) test classes against it, then runs every suite's main(). Each main()
# exits non-zero on failure; this script aggregates that into one exit code.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="$root/runtime/jvm"
classes_dir="$runtime_dir/classes"
test_classes_dir="$runtime_dir/test-classes"

"$root/tools/build_lox_rt.sh"

rm -rf "$test_classes_dir"
mkdir -p "$test_classes_dir"
javac -cp "$classes_dir" -d "$test_classes_dir" "$runtime_dir"/test/lox/*.java

# Discovered, not hardcoded, so a new *Test.java file is picked up on its own.
# TestSupport.java does not match the *Test.java suffix, so it is not a suite.
mapfile -t suites < <(cd "$runtime_dir/test/lox" && for f in *Test.java; do basename "$f" .java; done)

failures=0
for suite in "${suites[@]}"; do
    echo "== lox.$suite =="
    if ! java -cp "$classes_dir:$test_classes_dir" "lox.$suite"; then
        failures=$((failures + 1))
    fi
done

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures lox-rt test suite(s) failed."
    exit 1
fi
echo "All lox-rt test suites passed."
