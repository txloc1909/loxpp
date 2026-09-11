# Distribution design

Lox++ ships as a single static binary for x86_64 Linux: download, verify,
install, upgrade. This document records the design decisions that make that
channel smooth.

## Why static linking with musl

A statically linked binary needs no system libraries, no package manager, no
distro-specific build. It works on any x86_64 Linux kernel. We chose musl libc
because:

- musl is MIT licensed and statically links readily, with no GPL code reaching
  the binary.
- The binary is smaller and has no runtime libc dependency.
- A clean Alpine container makes the build reproducible and auditable.

The dev environment stays on glibc (Ubuntu) because:

- The Dart SDK used by the test runner is glibc-only.
- The JVM and CLR benchmarks on musl would shift the backend performance
  baseline the project tracks.
- ASan and UBSan are less mature on musl.

The `Dockerfile` reconciles both with separate stages:

- `dev` — Ubuntu, glibc, full toolchain (clang, GTest, Dart, Node, JVM/CLR)
- `release-static` — Alpine, musl, minimal (clang, build tools only)

Both stages keep the same clang major version so a compiler bump moves them
together.

## Why isocline, not GNU Readline

The REPL needed line editing: history, completion, up-arrow navigation. GNU
Readline is GPL-licensed and would force the shipped binary to be GPL. We
replaced it with isocline (MIT, by Daan Leijen):

- **Single translation unit:** `isocline.c` includes all its headers. One call
  to `clang` compiles it.
- **Self-contained:** raw `termios`, no ncurses or termcap dependency.
- **MIT licensed:** vendoring it keeps the binary MIT.
- **Full history and completion:** same REPL experience, one library.

Isocline is vendored in `third_party/isocline/`, copied **verbatim** from the
pinned upstream release (`v1.1.0`). No edits, no reformatting. Every per-file
copyright header is kept. The provenance record — upstream URL, tag, archive
SHA-256, fetch date, files vendored, directories dropped — is in
`third_party/isocline/README.md`.

## Why cosign keyless, not a long-lived GPG key

Release signatures prove the binary is built by the CI system, not corrupted in
transit. We use cosign keyless signing through GitHub Actions OIDC:

- **No key to manage:** the signature is tied to the GitHub Actions workflow,
  not a private key on a machine.
- **Sigstore transparency log:** signatures are logged to Rekor, making them
  auditable. A compromised timestamp cannot alter past proofs.
- **One maintainer:** a long-lived GPG key adds friction for a solo maintainer.

The workflow signs the `SHA256SUMS` file, not each asset. Users verify once
and check all assets with `sha256sum -c`.

Build provenance is also attested by `actions/attest-build-provenance`,
creating a separate cryptographic proof of the build inputs and outputs.

## Why standalone-only (no Homebrew, AUR, distro packages)

Lox++ ships only via GitHub Releases + `install.sh`. No Homebrew, AUR, `.deb`,
`.rpm`, or package-manager formulas. Reasons:

- **One channel means smooth upgrades:** `loxpp upgrade` reruns `install.sh`,
  which verifies checksums and replaces the binary atomically. One code path,
  tested.
- **No maintenance burden per formula:** Homebrew formulas, AUR PKGBUILDs, and
  distro maintainers each need updating, testing, SSH keys, and tap repos. For
  a solo maintainer, that is substantial work.
- **Stable URLs, no auth:** GitHub releases have built-in stable URLs with no
  API rate limit. No token needed to download.
- **Idempotent, atomic upgrade:** `install.sh` is smart about atomicity
  (rename within the same directory, resolving symlinks first). A package
  manager's post-install hooks cannot match that.

If Lox++ grows a large ecosystem or many users on corporate Linux distributions
with tight package policies, distro packaging becomes worth reconsidering. For
now, the one channel is smooth, and all effort goes there.

## Component manifest and build-set vs ship-set

The component manifest (`packaging/components.toml`) is the single source of
truth for what gets built and what gets shipped:

```toml
[[component]]
name    = "loxpp"
targets = ["loxpp"]
shipped = true

[[component]]
name    = "loxpp-lsp"
targets = ["loxpp-lsp"]
shipped = false
```

### Build-set

The `release-static` build compiles and smoke-tests **every binary** the CMake
tree produces:

- `loxpp` — the interpreter (shipped)
- `loxpp-lsp` — the language server (not shipped yet, but built and tested)

This means a later mission that ships the LSP starts from a known-good state:
no surprise link errors, no musl-specific issues hiding until release.

### Ship-set

The packaging step iterates the manifest and packages only components with
`shipped = true`. Today, that is only `loxpp`. The tarball contains:

- `loxpp` — the stripped static binary
- `LICENSE` and `THIRD_PARTY.md` — licensing
- `third_party/isocline/LICENSE` — vendor license
- `loxpp.1` — manual page
- Bash, Zsh, Fish completions
- `examples/` — sample programs

### Adding a component later

To ship a new component (e.g. the language server in a follow-up mission):

1. Add a `[[component]]` entry to `packaging/components.toml` with `shipped = true`.
2. If the component versions on its own cadence (e.g. the grammar follows
   language versions, not interpreter versions), use its own tag prefix
   (`grammar-v*`, `lsp-v*`, etc.) from the reserved namespace.
3. Add the component to `install.sh`'s `COMPONENTS` list and the per-component
   TODO sites that script flags.
4. Give it a CPack `COMPONENT` if it installs via CMake (e.g.
   `install(...COMPONENT lsp)`).

Everything else — packaging, signing, attestation, upload — reads the manifest
and needs no workflow changes.

## Asset naming and stable URLs

Every asset follows the naming scheme: `<component>-<version>-<target>.<ext>`

- `component` — from the manifest (e.g. `loxpp`, `loxpp-lsp`, `tree-sitter-loxpp`)
- `version` — the tag without the leading `v` (e.g. tag `v0.1.0` becomes `0.1.0`)
- `target` — `x86_64-linux` (only Linux supported today)
- `ext` — tarball (`.tar.gz`), debug sidecar (`.debug.tar.gz`), checksums (`SHA256SUMS`), etc.

One `SHA256SUMS` file per release covers **all** assets. No per-asset signature.

Stable download URLs require no auth and no API rate limit:

- `https://github.com/txloc1909/loxpp/releases/latest/download/<asset>` —
  redirects to the newest **non-prerelease** release.
- `https://github.com/txloc1909/loxpp/releases/download/vX.Y.Z/<asset>` —
  a pinned tag (even pre-releases).

Both URLs are used by `install.sh` and `loxpp upgrade --check` to download and
verify releases without GitHub API calls.

## Install and upgrade

The `install.sh` script is both the first-install and upgrade tool:

```bash
# First install
curl -fsSL https://raw.githubusercontent.com/txloc1909/loxpp/main/install.sh | sh

# Upgrade (same script, idempotent)
curl -fsSL https://raw.githubusercontent.com/txloc1909/loxpp/main/install.sh | sh

# Pin a version
install.sh --version 0.1.0

# Check if an update is available
loxpp upgrade --check
```

Key properties:

- **Idempotent:** re-running does nothing if the version is already current.
- **Atomic:** writes the new binary to a temp file, verifies the checksum, then
  `rename(2)`s it over the old one (all in the same directory). The running
  process keeps its memory mapping while the on-disk file is replaced.
- **Verified:** SHA256 checksum always; cosign signature if `cosign` is on PATH.
- **PATH-aware:** appends `~/.local/bin` to the shell's `PATH` if needed, with
  an option to opt out.

The `loxpp upgrade` subcommand is a thin wrapper: it fetches `install.sh` and
runs it with the same logic. No TLS stack in the binary — all download and
verify code lives in the script.

## "When would loxpp need a version manager?"

A runtime version manager (nvm, pyenv, rbenv, rustup toolchains, uv's Python
management) earns its place only when **all** of these hold:

1. Breaking changes ship often enough that a program written for an older
   `loxpp` fails on a newer one.
2. Projects or packages declare a required `loxpp` version (a manifest with
   something like `loxpp = ">=0.5,<0.6"`).
3. Users routinely have two or more such projects with conflicting requirements
   on one machine.
4. Switching version by "reinstall" is too disruptive for that workflow.

`loxpp` today meets none of these:

- One implementation, no module system, no package ecosystem.
- No third-party code pinning versions.
- Self-contained scripts run as-is.

Until a package ecosystem exists, "use the latest" is correct, and a version
manager is pure overhead.

### What covers the real near-term needs instead

- **`install.sh --version X.Y.Z`** — reproduce a bug on an old release, pin a
  CI job.
- **Side-by-side installs** — a future `install.sh --version 0.3.0 --slot 0.3`
  writing `loxpp-0.3` beside `loxpp`. ~10 lines, add on request.
- **A `.loxpp-version` shim** — a wrapper that walks up from the working
  directory for a version file and execs the matching binary. ~100 lines,
  only if demand appears.

### The modern lesson

If `loxpp` ever gets a real package manager with a lockfile (like Go modules or
Cargo), fold version selection into **that** tool. Do not ship a standalone
`loxenv` next to a standalone package manager.

### Trigger to revisit

The first bug report of the form "my program ran on `loxpp` 0.N and breaks on
0.M" that coincides with a second project needing 0.N.

## Deferred work

Not complex, but not urgent, or waiting on a decision. Each is a standalone
GitHub issue, not a list here:

- `ghcr` runtime image (`FROM scratch` + the static binary): near-trivial
  now the binary is static; deferred until there is demand for container
  distribution — https://github.com/txloc1909/loxpp/issues/202
- Nix flake, worth doing once the release machinery has users to validate
  against — https://github.com/txloc1909/loxpp/issues/203
- Background "new version available" nudge, on top of the
  `loxpp upgrade --check` that already exists —
  https://github.com/txloc1909/loxpp/issues/204
- Side-by-side slotted installs (`install.sh --version X --slot X`) —
  https://github.com/txloc1909/loxpp/issues/205

## Follow-up mission: editor-tooling distribution

A follow-up mission ships `loxpp-lsp`, `tree-sitter-loxpp`, and
`loxpp.nvim` through this mission's release machinery — the component
manifest, the build-set / ship-set split, the tag namespace (`lsp-v*`,
`grammar-v*`, `nvim-v*`), and CPack components all slot them in with no
workflow rework.

Tracked as https://github.com/txloc1909/loxpp/issues/201, with its
credential and repository-location prerequisites tracked as their own
linked issues.

---

See [RELEASING.md](../RELEASING.md) for the release process and [README.md](../README.md)
for installation and upgrade instructions.
