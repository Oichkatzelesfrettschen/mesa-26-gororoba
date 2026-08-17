#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel replay of the R2VB FLOAT_2 tuple burst pass.
#
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the
# Linux radeon source tree; the known-good arms prove the parser admits
# the composed burst as its declared draw count over the two BO roles,
# and the known-bad arms prove the acceptance is decided by the stream:
# a truncated tail, a carrier one member row short, and an undersized
# VAP_VTX_SIZE each reject.  R3V_KERNEL_REPLAY_TOOL adds the TCL-bypass
# width predicate over every member draw.  An unset variable is an
# absent configuration and skips.
#
# Usage: run_r2vb_float2_tuple_burst_replay.sh \
#            <r300_r2vb_float2_tuple_burst_manifest>

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <r300_r2vb_float2_tuple_burst_manifest-binary>" >&2
    exit 1
fi

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; burst replay not run" >&2
    exit 77
fi
if [ ! -x "${R3V_CS_TRACK_REPLAY_TOOL}" ]; then
    echo "replay tool ${R3V_CS_TRACK_REPLAY_TOOL} is not executable" >&2
    exit 1
fi

manifest_tool="$1"
if [ ! -x "${manifest_tool}" ]; then
    echo "manifest tool ${manifest_tool} is not executable" >&2
    exit 1
fi

workdir=$(mktemp -d)
trap 'rm -rf "${workdir}"' EXIT

# expect_reject NAME [ARGS...]: the mutated replay must report a parser
# rejection, exit status 1.  The tool reserves 2 for usage and I/O
# failure, so a crashed or misinvoked tool fails the arm rather than
# counting as a rejection.
expect_reject() {
    name="$1"
    shift
    status=0
    "${R3V_CS_TRACK_REPLAY_TOOL}" "$@" >/dev/null 2>&1 || status=$?
    if [ "${status}" -ne 1 ]; then
        echo "known-bad arm ${name} exited ${status}, not the parser" \
             "rejection 1" >&2
        exit 1
    fi
    echo "  reject: ${name}"
}

# Each draw count exercises a different composition depth; 4 is the
# smallest multi-member stream and 64 is the attended bound.
for draws in 4 64; do
    dir="${workdir}/d${draws}"
    mkdir "${dir}"
    if ! "${manifest_tool}" "${dir}" "${draws}" >/dev/null; then
        echo "manifest tool failed for draws=${draws}" >&2
        exit 1
    fi
    for artifact in ib.bin vertex.bin bo_table.json manifest.json; do
        if [ ! -s "${dir}/${artifact}" ]; then
            echo "manifest wrote no ${artifact} (draws=${draws})" >&2
            exit 1
        fi
    done

    carrier_size=$((draws * 64))
    cat > "${dir}/bundle.txt" <<BUNDLE
family rs480
bo 0 role=carrier size=${carrier_size} read_domains=0x2 write_domain=0x2
bo 1 role=vertex size=72 read_domains=0x2 write_domain=0x0
BUNDLE

    good=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${dir}/bundle.txt" \
        "${dir}/ib.bin")
    echo "${good}"
    case "${good}" in
        "replay dwords="*" relocs=2 draws=${draws} passed="*" verdict=ACCEPT") ;;
        *)
            echo "burst draws=${draws} did not replay as ${draws}" \
                 "accepted draws over the carrier and vertex objects" >&2
            exit 1
            ;;
    esac

    ib_bytes=$(wc -c < "${dir}/ib.bin")
    ib_dwords=$((ib_bytes / 4))

    # Truncated final packet: the last member's publication tail loses
    # its final dword, leaving a header claiming a payload past the
    # chunk end.
    expect_reject "truncated final packet (draws=${draws})" \
        --truncate $((ib_dwords - 1)) \
        "${dir}/bundle.txt" "${dir}/ib.bin"

    # Carrier one member row short: the last member's color-buffer
    # bound falls outside the BO, so the disjoint-row layout is what
    # the tracker's acceptance actually rests on.
    cat > "${dir}/bundle-short.txt" <<BUNDLE
family rs480
bo 0 role=carrier size=$((carrier_size - 64)) read_domains=0x2 write_domain=0x2
bo 1 role=vertex size=72 read_domains=0x2 write_domain=0x0
BUNDLE
    expect_reject "carrier one member row short (draws=${draws})" \
        "${dir}/bundle-short.txt" "${dir}/ib.bin"

    # Undersized VAP_VTX_SIZE under the six-dword tuple fetch.
    expect_reject "undersized VAP_VTX_SIZE (draws=${draws})" \
        --set-vtx-size 5 \
        "${dir}/bundle.txt" "${dir}/ib.bin"

    # TCL-bypass width predicate: every member draw sits inside the
    # extended synthesized-lane scope and must pass.
    if [ -n "${R3V_KERNEL_REPLAY_TOOL:-}" ]; then
        if [ ! -x "${R3V_KERNEL_REPLAY_TOOL}" ]; then
            echo "replay tool ${R3V_KERNEL_REPLAY_TOOL} is not" \
                 "executable" >&2
            exit 1
        fi
        width=$("${R3V_KERNEL_REPLAY_TOOL}" "${dir}/ib.bin")
        echo "${width}"
        case "${width}" in
            *"draws=${draws} pass=${draws} reject=0 decline=0"*) ;;
            *)
                echo "burst draws=${draws} did not pass the" \
                     "synthesized-lane width predicate on every member" >&2
                exit 1
                ;;
        esac
    fi
done

echo "run_r2vb_float2_tuple_burst_replay: the burst parses clean at both" \
     "depths, every known-bad arm rejects, and the width predicate" \
     "passes every member draw"
