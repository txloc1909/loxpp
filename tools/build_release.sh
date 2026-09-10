#!/bin/bash
set -euo pipefail

# Build a fully static release of Lox++ and loxpp-lsp.
# Reads the component manifest to determine which binaries to build, strip,
# smoke-test, and package. Only components marked shipped=true are laid out
# in dist/.

COMPONENTS_FILE="packaging/components.toml"
BUILD_DIR="build"
DIST_DIR="dist"
REPO_ROOT="."

# Honour SOURCE_DATE_EPOCH for reproducibility. When unset, use the current
# HEAD commit's committer date. Derive it safely so the script does not abort
# on git errors; instead print the value used.
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$REPO_ROOT" log -1 --format=%ct 2>/dev/null || date +%s)}"
export SOURCE_DATE_EPOCH
echo "Using SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"

# NOTE: SOURCE_DATE_EPOCH has no observable effect today (no timestamp is
# baked into the binary). Once D2 adds a version header, D4 must re-verify
# reproducibility with a stamped build.

# Parse components.toml without a TOML library. Return a list of records:
# "name|shipped|target1 target2 ..."
parse_manifest() {
  local name shipped targets
  while IFS= read -r line; do
    # Skip empty lines and comments
    [[ -z "$line" || "$line" =~ ^# ]] && continue

    # Start of a new component table
    if [[ "$line" =~ ^\[\[component\]\] ]]; then
      # Flush the previous component if any
      if [[ -n "${name:-}" ]]; then
        echo "$name|$shipped|$targets"
      fi
      name=""
      shipped=""
      targets=""
      continue
    fi

    # Parse name = "..."
    if [[ "$line" =~ ^name[[:space:]]*=[[:space:]]*\"([^\"]+)\" ]]; then
      name="${BASH_REMATCH[1]}"
      continue
    fi

    # Parse shipped = true|false
    if [[ "$line" =~ ^shipped[[:space:]]*=[[:space:]]*(true|false) ]]; then
      shipped="${BASH_REMATCH[1]}"
      continue
    fi

    # Parse targets = ["target1", "target2", ...]
    if [[ "$line" =~ ^targets[[:space:]]*=\[(.+)\] ]]; then
      local targets_str="${BASH_REMATCH[1]}"
      # Strip quotes and commas, split by whitespace
      targets=$(echo "$targets_str" | sed 's/"//g; s/,/ /g' | tr -s ' ')
      continue
    fi
  done < "$COMPONENTS_FILE"

  # Flush the last component
  if [[ -n "${name:-}" ]]; then
    echo "$name|$shipped|$targets"
  fi
}

# Build a list of all targets (build-set).
BUILD_SET=""
declare -A SHIP_MAP  # Maps component name to "shipped" status

while IFS='|' read -r name shipped target_list; do
  SHIP_MAP["$name"]="$shipped"
  for target in $target_list; do
    BUILD_SET="$BUILD_SET $target"
  done
done < <(parse_manifest)

BUILD_SET=$(echo "$BUILD_SET" | xargs)  # Trim and normalize whitespace
echo "Build-set: $BUILD_SET"

if [[ -z "$BUILD_SET" ]]; then
  echo "ERROR: No targets found in $COMPONENTS_FILE" >&2
  exit 1
fi

# Configure with the release-static preset.
echo "Configuring with the release-static preset..."
cmake -S . -B "$BUILD_DIR" --preset release-static

# Build every target in the build-set.
echo "Building all targets..."
for target in $BUILD_SET; do
  if ! cmake --build "$BUILD_DIR" --target "$target" 2>&1; then
    echo "ERROR: Failed to build target: $target" >&2
    exit 1
  fi
done

echo "All targets built successfully."

# Strip and split .debug sidecars for each binary.
echo "Stripping binaries and splitting debug symbols..."
for target in $BUILD_SET; do
  binary="$BUILD_DIR/$target"
  if [ ! -f "$binary" ]; then
    echo "ERROR: components.toml names target $target which CMake did not build" >&2
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

# Layout only shipped components to dist/. Read the manifest again to iterate
# only over shipped components.
while IFS='|' read -r name shipped target_list; do
  if [[ "$shipped" == "false" ]]; then
    echo "  (skipping $name, marked shipped=false)"
    continue
  fi

  mkdir -p "$DIST_DIR/$name"
  echo "Laying out $name component to $DIST_DIR/$name..."

  # Copy the primary binary and its debug sidecar.
  for target in $target_list; do
    binary="$BUILD_DIR/$target"
    debug="$binary.debug"

    cp "$binary" "$DIST_DIR/$name/"
    cp "$debug" "$DIST_DIR/$name/"

    echo "  installed $target and $target.debug"
  done

  # Copy support files.
  if [ -f LICENSE ]; then
    cp LICENSE "$DIST_DIR/$name/"
  fi
  if [ -f THIRD_PARTY.md ]; then
    cp THIRD_PARTY.md "$DIST_DIR/$name/"
  fi
  if [ -f third_party/isocline/LICENSE ]; then
    mkdir -p "$DIST_DIR/$name/third_party/isocline"
    cp third_party/isocline/LICENSE "$DIST_DIR/$name/third_party/isocline/"
  fi

  echo "  added LICENSE, THIRD_PARTY.md, and third_party/isocline/LICENSE"
done < <(parse_manifest)

echo ""
echo "Release build complete. Artifacts in $DIST_DIR/."
