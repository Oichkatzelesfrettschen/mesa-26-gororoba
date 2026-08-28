#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel-parser replay of the rasterizer probe two-pass cells.
#
# --rs-tex-adj and --rs-w-select each emit pass 0 as the control varying
# cell and pass 1 as the same bytes under the named candidate, position
# plus the TEX0 vector in an eight-dword record.  The kernel TCL-bypass
# vertex check (r300_tcl_bypass_vtx_check.h) admits that tuple, and the
# candidate words (RS_INST_0.TEX_ADJ, GB_SELECT.W_SELECT) lie outside
# the width decision, so every draw in both streams PASSes.
#
# R3V_KERNEL_REPLAY_TOOL names the replay binary run_offline_ib_replay.sh
# drives, built from the Linux radeon source tree.  An unset tool skips
# the test, keeping the default build graph independent of sibling
# checkouts.
set -eu

if [ -z "${R3V_KERNEL_REPLAY_TOOL:-}" ]; then
    echo "R3V_KERNEL_REPLAY_TOOL unset; rs-tex-adj kernel replay not run" >&2
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

for candidate in rs-tex-adj rs-w-select; do
    mkdir -p "${workdir}/${candidate}"
    "${manifest_tool}" "${workdir}/${candidate}" "--${candidate}" >/dev/null
    verdict=$("${R3V_KERNEL_REPLAY_TOOL}" "${workdir}/${candidate}/ib.bin")
    echo "${verdict}"
    case "${verdict}" in
        *"draws=2 pass=2 reject=0 decline=0"*) ;;
        *)
            echo "${candidate} probe cell did not replay pass=2 reject=0" \
                 "decline=0" >&2
            exit 1
            ;;
    esac
done

echo "run_rs_tex_adj_probe_kernel_replay: control+TEX_ADJ PASS x2," \
     "control+W_SELECT PASS x2"
