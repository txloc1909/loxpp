# Lox++ editor tooling — design note

This note records the editor-tooling stack that recent work built:
the `loxpp --check` diagnostics CLI, the `loxpp-lsp` language server, the
`tree-sitter-loxpp` grammar, and the `loxpp.nvim` plugin.

It is the durable record. An earlier design draft preceded this note; where
they differ, this note is correct.

Language semantics stay in `spec/`. This note does not repeat them.

---

## Architecture — four layers

```
+------------------------------------------------------------------+
| editors/loxpp.nvim/            Neovim plugin (Lua) + Vim fallback |
|   ftdetect / ftplugin, tree-sitter registration, LSP client,     |
|   nvim-lint + :compiler lox fallback to `loxpp --check`,          |
|   :checkhealth loxpp                                             |
+---------------+-----------------------------+--------------------+
                |                             |
+---------------v-------------+   +-----------v---------------------+
| editors/tree-sitter-loxpp/  |   | loxpp-lsp   (C++ binary)        |
|   grammar.js + queries/*.scm |   |   src/lsp/ - JSON-RPC 2.0,      |
|   -> highlighting, folds,     |   |   handlers                     |
|      indents, locals         |   |   diagnostics <- loxpp_core     |
|                              |   |   symbols/hover/def <- tooling  |
+------------------------------+   +-----------+--------------------+
                                               |
                             +-----------------v--------------------+
                             | src/tooling/   (standalone)          |
                             |   ast.h, tooling_parser, resolver,   |
                             |   symbol_table, document_model,      |
                             |   stdlib_names                       |
                             |   reuses src/scanner.cpp + token.h   |
                             +-----------------+-------------------+
                                               |
                             +-----------------v--------------------+
                             | loxpp_core   (OBJECT library)        |
                             |   the interpreter sources, minus     |
                             |   main.cpp                           |
                             |   + Token.offset                     |
                             |   + Parser DiagnosticSink            |
                             |   + analyze()  ->  `loxpp --check`   |
                             +-------------------------------------+
```

Build targets:

- `loxpp_core` — `add_library(loxpp_core OBJECT ...)` in `CMakeLists.txt`.
  Every interpreter source except `src/main.cpp`. The `loxpp` executable is
  `main.cpp` plus this library.
- `loxpp_tooling` — `add_library(loxpp_tooling OBJECT ...)`. The
  `src/tooling/` analysers. It links `loxpp_core` for the scanner and token
  types only.
- `loxpp` — the interpreter and the `--check` CLI.
- `loxpp-lsp` — the language server. Guarded by `option(LOXPP_LSP ... ON)`.
  Links `loxpp_tooling`, `loxpp_core`, and `Threads`. Includes
  `third_party/` for the vendored `nlohmann/json.hpp`.

---

## Why two parsers

`src/compiler.cpp` is a single-pass Pratt parser and bytecode compiler. It
keeps no persistent syntax tree (`src/compiler.h`). Each `Token` carries a
line number and a source byte offset (`src/token.h`), but the
compiler frees all structure as it emits code.

The compiler is the authority for **diagnostics**. It does a full parse, name
resolution, and the limit checks. The tooling work added a `DiagnosticSink` to
`Parser` (`src/diagnostic.h`): when a sink is set, `Parser::errorAt` pushes a
`Diagnostic { offset, length, line, severity, message }` instead of printing
to `stderr`. `analyze()` (`src/analyze.h`, `src/analyze.cpp`) drives
`compile()` with a sink and a throwaway `MemoryManager` and returns the list.

The compiler is useless for **navigation**, because it keeps no tree. So
`src/tooling/` is a second, standalone recursive-descent parser:

- `src/tooling/ast.h` — node structs for the whole grammar. Every node
  carries a source span (`offset`, `length`).
- `src/tooling/tooling_parser.{h,cpp}` — recursive descent over `Scanner`.
  It never throws and never writes to any stream. On malformed input it
  recovers (it synchronises to the next `;`, `}`, or declaration-keyword
  start) and returns a partial `Program`.
- `src/tooling/resolver.{h,cpp}`, `symbol_table.{h,cpp}` — the scope tree and
  name resolution. It emits **warnings** only; the authoritative errors come
  from `analyze()`.
- `src/tooling/document_model.{h,cpp}` — per open file: text, line index,
  `Program`, scope tree, reference map, and a `rebuild(newText)` entry point.
- `src/tooling/stdlib_names.{h,cpp}` — the set of known stdlib global names,
  so the resolver does not flag them as undefined.

The tooling parser bounds its tree depth. `tooling::kMaxTreeDepth`
(`src/tooling/tooling_parser.h`, value 500) is the upper bound on any
root-to-leaf path. The parser counts recursive-descent nesting and the length
of loop-built left-leaning chains on one counter. When the sum reaches the
bound it stops extending that path, records an error, recovers, and still
returns a `Program`. The bound lets the LSP worker thread walk or free the
tree on a small stack without overflow.

Position mapping is easy here. Source is ASCII (`spec/01-lexical.md`), so a
byte offset equals a UTF-16 code unit equals an LSP `character`. Each document
keeps a line-start array; offset-to-position is a binary search
(`LineIndex` in `src/analyze.h`).

---

## The `loxpp --check` CLI contract

This is the stable interface. CI, the pre-commit hook, and the editor
fallback path all treat it as an API. Keep it exact.

### Invocation

```
loxpp --check [--format text|json] <file>
```

- `--check` must be the first argument. `main()` intercepts it before the VM
  path, the same way it intercepts `--target`.
- `--format` is optional. It takes `text` or `json`. The default is `text`.
- Exactly one `<file>` is required.
- The program never runs. `--check` reports the compiler's static errors
  only.

### `--format text`

One line per diagnostic:

```
<path>:<line>:<col>: <severity>: <message>
```

- `<path>` is the path as given on the command line.
- `<line>` and `<col>` are **1-based**. This matches an editor
  `errorformat`.
- `<col>` is derived from the byte offset and the line-start index, not from
  the token line.
- `<severity>` is `error`, `warning`, or `info`. Today `--check` emits
  `error` only.
- The line index is built from the source, so a caret column past the last
  character of a line points just after it.

A Vim `errorformat` that reads it:

```
%f:%l:%c:\ %t%*[^:]:\ %m
```

### `--format json`

A single JSON array on one line. Each element:

```json
{
  "line": 0,
  "character": 0,
  "endLine": 0,
  "endCharacter": 0,
  "severity": "error",
  "message": "..."
}
```

- `line`, `character`, `endLine`, `endCharacter` are **0-based**. This is the
  LSP `Position` shape, so the LSP server and any future editor client can
  use the numbers without a shift.
- The end position is the start offset plus the token length, mapped through
  the same line index.
- `message` is JSON-escaped (`src/json_escape.h`).
- An empty result is `[]`.

**`json` is 0-based (LSP `Position`). `text` is 1-based (editor
`errorformat`).** This is the one difference between the two formats that a
caller must know.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | No error diagnostic. The file may still have warnings once the resolver feeds this path. |
| `1` | At least one diagnostic with severity `error`. Same idea as a linter. |
| `64` | Usage error — no `<file>` argument, a second positional argument, or a `--format` value other than `text` or `json`. An unknown flag such as `--xyz` is taken as the positional path, not rejected here. |
| `74` | The file cannot be read. |

`--check` does not emit `65` or `70`; those stay on the interpreter path
(`runFile`).

### Not in `--check` today

The resolver warnings (unused local, unknown name, and the static-rule
checks) reach the editor through `loxpp-lsp` only. `analyze()` returns the
compiler errors alone. If a later change feeds the resolver warnings into
`--check`, they arrive as `warning` severity and, per the table above, must
not change the exit code.

---

## `loxpp-lsp` capabilities (v1)

The `initialize` handler in `src/lsp/server.cpp` advertises exactly this
capability object:

```json
{
  "positionEncoding": "utf-16",
  "textDocumentSync": {
    "openClose": true,
    "change": 1,
    "save": { "includeText": false }
  },
  "hoverProvider": true,
  "definitionProvider": true,
  "referencesProvider": true,
  "documentHighlightProvider": true,
  "documentSymbolProvider": true,
  "completionProvider": { "triggerCharacters": ["."] }
}
```

`serverInfo` is `{ "name": "loxpp-lsp", "version": "0.1.0" }`.

- `textDocumentSync.change` is `1` = `Full`. The whole document text comes
  with every `didChange`. There is no incremental sync in v1.
- `positionEncoding` is `utf-16`. Source is ASCII, so byte == code unit, but
  the server still advertises the encoding honestly.
- The document store debounces re-analysis. A burst of edits collapses into
  one rebuild once the stream is quiet for `kDebounce` (150 ms,
  `src/lsp/document_store.h`). One worker thread does every rebuild.

Handlers implemented:

| Method | Notes |
|---|---|
| `initialize` / `initialized` / `shutdown` / `exit` | Lifecycle. |
| `textDocument/didOpen` / `didChange` / `didClose` / `didSave` | Full sync. `didClose` clears the client's diagnostics for the file. |
| `textDocument/publishDiagnostics` | `analyze()` errors **plus** the resolver warnings from `DocumentModel::warnings()`. |
| `textDocument/documentSymbol` | Hierarchical, from the tooling scope tree. An enum type is modelled as `Class` with its constructors as members; there is no LSP `Enum` symbol kind in use. |
| `textDocument/hover` | Keyword docs; stdlib global signature + arity + one-line doc; `math` members after a dot on `math`; Map / File methods matched by their unique name; and for a user symbol its kind and declaration. |
| `textDocument/completion` | Keywords, stdlib globals, in-scope user symbols, and five snippets (`if`, `for`, `fun`, `match`, `class`). After a dot, member completion is offered for the `math` receiver only — any other `x.` offers nothing, because the receiver type is unknown. |
| `textDocument/definition` | Single file. |
| `textDocument/references` | Single file. Honours `context.includeDeclaration`. |
| `textDocument/documentHighlight` | Single file. The symbol's declaration and every in-file use — the handler always calls `referencesAt` with `includeDeclaration=true`, so the set can be wider than a `references` request that sets it to `false`. |

Not advertised in v1, so a client must not expect them: `renameProvider`,
`documentFormattingProvider`, `signatureHelpProvider`,
`workspaceSymbolProvider`, `foldingRangeProvider`,
`semanticTokensProvider`, `codeActionProvider`. Folding and indentation come
from the tree-sitter queries, not from the server.

The resolver warnings that reach `publishDiagnostics`: unused local, unknown
name, use of a name in its own initializer, redeclaration in the same scope,
`this` / `super` outside a class, `return` / `break` / `continue` in the
wrong context, `enum` not at global scope, and or-pattern alternatives that
bind different names. Forward references to a global declared later in the
file are **not** flagged; globals are late-bound (`spec/04-semantics.md`).

---

## The `editors/` layout

```
editors/
  tree-sitter-loxpp/        the grammar
    grammar.js
    queries/               highlights.scm locals.scm folds.scm indents.scm
    src/                    generated: parser.c, grammar.json, node-types.json
    test/corpus/*.txt       parse tests
    test/highlight/*.lox     highlight tests
    package.json Cargo.toml binding.gyp tree-sitter.json
    bindings/               node + rust scaffold (not built in CI)
  loxpp.nvim/               the Neovim / Vim plugin
    ftdetect/  lox.vim lox.lua        *.lox -> filetype lox
    ftplugin/  lox.vim lox.lua        commentstring, comments, iskeyword, ...
    syntax/    lox.vim                regex highlighter (the fallback)
    compiler/  lox.vim                :compiler lox -> loxpp --check --format text
    queries/loxpp/*.scm               a verbatim copy of the grammar queries
    lua/loxpp/ init.lua lint.lua health.lua
    plugin/    loxpp.lua              load guard
    scripts/   build-parser.sh
    doc/       loxpp.txt
```

The four `queries/loxpp/*.scm` files in the plugin are a byte-for-byte copy
of `editors/tree-sitter-loxpp/queries/*.scm`. CI fails the build if the two
drift (the `Neovim plugin` job diffs them).

### Building the pieces

The two binaries, from the checkout root:

```sh
cmake --preset debug -DLOXPP_LSP=ON
cmake --build build --target loxpp loxpp-lsp
```

`LOXPP_LSP` is `ON` by default, so a plain `cmake --build build` also builds
`loxpp-lsp`.

The tree-sitter parser:

```sh
cd editors/tree-sitter-loxpp && tree-sitter generate && cd -
editors/loxpp.nvim/scripts/build-parser.sh
```

`tree-sitter generate` refreshes `src/parser.c` from `grammar.js` (the
committed tree already has a generated `src/`). `build-parser.sh` compiles
`src/parser.c` into `editors/loxpp.nvim/parser/loxpp.so` with any C compiler,
so Neovim's built-in tree-sitter runtime finds it on `runtimepath`. With
`nvim-treesitter` (`master` branch) installed, `:TSInstall loxpp` after
`require("loxpp").setup()` is the alternative.

### Docker stages

`Dockerfile` has these stages:

- `dev` — the C++ toolchain. Builds `loxpp` and `loxpp-lsp`. **No Node.js, no
  tree-sitter CLI, no Neovim.**
- `dev-editors` — `dev` plus Node.js 22, `tree-sitter-cli` 0.25.10, Neovim
  0.11.3, and `nvim-treesitter` + `nvim-lint` checkouts under
  `/opt/nvim-plugins`. Used by the grammar and plugin CI jobs. The C++
  toolchain is inherited, so this one image builds every piece.
- `dev-managed` — `dev` plus the JVM and CLR toolchains. Not used by the
  editor tooling.

### CI jobs (`.github/workflows/ci.yml`)

| Job | Image | What it does |
|---|---|---|
| `tree-sitter grammar` | `dev-editors` | `tree-sitter generate`, `tree-sitter test`, then `tree-sitter parse` over `examples/*.lox`, `bootstrap/*.lox`, and `test/translation-probes/*.lox`; fails on any `ERROR` or `MISSING` node. |
| `loxpp-lsp language server` | `dev` | Builds `loxpp-lsp` under the ASan/UBSan `debug` preset, then runs `tools/lsp_smoke.py`. Also runs the smoke test with a good file passed as the bad file to prove the assertion can fail. |
| `Neovim plugin` | `dev-editors` | Diffs the plugin queries against the grammar queries; builds `loxpp` + `loxpp-lsp` + the parser; runs `tools/check_nvim_plugin.sh` headless in both normal and `--fallback` mode; proves the headless test fails with a broken `loxpp-lsp`. |

The `Build & Test` job runs the GTest suites, which include
`test_check_diagnostics`, `test_tooling_parser`, and `test_tooling_resolver`.

---

## Feature parity — Lox++ v1 vs established servers

`pyright` / `pylsp` for Python, `tsserver` for TypeScript,
`lua-language-server` for Lua.

| Capability | Lox++ v1 | pyright / pylsp | tsserver | lua-ls |
|---|:---:|:---:|:---:|:---:|
| Static diagnostics | yes (compiler, via `--check` / LSP) | yes | yes | yes |
| Syntax highlighting | yes (tree-sitter; regex fallback) | yes | yes | yes |
| Folding / indentation | yes (tree-sitter queries) | yes | yes | yes |
| Hover — stdlib, keywords, user decl | yes | yes | yes | yes |
| Completion — keywords, stdlib, in-scope symbols, snippets | yes | yes | yes | yes |
| Member completion after `.` | `math.` only | yes | yes | yes |
| Document symbols / outline | yes | yes | yes | yes |
| Go to definition (in-file) | yes | yes | yes | yes |
| Find references / document highlight (in-file) | yes | yes | yes | yes |
| Unused-local / unknown-name / static-rule warnings | yes (resolver; LSP only, not `--check`) | yes | yes | yes |
| Signature help | no — v2 (arity is already in `stdlib_docs`) | yes | yes | yes |
| Rename (in-file) | no — v2 (the reference sets already exist) | yes | yes | yes |
| Workspace symbols / cross-file navigation | no — waits on a module system | yes | yes | yes |
| Formatting | no — no `loxpp fmt` yet | yes | yes | yes |
| Semantic tokens | no — tree-sitter covers highlighting | yes | yes | yes |
| Code actions / quick fixes | no — v2 | yes | yes | yes |
| Type inference | no — Lox++ is dynamically typed | yes | yes | partial |

Corrections against the plan's table:

- **Member completion** is `math.` only. The plan listed completion as a
  plain "yes". Any other receiver gives nothing, because the resolver cannot
  know its type.
- **Warnings** reach the editor through the LSP only. `loxpp --check` returns
  compiler errors alone today.
- **Folding / indentation** is a tree-sitter feature. `loxpp-lsp` does not
  advertise `foldingRangeProvider`.
- **Hover** for Map and File methods works by a unique-name match, not by
  knowing the receiver type.

---

## v2 backlog

Each item is small on this base.

- **Signature help.** `signatureHelpProvider` plus a handler. Arity and the
  parameter list are already in `src/lsp/stdlib_docs.*`.
- **In-file rename.** `renameProvider` plus a handler. The reference sets
  from `references` already give the edit list.
- **Code actions from compiler errors.** `codeActionProvider`. Add the
  missing `match` arm that the exhaustiveness check names; wrap a `+` operand
  in `str()`.
- **Formatting.** Build a `loxpp fmt` formatter first, then
  `documentFormattingProvider` and `textDocument/formatting`.
- **Incremental document sync.** Move from `Full` to `Incremental`, so
  `analyze()` (a full recompile) does not run on the whole file for every
  keystroke.
- **Semantic tokens.** Only if the tree-sitter highlighting proves not
  enough for some client.
- **Cross-file navigation and workspace symbols.** Waits on a module system
  (`notes/expressiveness-roadmap.md`, not scheduled).
- **A TextMate grammar** (`editors/loxpp.tmbundle`) for VS Code and GitHub
  Linguist reach.
- **A VS Code extension.**
- **Split `editors/tree-sitter-loxpp` and `editors/loxpp.nvim` into their own
  repositories**, so `:TSInstall loxpp` and the plugin managers can fetch
  them directly. The Node and Rust bindings under
  `editors/tree-sitter-loxpp/bindings/` are the `tree-sitter init` scaffold
  and are not built in CI; a split would make them real.

---

## Follow-up bug candidates

Found during this work, not fixed (each is out of scope for a docs or
tooling change). None changes the tooling's behaviour on valid source.

- **`lox_keywords()` in `src/scanner.cpp` is missing `in`.** The array is
  called "the single source of truth" for the keyword list
  (`src/scanner.h`), but it lists 22 of the 23 keyword tokens. Effect: the
  REPL does not tab-complete `in`. Scanning is not affected —
  `Scanner::identifierType()` is a separate hand-written trie that
  recognises `in`.
- **A two-dot run `..` scans as one `DOT` token with a two-character
  lexeme.** `Scanner::match` advances on every match that succeeds and does
  not step back when the next one fails, so `case '.'` consumes both dots and
  then builds a `DOT` token. Effect: `o..x` runs the same as `o.x` instead
  of being a lexical error. `spec/01-lexical.md` now claims a meaning for
  three dots (`...` = `ELIPSIS`) only, and gives none to a two-dot run.
- **The tree-sitter grammar treats `default` as an ordinary identifier.**
  `default` is a reserved word in the scanner (`src/token.h`) but no grammar
  rule uses it, and tree-sitter prunes a `reserved` word that only a pruned
  rule references. A program that uses `default` as a name is a compile
  error in real Lox++; the grammar does not flag it. No corpus file is
  affected. `match`, `enum`, and `case` are reserved correctly.

The tooling parser also accepts a few constructs the real compiler rejects —
a slice on an assignment left side (`a[x:y] = v`), a misplaced `enum`. That
is deliberate: the tooling parser builds structure for navigation and leaves
rejection to `analyze()` and the resolver.
