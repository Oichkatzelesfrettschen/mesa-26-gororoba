#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel-parser replay of the direct-write control's exact
# retained submit object.
#
# The direct-write harness runs its open-gate leg on the drm-shim and
# retains the semantic cell and the submit object under the caller's
# manifest directory; this script proves the two manifests bind the same
# IB content by digest, checks the submit relocation chunk carries the
# completion reference beside the one color reference, replays the
# retained ib.bin -- the exact dwords the DRM_RADEON_CS ioctl carried to
# the shim -- through the kernel decision code, and runs malformed
# controls against the same retained bytes.
#
# R3V_CS_TRACK_REPLAY_TOOL names the full CS parser/tracker replay built
# from the Linux radeon source tree; an unset tool skips the test,
# keeping the default build graph independent of sibling checkouts.
#
# Usage: run_direct_write_submit_object_replay.sh <direct-write-harness>

set -eu

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; direct-write submit-object" \
         "replay not run" >&2
    exit 77
fi
if [ ! -x "${R3V_CS_TRACK_REPLAY_TOOL}" ]; then
    echo "replay tool ${R3V_CS_TRACK_REPLAY_TOOL} is not executable" >&2
    exit 1
fi

harness="$1"
workdir=$(mktemp -d)
trap 'rm -rf "${workdir}"' EXIT

R3V_NATIVE_MANIFEST_DIR="${workdir}" "${harness}" open

for artifact in ib.bin relocs.bin manifest.json submit_relocs.bin \
                submit_manifest.json; do
    if [ ! -f "${workdir}/${artifact}" ]; then
        echo "retained artifact ${artifact} is missing" >&2
        exit 1
    fi
done

# The semantic cell and the submit object name the same IB content.
cell_digest=$(sed -n 's/.*"ib_blake3": "\([0-9a-f]*\)".*/\1/p' \
    "${workdir}/manifest.json")
submit_digest=$(sed -n 's/.*"ib_blake3": "\([0-9a-f]*\)".*/\1/p' \
    "${workdir}/submit_manifest.json")
if [ -z "${cell_digest}" ] || [ "${cell_digest}" != "${submit_digest}" ]; then
    echo "manifest digests disagree: '${cell_digest}' vs" \
         "'${submit_digest}'" >&2
    exit 1
fi

# The two manifests were computed from the same in-memory IB, so their
# agreement alone does not identify the retained file; recompute BLAKE3
# over ib.bin itself where an independent hasher is available, and prove
# the comparison by refusing a mutated copy.
if command -v b3sum >/dev/null 2>&1; then
    file_digest=$(b3sum --no-names "${workdir}/ib.bin")
    if [ "${file_digest}" != "${cell_digest}" ]; then
        echo "retained ib.bin hashes to ${file_digest}, manifest declares" \
             "${cell_digest}" >&2
        exit 1
    fi
    {
        dd if="${workdir}/ib.bin" bs=4 count=31 2>/dev/null
        printf '\377\377\377\377'
    } > "${workdir}/mutated_ib.bin"
    if [ "$(b3sum --no-names "${workdir}/mutated_ib.bin")" = \
         "${cell_digest}" ]; then
        echo "digest comparison failed to separate a mutated IB" >&2
        exit 1
    fi
else
    echo "b3sum absent; retained-file digest recomputation not run" >&2
fi

# The submit relocation chunk carries the color reference plus the
# completion reference; the size derives from the two factors so a
# failure names which one moved.
reloc_entry_bytes=16
expected_relocs=2
expected_consumed_relocs=1
submit_reloc_bytes=$(wc -c < "${workdir}/submit_relocs.bin")
if [ "${submit_reloc_bytes}" -ne      "$((reloc_entry_bytes * expected_relocs))" ]; then
    echo "submit reloc chunk is ${submit_reloc_bytes} bytes, not" \
         "${expected_relocs} entries of ${reloc_entry_bytes}" >&2
    exit 1
fi

# validate_relocs FILE: decode the retained drm_radeon_cs_reloc entries
# -- handle, read_domains, write_domain, flags per entry -- and hold the
# submit binding: both entries write-bind GTT (0x2) with no read domain,
# and the two handles are nonzero and distinct.
validate_relocs() {
    # Word splitting is the decode: od emits one hex token per dword.
    # shellcheck disable=SC2046
    set -- $(od -An -v -tx4 "$1")
    if [ "$#" -ne 8 ]; then
        echo "reloc chunk decodes to $# dwords, not 8" >&2
        return 1
    fi
    color_handle=$1; color_read=$2; color_write=$3; color_flags=$4
    completion_handle=$5; completion_read=$6; completion_write=$7
    completion_flags=$8
    if [ "${color_write}" != "00000002" ] || \
       [ "${completion_write}" != "00000002" ] || \
       [ "${color_read}" != "00000000" ] || \
       [ "${completion_read}" != "00000000" ]; then
        echo "reloc domains diverge from the GTT write binding:" \
             "color ${color_read}/${color_write}" \
             "completion ${completion_read}/${completion_write}" >&2
        return 1
    fi
    # Both relocation constructors pass priority zero, so a nonzero
    # flags dword (RADEON_RELOC_PRIO_MASK lives in its low bits) marks a
    # changed or corrupted submit path.
    if [ "${color_flags}" != "00000000" ] || \
       [ "${completion_flags}" != "00000000" ]; then
        echo "reloc flags diverge from priority zero:" \
             "color ${color_flags} completion ${completion_flags}" >&2
        return 1
    fi
    if [ "${color_handle}" = "00000000" ] || \
       [ "${completion_handle}" = "00000000" ] || \
       [ "${color_handle}" = "${completion_handle}" ]; then
        echo "reloc handles are not two distinct live objects:" \
             "${color_handle} ${completion_handle}" >&2
        return 1
    fi
    return 0
}

if ! validate_relocs "${workdir}/submit_relocs.bin"; then
    echo "retained submit relocation chunk failed validation" >&2
    exit 1
fi

# The chunk's entry order binds to the manifest's bo_table by
# reloc_index: the IB consumes relocation index 0, so a swapped chunk
# would point DST_PITCH_OFFSET at the completion buffer while the
# manifests still agree.  Compare each chunk handle to the handle the
# manifest records for that index.
manifest_handle_0=$(sed -n \
    's/.*"reloc_index": 0, "handle": \([0-9]*\),.*/\1/p' \
    "${workdir}/submit_manifest.json")
manifest_handle_1=$(sed -n \
    's/.*"reloc_index": 1, "handle": \([0-9]*\),.*/\1/p' \
    "${workdir}/submit_manifest.json")
chunk_handle_0=$((0x${color_handle}))
chunk_handle_1=$((0x${completion_handle}))
if [ -z "${manifest_handle_0}" ] || [ -z "${manifest_handle_1}" ] || \
   [ "${chunk_handle_0}" -ne "${manifest_handle_0}" ] || \
   [ "${chunk_handle_1}" -ne "${manifest_handle_1}" ]; then
    echo "chunk handles (${chunk_handle_0}, ${chunk_handle_1}) do not" \
         "bind to the manifest bo_table (${manifest_handle_0}," \
         "${manifest_handle_1}) by reloc_index" >&2
    exit 1
fi

# The validator earns its verdict by refusing a mutated chunk: zeroing
# the color write-domain dword must fail.
{
    dd if="${workdir}/submit_relocs.bin" bs=4 count=2 2>/dev/null
    printf '\000\000\000\000'
    dd if="${workdir}/submit_relocs.bin" bs=4 skip=3 2>/dev/null
} > "${workdir}/mutated_relocs.bin"
if validate_relocs "${workdir}/mutated_relocs.bin" 2>/dev/null; then
    echo "reloc validator accepted a zeroed write domain" >&2
    exit 1
fi

# The replay bundle mirrors the submit object's BO table: the 65536-byte
# GTT color target the cell writes and the 4096-byte GTT completion
# buffer the queue appends.
cat > "${workdir}/bundle.txt" <<EOF
family rs480
bo 0 role=color size=65536 read_domains=0x0 write_domain=0x2
bo 1 role=completion size=4096 read_domains=0x0 write_domain=0x2
EOF

# Known-good: the exact submitted dwords replay through the kernel
# parser and tracker with the accept-no-draw verdict a 2D-only stream
# earns.
good=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/ib.bin")
echo "${good}"
accept_replay_verdict() {
    case "$1" in
        *"relocs=${expected_consumed_relocs} "*"verdict=ACCEPT-NO-DRAW"*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}
if ! accept_replay_verdict "${good}"; then
    echo "retained direct-write submit object did not replay" \
         "ACCEPT-NO-DRAW" >&2
    exit 1
fi
if accept_replay_verdict \
        "relocs=${expected_relocs} verdict=ACCEPT-NO-DRAW"; then
    echo "replay verdict accepted the unconsumed completion relocation" >&2
    exit 1
fi

# Known-bad: rewriting the DST_PITCH_OFFSET header (dword 0) to
# register 0x1430, which the safe bitmap flags and r300_packet0_check
# does not name, must reject through the parser's default arm.
{
    printf '\014\005\000\000'
    dd if="${workdir}/ib.bin" bs=4 skip=1 2>/dev/null
} > "${workdir}/bad-register.bin"
# A crash, a missing input, or a nonexistent tool prints no verdict, so
# the leg demands the parser's own REJECT verdict rather than the mere
# absence of an accept.
bad=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/bad-register.bin" 2>/dev/null) && :
case "${bad}" in
    *"verdict=REJECT"*) ;;
    *)
        echo "default-reject register mutation did not earn" \
             "verdict=REJECT: ${bad}" >&2
        exit 1
        ;;
esac

# Known-bad: dropping the final dword cuts the last packet mid-body, so
# the stream must fail decode and never pass.  A cut on a packet
# boundary would be a shorter valid stream, so the truncation removes
# one dword from the end instead of prefixing.
ib_bytes=$(wc -c < "${workdir}/ib.bin")
head -c "$((ib_bytes - 4))" "${workdir}/ib.bin" > "${workdir}/truncated.bin"
bad=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/truncated.bin" 2>/dev/null) && :
case "${bad}" in
    *"verdict=REJECT"*) ;;
    *)
        echo "truncated submit object did not earn verdict=REJECT:" \
             "${bad}" >&2
        exit 1
        ;;
esac

echo "direct-write submit-object replay: retained bytes ACCEPT-NO-DRAW," \
     "malformed controls hold"
