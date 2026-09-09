#!/usr/bin/env bash
# Headless smoke test for editors/loxpp.nvim.
#
# Proves the three layers wire up in a real Neovim:
#   1. filetype detection            -> &filetype == "lox"
#   2. tree-sitter grammar           -> vim.treesitter.get_parser(0, "loxpp") works
#   3. loxpp-lsp server              -> a client attaches to a .lox buffer
#   4. live diagnostics              -> editing in "1 +;" raises a vim.diagnostic
#
# Requirements on PATH: nvim (>= 0.11), loxpp, loxpp-lsp, and a C compiler
# (cc / clang / gcc) to build the parser. nvim-treesitter and nvim-lint are
# taken from $LOXPP_NVIM_PLUGINS (default /opt/nvim-plugins, set by the
# dev-editors image) or cloned into a temp dir.
#
# Exit 0 on success; non-zero with a diagnostic on the first failed check.
set -euo pipefail

# --fallback : do not require loxpp-lsp; instead assert the `loxpp --check`
#              path through nvim-lint raises a diagnostic. This is the layer
#              a user gets when they build `loxpp` but not `loxpp-lsp`.
MODE="lsp"
[ "${1:-}" = "--fallback" ] && MODE="fallback"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN_DIR="$REPO_ROOT/editors/loxpp.nvim"
GRAMMAR_DIR="$REPO_ROOT/editors/tree-sitter-loxpp"
PLUGINS_DIR="${LOXPP_NVIM_PLUGINS:-/opt/nvim-plugins}"

fail() { echo "check_nvim_plugin: FAIL: $*" >&2; exit 1; }

command -v nvim  >/dev/null || fail "nvim not on PATH"
command -v loxpp >/dev/null || fail "loxpp not on PATH"
if [ "$MODE" = "lsp" ]; then
  command -v loxpp-lsp >/dev/null || fail "loxpp-lsp not on PATH"
fi

CC_BIN="${CC:-}"
for c in "$CC_BIN" cc clang gcc; do
  [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && { CC_BIN="$c"; break; }
done
[ -n "$CC_BIN" ] || fail "no C compiler (cc/clang/gcc) on PATH to build the parser"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# --- runtime plugin dependencies ------------------------------------------
NVIM_TS="$PLUGINS_DIR/nvim-treesitter"
NVIM_LINT="$PLUGINS_DIR/nvim-lint"
if [ ! -d "$NVIM_TS" ]; then
  NVIM_TS="$WORK/nvim-treesitter"
  git clone --depth 1 https://github.com/nvim-treesitter/nvim-treesitter "$NVIM_TS" >/dev/null 2>&1 \
    || fail "cannot obtain nvim-treesitter (set LOXPP_NVIM_PLUGINS)"
fi
if [ ! -d "$NVIM_LINT" ]; then
  NVIM_LINT="$WORK/nvim-lint"
  git clone --depth 1 https://github.com/mfussenegger/nvim-lint "$NVIM_LINT" >/dev/null 2>&1 || true
fi

# --- build the tree-sitter parser into a runtimepath dir ------------------
# The plugin's setup() registers the grammar with nvim-treesitter, but
# :TSInstall needs network for its own runtime. Compile parser.c straight
# into <parserdir>/parser/loxpp.so, which Neovim finds on runtimepath.
PARSER_RT="$WORK/parser-rt"
mkdir -p "$PARSER_RT/parser" "$PARSER_RT/queries/loxpp"
SRC="$GRAMMAR_DIR/src"
[ -f "$SRC/parser.c" ] || fail "generated parser missing at $SRC/parser.c (run tree-sitter generate)"
SCANNER=""
[ -f "$SRC/scanner.c" ] && SCANNER="$SRC/scanner.c"
# shellcheck disable=SC2086
"$CC_BIN" -O2 -fPIC -shared -std=c11 -I"$SRC" \
  "$SRC/parser.c" $SCANNER -o "$PARSER_RT/parser/loxpp.so" \
  || fail "parser compile failed"
cp "$GRAMMAR_DIR"/queries/*.scm "$PARSER_RT/queries/loxpp/"

# --- the headless Lua driver ---------------------------------------------
cat > "$WORK/driver.lua" <<'LUA'
local function die(msg) io.stderr:write("check_nvim_plugin: FAIL: " .. msg .. "\n"); vim.cmd("cq") end
local function okline(msg) io.stdout:write("  ok  " .. msg .. "\n") end

local example = vim.env.LOXPP_EXAMPLE
local mode = vim.env.LOXPP_CHECK_MODE or "lsp"
require("loxpp").setup({ lsp = { enable = mode == "lsp" } })

vim.cmd.edit(example)
local buf = vim.api.nvim_get_current_buf()

-- 1. filetype
if vim.bo[buf].filetype ~= "lox" then die("filetype is '" .. vim.bo[buf].filetype .. "', want 'lox'") end
okline("filetype = lox")

-- 2. tree-sitter parser
local pok, parser = pcall(vim.treesitter.get_parser, buf, "loxpp")
if not pok or not parser then die("vim.treesitter.get_parser(buf, 'loxpp') failed: " .. tostring(parser)) end
local root = parser:parse()[1]:root()
if root:has_error() then die("tree-sitter parsed the example with an ERROR node") end
okline("tree-sitter parser 'loxpp' active, example parses clean")

-- 3. diagnostics source
if mode == "lsp" then
  local attached = vim.wait(15000, function()
    return #vim.lsp.get_clients({ bufnr = buf, name = "loxpp_lsp" }) > 0
  end, 100)
  if not attached then die("loxpp-lsp did not attach within 15s") end
  okline("loxpp-lsp client attached")

  -- live: an in-buffer edit that breaks the syntax raises a diagnostic
  vim.api.nvim_buf_set_lines(buf, 0, -1, false, { "fun broken() {", "  return 1 +;", "}" })
  local got = vim.wait(15000, function()
    return #vim.diagnostic.get(buf) > 0
  end, 100)
  if not got then die("no vim.diagnostic after an edit that breaks the syntax") end
  local d = vim.diagnostic.get(buf)[1]
  okline(("live diagnostic at line %d: %s"):format(d.lnum + 1, d.message))
else
  -- fallback: `loxpp --check` through nvim-lint, on save, no LSP
  if not pcall(require, "lint") then die("nvim-lint not on runtimepath for --fallback mode") end
  local scratch = vim.fn.tempname() .. ".lox"
  vim.fn.writefile({ "fun broken() {", "  return 1 +;", "}" }, scratch)
  vim.cmd.edit(scratch)
  buf = vim.api.nvim_get_current_buf()
  vim.cmd("doautocmd BufWritePost")
  local got = vim.wait(15000, function()
    return #vim.diagnostic.get(buf) > 0
  end, 100)
  if not got then die("no vim.diagnostic from the loxpp --check fallback") end
  local d = vim.diagnostic.get(buf)[1]
  okline(("fallback diagnostic at line %d: %s"):format(d.lnum + 1, d.message))
end

io.stdout:write("check_nvim_plugin: PASS\n")
vim.cmd("qa!")
LUA

run_nvim() {
  LOXPP_EXAMPLE="$1" LOXPP_CHECK_MODE="$MODE" nvim --headless --clean \
    --cmd "set rtp+=$PLUGIN_DIR" \
    --cmd "set rtp+=$NVIM_TS" \
    ${NVIM_LINT:+--cmd "set rtp+=$NVIM_LINT"} \
    --cmd "set rtp+=$PARSER_RT" \
    -l "$WORK/driver.lua"
}

echo "check_nvim_plugin: nvim $(nvim --version | head -1 | awk '{print $2}'), mode=$MODE"
run_nvim "$REPO_ROOT/examples/enum_result.lox"
