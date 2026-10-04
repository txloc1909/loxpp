#!/usr/bin/env bash
#
# test/translation-probes/README.md is the index of the probe directory. Its
# table is hand-kept, so a probe can land with no row and nothing reports it.
# This check closes that gap: every *.lox file under the directory, at any
# depth (jvm-only/, qbe-only/, jvm-only/known-divergence/), must have a
# backticked row reference in the README. A file with no row fails the check.
#
# It checks only that direction. A README reference with no file behind it is
# not reported, because the README also holds backticked names that are not
# probes (opcodes, helper functions, relative note paths), and those are not
# separable from a stale probe name without a fragile allowlist.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dir="$root/test/translation-probes"
readme="$dir/README.md"

if [ ! -f "$readme" ]; then
    echo "check_probe_index.sh: no README at $readme" >&2
    exit 1
fi

if [ ! -d "$dir" ]; then
    echo "check_probe_index.sh: no probe directory at $dir" >&2
    exit 1
fi

missing=()
while IFS= read -r file; do
    rel="${file#"$dir"/}"
    name="${rel%.lox}"
    if ! grep -Fq "\`$name\`" "$readme"; then
        missing+=("$rel")
    fi
done < <(find "$dir" -name '*.lox' | sort)

if [ "${#missing[@]}" -ne 0 ]; then
    echo "check_probe_index.sh: ${#missing[@]} probe(s) have no README row:" >&2
    for probe in "${missing[@]}"; do
        echo "  $probe" >&2
    done
    exit 1
fi

echo "check_probe_index.sh: every probe has a README row"
