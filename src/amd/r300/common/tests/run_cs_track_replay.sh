#!/bin/sh
# Copyright 2026 Mesa3D authors
# SPDX-License-Identifier: MIT
#
# Full kernel CS-parser and tracker replay of the fixed cell.
#
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the Linux
# radeon source tree against that tree's r300 safe-register bitmap and
# vertex-width decision header.  R3V_CS_TRACK_CONTROLS names the control
# script beside it, which asserts the parser-invalid and semantically blank
# classes separately.  The clean verdict requires both: a replay whose
# negative controls did not run certifies only that the tool accepts what
# the tool accepts, so a missing control script skips the test rather than
# passing it.  An unset variable is an absent configuration and skips; a set
# variable naming something unusable is a broken configuration and fails.
#
# Usage: run_cs_track_replay.sh <r300_triangle_manifest-binary>

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <r300_triangle_manifest-binary>" >&2
    exit 1
fi

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; CS-track replay not run" >&2
    exit 77
fi
if [ -z "${R3V_CS_TRACK_CONTROLS:-}" ]; then
    echo "R3V_CS_TRACK_CONTROLS unset; the replay does not run without" \
         "its negative controls" >&2
    exit 77
fi
if [ ! -x "${R3V_CS_TRACK_REPLAY_TOOL}" ]; then
    echo "replay tool ${R3V_CS_TRACK_REPLAY_TOOL} is not executable" >&2
    exit 1
fi
if [ ! -x "${R3V_CS_TRACK_CONTROLS}" ]; then
    echo "control script ${R3V_CS_TRACK_CONTROLS} is not executable" >&2
    exit 1
fi

manifest_tool="$1"
if [ ! -x "${manifest_tool}" ]; then
    echo "manifest tool ${manifest_tool} is not executable" >&2
    exit 1
fi

workdir=$(mktemp -d)
trap 'rm -rf "${workdir}"' EXIT

if ! "${manifest_tool}" "${workdir}" >/dev/null; then
    echo "manifest tool failed" >&2
    exit 1
fi
if [ ! -s "${workdir}/ib.bin" ]; then
    echo "manifest wrote no ib.bin" >&2
    exit 1
fi

# The relocation chunk in entry order, with each buffer object's role, byte
# size, and domains.  The sizes are the ones the attended transport allocates
# for the cell: three FLOAT_4 vertices in one page, and a 64-pixel-pitch
# ARGB8888 target with the canary row the output oracle reads.
color_bytes=$((64 * 65 * 4))
cat > "${workdir}/bundle.txt" <<BUNDLE
family rs480
bo 0 role=vertex size=4096 read_domains=0x2 write_domain=0x0
bo 1 role=color size=${color_bytes} read_domains=0x0 write_domain=0x2
BUNDLE
if [ ! -s "${workdir}/bundle.txt" ]; then
    echo "bundle write failed" >&2
    exit 1
fi

"${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" "${workdir}/ib.bin"

"${R3V_CS_TRACK_CONTROLS}" "${R3V_CS_TRACK_REPLAY_TOOL}" \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"

echo "run_cs_track_replay: the retained cell parses and tracks clean," \
     "and every negative control held"
