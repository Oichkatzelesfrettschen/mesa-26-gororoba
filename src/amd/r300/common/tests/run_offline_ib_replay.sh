#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel-parser replay of the fixed TCL-bypass triangle cell.
#
# R3V_KERNEL_REPLAY_TOOL names the replay binary built from the Linux
# radeon source tree (scripts/replay_r300_tcl_bypass_ib compiled against
# r300_tcl_bypass_vtx_check.h); the harness walks the IB exactly as
# r300_cs_parse does, so its verdict is the parser's verdict for the same
# dwords.  An unset tool skips the test, keeping the default build graph
# independent of sibling checkouts.
#
# Usage: run_offline_ib_replay.sh <r300_triangle_manifest-binary>

set -eu

if [ -z "${R3V_KERNEL_REPLAY_TOOL:-}" ]; then
    echo "R3V_KERNEL_REPLAY_TOOL unset; offline kernel replay not run" >&2
    exit 77
fi
if [ ! -x "${R3V_KERNEL_REPLAY_TOOL}" ]; then
    echo "replay tool ${R3V_KERNEL_REPLAY_TOOL} is not executable" >&2
    exit 1
fi

manifest_tool="$1"
workdir=$(mktemp -d)
trap 'rm -rf "${workdir}"' EXIT

"${manifest_tool}" "${workdir}"

# Known-good: the golden cell passes with one draw and no rejection.
good=$("${R3V_KERNEL_REPLAY_TOOL}" "${workdir}/ib.bin")
echo "${good}"
case "${good}" in
    *"draws=1 pass=1 reject=0 decline=0"*) ;;
    *)
        echo "golden cell did not replay PASS" >&2
        exit 1
        ;;
esac

# Known-bad control: an undersized VAP_VTX_SIZE must REJECT.
bad=$("${R3V_KERNEL_REPLAY_TOOL}" --set-vtx-size 3 "${workdir}/ib.bin")
echo "${bad}"
case "${bad}" in
    *"reject=1"*) ;;
    *)
        echo "undersized VAP_VTX_SIZE control did not REJECT" >&2
        exit 1
        ;;
esac

# Known-bad control: a truncated stream must fail decode.
dd if="${workdir}/ib.bin" of="${workdir}/truncated.bin" bs=4 count=5 \
    2>/dev/null
if "${R3V_KERNEL_REPLAY_TOOL}" "${workdir}/truncated.bin" 2>/dev/null; then
    echo "truncated stream did not fail decode" >&2
    exit 1
fi

echo "run_offline_ib_replay: golden PASS, malformed controls held"
