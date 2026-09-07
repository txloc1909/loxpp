; Generic identifier fallback. Listed first so the specific rules below
; override it — this highlighter uses last-match-wins.
(identifier) @variable

; Comments
(comment) @comment

; Literals
(number) @number
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

; Match patterns
(constructor_pattern name: (identifier) @constructor)
(record_pattern name: (identifier) @constructor)
(record_pattern binding: (identifier) @variable.member)
(binding_pattern name: (identifier) @variable)

; this / super
(this_expression) @variable.builtin
(super_expression "super" @variable.builtin)

; Wildcard pattern (after everything else so it wins for `_`)
((identifier_pattern) @variable.builtin
 (#eq? @variable.builtin "_"))
