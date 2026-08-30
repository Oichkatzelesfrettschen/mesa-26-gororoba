#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel-parser replay of the NoPerspective q-lane two-pass cell:
# pass 0 is the control varying cell and pass 1 the q-lane cell, both the
# eight-dword position-plus-TEX0 record at VAP_VTX_SIZE 8 (the q-lane
# cell differs from the control in its fragment program alone,
# r300_noperspective_q_lane_plan.h).  The kernel TCL-bypass vertex check
# (r300_tcl_bypass_vtx_check.h) sums the declared texture components into
# the required width, so both draws PASS; the same stream with
# VAP_VTX_SIZE rewritten to 4 REJECTs both draws, the negative control.
#
# R3V_KERNEL_REPLAY_TOOL names the replay binary built from the Linux
# radeon source tree.  An unset tool skips the test.
set -eu

if [ -z "${R3V_KERNEL_REPLAY_TOOL:-}" ]; then
    echo "R3V_KERNEL_REPLAY_TOOL unset; q-lane kernel replay not run" >&2
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

"${manifest_tool}" "${workdir}" --noperspective-q-lane >/dev/null
verdict=$("${R3V_KERNEL_REPLAY_TOOL}" "${workdir}/ib.bin")
echo "${verdict}"
case "${verdict}" in
    *"draws=2 pass=2 reject=0 decline=0"*) ;;
    *)
        echo "q-lane cell did not replay pass=2 reject=0 decline=0" >&2
        exit 1
        ;;
esac
narrowed=$("${R3V_KERNEL_REPLAY_TOOL}" --set-vtx-size 4 "${workdir}/ib.bin")
echo "${narrowed}"
case "${narrowed}" in
    *"draws=2 pass=0 reject=2 decline=0"*) ;;
    *)
        echo "q-lane cell at VTX_SIZE 4 did not replay pass=0 reject=2" >&2
        exit 1
        ;;
esac

echo "run_noperspective_q_lane_kernel_replay: control+q-lane PASS x2 at" \
     "VTX_SIZE 8, VTX_SIZE 4 REJECTs both draws"
