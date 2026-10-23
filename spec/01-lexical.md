# Lox++ Lexical Grammar

## Source Encoding

Source files are ASCII text. The lexer operates on bytes; only printable ASCII
characters and standard whitespace (space, tab, CR, LF) are meaningful. Non-ASCII
bytes are not permitted in source files.

A byte that cannot begin any token is a static error.

---

## Whitespace

The following characters are treated as whitespace and ignored between tokens:

- Space (`0x20`)
- Horizontal tab (`0x09`)
- Carriage return (`0x0D`)
- Newline (`0x0A`)

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
| `%` | PERCENT |
| `@` | AT |
| `&` | AMPERSAND |
| `\|` | PIPE |
| `^` | CARET |
| `~` | TILDE |

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
| `<<` | LESS_LESS |
| `>>` | GREATER_GREATER |
| `>>>` | GREATER_GREATER_GREATER |

`>>>` has three characters. The scanner reads `>>>` in preference to `>>`,
`>>` in preference to `>`, and `<<` in preference to `<`. No token holds an
`=` after `<<` or `>>`, so `x >>= 1` is the tokens `x`, `>>`, `=`, and `1`:
Lox++ has no compound assignment.

### Compound Punctuation

The scanner checks for the longer lexeme before it falls back to the shorter
operator. When the longer lexeme cannot complete, it splits the run instead of
silently extending the shorter token's span (see ELIPSIS below).

| Lexeme | Name | Notes |
|---|---|---|
| `=>` | FAT_ARROW | Two characters. The scanner tests for `>` right after `=` before it treats `=` as EQUAL or EQUAL_EQUAL. Introduces a `match` arm body (see §02-syntax). |
| `...` | ELIPSIS | Three dots. The scanner makes an ELIPSIS only when it sees three dots in a row. Any other dot run carries no special meaning: each dot not consumed by an ELIPSIS is its own DOT token (so `..` is two DOT tokens, `....` is an ELIPSIS followed by a DOT). The grammar never places a DOT directly after a DOT, so a two-dot run is rejected at parse time. Used in sequence patterns (see §02-syntax). |

The `@` token (single character, above) binds a name to a whole value in a
`match` pattern (see §02-syntax).

---

## Keywords

The following identifiers are reserved and may not be used as user-defined
names:

| Keyword | Notes |
|---|---|
| `and` | Logical conjunction |
| `break` | Loop exit |
| `case` | Match arm label |
| `catch` | Catches the value a `try` block throws, binds one identifier (see §02-syntax) |
| `class` | Class declaration |
| `continue` | Loop next-iteration |
| `default` | Reserved. Scanned as a keyword, but no grammar rule uses it. A `match` arm uses `case _` for the fallback. |
| `defer` | Defers a call to function exit, runs LIFO (see §02-syntax) |
| `else` | Alternate branch |
| `enum` | Enum declaration |
| `false` | Boolean literal |
| `for` | For loop |
| `fun` | Function declaration |
| `if` | Conditional |
| `in` | Sequence membership test; for-in loop variable binding |
| `len` | Length operator; dispatches to `__len__` on an Instance (see §04-semantics) |
| `match` | Match expression (see §02-syntax) |
| `nil` | Nil literal |
| `or` | Logical disjunction |
| `print` | Print statement |
| `return` | Function return |
| `str` | String conversion operator; dispatches to `__str__` on an Instance (see §04-semantics) |
| `super` | Superclass accessor inside a method |
| `this` | Current instance reference inside a method |
| `throw` | Throws a value to the nearest enclosing `try` (see §02-syntax) |
| `true` | Boolean literal |
| `try` | Protected block with a `catch` handler (see §02-syntax) |
| `var` | Variable declaration |
| `while` | While loop |
| `yield` | Suspends the current coroutine and delivers a value to its resumer (see §04-semantics) |

Keywords are matched case-sensitively. `And` and `AND` are identifiers, not
keywords.

`case` and `default` are keyword tokens. The `match` wildcard pattern is the
identifier `_`, which is an ordinary identifier, not a keyword (see §02-syntax
and §04-semantics).

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

## Number and Int Literals

```
NUMBER   ::= DIGIT+ ( "." DIGIT+ )? ;
INT      ::= DIGIT+ "i" | "0x" HEXDIGIT+ | "0b" BINDIGIT+ ;
BADINT   ::= DIGIT+ "." DIGIT+ "i" ;
HEXDIGIT ::= [0-9a-fA-F] ;
BINDIGIT ::= [01] ;
```

`BADINT` is not a token of the language. It is the form of a fractional
literal with the suffix, and the scanner rejects it (row `fractional-suffix`
below). It is in the grammar so that the longest match gives the same answer
as the rule of the suffix: at `1.5in`, `BADINT` matches `1.5i`, which is longer
than the `NUMBER` `1.5`, so the scanner reads `BADINT` and rejects it.

A `NUMBER` is a Number literal. Its digits are decimal. An optional fractional
part is introduced by `.` followed by at least one digit. There are no octal
or exponential notations.

All Numbers are represented as IEEE 754 double-precision floating point.

An `INT` is an Int literal, a signed 64-bit integer (see
[§03-types](03-types.md#int)). It has one of three forms:

- Decimal digits directly followed by the suffix `i`: `0i`, `42i`, `007i`.
- `0x` followed by hexadecimal digits, in either case: `0xff`, `0xFF`.
- `0b` followed by binary digits: `0b1010`.

A hexadecimal or binary literal takes no suffix. It gives a 64-bit pattern:
`0xFFFFFFFFFFFFFFFF` is `-1i`, and `0x8000000000000000` is the smallest Int.
Leading zero digits do not count towards the 64 bits. The prefix is `0x` or
`0b` in lower case only. A `0x` or `0b` that no digit of its base follows is
not a prefix: the scanner reads the `0` as a NUMBER, so `0xg` and `0b2` are a
NUMBER followed by an identifier, which is a syntax error. There are no digit
separators.

The suffix is part of the token. The scanner reads an `i` directly after the
digits of a decimal literal, with a fractional part or without one, as the
suffix, whatever character follows it, so `1i` is one token. An `i` after
white space is not a suffix: `1 i` is the Number `1` and the name `i`, a
syntax error, as it was before the suffix existed. Four forms change from
valid to a syntax error. Before the suffix existed, `1in xs` was `1 in xs`,
and `case 1if t` was the pattern `1` with the guard `if t`. The same held for
`1.5in xs` and `case 1.5if t`. Now the scanner reads the `i` as the suffix.
`1in` and `1if` are each an Int literal followed by the name `n` or `f`.
`1.5in` and `1.5if` are each the literal `1.5i`, a fractional literal with the
suffix, which is a static error by itself. A program that writes a decimal
literal directly before `in` or `if` needs a space between them. No literal
that has no suffix and no `0x` or `0b` prefix is an Int: `1` and `1.0` are
Numbers, and `1.5i` is an error.

A hexadecimal or binary literal ends at the first character that is not a
digit of its base, and the scanner then starts the next token there. The
letter `i` is not a digit of either base, so `0x1in [1i]` is the Int `0x1`,
the keyword `in`, and the list `[1i]`, a valid program that prints `true`.
`case 0b1if t` reads the same way: the pattern `0b1` with the guard `if t`.
The suffix `i` belongs to decimal literals only: a hexadecimal or binary
literal has no suffix, so no rule makes the `i` after one a suffix.

Examples: Numbers `0`, `42`, `3.14`, `1.0`; Ints `0i`, `42i`, `0xFF`, `0b1010`.

### Static errors in Int literals

Each row of the table is a static error. The accepted limit of a row is the
nearest literal of the same form that is valid.

| Row | Static error | Example | Accepted limit |
|---|---|---|---|
| `decimal-range` | A decimal Int literal whose value is above 9223372036854775807, except the unary-minus exception below | `9223372036854775808i` | `9223372036854775807i` |
| `hex-width` | A hexadecimal literal whose value needs more than 64 bits | `0x10000000000000000` | `0xFFFFFFFFFFFFFFFF` |
| `binary-width` | A binary literal whose value needs more than 64 bits | `0b1` followed by 64 zeros | `0b` followed by 64 ones |
| `fractional-suffix` | A literal with a fractional part and the suffix `i`, which includes a fractional literal directly before `in` or `if` | `1.5i`, `1.0i`, `1.5in [1.5]`, `case 1.5if t` | `15i`, `1.5 in [1.5]` |
| `suffix-boundary` | A decimal literal and the letter `i` in the wrong position: the `i` directly after the digits with a name character after it, or the `i` after white space | `1in [1, 2]`, `case 1if t`, `1 i` | `1i in [1i, 2i]` |

**The unary-minus exception.** The literal `9223372036854775808i` is valid
when it is the direct operand of a unary `-`. The unary `-` and the literal
together then give the smallest Int, `-9223372036854775808i`. The rule is
about the grammar of [§02-syntax](02-syntax.md), not about the tokens that
stand next to the literal. The literal is the direct operand when it is the
whole `call` that the `unary` rule of a `-` takes: the `-` stands in prefix
position, and no `(`, `.`, or `[` follows the literal. Each of those three
binds to the literal first, as a call, a property read, or an index, so the
operand of the `-` is that larger expression and not the literal. A `-` with a
left operand is a binary `-`. A literal inside parentheses is the operand of
the parentheses, not of the `-`. White space between the tokens does not
matter, and a binary operator after the literal does not matter either. So
`-9223372036854775808i`, `- 9223372036854775808i`, and
`-9223372036854775808i + 1i` are valid. These are static errors:
`-(9223372036854775808i)`, `0i - 9223372036854775808i`,
`-9223372036854775809i`, `-9223372036854775808i[0]`,
`-9223372036854775808i.x`, and `-9223372036854775808i(1)`.

---

## String Literals

```
STRING   ::= '"' STR_CHAR* '"' ;
STR_CHAR ::= ESCAPE | <any character except '"' and '\'>
ESCAPE   ::= '\' ( '"' | '\' | 'n' | 't' | 'r' | '0' ) ;
```

A string literal is a sequence of characters enclosed in double quotes. The
token's value is the content between the quotes (the delimiters themselves are
not part of the value).

Strings may span multiple lines; newlines inside the string are included in
the value.

An unterminated string (reaching end-of-input before the closing `"`) is a
static error.

The following escape sequences are recognised inside string literals:

| Sequence | Value |
|----------|-------|
| `\"` | double-quote character (`"`) |
| `\\` | backslash character (`\`) |
| `\n` | newline (U+000A) |
| `\t` | horizontal tab (U+0009) |
| `\r` | carriage return (U+000D) |
| `\0` | null byte (U+0000) |

A `\` followed by any character not in the table above is a static error.

---

## End of File

`EOF` is a synthetic token produced after all source characters have been
consumed. The grammar uses `EOF` as the sentinel that ends a program.
