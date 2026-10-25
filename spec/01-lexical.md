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

A `NUMBER` is a Number literal. Its digits are decimal. An optional fractional
part is introduced by `.` followed by at least one digit. There are no octal
or exponential notations.

All Numbers are represented as IEEE 754 double-precision floating point.

**Range of a Number.** A `NUMBER` token denotes the nearest IEEE 754
double-precision value to the decimal that its digits spell, and a tie goes to
the value with an even last bit. The nearest value is taken over the whole
range of the format: a decimal at or above the midpoint between the largest
finite double and 2^1024 denotes positive infinity, and a decimal at or below
half the smallest subnormal denotes zero. The tie rule above decides both
ends: the midpoint at the top goes to the even value 2^1024, which is an
infinity, and the midpoint at the bottom goes to the even value zero. No
`NUMBER` token is a static error, whatever its value.

Each row of the table is an example of that rule, not a rule of its own.
`tools/check_number_literal_range.py` proves that a spec probe holds each row
and that the probe's check line agrees with the row.

| Case | Source | Value |
|---|---|---|
| `largest-finite` | the 309 digits of the largest finite double | `1.7976931348623157e+308` |
| `above-largest-finite` | `17976931348623158` followed by 292 zeros, which is nearer to the largest finite double than to infinity | `1.7976931348623157e+308` |
| `overflow` | `179769313486231581` followed by 291 zeros, which is above the midpoint between the largest finite double and 2^1024 | `inf` |
| `overflow-midpoint` | the 309 digits of the midpoint between the largest finite double and 2^1024, which is exactly that midpoint | `inf` |
| `far-above-range` | 400 `9`s | `inf` |
| `smallest-subnormal` | `0.` followed by 323 zeros and a `5` | `5e-324` |
| `below-smallest-subnormal` | `0.` followed by 323 zeros and a `49`, which is nearer to the smallest subnormal than to zero | `5e-324` |
| `underflow-midpoint` | `0.` followed by 1075 decimal places that spell half the smallest subnormal | `0` |
| `far-below-range` | `0.` followed by 400 zeros and a `1` | `0` |

An `INT` is an Int literal, a signed 64-bit integer (see
[§03-types](03-types.md#int)). `BADINT` is in the grammar so that the longest
match reads `1.5i` as one rejected lexeme, and not as the `NUMBER` `1.5` and a
name.

**Tokens.** At a digit, the scanner reads the longest prefix that matches
`NUMBER`, `INT`, or `BADINT`, as it does for operators. The next token starts
where that prefix ends. `BADINT` is a static error (row `fractional-suffix`
below).

**Range.** A decimal `INT` token denotes the integer that its digits spell. It
is a static error when that value is above 9223372036854775807 (row
`decimal-range`), except under the unary-minus exception below. A hexadecimal
or binary `INT` token denotes the 64-bit pattern of its value, read as a
two's-complement Int: `0xFFFFFFFFFFFFFFFF` is `-1i`. It is a static error when
that value is above 18446744073709551615 (rows `hex-width` and
`binary-width`).

### Static errors in Int literals

Each row of the table is a static error. The accepted limit of a row is the
nearest literal of the same form that is valid.

| Row | Static error | Example | Accepted limit |
|---|---|---|---|
| `decimal-range` | A decimal `INT` token whose value is above 9223372036854775807, except under the unary-minus exception below | `9223372036854775808i` | `9223372036854775807i` |
| `hex-width` | A hexadecimal `INT` token whose value is above 18446744073709551615 | `0x10000000000000000` | `0xFFFFFFFFFFFFFFFF` |
| `binary-width` | A binary `INT` token whose value is above 18446744073709551615 | `0b1` followed by 64 zeros | `0b` followed by 64 ones |
| `fractional-suffix` | A `BADINT` token: a literal with a fractional part and the suffix `i` | `1.5i` | `15i` |
| `suffix-boundary` | A name that follows a literal with no operator or punctuation between them. The longest match does not join the name to the literal, so the parser sees a literal and a name side by side, and rejects the pair | `1in [1, 2]` | `1i in [1i, 2i]` |

**The unary-minus exception.** A decimal Int literal whose value is
9223372036854775808 is valid when it is the direct operand of a unary `-`. The
unary `-` and the literal together then give the smallest Int. The rule is
about the grammar of [§02-syntax](02-syntax.md), not about the tokens that
stand next to the literal. The literal is the direct operand when it is the
whole `call` that the `unary` rule of a `-` takes: the `-` stands in prefix
position, and no `(`, `.`, or `[` follows the literal. Each of those three
binds to the literal first, as a call, a property read, or an index, so the
operand of the `-` is that larger expression and not the literal. A `-` with a
left operand is a binary `-`. A literal inside parentheses is the operand of
the parentheses, not of the `-`. White space between the tokens does not
matter, and a binary operator after the literal does not matter either.

### Examples of the literal rules

Each row follows from the three rules above: the longest match, the range, and
the unary-minus exception. A row is an example, not a rule. `Source` is the
text of the example. A `Source` that starts with `case` is an arm of a `match`
expression, and each other `Source` is an expression. `Tokens` lists what the
longest match reads: a literal or a name is written as its token kind and its
lexeme, and any other token as its lexeme. `Result` is `valid`, or the row of
"Static errors in Int literals" that the program breaks.
`tools/check_int_literal_rejections.py` checks that a spec probe holds each
`Source`.

| Source | Tokens | Result |
|---|---|---|
| `007i` | `INT 007i` | valid |
| `15i` | `INT 15i` | valid |
| `9223372036854775807i` | `INT 9223372036854775807i` | valid |
| `09223372036854775807i` | `INT 09223372036854775807i` | valid |
| `-9223372036854775808i` | `-` `INT 9223372036854775808i` | valid |
| `- 9223372036854775808i` | `-` `INT 9223372036854775808i` | valid |
| `-9223372036854775808i + 1i` | `-` `INT 9223372036854775808i` `+` `INT 1i` | valid |
| `-09223372036854775808i` | `-` `INT 09223372036854775808i` | valid |
| `0xFFFFFFFFFFFFFFFF` | `INT 0xFFFFFFFFFFFFFFFF` | valid |
| `0x0000000000000000FFFFFFFFFFFFFFFF` | `INT`, the whole text | valid |
| `0b1111111111111111111111111111111111111111111111111111111111111111` | `INT`, the whole text | valid |
| `0b000000001111111111111111111111111111111111111111111111111111111111111111` | `INT`, the whole text | valid |
| `1i in [1i, 2i]` | `INT 1i` `in` `[` `INT 1i` `,` `INT 2i` `]` | valid |
| `case 1i if t` | `case` `INT 1i` `if` `IDENTIFIER t` | valid |
| `1.5 in [1.5]` | `NUMBER 1.5` `in` `[` `NUMBER 1.5` `]` | valid |
| `case 1.5 if t` | `case` `NUMBER 1.5` `if` `IDENTIFIER t` | valid |
| `0x1in [1i]` | `INT 0x1` `in` `[` `INT 1i` `]` | valid |
| `0b1in [1i]` | `INT 0b1` `in` `[` `INT 1i` `]` | valid |
| `case 0x1if t` | `case` `INT 0x1` `if` `IDENTIFIER t` | valid |
| `1iin [1i]` | `INT 1i` `in` `[` `INT 1i` `]` | valid |
| `case 1iif t` | `case` `INT 1i` `if` `IDENTIFIER t` | valid |
| `1iand true` | `INT 1i` `and` `true` | valid |
| `0x1or false` | `INT 0x1` `or` `false` | valid |
| `9223372036854775808i` | `INT 9223372036854775808i` | `decimal-range` |
| `99999999999999999999i` | `INT 99999999999999999999i` | `decimal-range` |
| `-(9223372036854775808i)` | `-` `(` `INT 9223372036854775808i` `)` | `decimal-range` |
| `-9223372036854775809i` | `-` `INT 9223372036854775809i` | `decimal-range` |
| `0i - 9223372036854775808i` | `INT 0i` `-` `INT 9223372036854775808i` | `decimal-range` |
| `-9223372036854775808i[0]` | `-` `INT 9223372036854775808i` `[` `NUMBER 0` `]` | `decimal-range` |
| `-9223372036854775808i.x` | `-` `INT 9223372036854775808i` `.` `IDENTIFIER x` | `decimal-range` |
| `-9223372036854775808i(1)` | `-` `INT 9223372036854775808i` `(` `NUMBER 1` `)` | `decimal-range` |
| `0x10000000000000000` | `INT 0x10000000000000000` | `hex-width` |
| `0b10000000000000000000000000000000000000000000000000000000000000000` | `INT`, the whole text | `binary-width` |
| `1.5i` | `BADINT 1.5i` | `fractional-suffix` |
| `1.0i` | `BADINT 1.0i` | `fractional-suffix` |
| `1.5in [1.5]` | `BADINT 1.5i` `IDENTIFIER n` `[` `NUMBER 1.5` `]` | `fractional-suffix` |
| `case 1.5if t` | `case` `BADINT 1.5i` `IDENTIFIER f` `IDENTIFIER t` | `fractional-suffix` |
| `1 i` | `NUMBER 1` `IDENTIFIER i` | `suffix-boundary` |
| `1in [1, 2]` | `INT 1i` `IDENTIFIER n` `[` `NUMBER 1` `,` `NUMBER 2` `]` | `suffix-boundary` |
| `case 1if t` | `case` `INT 1i` `IDENTIFIER f` `IDENTIFIER t` | `suffix-boundary` |
| `0XFF` | `NUMBER 0` `IDENTIFIER XFF` | `suffix-boundary` |
| `0xFFi` | `INT 0xFF` `IDENTIFIER i` | `suffix-boundary` |
| `0b1i` | `INT 0b1` `IDENTIFIER i` | `suffix-boundary` |
| `0xFFand true` | `INT 0xFFa` `IDENTIFIER nd` `true` | `suffix-boundary` |
| `0x` | `NUMBER 0` `IDENTIFIER x` | `suffix-boundary` |

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
