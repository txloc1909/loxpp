-- Lox++ filetype plugin (Neovim path).
-- ftplugin/lox.vim runs first and sets b:did_ftplugin. This file then returns
-- early, so buffer options are applied once. The block below is the Lua
-- equivalent of that file for a config that loads only the Lua tree.

if vim.b.did_ftplugin then
  return
end
vim.b.did_ftplugin = 1

vim.bo.commentstring = "// %s"
vim.bo.comments = "://"
vim.bo.suffixesadd = ".lox"

vim.opt_local.iskeyword:append("_")
vim.opt_local.formatoptions:remove("t")
vim.opt_local.formatoptions:append("croql")

vim.b.undo_ftplugin = "setlocal commentstring< comments< iskeyword<"
  .. " suffixesadd< formatoptions<"
