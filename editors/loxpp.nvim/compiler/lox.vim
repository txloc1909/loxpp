" Vim compiler plugin for Lox++: `:compiler lox` then `:make` runs
" `loxpp --check` on the current file and loads the errors into the quickfix
" list. This is the zero-Lua fallback for users who build `loxpp` but not
" `loxpp-lsp` and do not use nvim-lint.

if exists("current_compiler")
  finish
endif
let current_compiler = "lox"

let s:cpo_save = &cpo
set cpo&vim

" `loxpp --check --format text %` prints one line per diagnostic:
"   path:line:col: severity: message
" The errorformat below reads that shape. `%t` takes the first letter of the
" severity word (e/w), `%*[^:]` eats the rest of the word up to the next `:`.
CompilerSet makeprg=loxpp\ --check\ --format\ text\ %
CompilerSet errorformat=%f:%l:%c:\ %t%*[^:]:\ %m

let &cpo = s:cpo_save
unlet s:cpo_save
