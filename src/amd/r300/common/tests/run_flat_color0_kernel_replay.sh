#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel-parser replay of the direct GA Flat two-pass cell.
#
# --flat-color0 emits both passes with varying=true and flat_color0=true,
# so each pass's vertex record carries no second FLOAT_4 -- the varying
# rides the TCL-bypass color 0 vector under
# r300_flat_color0_plan_direct_first, and VAP_OUTPUT_VTX_FMT_0 declares
# COLOR_0_PRESENT.  The kernel TCL-bypass vertex check
# (r300_tcl_bypass_vtx_check.h) reads only VAP_OUTPUT_VTX_FMT_0's
# position and texture-coordinate bits when it derives the expected
# vertex width, so a COLOR_0_PRESENT record widens the true PSC layout
# past what the check computes and every draw in the stream DECLINEs.
# --flat-replicate emits the same two-pass shape with varying=true alone,
# which routes the varying through the ordinary TEX0 record the check
# already covers, so both draws PASS.
#
# R3V_KERNEL_REPLAY_TOOL names the same replay binary
# run_offline_ib_replay.sh drives, built from the Linux radeon source
# tree (scripts/replay_r300_tcl_bypass_ib compiled against
# r300_tcl_bypass_vtx_check.h).  An unset tool skips the test, keeping
# the default build graph independent of sibling checkouts.
#
# Usage: run_flat_color0_kernel_replay.sh <r300_triangle_manifest-binary>

set -eu

if [ -z "${R3V_KERNEL_REPLAY_TOOL:-}" ]; then
    echo "R3V_KERNEL_REPLAY_TOOL unset; flat-color0 kernel replay not run" >&2
    exit 77
fi
if [ ! -x "${R3V_KERNEL_REPLAY_TOOL}" ]; then
    echo "replay tool ${R3V_KERNEL_REPLAY_TOOL} is not executable" >&2
    exit 1
fi
if [ "$#" -lt 1 ]; then
    echo "usage: $0 <r300_triangle_manifest-binary>" >&2
    exit 1
fi
manifest_tool="$1"
if [ ! -x "${manifest_tool}" ]; then
    echo "manifest tool ${manifest_tool} is not executable" >&2
    exit 1
fi

workdir=$(mktemp -d)
trap 'rm -rf "${workdir}"' EXIT

mkdir -p "${workdir}/direct" "${workdir}/replicate"

# The direct cell: the kernel check's vertex-width derivation misreads
# every COLOR_0_PRESENT record, so both draws in the stream DECLINE.
"${manifest_tool}" "${workdir}/direct" --flat-color0 >/dev/null

direct=$("${R3V_KERNEL_REPLAY_TOOL}" "${workdir}/direct/ib.bin")
echo "${direct}"
case "${direct}" in
    *"draws=2 pass=0 reject=0 decline=2"*) ;;
    *)
        echo "direct flat-color0 cell did not replay decline=2 pass=0" \
             "reject=0" >&2
        exit 1
        ;;
esac

# The per-draw reason token: the kernel check's decline names the
# register field it read past, fmt0_beyond_position -- print it so a
# future check on the exact wording has a captured baseline.
reasons=$("${R3V_KERNEL_REPLAY_TOOL}" --reasons "${workdir}/direct/ib.bin")
reason_line=$(printf '%s\n' "${reasons}" | grep -m1 '^reason ')
echo "direct flat-color0 draw reason: ${reason_line}"
case "${reason_line}" in
    *"fmt0_beyond_position"*) ;;
    *)
        echo "direct flat-color0 decline reason token did not carry" \
             "fmt0_beyond_position" >&2
        exit 1
        ;;
esac

# The replication control: the ordinary TEX0 varying record the check
# already covers, so both draws PASS.
"${manifest_tool}" "${workdir}/replicate" --flat-replicate >/dev/null

replicate=$("${R3V_KERNEL_REPLAY_TOOL}" "${workdir}/replicate/ib.bin")
echo "${replicate}"
case "${replicate}" in
    *"draws=2 pass=2 reject=0 decline=0"*) ;;
    *)
        echo "flat-replicate cell did not replay pass=2" >&2
        exit 1
        ;;
esac

echo "run_flat_color0_kernel_replay: direct cell DECLINE x2" \
     "(fmt0_beyond_position), replication cell PASS x2"
