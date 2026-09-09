#!/usr/bin/env bash
# Compile the Lox++ tree-sitter parser into editors/loxpp.nvim/parser/loxpp.so
# without nvim-treesitter. Neovim's built-in tree-sitter runtime then finds it
# on 'runtimepath' (this plugin's directory) and highlighting works.
#
# Needs a C compiler (cc/clang/gcc). The generated parser source lives in the
# sibling grammar checkout; run `tree-sitter generate` there first if it is
# missing (the committed tree already has it).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"          # editors/loxpp.nvim
src="$here/../tree-sitter-loxpp/src"
out="$here/parser/loxpp.so"

[ -f "$src/parser.c" ] || {
  echo "build-parser: $src/parser.c not found." >&2
  echo "  cd editors/tree-sitter-loxpp && tree-sitter generate" >&2
  exit 1
}

cc_bin="${CC:-}"
for c in "$cc_bin" cc clang gcc; do
  [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && { cc_bin="$c"; break; }
done
[ -n "$cc_bin" ] || { echo "build-parser: no C compiler on PATH" >&2; exit 1; }

scanner=""
[ -f "$src/scanner.c" ] && scanner="$src/scanner.c"

mkdir -p "$here/parser"
# shellcheck disable=SC2086
"$cc_bin" -O2 -fPIC -shared -std=c11 -I"$src" "$src/parser.c" $scanner -o "$out"
echo "build-parser: wrote $out"
