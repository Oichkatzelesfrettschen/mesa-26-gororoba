#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Full kernel CS-parser and tracker replay of the direct GA Flat two-pass
# cell, contrasted against its host-replicated control.
#
# The bundle names the merged four-slot layout both --flat-color0 and
# --flat-replicate bind: two vertex pages and two color targets, the
# second pass at merged indices 2 and 3
# (r300_triangle_multi_pass::second_vertex_index/second_color_index).
# Ordinary CS tracking (packet framing, register admission, relocation
# consumption, buffer-size bounds) does not read VAP_OUTPUT_VTX_FMT_0's
# COLOR_0_PRESENT bit, so both cells ACCEPT here.  The replay tool runs
# the kernel TCL-bypass vertex check (r300_tcl_bypass_vtx_check.h) at
# each draw as a second stage, and that check counts COLOR_0_PRESENT as
# four required dwords, so the "VAP_VTX_SIZE below the output width"
# control rejects the direct cell's stream at VAP_VTX_SIZE 3 exactly as
# it rejects the replicated cell's: every control holds for both.
#
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the
# Linux radeon source tree against that tree's r300 safe-register bitmap
# and vertex-width decision header.  R3V_CS_TRACK_CONTROLS names the
# control script beside it.  An unset variable is an absent
# configuration and skips (exit 77); a set variable naming something
# unusable is a broken configuration and fails.
#
# Usage: run_flat_color0_cs_track_replay.sh <r300_triangle_manifest-binary>

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <r300_triangle_manifest-binary>" >&2
    exit 1
fi
manifest_tool="$1"

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; flat-color0 CS-track replay" \
         "not run" >&2
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
if [ ! -x "${manifest_tool}" ]; then
    echo "manifest tool ${manifest_tool} is not executable" >&2
    exit 1
fi

workdir=$(mktemp -d)
trap 'rm -rf "${workdir}"' EXIT

# Merged four-slot bundle, the shape both --flat-color0 and
# --flat-replicate bind: one vertex page and one 64-pixel-pitch
# ARGB8888 target per pass, the second pass at indices 2 and 3.
color_bytes=$((64 * 65 * 4))
write_bundle() {
    cat > "$1/bundle.txt" <<BUNDLE
family rs480
bo 0 role=vertex size=4096 read_domains=0x2 write_domain=0x0
bo 1 role=color size=${color_bytes} read_domains=0x0 write_domain=0x2
bo 2 role=vertex size=4096 read_domains=0x2 write_domain=0x0
bo 3 role=color size=${color_bytes} read_domains=0x0 write_domain=0x2
BUNDLE
}

check_accept() {
    dir="$1"
    label="$2"
    result=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${dir}/bundle.txt" \
        "${dir}/ib.bin")
    echo "${result}"
    case "${result}" in
        *"draws=2 passed=2 verdict=ACCEPT"*) ;;
        *)
            echo "${label} did not replay verdict=ACCEPT draws=2" \
                 "passed=2" >&2
            exit 1
            ;;
    esac
}

mkdir -p "${workdir}/direct" "${workdir}/replicate"
"${manifest_tool}" "${workdir}/direct" --flat-color0 >/dev/null
"${manifest_tool}" "${workdir}/replicate" --flat-replicate >/dev/null
write_bundle "${workdir}/direct"
write_bundle "${workdir}/replicate"

check_accept "${workdir}/direct" "direct flat-color0 cell"
check_accept "${workdir}/replicate" "flat-replicate cell"

# The replicated cell: every control the script asserts must hold.
if ! replicate_controls=$("${R3V_CS_TRACK_CONTROLS}" \
    "${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/replicate/bundle.txt" \
    "${workdir}/replicate/ib.bin" 2>&1); then
    echo "${replicate_controls}"
    echo "flat-replicate cell: a control did not hold" >&2
    exit 1
fi
echo "${replicate_controls}"

# The direct cell: every control holds, the VAP_VTX_SIZE predicate
# included, because the width stage reads COLOR_0_PRESENT.
if ! direct_controls=$("${R3V_CS_TRACK_CONTROLS}" \
    "${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/direct/bundle.txt" \
    "${workdir}/direct/ib.bin" 2>&1); then
    echo "${direct_controls}"
    echo "direct flat-color0 cell: a control did not hold" >&2
    exit 1
fi
echo "${direct_controls}"

echo "run_flat_color0_cs_track_replay: both cells ACCEPT draws=2" \
     "passed=2 and hold every control"
