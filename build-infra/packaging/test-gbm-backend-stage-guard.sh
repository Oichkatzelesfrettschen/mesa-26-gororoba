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
    ;;
  debug)
    libdir="$destination${FAKE_PREFIX:?}/lib"
    ;;
  *) exit 1 ;;
esac

mkdir -p "$libdir"
touch "$libdir/libgbm.so.1" "$libdir/libGLX_mesa.so.0" "$libdir/libEGL_mesa.so.0"
if [ "${FAKE_GBM_BACKEND:-0}" = 1 ]; then
  mkdir -p "$libdir/gbm"
  touch "$libdir/gbm/dri_gbm.so"
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
  local builddir="$WORKDIR/debug-build-$label"
  local pkgdir="$WORKDIR/debug-$label"
  local prefix=/opt/mesa-gororoba-debug-optimized
  write_build_metadata "$builddir" "$prefix"

  (
    cd "$HERE/mesa-gororoba-debug-optimized"
    env PATH="$WORKDIR/bin:$PATH" \
        FAKE_STAGE_LAYOUT=debug \
        FAKE_GBM_BACKEND="$backend" \
        FAKE_PREFIX="$prefix" \
        GOROROBA_DEBUG_BUILDDIR="$builddir" \
        GOROROBA_DEBUG_SRCROOT="$WORKDIR/source" \
        pkgdir="$pkgdir" \
        srcdir="$PWD" \
        bash -c '. ./PKGBUILD; package'
  )
}

if run_release missing-backend 0; then
  echo "release package accepted a staged tree without dri_gbm.so" >&2
  exit 1
fi
run_release complete-stage 1

if run_debug missing-backend 0; then
  echo "debug package accepted a staged tree without dri_gbm.so" >&2
  exit 1
fi
run_debug complete-stage 1

echo "gbm backend stage-guard fixtures: PASS"
