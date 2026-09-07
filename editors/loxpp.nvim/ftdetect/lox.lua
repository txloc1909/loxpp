-- Lox++ filetype detection (Neovim path).
-- Registers the extension with the built-in filetype engine. Classic Vim
-- ignores this file and uses ftdetect/lox.vim instead. When both run under
-- Neovim, this mapping wins first and the `setf` in the Vim file is a no-op,
-- so the filetype is never set twice.
vim.filetype.add({
  extension = {
    lox = "lox",
  },
})
