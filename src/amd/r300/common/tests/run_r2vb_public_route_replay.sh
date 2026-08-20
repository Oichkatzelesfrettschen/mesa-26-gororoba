#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel replay of the public GPU-producer route.
#
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the
# Linux radeon source tree; the known-good arm proves the parser admits
# the composed stream -- the producer pass writing the carrier followed
# by the consumer cell fetching it -- as two draws over two relocation
# entries, and the known-bad arms prove the acceptance is decided by the
# stream: a truncated final packet, a carrier below the producer's
# color-buffer bound, and a color target below the consumer's each
# reject.  An unset variable is an absent configuration and skips.
#
# Usage: run_r2vb_public_route_replay.sh <r300_r2vb_public_route_manifest>

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <r300_r2vb_public_route_manifest-binary>" >&2
    exit 1
fi

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; public route replay not run" >&2
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
for artifact in ib.bin bo_table.json manifest.json; do
    if [ ! -s "${workdir}/${artifact}" ]; then
        echo "manifest wrote no ${artifact}" >&2
        exit 1
    fi
done

# The relocation chunk in entry order: the carrier crosses both engines
# -- the producer's color-backend write and the consumer's vertex fetch
# -- so it carries read and write; the color target holds the consumer
# draw plus its canary row.
color_bytes=$((64 * 65 * 4))
cat > "${workdir}/bundle.txt" <<BUNDLE
family rs480
bo 0 role=carrier size=64 read_domains=0x2 write_domain=0x2
bo 1 role=color size=${color_bytes} read_domains=0x0 write_domain=0x2
BUNDLE

good=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/ib.bin")
echo "${good}"
# The tool counts distinct relocation-chunk entries, so the three NOP
# sites over two buffer objects report relocs=2.
case "${good}" in
    "replay dwords="*" relocs=2 draws=2 passed="*" verdict=ACCEPT") ;;
    *)
        echo "public route stream did not replay as two accepted draws" \
             "over two relocation entries" >&2
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

# Truncated final packet: the last dword removed leaves the consumer's
# closing packet claiming a payload past the chunk end.
expect_reject "truncated final packet" \
    --truncate $((ib_dwords - 1)) \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"

# Carrier below the producer stage's color-buffer bound: the 4-pixel
# FP32x4 row needs 64 bytes; 32 rejects.
cat > "${workdir}/bundle-small-carrier.txt" <<BUNDLE
family rs480
bo 0 role=carrier size=32 read_domains=0x2 write_domain=0x2
bo 1 role=color size=${color_bytes} read_domains=0x0 write_domain=0x2
BUNDLE
expect_reject "carrier below the producer color-buffer bound" \
    "${workdir}/bundle-small-carrier.txt" "${workdir}/ib.bin"

# Color target below the consumer stage's bound: the 64-pixel pitch over
# the 64-row render extent needs 16384 bytes; 4096 rejects.
cat > "${workdir}/bundle-small-color.txt" <<BUNDLE
family rs480
bo 0 role=carrier size=64 read_domains=0x2 write_domain=0x2
bo 1 role=color size=4096 read_domains=0x0 write_domain=0x2
BUNDLE
expect_reject "color target below the consumer color-buffer bound" \
    "${workdir}/bundle-small-color.txt" "${workdir}/ib.bin"

echo "run_r2vb_public_route_replay: the composed route parses clean and" \
     "every known-bad arm rejects"
