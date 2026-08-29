#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel-parser replay of the producer cell's exact retained submit
# object.  The producer harness retains one 64-byte carrier relocation and
# the queue's four-byte completion relocation; the replay binds both entries
# to the submit manifest before the CS tracker sees the IB.
#
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the Linux
# radeon source tree.  An unset tool skips the test so the default Mesa build
# remains independent of the sibling kernel checkout.
#
# Usage: run_producer_submit_object_replay.sh <producer-cell-harness>

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <producer-cell-harness>" >&2
    exit 1
fi
if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; producer submit-object replay " \
        "not run" >&2
    exit 77
fi
if [ ! -x "${R3V_CS_TRACK_REPLAY_TOOL}" ]; then
    echo "replay tool ${R3V_CS_TRACK_REPLAY_TOOL} is not executable" >&2
    exit 1
fi
if ! command -v b3sum >/dev/null 2>&1; then
    echo "b3sum unavailable; producer submit-object replay not run" >&2
    exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 unavailable; producer submit-object replay not run" >&2
    exit 77
fi

harness=$1
if [ ! -x "${harness}" ]; then
    echo "producer-cell harness ${harness} is not executable" >&2
    exit 1
fi

workdir=$(mktemp -d)
trap 'rm -rf "${workdir}"' EXIT

# The shim absorbs the ioctl but retains the exact semantic and final submit
# objects that the native queue would carry to DRM_RADEON_CS.
R3V_NATIVE_MANIFEST_DIR="${workdir}" "${harness}" open

for artifact in ib.bin relocs.bin manifest.json submit_relocs.bin \
                submit_manifest.json; do
    if [ ! -f "${workdir}/${artifact}" ]; then
        echo "retained artifact ${artifact} is missing" >&2
        exit 1
    fi
done

cell_digest=$(sed -n 's/.*"ib_blake3": "\([0-9a-f]*\)".*/\1/p' \
    "${workdir}/manifest.json")
submit_digest=$(sed -n 's/.*"ib_blake3": "\([0-9a-f]*\)".*/\1/p' \
    "${workdir}/submit_manifest.json")
if [ -z "${cell_digest}" ] || [ "${cell_digest}" != "${submit_digest}" ]; then
    echo "semantic and submit manifests declare different IB digests" >&2
    exit 1
fi
file_digest=$(b3sum --no-names "${workdir}/ib.bin")
if [ "${file_digest}" != "${cell_digest}" ]; then
    echo "retained ib.bin hashes to ${file_digest}, manifest declares " \
        "${cell_digest}" >&2
    exit 1
fi
submit_reloc_digest=$(sed -n \
    's/.*"submit_relocs_blake3": "\([0-9a-f]*\)".*/\1/p' \
    "${workdir}/submit_manifest.json")
file_reloc_digest=$(b3sum --no-names "${workdir}/submit_relocs.bin")
if [ -z "${submit_reloc_digest}" ] || \
   [ "${file_reloc_digest}" != "${submit_reloc_digest}" ]; then
    echo "retained submit_relocs.bin hashes to ${file_reloc_digest}, " \
        "manifest declares ${submit_reloc_digest}" >&2
    exit 1
fi

# Decode the final relocation list and manifest together.  The producer cell
# has one 64-byte command carrier followed by one four-byte completion BO;
# both entries write GTT, while the carrier also reads GTT for the fetch.
python3 - "${workdir}/submit_manifest.json" \
    "${workdir}/submit_relocs.bin" "${workdir}/ib.bin" \
    "${workdir}/bundle.txt" <<'PY'
import json
import struct
import sys


manifest_path, reloc_path, ib_path, bundle_path = sys.argv[1:5]
expected_roles = [
    ("command", 64, 2, 2),
    ("completion", 4, 0, 2),
]
expected_chunks = [
    {"id": 1, "length_dw": 8},
]


def reject(message):
    print(f"producer submit object: {message}", file=sys.stderr)
    raise SystemExit(1)


def relocation_struct(byte_order=None):
    order = sys.byteorder if byte_order is None else byte_order
    if order not in ("little", "big"):
        reject(f"unsupported byte order {order!r}")
    return struct.Struct(("<" if order == "little" else ">") + "4I")


def decode(data, byte_order=None):
    decoder = relocation_struct(byte_order)
    if len(data) % decoder.size:
        reject("relocation bytes are not whole native entries")
    return list(decoder.iter_unpack(data))


# Calibrate both fixed-width byte orders so a swapped retained structure does
# not pass merely because the host is little endian.
calibration_words = (0x01020304, 0x10203040, 0x55667788, 0x99AABBCC)
for order in ("little", "big"):
    encoded = relocation_struct(order).pack(*calibration_words)
    if decode(encoded, order) != [calibration_words]:
        reject("native relocation calibration failed")
    opposite = "big" if order == "little" else "little"
    if decode(encoded, opposite) == [calibration_words]:
        reject("byte-swapped relocation calibration was accepted")

try:
    with open(manifest_path, encoding="utf-8") as stream:
        manifest = json.load(stream)
    with open(reloc_path, "rb") as stream:
        relocations = decode(stream.read())
    with open(ib_path, "rb") as stream:
        ib_bytes = stream.read()
except (OSError, json.JSONDecodeError) as error:
    reject(f"retained metadata cannot be read: {error}")

if manifest.get("object") != "submit-object":
    reject("manifest object is not submit-object")
if manifest.get("reloc_count") != len(expected_roles):
    reject("manifest relocation count is not two")
ib_dwords = manifest.get("ib_dwords")
if not isinstance(ib_dwords, int) or ib_dwords <= 0 or \
        len(ib_bytes) != ib_dwords * 4:
    reject("manifest IB length differs from ib.bin")
if len(relocations) != len(expected_roles):
    reject("submit relocation bytes do not contain two entries")

rows = manifest.get("bo_table")
if not isinstance(rows, list) or len(rows) != len(expected_roles):
    reject("bo_table does not contain the two final relocation rows")
handles = []
for index, (row, relocation, expected) in enumerate(
        zip(rows, relocations, expected_roles)):
    role, size, read_domains, write_domain = expected
    if not isinstance(row, dict) or row.get("reloc_index") != index:
        reject("bo_table indices do not follow final relocation order")
    if (row.get("role"), row.get("size"), row.get("read_domains"),
            row.get("write_domain")) != \
            (role, size, read_domains, write_domain):
        reject(f"bo_table row {index} differs from producer geometry")
    if tuple(relocation[1:]) != (read_domains, write_domain, 0):
        reject(f"relocation domains or flags differ at index {index}")
    if row.get("handle") != relocation[0]:
        reject(f"relocation handle differs from bo_table at index {index}")
    handle = row.get("handle")
    if not isinstance(handle, int) or handle == 0:
        reject(f"relocation handle {index} is not a live nonzero handle")
    handles.append(handle)
if handles[0] == handles[1]:
    reject("carrier and completion handles are not distinct")

if manifest.get("cs_flags") != [1, 0, 0]:
    reject("cs_flags differ from the retained GFX submit")
chunks = manifest.get("chunks")
if not isinstance(chunks, list) or len(chunks) != 3:
    reject("submit manifest does not contain three CS chunks")
if chunks[0] != expected_chunks[0] or \
        chunks[1] != {"id": 2, "length_dw": ib_dwords} or \
        chunks[2] != {"id": 3, "length_dw": 3}:
    reject("CS chunk descriptors differ from the retained submit")

with open(bundle_path, "w", encoding="utf-8") as bundle:
    bundle.write("family rs480\n")
    bundle.write("bo 0 role=carrier size=64 read_domains=0x2 "
                 "write_domain=0x2\n")
    bundle.write("bo 1 role=completion size=4 read_domains=0x0 "
                 "write_domain=0x2\n")
PY

submit_reloc_bytes=$(wc -c < "${workdir}/submit_relocs.bin")
if [ "${submit_reloc_bytes}" -ne 32 ]; then
    echo "submit reloc chunk is ${submit_reloc_bytes} bytes, not 32" >&2
    exit 1
fi

good=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/ib.bin")
echo "${good}"
case "${good}" in
    *"relocs=2"*"draws=1"*"passed=1"*"verdict=ACCEPT"*) ;;
    *)
        echo "producer submit object did not replay as an accepted draw" >&2
        exit 1
        ;;
esac

expect_reject() {
    name=$1
    shift
    output=""
    status=0
    output=$("${R3V_CS_TRACK_REPLAY_TOOL}" "$@" 2>&1) || status=$?
    echo "${output}"
    if [ "${status}" -ne 1 ]; then
        echo "known-bad arm ${name} exited ${status}, not parser reject 1" \
            >&2
        exit 1
    fi
    case "${output}" in
        *"verdict=REJECT"*) ;;
        *)
            echo "known-bad arm ${name} produced no REJECT verdict" >&2
            exit 1
            ;;
    esac
}

expect_reject "undersized VAP_VTX_SIZE" \
    --set-vtx-size 4 "${workdir}/bundle.txt" "${workdir}/ib.bin"
expect_reject "undersized carrier" \
    --set-bo-size 0=32 "${workdir}/bundle.txt" "${workdir}/ib.bin"

ib_bytes=$(wc -c < "${workdir}/ib.bin")
head -c "$((ib_bytes - 4))" "${workdir}/ib.bin" \
    > "${workdir}/truncated.bin"
expect_reject "truncated final packet" \
    "${workdir}/bundle.txt" "${workdir}/truncated.bin"

# The companion TCL-bypass model remains optional here.  When present, its
# declared scope declines the producer's immediate draw rather than treating
# that narrower predicate as CS-track acceptance or transport evidence.
if [ -n "${R3V_KERNEL_REPLAY_TOOL:-}" ]; then
    if [ ! -x "${R3V_KERNEL_REPLAY_TOOL}" ]; then
        echo "replay tool ${R3V_KERNEL_REPLAY_TOOL} is not executable" >&2
        exit 1
    fi
    width=$("${R3V_KERNEL_REPLAY_TOOL}" "${workdir}/ib.bin")
    echo "${width}"
    case "${width}" in
        *"draws=1"*"pass=0"*"reject=0"*"decline=1"*) ;;
        *)
            echo "producer draw did not decline the width predicate" >&2
            exit 1
            ;;
    esac
fi

echo "producer submit-object replay: exact two-relocation submit passes " \
    "with malformed controls rejected"
