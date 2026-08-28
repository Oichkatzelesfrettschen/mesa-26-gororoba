#!/usr/bin/env bash
# Exercise the release and debug package guards with fake Meson staging.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd -P)"
WORKDIR="$(mktemp -d "${TMPDIR:-/var/tmp}/gbm-stage-guard-test.XXXXXX")"

cleanup() {
  case "$WORKDIR" in
    "${TMPDIR:-/var/tmp}"/gbm-stage-guard-test.*) rm -rf -- "$WORKDIR" ;;
    *) echo "refusing to remove unexpected test directory: $WORKDIR" >&2; exit 1 ;;
  esac
}
trap cleanup EXIT

mkdir -p "$WORKDIR/bin"
cat > "$WORKDIR/bin/meson" <<'EOF'
#!/usr/bin/env sh
set -eu

destination=
while [ "$#" -gt 0 ]; do
  if [ "$1" = "--destdir" ]; then
    destination=$2
    shift 2
  else
    shift
  fi
done
[ -n "$destination" ] || exit 1

case "${FAKE_STAGE_LAYOUT:?}" in
  release)
    libdir="$destination/usr/lib"
    icd_dir="$destination/usr/share/vulkan/icd.d"
    ;;
  debug)
    libdir="$destination${FAKE_PREFIX:?}/lib"
    ;;
  *) exit 1 ;;
esac

mkdir -p "$libdir"
touch "$libdir/libgbm.so.1" "$libdir/libGLX_mesa.so.0" "$libdir/libEGL_mesa.so.0"
if [ "${FAKE_STAGE_LAYOUT}" = release ]; then
  mkdir -p "$icd_dir"
  touch "$icd_dir/r3v_icd.x86_64.json"
fi
if [ "${FAKE_GBM_BACKEND:-0}" = 1 ]; then
  mkdir -p "$libdir/gbm"
  touch "$libdir/gbm/dri_gbm.so"
fi

if [ "${FAKE_STAGE_LAYOUT}" = debug ]; then
  implicit_dir="$destination${FAKE_PREFIX:?}/share/vulkan/implicit_layer.d"
  mkdir -p "$implicit_dir"
  if [ "${FAKE_ANTI_LAG_LIBRARY:-0}" = 1 ]; then
    touch "$libdir/libVkLayer_MESA_anti_lag.so"
  fi
  if [ "${FAKE_ANTI_LAG_MANIFEST:-0}" = 1 ]; then
    touch "$implicit_dir/VkLayer_MESA_anti_lag.json"
  fi
  if [ "${FAKE_DEVICE_SELECT_LIBRARY:-0}" = 1 ]; then
    touch "$libdir/libVkLayer_MESA_device_select.so"
  fi
  if [ "${FAKE_DEVICE_SELECT_MANIFEST:-0}" = 1 ]; then
    touch "$implicit_dir/VkLayer_MESA_device_select.json"
  fi
fi
EOF
chmod +x "$WORKDIR/bin/meson"

write_build_metadata() {
  local builddir="$1"
  local prefix="$2"
  mkdir -p "$builddir/meson-info"
  : > "$builddir/build.ninja"
  python3 - "$builddir/meson-info/intro-buildoptions.json" "$prefix" <<'PYTHON'
import json
import sys
from pathlib import Path

Path(sys.argv[1]).write_text(json.dumps([
    {"name": "prefix", "value": sys.argv[2]},
    {"name": "sysconfdir", "value": "/etc"},
    {"name": "vulkan-layers", "value": ["anti-lag", "device-select", "overlay"]},
]) + "\n", encoding="utf-8")
PYTHON
}

run_release() {
  local label="$1"
  local backend="$2"
  local pkgdir="$WORKDIR/release-$label"

  (
    cd "$HERE/mesa-gororoba"
    env PATH="$WORKDIR/bin:$PATH" \
        FAKE_STAGE_LAYOUT=release \
        FAKE_GBM_BACKEND="$backend" \
        pkgdir="$pkgdir" \
        srcdir="$PWD" \
        bash -c '. ./PKGBUILD; package'
  )
}

run_debug() {
  local label="$1"
  local backend="$2"
  local anti_lag_library="$3"
  local anti_lag_manifest="$4"
  local device_select_library="$5"
  local device_select_manifest="$6"
  local builddir="$WORKDIR/debug-build-$label"
  local pkgdir="$WORKDIR/debug-$label"
  local prefix=/opt/mesa-gororoba-debug-optimized
  write_build_metadata "$builddir" "$prefix"

  (
    cd "$HERE/mesa-gororoba-debug-optimized"
    env PATH="$WORKDIR/bin:$PATH" \
        FAKE_STAGE_LAYOUT=debug \
        FAKE_GBM_BACKEND="$backend" \
        FAKE_ANTI_LAG_LIBRARY="$anti_lag_library" \
        FAKE_ANTI_LAG_MANIFEST="$anti_lag_manifest" \
        FAKE_DEVICE_SELECT_LIBRARY="$device_select_library" \
        FAKE_DEVICE_SELECT_MANIFEST="$device_select_manifest" \
        FAKE_PREFIX="$prefix" \
        MESA_DEBUG_BUILDDIR="$builddir" \
        MESA_DEBUG_SRCROOT="$WORKDIR/source" \
        pkgdir="$pkgdir" \
        srcdir="$PWD" \
        bash -c '. ./PKGBUILD; package'
  )
}

expect_release_failure() {
  local label="$1"
  local expected_diagnostic="$2"
  local backend="$3"
  local output="$WORKDIR/$label.out"

  if run_release "$label" "$backend" > "$output" 2>&1; then
    echo "release package accepted $label" >&2
    exit 1
  fi
  if ! grep -Fq "$expected_diagnostic" "$output"; then
    echo "release package did not report $label: $expected_diagnostic" >&2
    cat "$output" >&2
    exit 1
  fi
}

expect_debug_failure() {
  local label="$1"
  local expected_diagnostic="$2"
  shift 2
  local output="$WORKDIR/$label.out"

  if run_debug "$label" "$@" > "$output" 2>&1; then
    echo "debug package accepted $label" >&2
    exit 1
  fi
  if ! grep -Fq "$expected_diagnostic" "$output"; then
    echo "debug package did not report $label: $expected_diagnostic" >&2
    cat "$output" >&2
    exit 1
  fi
}

expect_release_failure missing-backend \
  "/usr/lib/gbm/dri_gbm.so" 0
run_release complete-stage 1 > "$WORKDIR/release-complete-stage.out" 2>&1

expect_debug_failure missing-backend \
  "/opt/mesa-gororoba-debug-optimized/lib/gbm/dri_gbm.so" \
  0 1 1 1 1
expect_debug_failure missing-anti-lag-library \
  "/opt/mesa-gororoba-debug-optimized/lib/libVkLayer_MESA_anti_lag.so" \
  1 0 1 1 1
expect_debug_failure missing-anti-lag-manifest \
  "/opt/mesa-gororoba-debug-optimized/share/vulkan/implicit_layer.d/VkLayer_MESA_anti_lag.json" \
  1 1 0 1 1
expect_debug_failure missing-device-select-library \
  "/opt/mesa-gororoba-debug-optimized/lib/libVkLayer_MESA_device_select.so" \
  1 1 1 0 1
expect_debug_failure missing-device-select-manifest \
  "/opt/mesa-gororoba-debug-optimized/share/vulkan/implicit_layer.d/VkLayer_MESA_device_select.json" \
  1 1 1 1 0
run_debug complete-stage 1 1 1 1 1 > "$WORKDIR/debug-complete-stage.out" 2>&1

echo "gbm backend stage-guard fixtures: PASS"
