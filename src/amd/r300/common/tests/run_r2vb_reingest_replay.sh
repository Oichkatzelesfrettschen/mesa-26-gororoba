#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel replay of the R2VB producer-plus-re-ingest stream.
#
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the
# Linux radeon source tree; the metadata checker joins the generated
# manifest, BO table, and IB before the parser runs.  The known-good arm
# proves the parser admits the concatenated stream with its three
# relocation sites over the two buffer objects and both draws, and the
# known-bad arms prove the acceptance is decided by the stream: a truncated
# final packet, and a carrier too small for the producer stage's
# color-buffer bound, each reject.  An unset variable is an absent
# configuration and skips.
#
# Usage: run_r2vb_reingest_replay.sh <manifest-tool> [manifest-checker]

set -eu

if [ "$#" -ne 1 ] && [ "$#" -ne 2 ]; then
    echo "usage: $0 <manifest-tool> [manifest-checker]" >&2
    exit 1
fi

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; re-ingest replay not run" >&2
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
manifest_checker="${2:-$(dirname "$0")/r300_r2vb_reingest_manifest_check.py}"
if [ ! -f "${manifest_checker}" ]; then
    echo "manifest checker ${manifest_checker} is not a regular file" >&2
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

# Join the generated metadata before constructing the parser bundle.  The
# checker independently verifies the IB digest, relocation NOP payloads, BO
# roles/domains, and the fixed carrier and color extents; the replay therefore
# consumes the allocations the manifest publishes instead of a larger
# hand-written control surface.
metadata_report=$(python3 "${manifest_checker}" --validate "${workdir}")
echo "${metadata_report}"
carrier_size=$(printf '%s\n' "${metadata_report}" |
    sed -n 's/.*carrier=\([0-9][0-9]*\) color=.*/\1/p')
color_size=$(printf '%s\n' "${metadata_report}" |
    sed -n 's/.*carrier=[0-9][0-9]* color=\([0-9][0-9]*\).*/\1/p')
case "${carrier_size}" in
    ''|*[!0-9]*)
        echo "metadata checker returned no numeric carrier size" >&2
        exit 1
        ;;
esac
case "${color_size}" in
    ''|*[!0-9]*)
        echo "metadata checker returned no numeric color size" >&2
        exit 1
        ;;
esac

# The relocation chunk in entry order: the carrier crosses both engines
# (color-backend write, vertex fetch), so it carries read and write; the
# color target holds the triangle draw plus its canary row.
cat > "${workdir}/bundle.txt" <<BUNDLE
family rs480
bo 0 role=carrier size=${carrier_size} read_domains=0x2 write_domain=0x2
bo 1 role=color size=${color_size} read_domains=0x0 write_domain=0x2
BUNDLE

good=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/ib.bin")
echo "${good}"
# The tool counts distinct relocation-chunk entries, so the three NOP
# sites over two buffer objects report relocs=2.
case "${good}" in
    "replay dwords="*" relocs=2 draws=2 passed="*" verdict=ACCEPT") ;;
    *)
        echo "re-ingest stream did not replay as two accepted draws over" \
             "two relocation entries" >&2
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
cat > "${workdir}/bundle-small.txt" <<BUNDLE
family rs480
bo 0 role=carrier size=32 read_domains=0x2 write_domain=0x2
bo 1 role=color size=${color_size} read_domains=0x0 write_domain=0x2
BUNDLE
expect_reject "carrier below the producer color-buffer bound" \
    "${workdir}/bundle-small.txt" "${workdir}/ib.bin"

echo "run_r2vb_reingest_replay: the re-ingest stream parses clean and" \
     "every known-bad arm rejects"
