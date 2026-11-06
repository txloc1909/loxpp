# The canonical text form of a Lox++ program

**Status:** proposal. Waits for the owner's approval. On acceptance it moves to
`docs/decisions/` and names its tracking issue.
**Tracking (draft):** #213, `loxpp fmt` and then `documentFormattingProvider`.

## Goal in VISION.md

The **Simple** design goal: "a small, learnable surface area; no hidden
complexity". Insignificant white space is hidden complexity of exactly the kind
that goal names. Two programs can hold the same token stream and look
different, and one of them can read as a different program:

```lox
print 0x1in [0x1];                               // the tokens INT 0x1, in, [, ...
print match 1 { case 1 => 5 case _ => 6 } + 1;   // `+ 1` is not in the arm body
while (i < 3) s = s + i;                         // the next line is not in the loop
i = i + 1;
```

A canonical text form removes that ambiguity without removing a control. It
changes no meaning, no token, and no cost.

## Owner decisions

The owner set the scope on 2026-10-24, in the review of #213.

| Fork | Decision |
|---|---|
| The mandate | "As long as the grammar is explicit, the job of the formatter is just to prevent ambiguity from insignificant white space." The formatter changes white space only. It does not reorder, rename, add, or remove a token. |
| The preparation | Of eight proposed preparation items, two stand: the token-stream invariant as a checked property (PR #845), and this document. The other six are dropped. |
| Where the canonical form binds | `spec/` binds meaning, and a formatter changes no meaning, so `spec/` does not state a canonical text. This decision binds the tools. |

## Why the formatter cannot decide this alone

`#213`'s second comment records the survey: 426 programs, with each finding
classified as **Kind 1** (white space causes the ambiguity, so a rule removes
it) or **Kind 2** (two meanings share one shape, so white space cannot remove
it). Every Kind 1 finding becomes a rule below. A Kind 2 finding becomes a fork,
because its only true fix is a change of vocabulary, and that is not this
decision's to make.

The safety argument is one sentence: the formatter prints the token stream that
it parsed, so a second scan of its output gives the same tokens. PR #845 checks
that over the repository: 544 programs round-trip with one space between two
tokens, and 516 of them change when every separator is removed. The space is
load-bearing, and the rule table below says where it goes.

## The rule table

The table is the countable set. The implementation of #213 adds a check that
every row has at least one format case, in the way `tools/check_int_operand_table.py`
checks the operand table.

| Id | Construct | Canonical text |
|---|---|---|
| R1 | binary operator, punctuation (`+ - * / % == != < <= > >= & \| ^ << >> >>>`) | one space on each side |
| R2 | binary operator, word (`and or in`) | one space on each side |
| R3 | unary operator (`! - ~`) | no space between it and its operand. R15 is the exception |
| R4 | assignment `=` | one space on each side |
| R5 | `,` | no space before, one space after |
| R6 | `;` | no space before; the statement ends the line |
| R7 | `:` in a map entry | no space before, one space after |
| R8 | `:` in a slice bound | fork F7 |
| R9 | `(` of a call, `len`, or `str` | no space before |
| R10 | `(` of `if`, `while`, `for`, or `catch` | one space before |
| R11 | `(` and `)` of a grouping, a call, or a parameter list | no space inside |
| R12 | `[` and `]` of an index, a slice, or a list literal | no space inside |
| R13 | `{` and `}` of a block | `{` ends the line, the body is indented, `}` starts a line |
| R14 | `{` and `}` of a match body | one arm per line, the body of an arm indented |
| R15 | two operators with nothing between them | one space between them: `a - -1`, `~ -1i` |
| R16 | `case PATTERN => BODY` | one space on each side of `=>`; one arm per line, always (F3) |
| R17 | a statement body with no brace | fork F4 |
| R18 | a `match` used as an operand, an argument, or the base of a postfix | in parentheses |
| R19 | a class pattern's field brace | fork F1 |
| R20 | an empty map at a value position, and an empty block at a statement position | fork F2 |
| R21 | a line wider than the limit | fork F5. A String token is never cut |

Three rules apply to the whole output, and none of them is a row:

- the token stream of the output equals the token stream of the input;
- formatting is idempotent: `fmt(fmt(x))` equals `fmt(x)`;
- a program with a static error is refused, and nothing is written.

## Forks the owner must decide

| Fork | The question | The recommendation |
|---|---|---|
| F1 | B3: is a class pattern written `Num{v}` or `Num {v}`? | `Num{v}`. The repository writes it that way in 24 places and never with a space, and a `{` after a name in a pattern is a field list, never a block |
| F2 | C3: `{}` is a Map at a value position and an empty block at a statement position | The formatter writes an empty block on its own line and keeps an empty Map inline, so the two shapes differ. The ambiguity itself needs a separate language decision: an empty block does nothing, so the spec could reject it. That is not this proposal |
| F3 | Is a two-arm match that fits on one line still split? | Yes. It is the Kind 1 finding with the largest gain (64 lines today), and one arm per line is what makes R16 decidable |
| F4 | Is a statement body with no brace ever allowed? | Only `return`, `break`, or `continue`, on the same line, and only when the whole `if` fits the width. Otherwise braces. This is the C1 finding: 1208 lines today, and one of them is a loop that never ends because the next line is outside it |
| F5 | The line width, and the wrap rule | 100 columns. Wrap at a binary operator, keeping the operator at the start of the next line. Never cut a String token; report it instead |
| F6 | Does the formatter honour `tabSize` and `insertSpaces` from the LSP? | No. One canonical text, by design: a canonical form that depends on editor settings cannot bind two tools. The LSP request ignores the two fields, and the decision says so |
| F7 | Slice bound `:` written tight (`l[1:3]`) or spaced (`l[a : b]`) | Tight. The repository writes it tight 110 times and spaced 15 times, so tight is the majority and it never makes a line longer |

## The contract

**Command line.** `loxpp fmt [--check] [--write] [-o <path>] <file>`. With no
output flag the text goes to stdout. Exit codes, in the style
`docs/decisions/editor-tooling.md` already fixes:

| Code | Meaning |
|---|---|
| 0 | the program formatted, or `--check` found it already canonical |
| 1 | `--check` found a difference |
| 64 | usage error |
| 65 | the program has a static error, so nothing was written |

**LSP.** `documentFormattingProvider`, full document only. Range formatting is
out of scope. One `TextEdit` that replaces the whole document.

## Consumers

`AGENTS.md` asks for every consumer before the work starts. The formatter's
correctness rests on every scanner agreeing about the token stream, so the list
includes the scanners.

| Consumer | What it owes |
|---|---|
| Native CLI (`src/main.cpp`, a new tooling source) | The rules and the contract above |
| LSP (`src/lsp/`) | The same text, through `documentFormattingProvider` |
| The native scanner | No change. It is the source of truth for the token stream, and PR #845 pins the property it must have |
| The bootstrap interpreter's scanner | Agreement about the token stream. If it disagrees, the formatter and the interpreter disagree about one program. That is N6's (#830) cost, not the formatter's |
| tree-sitter grammar and editors | No change. The canonical text is valid Lox++, so the grammar already parses it. Format-on-save calls the LSP |
| `spec/` | No change. `spec/01-lexical.md` keeps the normative sentence that white space between tokens is insignificant |

## Countable ground truth

- The rule table above, one row per construct, with a format case per row.
- Each Kind 1 finding of the `#213` survey, with its count before the formatter
  and its expected count after it. The count is the evidence that a rule works.

## Open questions for the owner

1. F1 to F7 above.
2. Does `loxpp fmt` belong in the single distribution binary, or as a separate
   entry point? `docs/decisions/distribution.md` owns that surface.
