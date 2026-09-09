# loxpp.nvim

Editor support for [Lox++](../../README.md) in Vim and Neovim.

Three layers, each optional:

| Layer | Gives you | Needs |
|---|---|---|
| tree-sitter grammar | scope-aware highlighting, folds, indents | a compiled `parser/loxpp.so` + a C compiler |
| `loxpp-lsp` | diagnostics, hover, completion, go-to-definition, references, document symbols | `loxpp-lsp` on `$PATH`, Neovim ≥ 0.11 (or 0.10 + `nvim-lspconfig`) |
| `loxpp --check` fallback | compiler static errors on save | `loxpp` on `$PATH`, [`nvim-lint`](https://github.com/mfussenegger/nvim-lint) |

Filetype detection, buffer options, a regex syntax fallback, and a
`:compiler lox` quickfix workflow (node N6) load on their own — no setup, and
they work in plain Vim too.

## Build the pieces

From the Lox++ checkout root:

```sh
# the interpreter and the language server
cmake --preset debug -DLOXPP_LSP=ON
cmake --build build --target loxpp loxpp-lsp
# put build/loxpp and build/loxpp-lsp on your PATH

# the tree-sitter parser -> editors/loxpp.nvim/parser/loxpp.so
cd editors/tree-sitter-loxpp && tree-sitter generate && cd -
editors/loxpp.nvim/scripts/build-parser.sh
```

`scripts/build-parser.sh` needs only a C compiler. With `nvim-treesitter`
(`master` branch) installed you can instead run `:TSInstall loxpp` after
`setup()`.

## Install

**lazy.nvim**

```lua
{
  dir = vim.fn.expand("~/src/loxpp/editors/loxpp.nvim"),  -- adjust
  ft = "lox",
  build = "./scripts/build-parser.sh",
  opts = {},
}
```

**packer.nvim**

```lua
use {
  "~/src/loxpp/editors/loxpp.nvim",
  run = "./scripts/build-parser.sh",
  config = function() require("loxpp").setup() end,
}
```

**Manual** — put this directory on `runtimepath`, run
`scripts/build-parser.sh`, then `:lua require("loxpp").setup()`.

## Configure

`setup()` takes per-layer options; see `:help loxpp-setup`. Every layer has an
`enable` flag. Defaults:

```lua
require("loxpp").setup({
  treesitter = { enable = true, parser_dir = nil, highlight = true, fold = true },
  lsp        = { enable = true, cmd = { "loxpp-lsp" }, root_markers = { "spec", ".git" } },
  lint       = { enable = true, cmd = "loxpp" },
})
```

## Check it

```
:checkhealth loxpp
```

reports the Neovim version, the two binaries, the compiled parser, and the
optional plugin dependencies.

## What you get, by what you have

| tree-sitter parser | `loxpp-lsp` | Result |
|---|---|---|
| ✅ | ✅ | full: tree-sitter highlighting + server diagnostics/hover/nav |
| ✅ | ❌ | tree-sitter highlighting + `loxpp --check` errors on save |
| ❌ | ✅ | regex highlighting + server diagnostics/hover/nav |
| ❌ | ❌ | regex highlighting + `:compiler lox` / `:make` quickfix |

## Files

```
ftdetect/    *.lox -> filetype lox            (N6)
ftplugin/    commentstring, comments, ...      (N6)
syntax/      regex highlighter, the fallback   (N6)
compiler/    :compiler lox -> loxpp --check    (N6)
queries/     tree-sitter highlights/locals/folds/indents  (from N4)
lua/loxpp/   init (setup), lint (nvim-lint spec), health
plugin/      load guard + lazy bootstrap
scripts/     build-parser.sh
doc/         :help loxpp
```
