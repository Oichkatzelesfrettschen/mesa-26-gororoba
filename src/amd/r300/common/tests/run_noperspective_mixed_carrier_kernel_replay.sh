#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel-parser replay of the mixed Smooth/NoPerspective carrier
# two-pass cell: pass 0 is the control varying cell at VAP_VTX_SIZE 8 and
# pass 1 the sixteen-dword three-vector cell at VAP_VTX_SIZE 16
# (r300_noperspective_mixed_carrier_plan.h).  The kernel TCL-bypass vertex
# check (r300_tcl_bypass_vtx_check.h) sums the declared texture components
# into the required width, so both draws PASS; the same stream with every
# VAP_VTX_SIZE rewritten to 12 REJECTs the mixed draw (three vectors
# declared over a twelve-dword fetch) and PASSes the control draw, whose
# eight-dword requirement twelve covers, the negative control the width
# check itself decides.
#
# R3V_KERNEL_REPLAY_TOOL names the replay binary built from the Linux
# radeon source tree.  An unset tool skips the test.
set -eu

if [ -z "${R3V_KERNEL_REPLAY_TOOL:-}" ]; then
    echo "R3V_KERNEL_REPLAY_TOOL unset; mixed-carrier kernel replay not run" >&2
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

"${manifest_tool}" "${workdir}" --noperspective-mixed-carrier >/dev/null
verdict=$("${R3V_KERNEL_REPLAY_TOOL}" "${workdir}/ib.bin")
echo "${verdict}"
case "${verdict}" in
    *"draws=2 pass=2 reject=0 decline=0"*) ;;
    *)
        echo "mixed-carrier cell did not replay pass=2 reject=0 decline=0" >&2
        exit 1
        ;;
esac
narrowed=$("${R3V_KERNEL_REPLAY_TOOL}" --set-vtx-size 12 "${workdir}/ib.bin")
echo "${narrowed}"
case "${narrowed}" in
    *"draws=2 pass=1 reject=1 decline=0"*) ;;
    *)
        echo "mixed-carrier cell at VTX_SIZE 12 did not replay pass=1 reject=1" >&2
        exit 1
        ;;
esac

echo "run_noperspective_mixed_carrier_kernel_replay: control PASS at 8," \
     "mixed carrier PASS at 16, VTX_SIZE 12 REJECTs the mixed draw"
