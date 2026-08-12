#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel replay of the R2VB producer pass.
#
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the
# Linux radeon source tree; the known-good arm proves the parser admits
# the exact producer stream with its one carrier relocation, and the
# known-bad arms prove the acceptance is decided by the stream: a
# truncated final packet, a draw header undercounting its body, and a
# carrier too small for the pitch * cpp * maxy color-buffer bound each
# reject.  R3V_KERNEL_REPLAY_TOOL adds the TCL-bypass width predicate,
# whose declared scope excludes PRIM_WALK-3 immediate draws, so its
# expected verdict for the producer is one declined draw with no
# rejection.  An unset variable is an absent configuration and skips.
#
# Usage: run_r2vb_producer_replay.sh <r300_r2vb_producer_manifest-binary>

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <r300_r2vb_producer_manifest-binary>" >&2
    exit 1
fi

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; producer replay not run" >&2
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

if ! "${manifest_tool}" "${workdir}" >/dev/null; then
    echo "manifest tool failed" >&2
    exit 1
fi
if [ ! -s "${workdir}/ib.bin" ]; then
    echo "manifest wrote no ib.bin" >&2
    exit 1
fi
for artifact in bo_table.json manifest.json; do
    if [ ! -s "${workdir}/${artifact}" ]; then
        echo "manifest wrote no ${artifact}" >&2
        exit 1
    fi
done

# One buffer object: the carrier, written through the color backend and
# read back as the vertex stream, so both domains are GTT and the entry
# carries read and write.
cat > "${workdir}/bundle.txt" <<'BUNDLE'
family rs480
bo 0 role=carrier size=4096 read_domains=0x2 write_domain=0x2
BUNDLE

good=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/ib.bin")
echo "${good}"
case "${good}" in
    "replay dwords="*" relocs=1 draws=1 passed="*" verdict=ACCEPT") ;;
    *)
        echo "producer pass did not replay as one accepted draw with one" \
             "relocation" >&2
        exit 1
        ;;
esac

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

ib_bytes=$(wc -c < "${workdir}/ib.bin")
ib_dwords=$((ib_bytes / 4))

# Truncated final packet: the last dword removed leaves the
# VAP_PVS_STATE_FLUSH_REG header claiming a payload past the chunk end.
expect_reject "truncated final packet" \
    --truncate $((ib_dwords - 1)) \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"

# Undersized VAP_VTX_SIZE: the embedded draw's dword count no longer
# equals vtx_size * num_vertices, the r100_cs_track_check IMMD identity.
expect_reject "undersized VAP_VTX_SIZE" \
    --set-vtx-size 4 \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"

# Carrier below the color-buffer bound: pitch 4 pixels of 16-byte FP32x4
# texels over one scissored row needs 64 bytes; 32 rejects.
cat > "${workdir}/bundle-small.txt" <<'BUNDLE'
family rs480
bo 0 role=carrier size=32 read_domains=0x2 write_domain=0x2
BUNDLE
expect_reject "carrier below the color-buffer bound" \
    "${workdir}/bundle-small.txt" "${workdir}/ib.bin"

# TCL-bypass width predicate: PRIM_WALK-3 immediate draws sit outside its
# declared scope, so the producer's one draw declines without rejection.
if [ -n "${R3V_KERNEL_REPLAY_TOOL:-}" ]; then
    if [ ! -x "${R3V_KERNEL_REPLAY_TOOL}" ]; then
        echo "replay tool ${R3V_KERNEL_REPLAY_TOOL} is not executable" >&2
        exit 1
    fi
    width=$("${R3V_KERNEL_REPLAY_TOOL}" "${workdir}/ib.bin")
    echo "${width}"
    case "${width}" in
        *"draws=1 pass=0 reject=0 decline=1"*) ;;
        *)
            echo "producer draw did not decline the width predicate" >&2
            exit 1
            ;;
    esac
fi

echo "run_r2vb_producer_replay: the producer pass parses clean and" \
     "every known-bad arm rejects"
