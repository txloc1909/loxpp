#!/bin/sh
# install.sh - install or upgrade loxpp, the Lox++ interpreter.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/txloc1909/loxpp/main/install.sh | sh
#   sh install.sh [--version X.Y.Z] [--bin-dir DIR] [--dry-run] [--quiet] [--no-modify-path]
#
# The same script does the first install and every later upgrade. Run it
# again with no --version to move to the newest release; with --version to
# pin or downgrade. The "loxpp upgrade" subcommand runs this exact script,
# so the download and verify logic lives in one place only.
#
# POSIX sh. Works with the BusyBox tools in Alpine (sha256sum, sed, grep,
# wget) and with GNU coreutils in a normal distribution.

set -eu

# --- constants -----------------------------------------------------------

REPO="txloc1909/loxpp"
RELEASES="https://github.com/${REPO}/releases"
TARGET="x86_64-linux"

# Components to install. One entry today. A later component (for example
# loxpp-lsp) is one more word here - the fetch_verify_install function and
# the loop below do not change.
COMPONENTS="loxpp"

# cosign keyless verification parameters. They must match the signing
# identity in .github/workflows/release.yml (the workflow file ref) and the
# GitHub Actions OIDC issuer.
COSIGN_IDENTITY_REGEXP='^https://github\.com/txloc1909/loxpp/\.github/workflows/release\.yml@refs/tags/v.*$'
COSIGN_ISSUER='https://token.actions.githubusercontent.com'

# --- options -----------------------------------------------------------

VERSION="${LOXPP_VERSION:-}"
BIN_DIR="${LOXPP_INSTALL_DIR:-${HOME:-}/.local/bin}"
DRY_RUN=0
QUIET=0
NO_MODIFY_PATH=0

# Set by resolve_version / resolve_paths and read by the install steps.
BASE_URL=""
INSTALL_PATH=""
INSTALL_DIR=""

# Temp working directory and an in-progress binary temp file. The trap
# removes both on any exit path.
WORK=""
TMP_BIN=""

# --- helpers -----------------------------------------------------------

say() {
    [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"
}

err() {
    printf 'install.sh: error: %s\n' "$*" >&2
}

# A follow-on line under an error (a hint or a command to copy). No prefix.
hint() {
    printf '%s\n' "$*" >&2
}

die() {
    err "$*"
    exit 1
}

print_help() {
    cat <<'EOF'
install.sh - install or upgrade loxpp (the Lox++ interpreter)

Usage:
  sh install.sh [options]

Options:
  --version X.Y.Z    Install this exact version (pin or downgrade).
  --bin-dir DIR      Install the binary here (default: $HOME/.local/bin).
  --dry-run          Print the steps. Change nothing.
  --quiet            Print errors only.
  --no-modify-path   Do not edit a shell profile for PATH.
  -h, --help         Print this help.

Environment:
  LOXPP_VERSION      Same as --version.
  LOXPP_INSTALL_DIR  Same as --bin-dir.
EOF
}

cleanup() {
    [ -n "$WORK" ] && [ -d "$WORK" ] && rm -rf "$WORK"
    [ -n "$TMP_BIN" ] && [ -f "$TMP_BIN" ] && rm -f "$TMP_BIN"
    return 0
}

# --- argument parsing -------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        --version)
            [ $# -ge 2 ] || die "--version needs a value"
            VERSION="$2"
            shift
            ;;
        --version=*) VERSION="${1#*=}" ;;
        --bin-dir)
            [ $# -ge 2 ] || die "--bin-dir needs a value"
            BIN_DIR="$2"
            shift
            ;;
        --bin-dir=*) BIN_DIR="${1#*=}" ;;
        --dry-run) DRY_RUN=1 ;;
        --quiet) QUIET=1 ;;
        --no-modify-path) NO_MODIFY_PATH=1 ;;
        -h|--help)
            print_help
            exit 0
            ;;
        *)
            err "unknown option: $1"
            print_help >&2
            exit 2
            ;;
    esac
    shift
done

# A leading "v" on the version is a common mistake. Accept it.
VERSION="${VERSION#v}"

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

# --- interactivity ---------------------------------------------------

# A "curl ... | sh" run has no controlling terminal and its stdin is the
# pipe. In that case every prompt is skipped and PATH changes are printed,
# not written.
INTERACTIVE=0
if [ -t 0 ] && [ -t 1 ]; then
    INTERACTIVE=1
fi

# --- platform gate -------------------------------------------------

OS="$(uname -s 2>/dev/null || echo unknown)"
ARCH="$(uname -m 2>/dev/null || echo unknown)"

[ "$OS" = "Linux" ] || die "loxpp ships for Linux only (this system: ${OS})."
case "$ARCH" in
    x86_64|amd64) ;;
    *) die "loxpp ships for x86_64 only (this system: ${ARCH})." ;;
esac

# --- fetch backend ----------------------------------------------

# fetch <url> <dest-file>   - download to a file
# fetch_out <url>           - download to stdout
if command -v curl >/dev/null 2>&1; then
    # -f fails on HTTP >= 400 (a bare curl prints the 404 body and exits 0).
    # -sS is quiet but keeps error messages. -L follows redirects.
    fetch() { curl -fsSL -o "$2" "$1"; }
    fetch_out() { curl -fsSL "$1"; }
elif command -v wget >/dev/null 2>&1; then
    # BusyBox wget understands -q and -O; it has no long options.
    fetch() { wget -q -O "$2" "$1"; }
    fetch_out() { wget -q -O - "$1"; }
else
    err "need curl or wget. Install one, then run this script again."
    hint "manual download:"
    hint "  ${RELEASES}/latest/download/loxpp-<version>-${TARGET}.tar.gz"
    exit 1
fi

# --- temp working directory ------------------------------------

WORK="$(mktemp -d "${TMPDIR:-/tmp}/loxpp-install.XXXXXX" 2>/dev/null || true)"
if [ -z "$WORK" ] || [ ! -d "$WORK" ]; then
    WORK="${TMPDIR:-/tmp}/loxpp-install.$$"
    mkdir -p "$WORK" || die "cannot create a temp directory under ${TMPDIR:-/tmp}"
fi

# --- version resolve ----------------------------------------------

resolve_version() {
    if [ -n "$VERSION" ]; then
        BASE_URL="${RELEASES}/download/v${VERSION}"
        return 0
    fi

    # Latest: read SHA256SUMS from the "latest" redirect and take the
    # version out of an asset name. github.com (not api.github.com), so no
    # API rate limit. GitHub redirects "latest" only to a release that is
    # not marked pre-release.
    say "Resolving the latest release..."
    _sums="$(fetch_out "${RELEASES}/latest/download/SHA256SUMS")" || {
        err "cannot read ${RELEASES}/latest/download/SHA256SUMS"
        err "there may be no published release yet, or the network is down."
        exit 1
    }
    VERSION="$(printf '%s\n' "$_sums" \
        | sed -n "s/.*loxpp-\\([0-9][0-9A-Za-z.-]*\\)-${TARGET}\\.tar\\.gz.*/\\1/p" \
        | head -1)"
    [ -n "$VERSION" ] || die "cannot find a loxpp asset in the latest SHA256SUMS."
    BASE_URL="${RELEASES}/download/v${VERSION}"
}

# --- path resolve ----------------------------------------------

# Follow a symlink at $BIN_DIR/loxpp to its real target, so an upgrade
# writes the file the symlink points at, not the link. The atomic rename
# then happens inside the real directory (one filesystem).
resolve_paths() {
    _dest="${BIN_DIR}/loxpp"
    if [ -L "$_dest" ] && command -v readlink >/dev/null 2>&1; then
        _real="$(readlink -f "$_dest" 2>/dev/null || true)"
        [ -n "$_real" ] && _dest="$_real"
    fi
    INSTALL_PATH="$_dest"
    INSTALL_DIR="$(dirname "$_dest")"
}

# --- currently installed version ---------------------------------

installed_version() {
    _bin=""
    if [ -x "${BIN_DIR}/loxpp" ]; then
        _bin="${BIN_DIR}/loxpp"
    elif command -v loxpp >/dev/null 2>&1; then
        _bin="$(command -v loxpp)"
    fi
    [ -n "$_bin" ] || return 0
    # Line 1 of --version is: loxpp <version>
    "$_bin" --version 2>/dev/null | head -1 | sed -n 's/^loxpp \([^ ]*\).*/\1/p'
}

# --- bin dir writability (before any download) ------------------

ensure_bindir() {
    if [ ! -d "$INSTALL_DIR" ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
            say "[dry-run] would create ${INSTALL_DIR}"
            return 0
        fi
        mkdir -p "$INSTALL_DIR" 2>/dev/null || die \
            "cannot create ${INSTALL_DIR}. Set LOXPP_INSTALL_DIR to a writable path, or use sudo."
    fi
    if [ "$DRY_RUN" -eq 0 ] && [ ! -w "$INSTALL_DIR" ]; then
        err "install directory is not writable: ${INSTALL_DIR}"
        err "set LOXPP_INSTALL_DIR to a writable directory, or run this script with sudo."
        exit 1
    fi
}

# --- signature verify ------------------------------------------

verify_signature() {
    if command -v cosign >/dev/null 2>&1; then
        say "Verifying the cosign signature of SHA256SUMS..."
        (
            cd "$WORK" || exit 1
            cosign verify-blob \
                --bundle SHA256SUMS.cosign-bundle \
                --certificate-identity-regexp "$COSIGN_IDENTITY_REGEXP" \
                --certificate-oidc-issuer "$COSIGN_ISSUER" \
                SHA256SUMS
        ) >/dev/null 2>&1 || die \
            "cosign could not verify SHA256SUMS. Nothing was installed."
        say "Signature OK."
        return 0
    fi

    say "cosign is not installed; skipped the signature check."
    say "To verify the signature later:"
    say "  cosign verify-blob \\"
    say "    --bundle SHA256SUMS.cosign-bundle \\"
    say "    --certificate-identity-regexp '${COSIGN_IDENTITY_REGEXP}' \\"
    say "    --certificate-oidc-issuer '${COSIGN_ISSUER}' \\"
    say "    SHA256SUMS"
    say "To check the build provenance later:"
    say "  gh attestation verify loxpp-${VERSION}-${TARGET}.tar.gz --repo ${REPO}"
}

# --- completions + man page ----------------------------------

# place_file <src> <dir> <name>  - copy one support file, best effort. Never
# fatal: a read-only completion directory must not fail the whole install.
# Distinct variable names because a caller (place_aux) keeps its own state
# across calls and POSIX sh has no function-local scope.
place_file() {
    _pf_src="$1"
    _pf_dir="$2"
    _pf_name="$3"
    [ -f "$_pf_src" ] || return 0
    if mkdir -p "$_pf_dir" 2>/dev/null && [ -w "$_pf_dir" ]; then
        cp "$_pf_src" "${_pf_dir}/${_pf_name}"
        say "Installed ${_pf_dir}/${_pf_name}"
    else
        say "Skipped ${_pf_name}: ${_pf_dir} is not writable."
    fi
}

place_aux() {
    _pa_dir="$1"
    _pa_comp="$2"
    [ "$_pa_comp" = "loxpp" ] || return 0

    _pa_data="${XDG_DATA_HOME:-${HOME:-}/.local/share}"
    place_file "${_pa_dir}/loxpp.1" "${HOME:-}/.local/share/man/man1" "loxpp.1"
    place_file "${_pa_dir}/loxpp.bash" \
        "${_pa_data}/bash-completion/completions" "loxpp"
    place_file "${_pa_dir}/loxpp.zsh" "${_pa_data}/zsh/site-functions" "_loxpp"
    place_file "${_pa_dir}/loxpp.fish" \
        "${_pa_data}/fish/vendor_completions.d" "loxpp.fish"
}

# --- one component: download, verify, place -----------------

fetch_verify_install() {
    _comp="$1"
    _tarball="${_comp}-${VERSION}-${TARGET}.tar.gz"
    _url="${BASE_URL}/${_tarball}"

    if [ "$DRY_RUN" -eq 1 ]; then
        say "[dry-run] would download ${_url}"
        say "[dry-run] would download ${BASE_URL}/SHA256SUMS"
        say "[dry-run] would download ${BASE_URL}/SHA256SUMS.cosign-bundle"
        say "[dry-run] would check the SHA256 of ${_tarball} on disk"
        say "[dry-run] would check the cosign signature (when cosign is present)"
        say "[dry-run] would install ${_comp} to ${INSTALL_PATH}"
        return 0
    fi

    say "Downloading ${_tarball}..."
    fetch "$_url" "${WORK}/${_tarball}" || die \
        "download failed: ${_url} (check the version number and the network)."
    fetch "${BASE_URL}/SHA256SUMS" "${WORK}/SHA256SUMS" || die \
        "download failed: ${BASE_URL}/SHA256SUMS"
    fetch "${BASE_URL}/SHA256SUMS.cosign-bundle" "${WORK}/SHA256SUMS.cosign-bundle" || die \
        "download failed: ${BASE_URL}/SHA256SUMS.cosign-bundle"

    # Check the hash of the file on disk. A truncated download can still
    # have a plausible size, so the on-disk hash is the real gate. Filter
    # SHA256SUMS to the one asset we fetched.
    say "Checking the SHA256..."
    (
        cd "$WORK" || exit 1
        grep " ${_tarball}$" SHA256SUMS > SHA256SUMS.one \
            || { echo "no ${_tarball} line in SHA256SUMS" >&2; exit 1; }
        sha256sum -c SHA256SUMS.one
    ) >/dev/null 2>&1 || die \
        "SHA256 check failed for ${_tarball}. The download is corrupt or changed. Nothing was installed."
    say "SHA256 OK."

    verify_signature

    say "Installing ${_comp}..."
    ( cd "$WORK" && tar -xzf "$_tarball" )
    _extracted="${WORK}/${_comp}-${VERSION}-${TARGET}"
    [ -f "${_extracted}/${_comp}" ] || die \
        "the tarball has no ${_comp} binary."

    # Write into the target directory, then rename over the old binary. The
    # rename is on one filesystem, so it is atomic; the running process
    # keeps its open mapping and finishes normally. A temp file in /tmp
    # could be on another filesystem and the move would not be atomic.
    TMP_BIN="${INSTALL_DIR}/.loxpp.new.$$"
    cp "${_extracted}/${_comp}" "$TMP_BIN" || die \
        "cannot write to ${INSTALL_DIR}."
    chmod 0755 "$TMP_BIN"
    mv -f "$TMP_BIN" "$INSTALL_PATH"
    TMP_BIN=""
    say "Installed ${INSTALL_PATH}"

    place_aux "$_extracted" "$_comp"
}

# --- PATH handling -------------------------------------------

# The $PATH in these two must stay literal: the text is written verbatim
# into a shell rc file, where the shell expands it at login time.
# shellcheck disable=SC2016
fish_line() {
    printf 'set -gx PATH %s $PATH' "$1"
}

# shellcheck disable=SC2016
posix_line() {
    printf 'export PATH="%s:$PATH"' "$1"
}

rc_file() {
    _shell="$(basename "${SHELL:-}" 2>/dev/null || echo sh)"
    case "$_shell" in
        zsh) printf '%s\n' "${ZDOTDIR:-${HOME:-}}/.zshrc" ;;
        bash)
            if [ -f "${HOME:-}/.bashrc" ]; then
                printf '%s\n' "${HOME:-}/.bashrc"
            else
                printf '%s\n' "${HOME:-}/.bash_profile"
            fi
            ;;
        fish) printf '%s\n' "${HOME:-}/.config/fish/config.fish" ;;
        *) printf '%s\n' "${HOME:-}/.profile" ;;
    esac
}

handle_path() {
    case ":${PATH}:" in
        *":${BIN_DIR}:"*) return 0 ;;
    esac

    _rc="$(rc_file)"
    case "$_rc" in
        *config.fish) _line="$(fish_line "$BIN_DIR")" ;;
        *) _line="$(posix_line "$BIN_DIR")" ;;
    esac

    if [ "$DRY_RUN" -eq 1 ]; then
        say "[dry-run] ${BIN_DIR} is not on PATH; would add to ${_rc}:"
        say "  ${_line}"
        return 0
    fi

    if [ "$NO_MODIFY_PATH" -eq 1 ] || [ "$INTERACTIVE" -eq 0 ]; then
        say ""
        say "${BIN_DIR} is not on your PATH. Add this line to your shell profile:"
        say "  ${_line}"
        return 0
    fi

    if [ -f "$_rc" ] && grep -qF "$_line" "$_rc" 2>/dev/null; then
        say "${BIN_DIR} is already in ${_rc}. Restart your shell, or run:"
        say "  ${_line}"
        return 0
    fi

    if [ -n "$_rc" ] && mkdir -p "$(dirname "$_rc")" 2>/dev/null; then
        printf '\n# Added by the loxpp installer\n%s\n' "$_line" >> "$_rc"
        say "Added ${BIN_DIR} to PATH in ${_rc}."
        say "Restart your shell, or run:  ${_line}"
    else
        say "${BIN_DIR} is not on your PATH. Add this line to your shell profile:"
        say "  ${_line}"
    fi
}

# --- main flow ----------------------------------------------

resolve_version
resolve_paths

CURRENT="$(installed_version || true)"
if [ -n "$CURRENT" ] && [ "$CURRENT" = "$VERSION" ]; then
    say "loxpp ${VERSION} is already installed"
    exit 0
fi

if [ "$DRY_RUN" -eq 1 ]; then
    if [ -n "$CURRENT" ]; then
        say "[dry-run] loxpp ${CURRENT} -> ${VERSION}"
    else
        say "[dry-run] loxpp ${VERSION} (fresh install)"
    fi
else
    if [ -n "$CURRENT" ]; then
        say "loxpp ${CURRENT} -> ${VERSION}"
    else
        say "loxpp ${VERSION}"
    fi
fi

ensure_bindir

for _c in $COMPONENTS; do
    fetch_verify_install "$_c"
done

handle_path

if [ "$DRY_RUN" -eq 1 ]; then
    say "[dry-run] done. Nothing changed."
else
    say "Done. Run:  loxpp --version"
fi
