#!/usr/bin/env sh
# Fixture tests for the repackaged Meson configuration metadata gates.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd -P)
WORKDIR=$(mktemp -d "${TMPDIR:-/var/tmp}/repack-config-gates-test.XXXXXX")

cleanup() {
  case "$WORKDIR" in
    "${TMPDIR:-/var/tmp}"/repack-config-gates-test.*) rm -rf -- "$WORKDIR" ;;
    *) echo "refusing to remove unexpected test directory: $WORKDIR" >&2; exit 1 ;;
  esac
}
trap cleanup EXIT

gate_source() {
  awk '
    /^import json$/ { copy = 1 }
    copy && /^PYGATE$/ { exit }
    copy { print }
  ' "$1"
}

run_gate() {
  package="$1"
  builddir="$2"
  gate_source "$package" | python3 - "$builddir" /opt/repack-fixture
}

expect_failure() {
  package="$1"
  builddir="$2"
  label="$3"
  if run_gate "$package" "$builddir" > "$WORKDIR/$label.out" 2>&1; then
    echo "expected $label to fail for $package" >&2
    exit 1
  fi
  grep -Fq 'reconfigure via the build-infra Makefile' "$WORKDIR/$label.out"
}

write_options() {
  builddir="$1"
  prefix="$2"
  mkdir -p "$builddir/meson-info"
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

for package in \
  "$HERE/mesa-gororoba-debug-optimized/PKGBUILD" \
  "$HERE/mesa-gororoba-debug-o0/PKGBUILD" \
  "$HERE/mesa-gororoba-debug-asan/PKGBUILD"; do
  label=$(basename "$(dirname "$package")")
  missing="$WORKDIR/$label-missing"
  expect_failure "$package" "$missing" "$label-missing"

  malformed="$WORKDIR/$label-malformed"
  mkdir -p "$malformed/meson-info"
  printf '{\n' > "$malformed/meson-info/intro-buildoptions.json"
  expect_failure "$package" "$malformed" "$label-malformed"

  structural="$WORKDIR/$label-structural"
  mkdir -p "$structural/meson-info"
  printf '[{"name": "prefix"}]\n' > "$structural/meson-info/intro-buildoptions.json"
  expect_failure "$package" "$structural" "$label-structural"

  mismatch="$WORKDIR/$label-mismatch"
  write_options "$mismatch" /opt/wrong-prefix
  expect_failure "$package" "$mismatch" "$label-mismatch"

  valid="$WORKDIR/$label-valid"
  write_options "$valid" /opt/repack-fixture
  run_gate "$package" "$valid"
done

echo "repack config-gate fixtures: PASS"
