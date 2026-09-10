# Lox++

[![CI](https://github.com/txloc1909/loxpp/actions/workflows/ci.yml/badge.svg)](https://github.com/txloc1909/loxpp/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)

A bytecode-compiled virtual machine for the Lox++ scripting language, written
in C++20.

The `++` is both a nod to the implementation language and a tribute to
[Lox](https://craftinginterpreters.com). Lox++ is its own language, shaped by
the same philosophy but taken further with pattern matching, algebraic types,
destructuring, collections, file I/O, and a self-hosted bootstrap interpreter.

---

## Features

| Feature | What it gives you |
|---|---|
| `enum` + `match` | Algebraic data types with exhaustive pattern matching |
| Destructuring | Unpack class fields (`var {x, y} = pt`) and sequences (`var [a, b] = lst`) |
| Lists & Maps | First-class dynamic collections with slice syntax (`lst[1:3]`) |
| Closures + GC | Lexical closures with mark-and-sweep garbage collection |
| `for-in` / `break` / `continue` | Iterator protocol over any sequence or map |
| File I/O | `open()`, `.read()`, `.write()`, `.close()` |
| Math library | `math.sqrt`, `math.pow`, `math.floor`, `math.pi`, … |
| Self-hosted | A complete Lox++ interpreter [written in Lox++](bootstrap/loxpp_interpreter.lox) |

---

## Quick demo

```lox
// Algebraic types with pattern matching
enum Result { Ok(value)  Err(msg) }

fun safe_div(a, b) {
    if (b == 0) return Err("division by zero");
    return Ok(a / b);
}

// Closures as first-class values
fun makeMultiplier(factor) {
    fun mult(x) { return x * factor; }
    return mult;
}

var double = makeMultiplier(2);

// Lists, for-in, and built-ins
var nums = [1, 2, 3, 4, 5, 6];
var evens = [];
for (n in nums) {
    if (n % 2 == 0) evens.append(double(n));
}

// Pattern match on enum variants
for (n in evens) {
    print match safe_div(60, n) {
        case Ok(v)  => str(60) + " / " + str(n) + " = " + str(v)
        case Err(m) => "error: " + m
    };
}
// 60 / 4 = 15
// 60 / 8 = 7.5
// 60 / 12 = 5
```

More programs in [`examples/`](examples/) — from graph traversal and Huffman
coding to a [self-hosted interpreter](bootstrap/loxpp_interpreter.lox).

---

## Install

The Lox++ binary is a single statically linked file for x86_64 Linux. No
system dependencies, no shared libraries.

### Install

```bash
curl -fsSL https://raw.githubusercontent.com/txloc1909/loxpp/main/install.sh | sh
```

You can also pin a specific version:

```bash
curl -fsSL https://raw.githubusercontent.com/txloc1909/loxpp/main/install.sh | sh -s -- --version 0.1.0
```

The script downloads to `~/.local/bin/` by default. If you have `~/.local/bin`
on your `PATH`, you can now run `loxpp`. The script verifies the SHA256 checksum
always, and the cosign signature if `cosign` is on your `PATH`.

### Upgrade

Re-run the install command (it is idempotent — re-running does not reinstall if
you are already current):

```bash
curl -fsSL https://raw.githubusercontent.com/txloc1909/loxpp/main/install.sh | sh
```

Or use the `loxpp upgrade` subcommand to fetch and run the script:

```bash
loxpp upgrade
```

Check if an update is available without installing:

```bash
loxpp upgrade --check
```

### Verify (optional, for the cautious)

Verify the SHA256 checksum and cosign signature of the latest release:

```bash
# Download the assets
curl -fsSL https://github.com/txloc1909/loxpp/releases/latest/download/SHA256SUMS -O
curl -fsSL https://github.com/txloc1909/loxpp/releases/latest/download/SHA256SUMS.cosign-bundle -O
curl -fsSL https://github.com/txloc1909/loxpp/releases/latest/download/loxpp-0.1.0-x86_64-linux.tar.gz -O

# Verify the checksum
sha256sum -c SHA256SUMS

# Verify the cosign signature (requires cosign installed)
cosign verify-blob \
  --bundle SHA256SUMS.cosign-bundle \
  --certificate-identity-regexp '^https://github\.com/txloc1909/loxpp/\.github/workflows/release\.yml@refs/tags/v.*$' \
  --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' \
  SHA256SUMS

# Verify build provenance (requires gh installed)
gh attestation verify loxpp-0.1.0-x86_64-linux.tar.gz --repo txloc1909/loxpp
```

### Uninstall

Remove these paths:

- `~/.local/bin/loxpp` — the binary
- `~/.local/share/man/man1/loxpp.1` — the manual page
- `~/.config/bash/completion.d/loxpp` — bash completions (if present)
- `~/.config/zsh/completion.d/_loxpp` — zsh completions (if present)
- `~/.config/fish/completions/loxpp.fish` — fish completions (if present)
- `~/.cache/loxpp/` — the REPL history cache

---

## Language tour

| Resource | Contents |
|---|---|
| [`spec/`](spec/) | Full language specification — lexical rules, grammar, type system, semantics, stdlib |
| [`examples/`](examples/) | 40+ runnable programs: algorithms, OOP, pattern matching, file I/O |
| [`bootstrap/loxpp_interpreter.lox`](bootstrap/loxpp_interpreter.lox) | A complete Lox++ interpreter written in Lox++ itself |

---

## Editor support

Lox++ ships a first editor-tooling stack in [`editors/`](editors/):

| Piece | Gives you | Needs |
|---|---|---|
| [`tree-sitter-loxpp`](editors/tree-sitter-loxpp/) | scope-aware highlighting, folds, indents | a compiled parser (or a Vim regex fallback) |
| `loxpp-lsp` | diagnostics, hover, completion, document symbols, go-to-definition, references (all within one file) | the `loxpp-lsp` binary |
| `loxpp --check` | the compiler's static errors, as text or JSON | the `loxpp` binary; a fallback for editors without the server |
| [`loxpp.nvim`](editors/loxpp.nvim/) | all of the above wired for Neovim, plus filetype detection and buffer options for plain Vim | Neovim (a Vim regex + quickfix path works without the binaries) |

Build the two binaries:

```sh
cmake --preset debug -DLOXPP_LSP=ON
cmake --build build --target loxpp loxpp-lsp
```

Build the tree-sitter parser:

```sh
cd editors/tree-sitter-loxpp && tree-sitter generate && cd -
editors/loxpp.nvim/scripts/build-parser.sh
```

Install the Neovim plugin: see [`editors/loxpp.nvim/README.md`](editors/loxpp.nvim/README.md).

Design, the `loxpp --check` contract, the LSP capability set, and the
feature-parity table: [`notes/editor-tooling.md`](notes/editor-tooling.md).

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for setup, workflow, and conventions.
AI agents: see [AGENTS.md](AGENTS.md) for the complete task loop.

---

## Third-party code

Lox++ vendors a small set of third-party libraries. Each is copied **verbatim**
from the upstream source and listed in [`THIRD_PARTY.md`](THIRD_PARTY.md) with
its copyright holder, license, and the location of the license text. See that
file for details.

---

## License

[MIT](LICENSE) — Copyright 2025 Tran Xuan Loc
