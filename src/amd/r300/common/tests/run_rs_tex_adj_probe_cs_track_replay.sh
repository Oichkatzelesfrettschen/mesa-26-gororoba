#!/bin/sh
# SPDX-License-Identifier: MIT
#
#
# Offline CS-track replay of the rasterizer probe two-pass cells: the
# --rs-tex-adj and --rs-w-select streams (pass 0 the control varying
# cell, pass 1 the same bytes under the candidate word) each replay
# through r100_cs_track_check's TCL-bypass path to ACCEPT, and every
# negative control the controls script asserts holds on both, since the
# candidate words lie outside the width decision and the tracker's
# pitch, extent, and vertex-buffer bounds.
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the
# Linux radeon source tree against that tree's r300 safe-register bitmap
# and vertex-width decision header.  R3V_CS_TRACK_CONTROLS names the
# control script beside it.  An unset variable is an absent
# configuration and skips (exit 77); a set variable naming something
# unusable is a broken configuration and fails.
#
# Usage: run_rs_tex_adj_probe_cs_track_replay.sh <r300_triangle_manifest-binary>

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <r300_triangle_manifest-binary>" >&2
    exit 1
fi
manifest_tool="$1"

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; rs-tex-adj CS-track replay" \
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

# Merged four-slot bundle, the shape both --rs-tex-adj and
# --rs-w-select bind: one vertex page and one 64-pixel-pitch
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

for candidate in rs-tex-adj rs-w-select; do
    mkdir -p "${workdir}/${candidate}"
    "${manifest_tool}" "${workdir}/${candidate}" "--${candidate}" >/dev/null
    write_bundle "${workdir}/${candidate}"
    check_accept "${workdir}/${candidate}" "${candidate} probe cell"
    if ! controls=$("${R3V_CS_TRACK_CONTROLS}" \
        "${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/${candidate}/bundle.txt" \
        "${workdir}/${candidate}/ib.bin" 2>&1); then
        echo "${controls}"
        echo "${candidate} probe cell: a control did not hold" >&2
        exit 1
    fi
    echo "${controls}"
done

echo "run_rs_tex_adj_probe_cs_track_replay: both probe cells ACCEPT" \
     "draws=2 passed=2 and hold every control"
