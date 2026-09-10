# Releasing Lox++

This document describes how to cut and publish a release of Lox++ for x86_64
Linux.

## Prerequisites

No long-lived credentials needed. Publishing uses GitHub's automatic
`GITHUB_TOKEN` and keyless OIDC:

- You must be a maintainer on `github.com/txloc1909/loxpp` — not a fork. Only
  a maintainer can trigger the release workflow and its `id-token: write`
  permission.
- The release workflow handles all build, sign, attest, and publish steps. You
  only push a tag.

## Cut a release

1. Confirm `main` is green (all CI jobs pass).
2. Tag the commit: `git tag vX.Y.Z` — use semantic versioning (e.g. `v0.1.0`,
   `v0.2.0`).
3. Push the tag: `git push origin vX.Y.Z`.
4. Watch the `release.yml` workflow: `gh run watch --repo txloc1909/loxpp`.
   It runs in a few minutes.

The workflow builds the static binary, smoke-tests it, packages it with the
man page and completions, signs the checksums, attests the build provenance,
and publishes a GitHub Release with all assets attached.

## What the workflow produces

For each release, the workflow creates these assets:

| Asset | Note |
|---|---|
| `loxpp-X.Y.Z-x86_64-linux.tar.gz` | Stripped static binary, license files, manual page, completions, and examples |
| `loxpp-X.Y.Z-x86_64-linux.debug.tar.gz` | Debug symbols in a separate tarball |
| `SHA256SUMS` | Checksum for every asset in the release |
| `SHA256SUMS.cosign-bundle` | Keyless cosign signature of the checksums file |
| `loxpp-X.Y.Z.spdx.json` | Software bill of materials (SPDX format) |
| `install.sh` | The installer script, also available as an asset |

All assets are attached to the GitHub Release and also available at stable URLs:

- `https://github.com/txloc1909/loxpp/releases/latest/download/<asset>` — redirects to the newest stable release
- `https://github.com/txloc1909/loxpp/releases/download/vX.Y.Z/<asset>` — a pinned release

### Pre-release tags

A tag with a pre-release suffix (`-rc`, `-beta`, `-alpha`) creates a GitHub
pre-release. Pre-releases never become "latest" — `releases/latest/download/`
always points at the newest stable release, and `loxpp upgrade --check` ignores
pre-releases.

Example pre-release tags: `v0.1.0-rc1`, `v0.1.0-beta`, `v0.2.0-alpha`.

## Verify the release

Before announcing a release, verify it as an outside user would. Download the
assets and run these commands:

```bash
# Verify the checksum
sha256sum -c SHA256SUMS

# Verify the cosign signature (requires cosign installed)
cosign verify-blob \
  --bundle SHA256SUMS.cosign-bundle \
  --certificate-identity-regexp '^https://github\.com/txloc1909/loxpp/\.github/workflows/release\.yml@refs/tags/v.*$' \
  --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' \
  SHA256SUMS

# Verify build provenance (requires gh installed)
gh attestation verify loxpp-X.Y.Z-x86_64-linux.tar.gz --repo txloc1909/loxpp
```

Extract the tarball and test the binary:

```bash
tar -xzf loxpp-X.Y.Z-x86_64-linux.tar.gz
./loxpp-X.Y.Z-x86_64-linux/loxpp --version
./loxpp-X.Y.Z-x86_64-linux/loxpp examples/hello.lox
```

Also test the installer in a clean environment:

```bash
docker run -it --rm ubuntu:24.04 bash -c '
  apt-get update && apt-get install -y curl
  curl -fsSL https://github.com/txloc1909/loxpp/releases/download/vX.Y.Z/install.sh | sh
  loxpp --version
  loxpp examples/hello.lox  # if examples are installed
'
```

## Dry run

To test the release pipeline without publishing:

1. Push a pre-release tag (e.g. `git tag v0.0.0-rc1 && git push origin v0.0.0-rc1`).
2. Or use `workflow_dispatch`: `gh workflow run release.yml --repo txloc1909/loxpp -f ref=main -f dry_run=true`.

A dry run builds and uploads to a uniquely named draft release, leaving
`releases/latest` untouched.

## Rollback

If a release has a critical bug:

1. Delete the GitHub Release: `gh release delete vX.Y.Z --repo txloc1909/loxpp --yes --cleanup-tag`.
2. Remind users to pin a safe version: `install.sh --version <safe-version>`.

Users can roll back by re-running the install script with a specific version:

```bash
curl -fsSL https://raw.githubusercontent.com/txloc1909/loxpp/main/install.sh | sh -s -- --version 0.1.0
```

## Tag namespace

This mission ships only the `loxpp` interpreter. The tag namespace is reserved
for future components with their own release cadence:

| Tag prefix | Component | Notes |
|---|---|---|
| `v*` (e.g. `v0.1.0`) | `loxpp` interpreter | Semantic versioning. This release's namespace. |
| `lsp-v*` | `loxpp-lsp` (language server) | Reserved for the LSP, if released separately |
| `grammar-v*` | `tree-sitter-loxpp` (parser grammar) | Reserved for the grammar (versions independently) |
| `nvim-v*` | `loxpp.nvim` (Neovim plugin) | Reserved for the plugin, if released separately |

Each component tags on its own cadence. Today, only `v*` exists. Future
missions can ship editor tooling with their own tags (`lsp-v0.1.0`, etc.)
without collision.
