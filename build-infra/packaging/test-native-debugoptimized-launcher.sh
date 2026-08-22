#!/usr/bin/env sh
# SPDX-License-Identifier: MIT
# The CPU-only fixture exercises the packaged native launcher's refusal and
# successful environment-selection paths with a synthetic readable manifest.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
WORKDIR=$(mktemp -d "${TMPDIR:-/var/tmp}/native-debugoptimized-launcher-test.XXXXXX")
launcher="$HERE/mesa-gororoba-debug-optimized/mesa-gororoba-debug-optimized-native-run"
alternate_prefix="$WORKDIR/alternate-prefix"
manifest="$alternate_prefix/share/vulkan/icd.d/r3v_icd.x86_64.json"
env_file="$WORKDIR/alternate-env.sh"

cleanup() {
  case "$WORKDIR" in
    "${TMPDIR:-/var/tmp}"/native-debugoptimized-launcher-test.*) rm -rf -- "$WORKDIR" ;;
    *) echo "refusing to remove unexpected test directory: $WORKDIR" >&2; exit 1 ;;
  esac
}
trap cleanup EXIT

mkdir -p "$(dirname -- "$manifest")"
printf '%s\n' '{}' > "$manifest"
cat > "$env_file" <<'EOF'
GOROROBA_MESA_PREFIX=${NATIVE_LAUNCHER_PREFIX:?}
export GOROROBA_MESA_PREFIX
export LD_LIBRARY_PATH="${GOROROBA_MESA_PREFIX}/lib"
EOF

if NATIVE_LAUNCHER_PREFIX="$WORKDIR/missing-prefix" \
  GOROROBA_MESA_ENV="$env_file" "$launcher" true > "$WORKDIR/missing.out" 2>&1; then
  echo "expected missing native manifest to fail" >&2
  exit 1
fi
grep -Fqx \
  "native r3v ICD manifest is unavailable: $WORKDIR/missing-prefix/share/vulkan/icd.d/r3v_icd.x86_64.json" \
  "$WORKDIR/missing.out"

NATIVE_LAUNCHER_PREFIX="$alternate_prefix" \
  GOROROBA_MESA_ENV="$env_file" "$launcher" env > "$WORKDIR/success.env"
grep -Fqx "GOROROBA_MESA_PREFIX=$alternate_prefix" "$WORKDIR/success.env"
grep -Fqx "LD_LIBRARY_PATH=$alternate_prefix/lib" "$WORKDIR/success.env"
grep -Fqx "VK_DRIVER_FILES=$manifest" "$WORKDIR/success.env"
grep -Fqx "VK_ICD_FILENAMES=$manifest" "$WORKDIR/success.env"

echo "native debugoptimized launcher fixtures: PASS"
