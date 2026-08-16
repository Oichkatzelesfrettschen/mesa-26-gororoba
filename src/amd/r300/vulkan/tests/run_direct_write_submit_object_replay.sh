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
if ! command -v b3sum >/dev/null 2>&1; then
    echo "b3sum unavailable; retained-file digest control not run" >&2
    exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 unavailable; submit-manifest control not run" >&2
    exit 77
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
# agreement alone does not identify the retained file.  b3sum is a required
# independent hasher for this control; a missing tool returns skip 77 above.
file_digest=$(b3sum --no-names "${workdir}/ib.bin")
if [ "${file_digest}" != "${cell_digest}" ]; then
    echo "retained ib.bin hashes to ${file_digest}, manifest declares" \
         "${cell_digest}" >&2
    exit 1
fi
submit_reloc_digest=$(sed -n \
    's/.*"submit_relocs_blake3": "\([0-9a-f]*\)".*/\1/p' \
    "${workdir}/submit_manifest.json")
file_reloc_digest=$(b3sum --no-names "${workdir}/submit_relocs.bin")
if [ -z "${submit_reloc_digest}" ] || \
   [ "${file_reloc_digest}" != "${submit_reloc_digest}" ]; then
    echo "retained submit_relocs.bin hashes to ${file_reloc_digest}," \
         "manifest declares ${submit_reloc_digest}" >&2
    exit 1
fi
{
    ib_bytes=$(wc -c < "${workdir}/ib.bin")
    if [ "${ib_bytes}" -lt 4 ]; then
        echo "retained ib.bin is too short for mutation" >&2
        exit 1
    fi
    head -c "$((ib_bytes - 4))" "${workdir}/ib.bin"
    printf '\377\377\377\377'
} > "${workdir}/mutated_ib.bin"
if [ "$(b3sum --no-names "${workdir}/mutated_ib.bin")" = \
     "${cell_digest}" ]; then
    echo "digest comparison failed to separate a mutated IB" >&2
    exit 1
fi

# The submit relocation chunk carries the color reference plus the
# completion reference; the size derives from the two factors so a
# failure names which one moved.
reloc_entry_bytes=16
expected_relocs=2
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

# The manifest fields mirror radeon_drm_vk_cs_build and
# radeon_drm_vk_completion_init (rg --fixed-strings
# radeon_drm_vk_cs_build src/ and rg --fixed-strings
# radeon_drm_vk_completion_init src/).  Decode the JSON and the retained
# relocation bytes together so the replay bundle cannot substitute a hand-
# written BO size, role, handle, domain, flag, or chunk descriptor.
validate_submit_manifest() {
    manifest_path=$1
    reloc_path=$2
    bundle_path=${3:-}
    python3 - "${manifest_path}" "${reloc_path}" "${workdir}/ib.bin" \
        "${bundle_path}" <<'PY'
import json
import struct
import sys


manifest_path, reloc_path, ib_path, bundle_path = sys.argv[1:5]
expected_color_size = 65536
expected_completion_size = 4
expected_reloc_count = 2
expected_flags = [1, 0, 0]


def relocation_struct(byte_order=None):
    if byte_order is None:
        byte_order = sys.byteorder
    if byte_order not in ("little", "big"):
        raise ValueError("unsupported byte order")
    prefix = "<" if byte_order == "little" else ">"
    return struct.Struct(prefix + "4I")


def decode_relocations(reloc_bytes, byte_order=None):
    decoder = relocation_struct(byte_order)
    if len(reloc_bytes) % decoder.size != 0:
        raise ValueError("relocation bytes are not whole entries")
    return list(decoder.iter_unpack(reloc_bytes))


# Calibrate both fixed-width native byte orders so the replay contract
# rejects a byte-swapped retained struct instead of relying on a little-endian
# host to exercise only one branch.
calibration_words = (0x01020304, 0x10203040, 0x55667788, 0x99AABBCC)
for calibration_order in ("little", "big"):
    calibration_struct = relocation_struct(calibration_order)
    calibration_bytes = calibration_struct.pack(*calibration_words)
    if decode_relocations(calibration_bytes, calibration_order) != [
            calibration_words]:
        print("native relocation calibration failed", file=sys.stderr)
        raise SystemExit(1)
    opposite_order = "big" if calibration_order == "little" else "little"
    if decode_relocations(calibration_bytes, opposite_order) == [
            calibration_words]:
        print("byte-swapped relocation calibration was accepted",
              file=sys.stderr)
        raise SystemExit(1)

try:
    with open(manifest_path, encoding="utf-8") as stream:
        manifest = json.load(stream)
    with open(reloc_path, "rb") as stream:
        reloc_bytes = stream.read()
    with open(ib_path, "rb") as stream:
        ib_bytes = stream.read()
except (OSError, json.JSONDecodeError) as error:
    print(f"submit manifest read failed: {error}", file=sys.stderr)
    raise SystemExit(1)

if manifest.get("object") != "submit-object":
    print("submit manifest object is not submit-object", file=sys.stderr)
    raise SystemExit(1)
if manifest.get("reloc_count") != expected_reloc_count:
    print("submit manifest relocation count is not two", file=sys.stderr)
    raise SystemExit(1)
ib_dwords = manifest.get("ib_dwords")
if not isinstance(ib_dwords, int) or ib_dwords <= 0 or \
        len(ib_bytes) != ib_dwords * 4:
    print("submit manifest IB length differs from ib.bin", file=sys.stderr)
    raise SystemExit(1)
if len(reloc_bytes) != expected_reloc_count * 16:
    print("submit relocation bytes are not two drm_radeon_cs_reloc entries",
          file=sys.stderr)
    raise SystemExit(1)

rows = manifest.get("bo_table")
if not isinstance(rows, list) or len(rows) != expected_reloc_count:
    print("submit manifest bo_table does not match relocation count",
          file=sys.stderr)
    raise SystemExit(1)
expected_roles = [("command", expected_color_size),
                  ("completion", expected_completion_size)]
handles = []
for index, (row, (role, size)) in enumerate(zip(rows, expected_roles)):
    if not isinstance(row, dict) or row.get("reloc_index") != index:
        print("submit manifest relocation indices are not final-list order",
              file=sys.stderr)
        raise SystemExit(1)
    if row.get("role") != role or row.get("size") != size or \
            row.get("read_domains") != 0 or row.get("write_domain") != 2:
        print("submit manifest BO role, size, or domain differs from the "
              "direct-write contract", file=sys.stderr)
        raise SystemExit(1)
    handles.append(row.get("handle"))

if any(not isinstance(handle, int) or handle == 0 for handle in handles) or \
        handles[0] == handles[1]:
    print("submit manifest handles are not two distinct live objects",
          file=sys.stderr)
    raise SystemExit(1)

try:
    relocations = decode_relocations(reloc_bytes)
except ValueError as error:
    print(f"submit relocation decode failed: {error}", file=sys.stderr)
    raise SystemExit(1)

for index, expected in enumerate(zip(handles, rows)):
    handle, read_domains, write_domain, flags = relocations[index]
    manifest_handle, _ = expected
    if (handle, read_domains, write_domain, flags) != \
            (manifest_handle, 0, 2, 0):
        print("submit relocation bytes differ from the manifest BO table",
              file=sys.stderr)
        raise SystemExit(1)

if manifest.get("cs_flags") != expected_flags:
    print("submit manifest cs_flags differ from the GFX retained contract",
          file=sys.stderr)
    raise SystemExit(1)
expected_chunks = [
    {"id": 1, "length_dw": expected_reloc_count * 4},
    {"id": 2, "length_dw": ib_dwords},
    {"id": 3, "length_dw": 3},
]
if manifest.get("chunks") != expected_chunks:
    print("submit manifest chunks differ from the three-chunk CS layout",
          file=sys.stderr)
    raise SystemExit(1)
submit_relocs_digest = manifest.get("submit_relocs_blake3")
if not isinstance(submit_relocs_digest, str) or \
        len(submit_relocs_digest) != 64 or \
        submit_relocs_digest.lower() != submit_relocs_digest:
    print("submit manifest has no lowercase relocation BLAKE3", file=sys.stderr)
    raise SystemExit(1)

if bundle_path:
    with open(bundle_path, "w", encoding="utf-8") as bundle:
        bundle.write("family rs480\n")
        for index, row in enumerate(rows):
            bundle.write(
                f"bo {index} role={row['role']} size={row['size']} "
                f"read_domains=0x{row['read_domains']:x} "
                f"write_domain=0x{row['write_domain']:x}\n")
PY
}

if ! validate_submit_manifest "${workdir}/submit_manifest.json" \
        "${workdir}/submit_relocs.bin" "${workdir}/bundle.txt"; then
    echo "submit manifest does not bind the retained submit object" >&2
    exit 1
fi

# A swapped relocation artifact must fail the handle-to-role binding.
{
    dd if="${workdir}/submit_relocs.bin" bs=16 skip=1 count=1 2>/dev/null
    dd if="${workdir}/submit_relocs.bin" bs=16 count=1 2>/dev/null
} > "${workdir}/swapped_relocs.bin"
if validate_submit_manifest "${workdir}/submit_manifest.json" \
        "${workdir}/swapped_relocs.bin" 2>/dev/null; then
    echo "manifest accepted swapped relocation roles" >&2
    exit 1
fi

# A completion-size mutation must fail before the replay tool can ignore its
# unconsumed relocation slot.
sed 's/"size": 4, "role": "completion"/"size": 8, "role": "completion"/' \
    "${workdir}/submit_manifest.json" > "${workdir}/bad_completion_manifest.json"
if cmp -s "${workdir}/submit_manifest.json" \
       "${workdir}/bad_completion_manifest.json"; then
    echo "completion-size calibration did not mutate the manifest" >&2
    exit 1
fi
if validate_submit_manifest "${workdir}/bad_completion_manifest.json" \
        "${workdir}/submit_relocs.bin" 2>/dev/null; then
    echo "manifest accepted a mismatched completion BO size" >&2
    exit 1
fi

# A chunk descriptor mutation must fail before any parser verdict is trusted.
sed 's/"id": 1/"id": 99/' \
    "${workdir}/submit_manifest.json" > "${workdir}/bad_chunk_manifest.json"
if cmp -s "${workdir}/submit_manifest.json" \
       "${workdir}/bad_chunk_manifest.json"; then
    echo "chunk-descriptor calibration did not mutate the manifest" >&2
    exit 1
fi
if validate_submit_manifest "${workdir}/bad_chunk_manifest.json" \
        "${workdir}/submit_relocs.bin" 2>/dev/null; then
    echo "manifest accepted a malformed CS chunk descriptor" >&2
    exit 1
fi

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

# Known-good: the exact submitted dwords replay through the kernel
# parser and tracker with the accept-no-draw verdict a 2D-only stream
# earns.
good=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/ib.bin")
echo "${good}"
# The replay tool's relocs field reports the number of BO slots in the
# supplied bundle, not the number of IB relocation sites consumed.  The
# bundle mirrors both submitted entries, while reloc_index handle validation
# above binds the one consumed site to the color entry.
accept_replay_verdict() {
    case "$1" in
        *"relocs=${expected_relocs} "*"verdict=ACCEPT-NO-DRAW"*)
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
        "relocs=1 verdict=ACCEPT-NO-DRAW"; then
    echo "replay matcher accepted a bundle with one BO slot" >&2
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
