; Generic identifier fallback. Listed first so the specific rules below
; override it — this highlighter uses last-match-wins.
(identifier) @variable

; Comments
(comment) @comment

; Literals
(int) @number
(number) @number
(bad_int) @error
(string) @string
(escape_sequence) @string.escape
[
  (true)
  (false)
] @boolean
(nil) @constant.builtin

; Keywords
[
  "var"
  "fun"
  "class"
  "enum"
  "return"
  "print"
] @keyword

[
  "if"
  "else"
  "match"
  "case"
] @keyword.conditional

[
  "for"
  "while"
  "break"
  "continue"
] @keyword.repeat

"yield" @keyword.coroutine

[
  "and"
  "or"
  "in"
] @keyword.operator

; Operators
[
  "="
  "=="
  "!="
  "<"
  "<="
  ">"
  ">="
  "+"
  "-"
  "*"
  "/"
  "%"
  "!"
  "&"
  "|"
  "^"
  "~"
  "<<"
  ">>"
  ">>>"
  "=>"
  "@"
  "..."
] @operator

; Punctuation
[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

[
  ","
  ";"
  ":"
  "."
] @punctuation.delimiter

; Property access (any `.name`)
(field_expression
  property: (identifier) @variable.member)
(super_expression
  property: (identifier) @variable.member)

; Declarations
(parameters (identifier) @variable.parameter)
(function_declaration name: (identifier) @function)
(method_definition name: (identifier) @function.method)

(class_declaration name: (identifier) @type)
(class_declaration superclass: (identifier) @type)
(enum_declaration name: (identifier) @type)
(enum_variant name: (identifier) @constructor)
(enum_variant field: (identifier) @variable.member)

; Calls
(call_expression
  function: (identifier) @function.call)
(call_expression
  function: (field_expression
    property: (identifier) @function.method.call))

; Match patterns. `E.A`, `E.A(...)` and `E.A{...}` name the enum E and its
; constructor A. A record pattern with no enum is a class pattern.
(constructor_pattern enum: (identifier) @type)
(constructor_pattern name: (identifier) @constructor)
(constructor_tag_pattern enum: (identifier) @type)
(constructor_tag_pattern name: (identifier) @constructor)
(record_pattern name: (identifier) @type)
(record_pattern enum: (identifier) @type)
(record_pattern enum: (identifier) name: (identifier) @constructor)
(record_pattern binding: (identifier) @variable.member)
(binding_pattern name: (identifier) @variable)

; this / super
(this_expression) @variable.builtin
(super_expression "super" @variable.builtin)

; Bare identifier arm pattern: a value binding, or a class test (`case Point
; =>`). It is never a constructor test. Syntax alone cannot tell a binding
; from a class test, so fall back on the naming convention: a leading capital
; reads as a class.
((identifier_pattern) @variable)
((identifier_pattern) @type
 (#match? @type "^[A-Z]"))

; `E.A` in an expression: the constructor A of the enum E. Syntax alone
; cannot tell it from a property read, so fall back on the naming
; convention: a capital on both names reads as an enum and a constructor.
((field_expression
  object: (identifier) @type
  property: (identifier) @constructor)
 (#match? @type "^[A-Z]")
 (#match? @constructor "^[A-Z]"))

; Wildcard pattern (after everything else so it wins for `_`)
((identifier_pattern) @variable.builtin
 (#eq? @variable.builtin "_"))
