# loxpp.nvim

Editor support for Lox++ in Vim and Neovim.

> Stub. Node N9 fills this out with install steps, the tree-sitter setup, and
> the LSP client configuration.

## What is here now (node N6)

The zero-dependency fallback path. It works in plain Vim and in Neovim
without any plugin manager.

| File | Job |
|---|---|
| `ftdetect/lox.vim`, `ftdetect/lox.lua` | Map `*.lox` to filetype `lox`. |
| `ftplugin/lox.vim`, `ftplugin/lox.lua` | Buffer options: `commentstring`, `comments`, `iskeyword`, `suffixesadd`, `formatoptions`. |
| `syntax/lox.vim` | Regex syntax highlighting. |

The `.vim` and `.lua` files are two paths to the same result. Classic Vim
reads only the `.vim` files. Neovim reads both: the `.lua` filetype mapping
wins first, and the `.vim` files then do nothing, so nothing is set twice.

## Highlighting: which path runs

`syntax/lox.vim` is the **fallback**. It is a shallow regex highlighter for
keywords, literals, strings, comments, and operators.

The **preferred** path is the tree-sitter grammar in
`editors/tree-sitter-loxpp/` (node N4). It gives scope-aware highlighting,
folds, and indents. Node N9 registers that grammar and installs its queries.
When tree-sitter is active for a buffer, it takes over and this file steps
back.

## Requirements

- Classic Vim: any recent version.
- Neovim: 0.10 or later for the built-in `gc` comment mapping to use the
  `commentstring` set here.
