#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline CS-tracking replay of the NoPerspective reciprocal-carrier
# two-pass cell through the r300 parser model: both draws ACCEPT and every
# negative control the control script drives holds.  The bundle names the
# four buffer objects the merged two-pass binding references.
set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <r300_triangle_manifest-binary>" >&2
    exit 1
fi
manifest_tool="$1"

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; q-lane CS-track replay not run" >&2
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

color_bytes=$((64 * 65 * 4))
"${manifest_tool}" "${workdir}" --noperspective-q-lane >/dev/null
cat > "${workdir}/bundle.txt" <<BUNDLE
family rs480
bo 0 role=vertex size=4096 read_domains=0x2 write_domain=0x0
bo 1 role=color size=${color_bytes} read_domains=0x0 write_domain=0x2
bo 2 role=vertex size=4096 read_domains=0x2 write_domain=0x0
bo 3 role=color size=${color_bytes} read_domains=0x0 write_domain=0x2
BUNDLE
result=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/ib.bin")
echo "${result}"
case "${result}" in
    *"draws=2 passed=2 verdict=ACCEPT"*) ;;
    *)
        echo "q-lane cell did not replay verdict=ACCEPT draws=2 passed=2" >&2
        exit 1
        ;;
esac
if ! controls=$("${R3V_CS_TRACK_CONTROLS}" "${R3V_CS_TRACK_REPLAY_TOOL}" \
    "${workdir}/bundle.txt" "${workdir}/ib.bin" 2>&1); then
    echo "${controls}"
    echo "q-lane cell: a control did not hold" >&2
    exit 1
fi
echo "${controls}"

echo "run_noperspective_q_lane_cs_track_replay: q-lane cell ACCEPT" \
     "draws=2 passed=2 and holds every control"
