#!/bin/sh
# Copyright (c) 2026 Terascale Functionalists
# SPDX-License-Identifier: MIT

set -eu

usage() {
   cat <<'EOF'
Usage:
  run_r300vk_4096_validation.sh OUT_DIR

Required environment:
  VK_ICD_FILENAMES  path to the r300vk ICD JSON under test

Optional environment:
  CC                C compiler used for the standalone probe
  PKG_CONFIG        pkg-config command
  SUDO              sudo command used for optional dmesg capture
EOF
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
   usage
   exit 0
fi

if [ "$#" -ne 1 ]; then
   usage >&2
   exit 2
fi

if [ -z "${VK_ICD_FILENAMES:-}" ]; then
   echo "VK_ICD_FILENAMES must point at the r300vk ICD JSON under test" >&2
   exit 2
fi

out_dir=$1
repo_root=$(git rev-parse --show-toplevel)
probe_src=$repo_root/src/amd/r300/vulkan/r300vk/tests/r300vk_4096_image_probe.c
cc_bin=${CC:-cc}
pkg_config=${PKG_CONFIG:-pkg-config}
sudo_bin=${SUDO:-sudo}

mkdir -p "$out_dir"

probe_bin=$out_dir/r300vk_4096_image_probe
compile_log=$out_dir/compile.log
run_log=$out_dir/probe.jsonl
dmesg_log=$out_dir/dmesg-filtered-tail.txt
summary=$out_dir/summary.txt

"$cc_bin" -Wall -Wextra -Werror "$probe_src" \
   $("${pkg_config}" --cflags --libs vulkan) \
   -o "$probe_bin" >"$compile_log" 2>&1

R300VK_HYBRID_COMPUTE_EXPERIMENTAL=${R300VK_HYBRID_COMPUTE_EXPERIMENTAL:-1} \
   "$probe_bin" >"$run_log" 2>&1

if command -v "$sudo_bin" >/dev/null 2>&1; then
   "$sudo_bin" -n dmesg 2>/dev/null |
      grep -Ei 'r300|r600|radeon|drm|cs|gpu|ring|lockup|fault|vm' |
      tail -80 >"$dmesg_log" || :
else
   : >"$dmesg_log"
fi

{
   echo "probe=$probe_bin"
   echo "icd=$VK_ICD_FILENAMES"
   echo "run_log=$run_log"
   echo "dmesg_filtered_lines=$(wc -l < "$dmesg_log")"
   if grep -q '"status":"fail"' "$run_log"; then
      echo "status=fail"
   else
      echo "status=pass"
   fi
} >"$summary"

cat "$summary"
! grep -q '"status":"fail"' "$run_log"
