-- `:checkhealth loxpp`

local M = {}

local health = vim.health
local start = health.start or health.report_start
local ok = health.ok or health.report_ok
local warn = health.warn or health.report_warn
local error_ = health.error or health.report_error

local function first_line(cmd)
  local out = vim.fn.system(cmd)
  if vim.v.shell_error ~= 0 then
    return nil
  end
  return vim.split(out, "\n", { plain = true })[1]
end

function M.check()
  start("loxpp.nvim")

  -- Neovim version
  if vim.fn.has("nvim-0.10") == 1 then
    ok("Neovim " .. tostring(vim.version()))
  else
    error_("Neovim 0.10 or later is required")
  end
  if vim.fn.has("nvim-0.11") == 1 then
    ok("LSP client API: vim.lsp.config / vim.lsp.enable (Neovim >= 0.11)")
  else
    warn("Neovim < 0.11: the LSP layer needs nvim-lspconfig")
  end

  -- Binaries
  -- `loxpp` has no --version flag; `loxpp-lsp` does.
  if vim.fn.executable("loxpp") == 1 then
    ok("loxpp on PATH")
  else
    warn("loxpp not on PATH: the `--check` diagnostics fallback is off")
  end
  if vim.fn.executable("loxpp-lsp") == 1 then
    local v = first_line({ "loxpp-lsp", "--version" }) or "loxpp-lsp"
    ok(v .. " on PATH")
  else
    warn("loxpp-lsp not on PATH: the language server layer is off")
  end

  -- nvim-treesitter
  if pcall(require, "nvim-treesitter") then
    ok("nvim-treesitter installed")
  else
    warn("nvim-treesitter not found: the tree-sitter layer is off")
  end

  -- The compiled parser. `vim.treesitter.language.add` returns `true` on
  -- success and `nil, <message>` on a missing parser (it does not raise), so
  -- pcall alone always looks like success.
  local add_ok, added = pcall(vim.treesitter.language.add, "loxpp")
  if add_ok and added == true then
    ok("tree-sitter parser `loxpp` is compiled and on runtimepath")
  else
    warn(
      "tree-sitter parser `loxpp` is not built: run "
        .. "`editors/loxpp.nvim/scripts/build-parser.sh` or `:TSInstall loxpp`. "
        .. "The regex syntax stays active until then."
    )
  end

  -- nvim-lint
  if pcall(require, "lint") then
    ok("nvim-lint installed (backs the `--check` fallback)")
  else
    warn("nvim-lint not found: the `--check` fallback runs through `:compiler lox` only")
  end
end

return M
