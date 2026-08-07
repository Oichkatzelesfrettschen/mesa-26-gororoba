#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Full kernel CS-parser and tracker replay of the fixed cell.
#
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the Linux
# radeon source tree against that tree's r300 safe-register bitmap and
# vertex-width decision header, so its verdict is the CS parser's verdict for
# the same dwords and the same buffer objects.  R3V_CS_TRACK_CONTROLS names
# the control script beside it, which asserts the parser-invalid and
# semantically blank classes separately.  An unset tool skips the test, which
# keeps the default build graph independent of sibling checkouts.
#
# Usage: run_cs_track_replay.sh <r300_triangle_manifest-binary>

set -eu

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; CS-track replay not run" >&2
    exit 77
fi
if [ ! -x "${R3V_CS_TRACK_REPLAY_TOOL}" ]; then
    echo "replay tool ${R3V_CS_TRACK_REPLAY_TOOL} is not executable" >&2
    exit 1
fi

manifest_tool="$1"
workdir=$(mktemp -d)
trap 'rm -rf "${workdir}"' EXIT

"${manifest_tool}" "${workdir}" >/dev/null

# The relocation chunk in entry order, with each buffer object's role, byte
# size, and domains.  The sizes are the ones the transport allocates for the
# cell: three FLOAT_4 vertices in one page, and a 64-pixel-pitch ARGB8888
# target with the canary row the output oracle reads.
cat > "${workdir}/bundle.txt" <<'BUNDLE'
family rs480
bo 0 role=vertex size=4096 read_domains=0x2 write_domain=0x0
bo 1 role=color size=65536 read_domains=0x0 write_domain=0x2
BUNDLE

"${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" "${workdir}/ib.bin"

if [ -n "${R3V_CS_TRACK_CONTROLS:-}" ]; then
    if [ ! -x "${R3V_CS_TRACK_CONTROLS}" ]; then
        echo "control script ${R3V_CS_TRACK_CONTROLS} is not executable" >&2
        exit 1
    fi
    "${R3V_CS_TRACK_CONTROLS}" "${R3V_CS_TRACK_REPLAY_TOOL}" \
        "${workdir}/bundle.txt" "${workdir}/ib.bin"
else
    echo "R3V_CS_TRACK_CONTROLS unset; only the known-good leg ran" >&2
fi

echo "run_cs_track_replay: the retained cell parses and tracks clean"
