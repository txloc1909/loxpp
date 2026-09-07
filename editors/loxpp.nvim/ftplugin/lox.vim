" Lox++ filetype plugin (classic Vim path).
" Neovim also loads ftplugin/lox.lua; it checks b:did_ftplugin and returns
" early when this file has already run, so the buffer options are set once.

if exists("b:did_ftplugin")
  finish
endif
let b:did_ftplugin = 1

let s:cpo_save = &cpo
set cpo&vim

" Line comments start with `//` (spec/01-lexical.md). There are no block
" comments, so `commentstring` and `comments` only describe the `//` form.
setlocal commentstring=//\ %s
setlocal comments=://

" `_` is part of an identifier (spec/01-lexical.md). Vim's default keeps it,
" but a user config may have removed it, so make the buffer self-sufficient.
setlocal iskeyword+=_

" `gf` and `:find` add the extension when it is missing.
setlocal suffixesadd=.lox

" Auto-wrap comments, keep the leader on `o`/`O` and `<CR>`, let `gq` format,
" but do not auto-wrap plain code.
setlocal formatoptions-=t
setlocal formatoptions+=croql

let b:undo_ftplugin = "setlocal commentstring< comments< iskeyword<"
      \ . " suffixesadd< formatoptions<"

let &cpo = s:cpo_save
unlet s:cpo_save
