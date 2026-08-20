#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Full kernel CS-parser and tracker replay of the depth control cell.
#
# The cell binds a depth surface, so ZB_DEPTHOFFSET carries a relocation
# and ZB_CNTL's Z_ENABLE arms r300_cs_track_check's depth-buffer size
# test: pitch * cpp * maxy + offset against the depth object's size.  The
# replay is what proves the kernel admits that binding before any boot
# carries it.
#
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the Linux
# radeon source tree against that tree's r300 safe-register bitmap and
# vertex-width decision header.  An unset variable is an absent
# configuration and skips; a set variable naming something unusable is a
# broken configuration and fails.
#
# The controls run here rather than through run_r300_cs_track_controls.sh
# beside the tool: that script's buffer-bound arms carry the fixed triangle
# cell's three-vertex array, which this six-vertex draw exceeds, and it
# has no depth arm at all.  A replay whose negative controls did not run
# certifies only that the tool accepts what the tool accepts, so the
# calibration lives with the cell whose geometry it names.
#
# Usage: run_zb_depth_control_replay.sh <r300_zb_depth_control_manifest>

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <r300_zb_depth_control_manifest-binary>" >&2
    exit 1
fi

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; CS-track replay not run" >&2
    exit 77
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
# size, and domains.  The sizes are the ones the attended transport
# allocates: six FLOAT_4 vertices in one page, a 64-pixel-pitch ARGB8888
# target, and a 64-pixel-pitch 16-bit depth surface, each with the canary
# row its oracle reads.  The depth object carries both domains because the
# host fills it before the draw and the device writes it during.
color_bytes=$((64 * 65 * 4))
depth_bytes=$((64 * 65 * 2))
cat > "${workdir}/bundle.txt" <<BUNDLE
family rs480
bo 0 role=vertex size=4096 read_domains=0x2 write_domain=0x0
bo 1 role=color size=${color_bytes} read_domains=0x0 write_domain=0x2
bo 2 role=depth size=${depth_bytes} read_domains=0x2 write_domain=0x2
BUNDLE
if [ ! -s "${workdir}/bundle.txt" ]; then
    echo "bundle write failed" >&2
    exit 1
fi


failures=0
expect() {
    want="$1"
    label="$2"
    shift 2
    if "$@" >/dev/null 2>&1; then
        got=accept
    else
        got=reject
    fi
    if [ "${got}" = "${want}" ]; then
        printf '  %-46s %s\n' "${label}" "${got}"
    else
        printf '  %-46s %s, %s expected\n' "${label}" "${got}" "${want}"
        failures=$((failures + 1))
    fi
}

# Each bound below comes from the kernel's own arithmetic, so an arm states
# a prediction the tool then confirms.  r300_cs_track_check sizes the depth
# surface as pitch * cpp * maxy + offset, which is 64 * 2 * 64 here, and the
# color target as pitch * cpp * maxy, 64 * 4 * 64.  The draw walks a vertex
# list, so r100_cs_track_check sizes each array as esize * (nverts - 1) * 4,
# which is 4 * 5 * 4 for six FLOAT_4 vertices -- the bound the fixed triangle
# cell's controls carry at three.
depth_exact=$((64 * 2 * 64))
color_exact=$((64 * 4 * 64))
vertex_exact=$((4 * (6 - 1) * 4))

# The relocation NOP the depth binding depends on, located through the
# manifest's own site table rather than a hardcoded index, so a stream whose
# layout moves keeps its control.
ib_dwords=$(python3 - "${workdir}/manifest.json" <<'PY'
import json, sys
print(json.load(open(sys.argv[1]))["ib_dwords"])
PY
)

depth_reloc=$(python3 - "${workdir}/manifest.json" <<'PY'
import json, sys
sites = json.load(open(sys.argv[1]))["reloc_sites"]
print(next(s["ib_index"] for s in sites if s["slot"] == 2))
PY
)

# The retained stream through the parser, with the tool's own verdict
# line in the log beside the controls that calibrate it.
"${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" "${workdir}/ib.bin"

echo "controls:"
expect accept "the retained cell" \
    "${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect reject "depth buffer one byte too small" \
    "${R3V_CS_TRACK_REPLAY_TOOL}" --set-bo-size "2=$((depth_exact - 1))" \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect accept "depth buffer exactly large enough" \
    "${R3V_CS_TRACK_REPLAY_TOOL}" --set-bo-size "2=${depth_exact}" \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect reject "color buffer one byte too small" \
    "${R3V_CS_TRACK_REPLAY_TOOL}" --set-bo-size "1=$((color_exact - 1))" \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect accept "color buffer exactly large enough" \
    "${R3V_CS_TRACK_REPLAY_TOOL}" --set-bo-size "1=${color_exact}" \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect reject "vertex buffer one byte too small" \
    "${R3V_CS_TRACK_REPLAY_TOOL}" --set-bo-size "0=$((vertex_exact - 1))" \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect accept "vertex buffer exactly large enough" \
    "${R3V_CS_TRACK_REPLAY_TOOL}" --set-bo-size "0=${vertex_exact}" \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"
# A plain type-2 NOP where the relocation NOP stands leaves ZB_DEPTHOFFSET
# consuming no relocation.
expect reject "depth relocation NOP replaced" \
    "${R3V_CS_TRACK_REPLAY_TOOL}" \
    --set-dword "$((depth_reloc - 1))=0x80000000" \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect reject "depth relocation index past the chunk" \
    "${R3V_CS_TRACK_REPLAY_TOOL}" --set-dword "${depth_reloc}=64" \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"
# One dword short of the full stream drops the payload of the closing
# ZB_ZCACHE_CTLSTAT write, leaving its header claiming a payload past the
# chunk end whatever the layout ahead of it.
expect reject "truncated stream" \
    "${R3V_CS_TRACK_REPLAY_TOOL}" --truncate "$((ib_dwords - 1))" \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"

if [ "${failures}" -ne 0 ]; then
    echo "run_zb_depth_control_replay: ${failures} controls did not hold" >&2
    exit 1
fi

echo "run_zb_depth_control_replay: the depth binding parses and tracks" \
     "clean, and every negative control held"
