#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel CS-parser and tracker replay of the reference compute
# identity carrier pass: the fetched producer reading the slot and source
# arrays and writing the carrier row, one draw over three relocation
# entries.  R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track from the
# Linux radeon source tree; an unset variable is an absent configuration
# and skips, a set variable naming something unusable fails.  The
# known-bad arms prove the acceptance is decided by the stream and the
# bundle: a truncated final packet, an output below the color-buffer
# bound, and a slot or input array below the parser's vertex-array bound.
#
# Usage: run_compute_identity_carrier_replay.sh <manifest-binary>

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <r300_compute_identity_carrier_manifest-binary>" >&2
    exit 1
fi
manifest_tool="$1"

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; CS-track replay not run" >&2
    exit 77
fi
for tool in "${R3V_CS_TRACK_REPLAY_TOOL}" "${manifest_tool}"; do
    if [ ! -x "${tool}" ]; then
        echo "${tool} is not executable" >&2
        exit 1
    fi
done

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

# The relocation chunk in entry order: the output (carrier) row written by
# the color backend -- sixteen FP32x4 slots, 256 bytes -- and the slot and
# input (source) pages the vertex fetch reads.
write_bundle() {
    cat > "$1" <<BUNDLE
family rs480
bo 0 role=carrier size=$2 read_domains=0x0 write_domain=0x2
bo 1 role=slot size=$3 read_domains=0x2 write_domain=0x0
bo 2 role=source size=$4 read_domains=0x2 write_domain=0x0
BUNDLE
}
write_bundle "${workdir}/bundle.txt" 256 4096 4096

good=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/ib.bin")
echo "${good}"
case "${good}" in
    "replay dwords="*" relocs=3 draws=1 passed=1 verdict=ACCEPT") ;;
    *)
        echo "compute identity carrier pass did not replay as one accepted" \
             "draw over three relocation entries" >&2
        exit 1
        ;;
esac

# expect_reject NAME [ARGS...]: the mutated replay must report a parser
# rejection, exit status 1; the tool reserves 2 for usage and I/O
# failure, so a crashed or misinvoked tool fails the arm.
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

# Truncated final packet: the last dword removed leaves the publication
# tail's closing packet claiming a payload past the chunk end.
expect_reject "truncated final packet" \
    --truncate $((ib_dwords - 1)) \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"

# Output below the color-buffer bound: the 16-slot FP32x4 row needs 256
# bytes; 255 rejects.
expect_reject "output below the color-buffer bound" \
    --set-bo-size 0=255 "${workdir}/bundle.txt" "${workdir}/ib.bin"

# The parser's vertex-array bound is stride * (count - 1) bytes for each
# fetched array, offset-blind: 240 still parses, 239 rejects; the
# emitter's own -ERANGE bound covers the last record.
expect_accept "slot array at the parser vertex-array bound" \
    --set-bo-size 1=240 "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect_reject "slot array below the parser vertex-array bound" \
    --set-bo-size 1=239 "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect_accept "input array at the parser vertex-array bound" \
    --set-bo-size 2=240 "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect_reject "input array below the parser vertex-array bound" \
    --set-bo-size 2=239 "${workdir}/bundle.txt" "${workdir}/ib.bin"

echo "run_compute_identity_carrier_replay: the pass parses clean and" \
     "every known-bad arm rejects"
