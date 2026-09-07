" Vim syntax file for Lox++ (the zero-dependency fallback).
"
" This is a shallow regex highlighter. It runs in classic Vim and in Neovim
" without tree-sitter. The tree-sitter grammar in editors/tree-sitter-loxpp is
" the preferred path and gives deeper, scope-aware highlighting; keep this file
" simple on purpose.
"
" Token list follows src/token.h and spec/01-lexical.md.

if exists("b:current_syntax")
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

syntax case match

" --- Keywords (23, from src/token.h) --------------------------------------
" `true` / `false` / `nil` are constants, handled below. The other 20:
syntax keyword loxKeyword       and or in var
syntax keyword loxKeyword       this super
syntax keyword loxConditional   if else match case default
syntax keyword loxRepeat        for while
syntax keyword loxStatement     return break continue
" `print` is a statement keyword, not a builtin function.
syntax keyword loxStatement     print

syntax keyword loxStructure     class enum skipwhite nextgroup=loxTypeName
syntax keyword loxKeyword       fun          skipwhite nextgroup=loxFunctionName

syntax match   loxFunctionName  /\<\h\w*/ contained
syntax match   loxTypeName      /\<\h\w*/ contained

" --- Constants ----------------------------------------------------------
syntax keyword loxBoolean       true false
syntax keyword loxConstant      nil

" --- Numbers ----------------------------------------------------------------
" DIGIT+ ( "." DIGIT+ )?  -- decimal only, no exponent (spec/01-lexical.md).
syntax match   loxNumber        /\<\d\+\%(\.\d\+\)\?\>/

" --- Operators ------------------------------------------------------------
syntax match   loxOperator      /[-+*/%!<>=]/
syntax match   loxOperator      /==\|!=\|>=\|<=\|=>/
syntax match   loxOperator      /\.\.\./
syntax match   loxOperator      /@/

" --- Strings --------------------------------------------------------------
" Six escapes are valid: \" \\ \n \t \r \0 . Any other \<char> is an error.
" loxStringEscape is defined after loxStringEscapeError so the valid escape
" wins when both match at the same position (last match wins on equal
" priority).
syntax match   loxStringEscapeError /\\./       contained
syntax match   loxStringEscape      /\\["\\ntr0]/ contained
syntax region  loxString        start=/"/ skip=/\\./ end=/"/
      \ contains=loxStringEscape,loxStringEscapeError

" --- Comments -----------------------------------------------------------
syntax keyword loxTodo          TODO FIXME contained
syntax match   loxComment       "//.*$" contains=loxTodo,@Spell

" Comments and strings must beat the `/` and `"` operator/text matches.
syntax sync minlines=10

" --- Highlight links ---------------------------------------------------
highlight default link loxKeyword          Keyword
highlight default link loxConditional      Conditional
highlight default link loxRepeat           Repeat
highlight default link loxStatement        Statement
highlight default link loxStructure        Structure
highlight default link loxFunctionName     Function
highlight default link loxTypeName         Type
highlight default link loxBoolean          Boolean
highlight default link loxConstant         Constant
highlight default link loxNumber           Number
highlight default link loxOperator         Operator
highlight default link loxString           String
highlight default link loxStringEscape     SpecialChar
highlight default link loxStringEscapeError Error
highlight default link loxComment          Comment
highlight default link loxTodo             Todo

let b:current_syntax = "lox"

let &cpo = s:cpo_save
unlet s:cpo_save
