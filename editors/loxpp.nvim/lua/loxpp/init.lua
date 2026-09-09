-- loxpp.nvim: wires the three editor layers for Lox++.
--
--   1. tree-sitter grammar  (editors/tree-sitter-loxpp)
--   2. the loxpp-lsp language server
--   3. `loxpp --check`        diagnostics fallback, used only when
--      the LSP is not running
--
-- The Vim regex syntax, ftdetect, and ftplugin files sit beside
-- this tree and load on their own from 'runtimepath'; they need no setup.

local M = {}

M.did_setup = false

-- The tree-sitter parser is named after the grammar (`loxpp`), not after the
-- filetype (`lox`). The generated parser exports `tree_sitter_loxpp`, so the
-- nvim-treesitter parser name and the Neovim language name must both be
-- `loxpp` for the symbol lookup to match. The filetype stays `lox` and is
-- mapped to the `loxpp` language. Queries therefore live in `queries/loxpp/`.
local LANG = "loxpp"
local FILETYPE = "lox"

local defaults = {
  -- tree-sitter registration with nvim-treesitter. `parser_dir` points at the
  -- grammar source directory; when nil the sibling `editors/tree-sitter-loxpp`
  -- is used if present, otherwise the grammar is fetched from the loxpp repo.
  treesitter = {
    enable = true,
    parser_dir = nil,
    highlight = true, -- run `vim.treesitter.start` on a `lox` buffer
    fold = true, -- set a tree-sitter 'foldexpr' on a `lox` buffer
  },
  -- loxpp-lsp language server.
  lsp = {
    enable = true,
    cmd = { "loxpp-lsp" },
    root_markers = { "spec", ".git" },
  },
  -- `loxpp --check` diagnostics through nvim-lint. Runs on save and on read,
  -- but only for a buffer with no loxpp-lsp client attached.
  lint = {
    enable = true,
    cmd = "loxpp",
  },
}

M.config = vim.deepcopy(defaults)

-- Directory of this plugin (…/editors/loxpp.nvim), from this file's path
-- (…/lua/loxpp/init.lua).
local function plugin_root()
  local this = debug.getinfo(1, "S").source:sub(2)
  return vim.fn.fnamemodify(this, ":h:h:h")
end

-- Where the grammar source lives, or nil to fetch it from the repo.
local function grammar_dir(cfg)
  if cfg.parser_dir and cfg.parser_dir ~= "" then
    return vim.fn.expand(cfg.parser_dir)
  end
  local sibling = plugin_root() .. "/../tree-sitter-loxpp"
  if vim.fn.isdirectory(sibling) == 1 then
    return vim.fn.fnamemodify(sibling, ":p")
  end
  return nil
end

-- Register the `loxpp` language so a `lox` buffer can use it. This is all the
-- built-in Neovim tree-sitter runtime needs: the queries ship in this
-- plugin's `queries/loxpp/`, and highlighting works as soon as a compiled
-- `parser/loxpp.so` is anywhere on 'runtimepath'. nvim-treesitter is used
-- only, and only if present, to *install* that parser with `:TSInstall`.
local function setup_treesitter(cfg)
  pcall(vim.treesitter.language.register, LANG, FILETYPE)

  -- nvim-treesitter `master` API: register an install target for
  -- `:TSInstall loxpp`. The `main` rewrite has no get_parser_configs and
  -- needs no registration, so a failure here is not an error.
  local ok, ts_parsers = pcall(require, "nvim-treesitter.parsers")
  if ok and type(ts_parsers.get_parser_configs) == "function" then
    local dir = grammar_dir(cfg)
    local install_info = {
      files = { "src/parser.c" },
      generate_requires_npm = false,
      requires_generate_from_grammar = false,
    }
    if dir then
      install_info.url = dir
    else
      install_info.url = "https://github.com/txloc1909/loxpp"
      install_info.location = "editors/tree-sitter-loxpp"
      install_info.branch = "main"
    end
    local configs = ts_parsers.get_parser_configs()
    configs[LANG] = { install_info = install_info, filetype = FILETYPE }
  end
end

-- True when a compiled parser for `loxpp` is reachable on 'runtimepath'.
-- On Neovim >= 0.11 `vim.treesitter.language.add` returns `true` on success
-- and `nil, <message>` for a missing parser (no raise), so pcall alone is not
-- enough. On 0.10 it raises on failure and returns nil on success, which the
-- `added ~= nil or ok` shape below also covers.
local function parser_available()
  local ok, added = pcall(vim.treesitter.language.add, LANG)
  if not ok then
    return false
  end
  if vim.fn.has("nvim-0.11") == 1 then
    return added == true
  end
  return true -- 0.10: pcall succeeded, so the parser loaded
end

local warned_no_parser = false

local function start_treesitter_buffer(cfg)
  if not parser_available() then
    if not warned_no_parser then
      warned_no_parser = true
      vim.notify(
        "loxpp.nvim: no compiled tree-sitter parser for `loxpp`. "
          .. "Run `:TSInstall loxpp` (nvim-treesitter), or build "
          .. "`parser/loxpp.so` from editors/tree-sitter-loxpp. "
          .. "The Vim regex syntax stays active until then.",
        vim.log.levels.WARN
      )
    end
    return
  end
  if cfg.highlight then
    pcall(vim.treesitter.start, 0, LANG)
  end
  if cfg.fold then
    vim.wo[0][0].foldmethod = "expr"
    vim.wo[0][0].foldexpr = "v:lua.vim.treesitter.foldexpr()"
  end
end

local function setup_lsp(cfg)
  if vim.fn.has("nvim-0.11") == 1 then
    vim.lsp.config("loxpp_lsp", {
      cmd = cfg.cmd,
      filetypes = { FILETYPE },
      root_markers = cfg.root_markers,
    })
    vim.lsp.enable("loxpp_lsp")
    return
  end

  -- Neovim < 0.11 has no vim.lsp.config / vim.lsp.enable.
  local ok, lspconfig = pcall(require, "lspconfig")
  if not ok then
    vim.notify(
      "loxpp.nvim: Neovim < 0.11 needs nvim-lspconfig for the LSP layer",
      vim.log.levels.WARN
    )
    return
  end
  local lsp_configs = require("lspconfig.configs")
  if not lsp_configs.loxpp_lsp then
    lsp_configs.loxpp_lsp = {
      default_config = {
        cmd = cfg.cmd,
        filetypes = { FILETYPE },
        root_dir = lspconfig.util.root_pattern(unpack(cfg.root_markers)),
        single_file_support = true,
      },
    }
  end
  lspconfig.loxpp_lsp.setup({})
end

local function lox_has_lsp()
  return #vim.lsp.get_clients({ bufnr = 0, name = "loxpp_lsp" }) > 0
end

local function setup_lint(cfg)
  local ok, lint = pcall(require, "lint")
  if not ok then
    vim.notify(
      "loxpp.nvim: nvim-lint not found; the --check fallback is off",
      vim.log.levels.WARN
    )
    return
  end
  local spec = require("loxpp.lint")
  spec.cmd = cfg.cmd
  lint.linters.loxpp_check = spec
  lint.linters_by_ft[FILETYPE] = { "loxpp_check" }

  local group = vim.api.nvim_create_augroup("loxpp_lint", { clear = true })
  vim.api.nvim_create_autocmd({ "BufWritePost", "BufReadPost" }, {
    group = group,
    pattern = "*.lox",
    callback = function()
      -- The LSP owns diagnostics when it is attached.
      if lox_has_lsp() then
        return
      end
      require("lint").try_lint("loxpp_check")
    end,
  })
end

function M.setup(opts)
  M.config = vim.tbl_deep_extend("force", vim.deepcopy(defaults), opts or {})
  local cfg = M.config

  if cfg.treesitter.enable then
    setup_treesitter(cfg.treesitter)
  end
  if cfg.lsp.enable then
    setup_lsp(cfg.lsp)
  end
  if cfg.lint.enable then
    setup_lint(cfg.lint)
  end

  local group = vim.api.nvim_create_augroup("loxpp_ft", { clear = true })
  vim.api.nvim_create_autocmd("FileType", {
    group = group,
    pattern = FILETYPE,
    callback = function()
      if cfg.treesitter.enable then
        start_treesitter_buffer(cfg.treesitter)
      end
    end,
  })
  -- Apply to a `lox` buffer that is already open at setup time.
  if vim.bo.filetype == FILETYPE and cfg.treesitter.enable then
    start_treesitter_buffer(cfg.treesitter)
  end

  M.did_setup = true
end

return M
