#!/usr/bin/env sh
# Verify that x86_64-only system packages retain the stock 32-bit ICD.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd -P)

assert_metadata() {
  package_dir=$1
  metadata=$(cd "$package_dir" && makepkg --printsrcinfo)

  if printf '%s\n' "$metadata" | awk -F ' = ' \
       '{ field = $1; sub(/^\t/, "", field) }
        field == "conflicts" || field == "replaces" { print $2 }' | \
       grep -Fx 'lib32-vulkan-radeon' >/dev/null; then
    echo "x86_64 package claims lib32-vulkan-radeon: $package_dir" >&2
    exit 1
  fi

  for field in conflicts replaces; do
    if ! printf '%s\n' "$metadata" | awk -F ' = ' -v field="$field" \
         '{ name = $1; sub(/^\t/, "", name) }
          name == field { print $2 }' | grep -Fx 'vulkan-radeon' >/dev/null; then
      echo "x86_64 package no longer replaces vulkan-radeon: $package_dir" >&2
      exit 1
    fi
  done
}

assert_metadata "$HERE/mesa-gororoba"
assert_metadata "$HERE/mesa-gororoba-debug-optimized"

echo "radeon multilib ownership metadata: PASS"
