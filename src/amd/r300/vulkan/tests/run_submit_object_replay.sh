#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel-parser replay of the exact retained submit object.
#
# The triangle-cell harness runs its open-gate leg on the drm-shim and
# retains the semantic cell and the submit object under the caller's
# manifest directory; this script then replays the retained ib.bin --
# the exact dwords the DRM_RADEON_CS ioctl carried to the shim -- through
# the kernel decision code, proves the two manifests bind the same IB
# content by digest, checks the submit relocation chunk carries the
# completion reference, and runs the malformed controls (mutated
# VAP_VTX_SIZE, truncated stream) against the same retained bytes.
#
# R3V_KERNEL_REPLAY_TOOL names the replay binary built from the Linux
# radeon source tree; an unset tool skips the test, keeping the default
# build graph independent of sibling checkouts.
#
# Usage: run_submit_object_replay.sh <triangle-cell-harness-binary>

set -eu

if [ -z "${R3V_KERNEL_REPLAY_TOOL:-}" ]; then
    echo "R3V_KERNEL_REPLAY_TOOL unset; submit-object replay not run" >&2
    exit 77
fi
if [ ! -x "${R3V_KERNEL_REPLAY_TOOL}" ]; then
    echo "replay tool ${R3V_KERNEL_REPLAY_TOOL} is not executable" >&2
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

# The submit relocation chunk carries the two cell references plus the
# completion reference, sixteen bytes per drm_radeon_cs_reloc.
submit_reloc_bytes=$(wc -c < "${workdir}/submit_relocs.bin")
if [ "${submit_reloc_bytes}" -ne 48 ]; then
    echo "submit reloc chunk is ${submit_reloc_bytes} bytes, not 48" >&2
    exit 1
fi

# Known-good: the exact submitted dwords replay PASS with one draw.
good=$("${R3V_KERNEL_REPLAY_TOOL}" "${workdir}/ib.bin")
echo "${good}"
case "${good}" in
    *"draws=1 pass=1 reject=0 decline=0"*) ;;
    *)
        echo "retained submit object did not replay PASS" >&2
        exit 1
        ;;
esac

# Known-bad: an undersized VAP_VTX_SIZE on the same retained bytes must
# reject.
bad=$("${R3V_KERNEL_REPLAY_TOOL}" --set-vtx-size 3 "${workdir}/ib.bin")
echo "${bad}"
case "${bad}" in
    *"reject=1"*) ;;
    *)
        echo "mutated submit object did not produce reject=1" >&2
        exit 1
        ;;
esac

# Known-bad: a truncated stream must fail decode, and never pass.
dd if="${workdir}/ib.bin" of="${workdir}/truncated.bin" bs=32 count=1 \
    2>/dev/null
if "${R3V_KERNEL_REPLAY_TOOL}" "${workdir}/truncated.bin"; then
    echo "truncated submit object replayed successfully" >&2
    exit 1
fi

echo "submit-object replay: retained bytes PASS, malformed controls hold"
