#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel replay of the composed fetched GPU-producer route for one
# source width.
#
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the
# Linux radeon source tree; the known-good arm proves the parser admits
# the composed stream -- the fetched producer reading the slot and source
# arrays and writing the carrier, followed by the consumer cell fetching
# the carrier -- as two draws over four relocation entries, and the
# known-bad arms prove the acceptance is decided by the stream and the
# bundle: a truncated final packet, a carrier below the producer's
# color-buffer bound, a color target below the consumer's, a slot or
# source array below the parser's vertex-array bound (stride times
# count - 1, offset-blind: the last record's own bytes and the array
# offset stay outside the kernel check, so userspace enforces the fetch
# window), and a source relocation payload naming a chunk entry past the
# table.  An unset variable is an absent configuration and skips.
#
# Usage: run_r2vb_fetched_route_replay.sh <r300_r2vb_fetched_route_manifest>
#        [f32_4|f32_3|f32_2]

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 <r300_r2vb_fetched_route_manifest-binary> [width]" >&2
    exit 1
fi

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; fetched route replay not run" >&2
    exit 77
fi
if [ ! -x "${R3V_CS_TRACK_REPLAY_TOOL}" ]; then
    echo "replay tool ${R3V_CS_TRACK_REPLAY_TOOL} is not executable" >&2
    exit 1
fi

manifest_tool="$1"
width="${2:-f32_4}"
if [ ! -x "${manifest_tool}" ]; then
    echo "manifest tool ${manifest_tool} is not executable" >&2
    exit 1
fi

workdir=$(mktemp -d)
trap 'rm -rf "${workdir}"' EXIT

if ! "${manifest_tool}" "${workdir}" "${width}" >/dev/null; then
    echo "manifest tool failed" >&2
    exit 1
fi
for artifact in ib.bin bo_table.json manifest.json; do
    if [ ! -s "${workdir}/${artifact}" ]; then
        echo "manifest wrote no ${artifact}" >&2
        exit 1
    fi
done

# Geometry facts from the manifest: the source stride decides the
# parser's vertex-array bound, and the source relocation site is the
# dword the chunk-index known-bad rewrites.
stride=$(sed -n 's/^ *"source_stride_bytes": \([0-9]*\),$/\1/p' \
    "${workdir}/manifest.json")
count=$(sed -n 's/^ *"vertex_count": \([0-9]*\),$/\1/p' \
    "${workdir}/manifest.json")
source_site=$(sed -n 's/.*{"role": "source", "ib_index": \([0-9]*\),.*/\1/p' \
    "${workdir}/manifest.json")
if [ -z "${stride}" ] || [ -z "${count}" ] || [ -z "${source_site}" ]; then
    echo "manifest lacks source_stride_bytes, vertex_count, or the source" \
         "relocation site" >&2
    exit 1
fi
source_bound=$((stride * (count - 1)))
slot_bound=$((16 * (count - 1)))

# The relocation chunk in entry order: the carrier crosses both engines
# -- the producer's color-backend write and the consumer's vertex fetch
# -- so it carries read and write; the color target holds the consumer
# draw plus its canary row; the slot and source arrays are device-read
# pages.
color_bytes=$((64 * 65 * 4))
write_bundle() {
    cat > "$1" <<BUNDLE
family rs480
bo 0 role=carrier size=$2 read_domains=0x2 write_domain=0x2
bo 1 role=color size=$3 read_domains=0x0 write_domain=0x2
bo 2 role=slot size=$4 read_domains=0x2 write_domain=0x0
bo 3 role=source size=$5 read_domains=0x2 write_domain=0x0
BUNDLE
}
write_bundle "${workdir}/bundle.txt" 64 "${color_bytes}" 4096 4096

good=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/ib.bin")
echo "${good}"
# The tool counts distinct relocation-chunk entries, so the five NOP
# sites over four buffer objects report relocs=4.
case "${good}" in
    "replay dwords="*" relocs=4 draws=2 passed=2 verdict=ACCEPT") ;;
    *)
        echo "fetched route stream (${width}) did not replay as two" \
             "accepted draws over four relocation entries" >&2
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
# expect_accept NAME [ARGS...]: the boundary value still parses clean.
expect_accept() {
    name="$1"
    shift
    status=0
    "${R3V_CS_TRACK_REPLAY_TOOL}" "$@" >/dev/null 2>&1 || status=$?
    if [ "${status}" -ne 0 ]; then
        echo "boundary arm ${name} exited ${status}, not accepted" >&2
        exit 1
    fi
    echo "  accept: ${name}"
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
expect_reject "carrier below the producer color-buffer bound" \
    --set-bo-size 0=32 "${workdir}/bundle.txt" "${workdir}/ib.bin"

# Color target below the consumer stage's bound: the 64-pixel pitch over
# the 64-row render extent needs 16384 bytes; 4096 rejects.
expect_reject "color target below the consumer color-buffer bound" \
    --set-bo-size 1=4096 "${workdir}/bundle.txt" "${workdir}/ib.bin"

# The parser's vertex-array bound is stride * (count - 1) bytes for each
# fetched array: that many bytes still parse, one fewer rejects.  The
# bound is offset-blind and leaves the last record's bytes out, which is
# the userspace enforcement the emitter's -ERANGE bound supplies.
expect_accept "slot array at the parser vertex-array bound (${slot_bound})" \
    --set-bo-size 2="${slot_bound}" "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect_reject "slot array below the parser vertex-array bound" \
    --set-bo-size 2=$((slot_bound - 1)) \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect_accept "source array at the parser vertex-array bound (${source_bound})" \
    --set-bo-size 3="${source_bound}" "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect_reject "source array below the parser vertex-array bound" \
    --set-bo-size 3=$((source_bound - 1)) \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"

# The source relocation payload naming chunk entry 4 of a four-entry
# table: the parser has no buffer object to bind the fetch to.
expect_reject "source relocation past the chunk table" \
    --set-dword "${source_site}=16" \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"

echo "run_r2vb_fetched_route_replay (${width}): the composed route parses" \
     "clean and every known-bad arm rejects"
