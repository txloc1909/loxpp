" Lox++ filetype detection (classic Vim path).
" Neovim also loads ftdetect/lox.lua; `setf` only sets the filetype when it is
" still unset, so the two paths never fight.
autocmd BufRead,BufNewFile *.lox setf lox
