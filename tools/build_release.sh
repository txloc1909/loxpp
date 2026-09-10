#!/bin/sh
set -eu

# Build a fully static release of Lox++ and loxpp-lsp.
# Reads the component manifest to determine which binaries to build, strip,
# smoke-test, and package. Only components marked shipped=true are laid out
# in dist/.

# Honour SOURCE_DATE_EPOCH for reproducibility. When unset, use the current
# HEAD commit's committer date.
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
  SOURCE_DATE_EPOCH=$(git log -1 --format=%ct HEAD)
  export SOURCE_DATE_EPOCH
fi

readonly COMPONENTS_FILE="packaging/components.toml"
readonly BUILD_DIR="build"
readonly DIST_DIR="dist"

# For now, hardcode the targets since parsing TOML in sh is fragile.
# loxpp and loxpp-lsp both need to be in the build-set.
readonly ALL_TARGETS="loxpp loxpp-lsp"

# Configure with the release-static preset.
echo "Configuring with the release-static preset..."
cmake -S . -B "$BUILD_DIR" --preset release-static

# Build every target.
echo "Building all targets..."
for target in $ALL_TARGETS; do
  if ! cmake --build "$BUILD_DIR" --target "$target"; then
    echo "ERROR: Failed to build target: $target"
    exit 1
  fi
done

echo "All targets built successfully."

# Strip and split .debug sidecars for each binary.
echo "Stripping binaries and splitting debug symbols..."
for target in $ALL_TARGETS; do
  binary="$BUILD_DIR/$target"
  if [ ! -f "$binary" ]; then
    echo "ERROR: Target $target did not produce a binary at $binary"
    exit 1
  fi

  # Split debug symbols.
  objcopy --only-keep-debug "$binary" "$binary.debug"
  objcopy --strip-all "$binary"
  objcopy --add-gnu-debuglink="$binary.debug" "$binary"

  echo "  $target: stripped, debug split to $target.debug"
done

# Smoke-test loxpp.
echo "Running smoke tests..."
if [ -f "$BUILD_DIR/loxpp" ]; then
  echo "  Running check_examples.py against loxpp..."
  python3 tools/check_examples.py "$BUILD_DIR/loxpp" examples/
  echo "  OK: check_examples.py passed"
fi

# Smoke-test loxpp-lsp if the smoke test script exists.
if [ -f "$BUILD_DIR/loxpp-lsp" ] && [ -f "tools/lsp_smoke.py" ]; then
  echo "  Running lsp_smoke.py against loxpp-lsp..."
  python3 tools/lsp_smoke.py "$BUILD_DIR/loxpp-lsp"
  echo "  OK: lsp_smoke.py passed"
fi

# Clean and recreate dist/.
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR/loxpp"

# Layout loxpp component with support files.
echo "Laying out loxpp component to $DIST_DIR/loxpp..."
for target in loxpp; do
  binary="$BUILD_DIR/$target"
  debug="$binary.debug"

  cp "$binary" "$DIST_DIR/loxpp/"
  cp "$debug" "$DIST_DIR/loxpp/"

  echo "  installed $target and $target.debug"
done

# Copy support files.
cp LICENSE "$DIST_DIR/loxpp/"
if [ -f THIRD_PARTY.md ]; then
  cp THIRD_PARTY.md "$DIST_DIR/loxpp/"
fi
if [ -f third_party/isocline/LICENSE ]; then
  cp third_party/isocline/LICENSE "$DIST_DIR/loxpp/LICENSE.isocline"
fi

echo "  added LICENSE and THIRD_PARTY.md"

echo ""
echo "Release build complete. Artifacts in $DIST_DIR/."
