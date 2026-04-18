#!/bin/sh
# info.sh -- print host + toolchain + profile overview.
# Useful sanity check before a build.

set -eu

HERE="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== host ==="
hostname -f 2>/dev/null || hostname
uname -a
echo
echo "=== cpu ==="
grep -m1 "model name" /proc/cpuinfo 2>/dev/null | sed "s/.*: //"
nproc
echo
echo "=== toolchain ==="
for c in gcc clang clang-22 clang++ clang++-22 meson ninja distcc ccache; do
    which "$c" >/dev/null 2>&1 && printf "%-12s %s\n" "$c" "$($c --version 2>&1 | head -1)"
done
echo
echo "=== build-infra profiles ==="
for f in "$HERE"/configs/*.meson; do
    p=$(basename "$f" .meson)
    grep -E "gallium-drivers|vulkan-drivers" "$f" | sed "s|^|$p: |"
done
