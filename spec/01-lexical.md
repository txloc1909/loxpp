# Lox++ Lexical Grammar

## Source Encoding

Source files are UTF-8 encoded text. The lexer operates on bytes; multi-byte
UTF-8 sequences are permitted inside string literals and line comments but have
no special meaning outside them.

---

## Whitespace

The following characters are treated as whitespace and ignored between tokens:

- Space (`U+0020`)
- Horizontal tab (`U+0009`)
- Carriage return (`U+000D`)
- Newline (`U+000A`)

Lox++ has no significant whitespace. Newlines do not terminate statements.

---

## Comments

A line comment begins with `//` and extends to the end of the current line.
Comments are treated as whitespace — they produce no tokens.

```
// This entire line is a comment.
var x = 1; // Inline comment after a statement.
```

There are no block comments.

---

## Token Categories

### Single-Character Tokens

| Lexeme | Name |
|---|---|
| `(` | LEFT_PAREN |
| `)` | RIGHT_PAREN |
| `{` | LEFT_BRACE |
| `}` | RIGHT_BRACE |
| `[` | LEFT_BRACKET |
| `]` | RIGHT_BRACKET |
| `,` | COMMA |
| `:` | COLON |
| `.` | DOT |
| `-` | MINUS |
| `+` | PLUS |
| `;` | SEMICOLON |
| `/` | SLASH |
| `*` | STAR |

### One-or-Two-Character Tokens

The scanner always prefers the longer match.

| Lexeme | Name |
|---|---|
| `!` | BANG |
| `!=` | BANG_EQUAL |
| `=` | EQUAL |
| `==` | EQUAL_EQUAL |
| `>` | GREATER |
| `>=` | GREATER_EQUAL |
| `<` | LESS |
| `<=` | LESS_EQUAL |

---

## Keywords

The following identifiers are reserved and may not be used as user-defined
names:

| Keyword | Notes |
|---|---|
| `and` | Logical conjunction |
| `break` | Loop/switch exit |
| `case` | Switch arm label |
| `class` | Class declaration |
| `continue` | Loop next-iteration |
| `default` | Switch fallback arm label |
| `else` | Alternate branch |
| `false` | Boolean literal |
| `for` | For loop |
| `fun` | Function declaration |
| `if` | Conditional |
| `nil` | Nil literal |
| `or` | Logical disjunction |
| `print` | Print statement |
| `return` | Function return |
| `super` | Superclass accessor inside a method |
| `switch` | Switch statement |
| `this` | Current instance reference inside a method |
| `true` | Boolean literal |
| `var` | Variable declaration |
| `while` | While loop |

Keywords are matched case-sensitively. `And` and `AND` are identifiers, not
keywords.

---

## Identifiers

```
IDENTIFIER ::= ALPHA ( ALPHA | DIGIT )* ;
ALPHA      ::= [a-zA-Z_] ;
DIGIT      ::= [0-9] ;
```

An identifier begins with a letter (`a`–`z`, `A`–`Z`) or underscore (`_`),
followed by zero or more letters, digits (`0`–`9`), or underscores.

---

## Number Literals

```
NUMBER ::= DIGIT+ ( "." DIGIT+ )? ;
```

Numbers are decimal. An optional fractional part is introduced by `.` followed
by at least one digit. There are no hexadecimal, octal, binary, or exponential
notations.

All numbers are represented as IEEE 754 double-precision floating point. There
is no separate integer type.

Examples: `0`, `42`, `3.14`, `1.0`

---

## String Literals

```
STRING ::= '"' CHARACTER* '"' ;
```

A string literal is a sequence of characters enclosed in double quotes. The
token's value is the content between the quotes (the delimiters themselves are
not part of the value).

Strings may span multiple lines; newlines inside the string are included in
the value.

An unterminated string (reaching end-of-input before the closing `"`) is a
static error.

There are currently no recognized escape sequences — each character in the
source appears literally in the string value.

---

## End of File

`EOF` is a synthetic token produced after all source characters have been
consumed. The grammar uses `EOF` as the sentinel that ends a program.
