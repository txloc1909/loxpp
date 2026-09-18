# loxpp.tmbundle

TextMate grammar for [Lox++](../../README.md). It highlights `.lox` source
files in TextMate, VS Code, and any other editor that reads TextMate
grammars.

## Files

```
info.plist                  bundle metadata (name, uuid, contact)
Syntaxes/Lox++.tmLanguage.json   the grammar, JSON format
Syntaxes/Lox++.tmLanguage        the same grammar, XML plist format
```

The JSON file is a byte-for-byte copy of
`../loxpp-vscode/syntaxes/lox.tmLanguage.json`. The VS Code extension tests
(`npm run test --prefix editors/loxpp-vscode`) test that file. This copy
inherits that coverage. Do not edit one copy without the other. CI fails
when the two files differ. See `tools/check_textmate_grammar.py`.

The XML plist carries the same patterns and repository rules as the JSON
file. It adds `fileTypes` (`lox`) and a grammar `uuid` for TextMate and
GitHub Linguist consumers. The JSON file leaves extension mapping to each
editor: `package.json` (VS Code), `ftdetect/` (Vim), `tree-sitter.json`
(tree-sitter).

## Use

**TextMate** — double-click the `.tmbundle` directory. Or copy it to
`~/Library/Application Support/TextMate/Bundles/`.

**VS Code** — use the [`loxpp-vscode`](../loxpp-vscode/) extension. It
ships this grammar. You do not need this bundle for VS Code.

**GitHub Linguist** — Linguist does not read this directory directly.
To propose Lox++ highlighting on github.com, open a Linguist pull request
that references this grammar. This bundle is the artifact that request
needs.

## Scope

Token list follows `spec/01-lexical.md` and `src/token.h`. The grammar
covers all 27 keywords, numbers (`DIGIT+ ( "." DIGIT+ )?`, decimal only),
double-quoted strings with the six valid escapes (`\" \\ \n \t \r \0`),
line comments (`//`), operators (`== != <= >= => @ ...` and the single
forms), and `class` / `fun` / `enum` declaration names plus call-position
names.
