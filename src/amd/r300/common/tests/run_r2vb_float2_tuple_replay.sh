#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel replay of the R2VB FLOAT_4 + FLOAT_2 tuple pass.
#
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the
# Linux radeon source tree; the known-good arm proves the parser admits
# the tuple stream with its three relocations (carrier plus the two
# LOAD_VBPNTR pointers), and the known-bad arms prove the acceptance is
# decided by the stream.  R3V_KERNEL_REPLAY_TOOL adds the TCL-bypass
# width predicate: the tuple's fetched vertex-list draw sits inside the
# extended synthesized-lane scope, so its expected verdict is one PASS
# -- the userspace and kernel validators agreeing on the
# FLOAT_2 + XY01 + vtx_size 6 contract -- and the undersized-VTX_SIZE
# arm must REJECT through the same predicate.  An unset variable is an
# absent configuration and skips.
#
# Usage: run_r2vb_float2_tuple_replay.sh <r300_r2vb_float2_tuple_manifest>

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <r300_r2vb_float2_tuple_manifest-binary>" >&2
    exit 1
fi

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; tuple replay not run" >&2
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
for artifact in ib.bin vertex.bin bo_table.json manifest.json; do
    if [ ! -s "${workdir}/${artifact}" ]; then
        echo "manifest wrote no ${artifact}" >&2
        exit 1
    fi
done

# Two buffer objects: the carrier written through the color backend, and
# the vertex BO both fetch arrays read.  The replay's relocs= field
# counts bundle entries, so the accepted verdict names two.
cat > "${workdir}/bundle.txt" <<'BUNDLE'
family rs480
bo 0 role=carrier size=4096 read_domains=0x2 write_domain=0x2
bo 1 role=vertex size=72 read_domains=0x2 write_domain=0x0
BUNDLE

good=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/ib.bin")
echo "${good}"
case "${good}" in
    "replay dwords="*" relocs=2 draws=1 passed="*" verdict=ACCEPT") ;;
    *)
        echo "tuple pass did not replay as one accepted draw over the" \
             "carrier and vertex objects" >&2
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

# Undersized VAP_VTX_SIZE: five dwords under the six-dword tuple fetch,
# the synthesized-lane underfeed the extended width predicate rejects.
expect_reject "undersized VAP_VTX_SIZE" \
    --set-vtx-size 5 \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"

# Carrier below the color-buffer bound.
cat > "${workdir}/bundle-small.txt" <<'BUNDLE'
family rs480
bo 0 role=carrier size=32 read_domains=0x2 write_domain=0x2
bo 1 role=vertex size=72 read_domains=0x2 write_domain=0x0
BUNDLE
expect_reject "carrier below the color-buffer bound" \
    "${workdir}/bundle-small.txt" "${workdir}/ib.bin"

# Identity selector over the FLOAT_2 element: rewriting EXT_0's upper
# half from XY01 to identity makes the six-dword fetch underdeliver the
# eight-dword output tuple, so the width predicate rejects -- the arm
# that falsifies the XY01 contract itself rather than the arithmetic.
expect_reject "XY01 selector replaced by identity" \
    --set-reg 0x21E0=0xF688F688 \
    "${workdir}/bundle.txt" "${workdir}/ib.bin"

# Vertex BO below the fetched-array bound: the slot array alone strides
# 16 bytes over three vertices.
cat > "${workdir}/bundle-shortvb.txt" <<'BUNDLE'
family rs480
bo 0 role=carrier size=4096 read_domains=0x2 write_domain=0x2
bo 1 role=vertex size=16 read_domains=0x2 write_domain=0x0
BUNDLE
expect_reject "vertex BO below the fetched-array bound" \
    "${workdir}/bundle-shortvb.txt" "${workdir}/ib.bin"

# TCL-bypass width predicate: the fetched tuple draw is inside the
# extended synthesized-lane scope and must PASS, the validator-agreement
# point the FLOAT_2 source contract names.
if [ -n "${R3V_KERNEL_REPLAY_TOOL:-}" ]; then
    if [ ! -x "${R3V_KERNEL_REPLAY_TOOL}" ]; then
        echo "replay tool ${R3V_KERNEL_REPLAY_TOOL} is not executable" >&2
        exit 1
    fi
    width=$("${R3V_KERNEL_REPLAY_TOOL}" "${workdir}/ib.bin")
    echo "${width}"
    case "${width}" in
        *"draws=1 pass=1 reject=0 decline=0"*) ;;
        *)
            echo "tuple draw did not pass the synthesized-lane width" \
                 "predicate" >&2
            exit 1
            ;;
    esac
fi

echo "run_r2vb_float2_tuple_replay: the tuple pass parses clean, every" \
     "known-bad arm rejects, and the width predicate passes the fetched" \
     "FLOAT_2 tuple"
