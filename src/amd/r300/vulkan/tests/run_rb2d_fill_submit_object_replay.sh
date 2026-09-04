#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Offline kernel-parser replay of the public RB2D fill route's exact
# retained submit object.
#
# The loader-only fill application submits the attended cell through
# vkQueueSubmit under the radeon drm-shim and the driver retains the
# semantic cell and the submit object under the evidence directory.  This
# script proves the two manifests bind the same IB content by digest with
# an independent hasher, decodes the relocation chunk against the
# manifest's buffer-object table, replays the retained ib.bin -- the exact
# dwords DRM_RADEON_CS carried to the shim -- through the kernel decision
# code, and holds every mutation to the class the kernel-derived parser
# gives it.
#
# The parser owns three things on this stream: the PACKET0 register
# admission (r300_packet0_check over the safe-register bitmap), the
# relocation protocol (a PACKET3 NOP after DST_PITCH_OFFSET), and the
# stream framing.  The 2D destination geometry -- which buffer object the
# relocation names, the pitch, the base offset, the rectangles, the
# scissor, the wait state -- passes the kernel unchecked: r100_cs_track
# tracks 3D color and depth targets and no 2D destination.  Those
# mutations therefore replay ACCEPT and are asserted as ACCEPT here, so
# a kernel that starts refusing one moves its row and this test names it;
# the Mesa fill plan, memory contract, and submission identity are their
# sole owners, and r3v-native-fill-route refuses each of them in process.
#
# R3V_CS_TRACK_REPLAY_TOOL names the full CS parser/tracker replay built
# from the Linux radeon source tree; an unset tool skips the test.
#
# Usage: run_rb2d_fill_submit_object_replay.sh <loader-fill-application>
#            <rb2d-fill-arming-runner>

set -eu

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; RB2D fill submit-object replay" \
         "not run" >&2
    exit 77
fi
if [ ! -x "${R3V_CS_TRACK_REPLAY_TOOL}" ]; then
    echo "replay tool ${R3V_CS_TRACK_REPLAY_TOOL} is not executable" >&2
    exit 1
fi
for tool in b3sum python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "$tool unavailable; RB2D fill submit-object replay not run" >&2
        exit 77
    fi
done
if [ "$#" -ne 2 ]; then
    echo "usage: $0 <loader-fill-application> <rb2d-fill-arming-runner>" >&2
    exit 2
fi
application="$1"
runner="$2"

workdir=$(mktemp -d)
trap 'rm -rf "${workdir}"' EXIT
evidence="${workdir}/evidence"
mkdir "${evidence}"

# The declaration the armed submission needs, from the runner's independent
# emission over a fixture srcversion the shim presents.
fixture_srcversion=FIXTURESRCVERSION0000000
mkdir -p "${workdir}/sys/module/radeon"
echo "${fixture_srcversion}" > "${workdir}/sys/module/radeon/srcversion"
runner_report=$(R3V_NATIVE_RUNNER_SYSFS_ROOT="${workdir}/sys" \
    R3V_NATIVE_RUNNER_DESTINATION_HANDLE=1 \
    "${runner}" --emit-ib "${workdir}/reference-ib.bin" "${workdir}" || :)
ib_digest=$(printf '%s\n' "${runner_report}" | sed -n 's/^ib_blake3=//p')
identity=$(printf '%s\n' "${runner_report}" | \
    sed -n 's/^fill_identity_blake3=//p')
if [ -z "${ib_digest}" ] || [ -z "${identity}" ]; then
    echo "runner report carries no digest or identity" >&2
    exit 1
fi

R3V_DRM_SHIM_SUBSYSTEM_ID=1028:022a \
R3V_DRM_SHIM_DMI_PRODUCT_NAME="Vostro 1000" \
R3V_DRM_SHIM_MODULE_SRCVERSION="${fixture_srcversion}" \
R3V_NATIVE_EXECUTION_POLICY=gpu_only \
R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL=1 \
R3V_NATIVE_MANIFEST_DIR="${evidence}" \
R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1 \
R3V_NATIVE_AUTHORIZED_IB_BLAKE3="${ib_digest}" \
R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE="$(uname -r)" \
R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION="${fixture_srcversion}" \
R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3="${identity}" \
R3V_LOADER_FILL_EXPECT=submitted \
"${application}" > "${workdir}/application.txt"
cat "${workdir}/application.txt"

for artifact in ib.bin relocs.bin manifest.json submit_relocs.bin \
                submit_manifest.json attempt.token; do
    if [ ! -f "${evidence}/${artifact}" ]; then
        echo "retained artifact ${artifact} is missing" >&2
        exit 1
    fi
done

# The semantic cell, the submit object, and the runner's emission name one
# IB, and the retained file hashes to it under an independent hasher.
cell_digest=$(sed -n 's/.*"ib_blake3": "\([0-9a-f]*\)".*/\1/p' \
    "${evidence}/manifest.json")
submit_digest=$(sed -n 's/.*"ib_blake3": "\([0-9a-f]*\)".*/\1/p' \
    "${evidence}/submit_manifest.json")
if [ -z "${cell_digest}" ] || [ "${cell_digest}" != "${submit_digest}" ] || \
   [ "${cell_digest}" != "${ib_digest}" ]; then
    echo "manifest digests disagree: cell '${cell_digest}' submit" \
         "'${submit_digest}' runner '${ib_digest}'" >&2
    exit 1
fi
file_digest=$(b3sum --no-names "${evidence}/ib.bin")
if [ "${file_digest}" != "${cell_digest}" ]; then
    echo "retained ib.bin hashes to ${file_digest}, manifest declares" \
         "${cell_digest}" >&2
    exit 1
fi
if ! cmp -s "${evidence}/ib.bin" "${workdir}/reference-ib.bin"; then
    echo "retained ib.bin differs from the runner's emission" >&2
    exit 1
fi
submit_reloc_digest=$(sed -n \
    's/.*"submit_relocs_blake3": "\([0-9a-f]*\)".*/\1/p' \
    "${evidence}/submit_manifest.json")
if [ -z "${submit_reloc_digest}" ] || \
   [ "$(b3sum --no-names "${evidence}/submit_relocs.bin")" != \
     "${submit_reloc_digest}" ]; then
    echo "retained submit_relocs.bin does not hash to its manifest" >&2
    exit 1
fi

# The submit manifest decoded together with the relocation bytes: two
# entries, the 64 KiB destination at index 0 and the four-byte completion
# at index 1, both GTT write-bound with no read domain, handles distinct,
# the three-chunk CS layout, and the bundle the replay tool consumes.
validate_submit_manifest() {
    python3 - "$1" "$2" "${evidence}/ib.bin" "${3:-}" <<'PY'
import json
import struct
import sys

manifest_path, reloc_path, ib_path, bundle_path = sys.argv[1:5]
DESTINATION_BYTES = 65536
COMPLETION_BYTES = 4
GTT = 2

with open(manifest_path, encoding="utf-8") as stream:
    manifest = json.load(stream)
with open(reloc_path, "rb") as stream:
    reloc_bytes = stream.read()
with open(ib_path, "rb") as stream:
    ib_bytes = stream.read()

if manifest.get("object") != "submit-object":
    raise SystemExit("submit manifest object is not submit-object")
if manifest.get("reloc_count") != 2:
    raise SystemExit("submit manifest relocation count is not two")
ib_dwords = manifest.get("ib_dwords")
if not isinstance(ib_dwords, int) or len(ib_bytes) != ib_dwords * 4:
    raise SystemExit("submit manifest IB length differs from ib.bin")
rows = manifest.get("bo_table")
if not isinstance(rows, list) or len(rows) != 2:
    raise SystemExit("submit manifest bo_table does not carry two rows")
destination, completion = rows
for index, row, size, role in ((0, destination, DESTINATION_BYTES, None),
                               (1, completion, COMPLETION_BYTES,
                                "completion")):
    if row.get("reloc_index") != index or row.get("size") != size or \
            row.get("read_domains") != 0 or row.get("write_domain") != GTT:
        raise SystemExit(f"bo_table row {index} differs from the fill "
                         f"contract: {row}")
    if role is not None and row.get("role") != role:
        raise SystemExit(f"bo_table row {index} role is not {role}")
handles = [destination.get("handle"), completion.get("handle")]
if any(not isinstance(h, int) or h == 0 for h in handles) or \
        handles[0] == handles[1]:
    raise SystemExit("bo_table handles are not two distinct live objects")
if len(reloc_bytes) != 32:
    raise SystemExit("submit_relocs.bin is not two drm_radeon_cs_reloc")
entries = list(struct.Struct("<4I").iter_unpack(reloc_bytes))
for index, (handle, read_domains, write_domain, flags) in enumerate(entries):
    if (handle, read_domains, write_domain, flags) != (handles[index], 0, GTT,
                                                       0):
        raise SystemExit("submit relocation bytes differ from the bo_table")
if manifest.get("cs_flags") != [1, 0, 0]:
    raise SystemExit("submit manifest cs_flags differ from the GFX contract")
if manifest.get("chunks") != [{"id": 1, "length_dw": 8},
                              {"id": 2, "length_dw": ib_dwords},
                              {"id": 3, "length_dw": 3}]:
    raise SystemExit("submit manifest chunks differ from the CS layout")
if bundle_path:
    with open(bundle_path, "w", encoding="utf-8") as bundle:
        bundle.write("family rs480\n")
        for index, row in enumerate(rows):
            bundle.write(f"bo {index} role={row['role']} size={row['size']} "
                         f"read_domains=0x{row['read_domains']:x} "
                         f"write_domain=0x{row['write_domain']:x}\n")
print(f"submit object: destination handle {handles[0]} completion handle "
      f"{handles[1]}, {ib_dwords} dwords")
PY
}
if ! validate_submit_manifest "${evidence}/submit_manifest.json" \
        "${evidence}/submit_relocs.bin" "${workdir}/bundle.txt"; then
    echo "submit manifest does not bind the retained submit object" >&2
    exit 1
fi
# The validator earns its verdict: a swapped relocation chunk and a
# resized completion row each refuse.
{
    dd if="${evidence}/submit_relocs.bin" bs=16 skip=1 count=1 2>/dev/null
    dd if="${evidence}/submit_relocs.bin" bs=16 count=1 2>/dev/null
} > "${workdir}/swapped_relocs.bin"
if validate_submit_manifest "${evidence}/submit_manifest.json" \
        "${workdir}/swapped_relocs.bin" 2>/dev/null; then
    echo "manifest validator accepted swapped relocation entries" >&2
    exit 1
fi
sed 's/"size": 4, "role": "completion"/"size": 8, "role": "completion"/' \
    "${evidence}/submit_manifest.json" > "${workdir}/bad_completion.json"
if cmp -s "${evidence}/submit_manifest.json" "${workdir}/bad_completion.json"; then
    echo "completion-size calibration did not mutate the manifest" >&2
    exit 1
fi
if validate_submit_manifest "${workdir}/bad_completion.json" \
        "${evidence}/submit_relocs.bin" 2>/dev/null; then
    echo "manifest validator accepted a resized completion row" >&2
    exit 1
fi

# The stream's own layout, found by walking it rather than hard-coded, so
# the mutations below name the dword they rewrite by register.
ib="${evidence}/ib.bin"
find_payload() {
    python3 - "${ib}" "$1" "${2:-1}" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
reg = int(sys.argv[2], 0)
which = int(sys.argv[3])
words = struct.unpack('<%dI' % (len(data) // 4), data)
i = 0
seen = 0
while i < len(words):
    header = words[i]
    ptype = (header >> 30) & 3
    count = (header >> 16) & 0x3FFF
    if ptype == 0:
        base = (header & 0x1FFF) << 2
        one_reg = (header >> 15) & 1
        for k in range(count + 1):
            here = base if one_reg else base + 4 * k
            if here == reg:
                seen += 1
                if seen == which:
                    print(i + 1 + k)
                    sys.exit(0)
    i += count + 2
sys.exit(1)
PY
}
dst_pitch_offset=0x142c
sc_bottom_right=0x16f0
dp_cntl=0x16c0
dst_y_x=0x1438
dst_width_height=0x1598
dstcache_ctlstat=0x1714
wait_until=0x1720
reloc_payload=$(( $(find_payload ${dst_pitch_offset}) + 2 ))
reloc_header=$(( reloc_payload - 1 ))
dp_cntl_header=$(( $(find_payload ${dp_cntl}) - 1 ))
dstcache_header=$(( $(find_payload ${dstcache_ctlstat}) - 1 ))
second_rect_y_x=$(find_payload ${dst_y_x} 2)
second_rect_size=$(find_payload ${dst_width_height} 2)
ib_dwords=$(( $(wc -c < "${ib}") / 4 ))
last_wait=$(( $(find_payload ${wait_until}) - 1 ))
if [ "${last_wait}" -ne $(( ib_dwords - 2 )) ]; then
    echo "the final WAIT_UNTIL is not the stream's last packet" >&2
    exit 1
fi

# The known-good replay, with every register class the stream carries
# admitted by name in the parser's own trace.
"${R3V_CS_TRACK_REPLAY_TOOL}" --verbose "${workdir}/bundle.txt" "${ib}" \
    > "${workdir}/good.txt" 2>&1
cat "${workdir}/good.txt" | tail -1
case "$(tail -1 "${workdir}/good.txt")" in
    *"relocs=2 "*"verdict=ACCEPT-NO-DRAW"*) ;;
    *)
        echo "retained RB2D fill submit object did not replay" \
             "ACCEPT-NO-DRAW with two relocation slots" >&2
        exit 1
        ;;
esac
admitted() {
    label="$1"; shift
    for reg in "$@"; do
        if ! grep -q -i "reg ${reg} passes" "${workdir}/good.txt"; then
            echo "${label}: register ${reg} was not admitted by the parser" >&2
            exit 1
        fi
    done
    echo "admitted: ${label}"
}
if ! grep -q "reloc -> entry 0" "${workdir}/good.txt"; then
    echo "DST_PITCH_OFFSET relocation did not resolve to entry 0" >&2
    exit 1
fi
echo "admitted: DST_PITCH_OFFSET relocation -> entry 0"
admitted "scissor registers" 0x16ec 0x16f0 0x16e8
admitted "brush and master-control registers" 0x146c 0x16c0 0x16cc 0x147c
admitted "rectangle registers" 0x1438 0x1598
admitted "destination cache flush" 0x1714
admitted "wait state" 0x1720

# Every mutation, held to the class the kernel-derived parser gives it.
verdict_of() {
    out=$("$@" 2>&1) && :
    printf '%s\n' "${out}" | grep -o 'verdict=[A-Z-]*' | head -1
}
expect() {
    want="$1"; label="$2"; shift 2
    got=$(verdict_of "$@")
    printf '%-10s %-48s %s\n' "${want}" "${label}" "${got}"
    if [ "${got}" != "verdict=${want}" ]; then
        echo "${label}: expected verdict=${want}, got '${got}'" >&2
        exit 1
    fi
}
T="${R3V_CS_TRACK_REPLAY_TOOL}"
B="${workdir}/bundle.txt"
echo "parser-owned mutations (REJECT):"
expect REJECT "missing relocation (NOP -> PACKET0 SC_TOP_LEFT)" \
    "$T" --set-dword "${reloc_header}=0x000005bb" \
         --set-dword "${reloc_payload}=0" "$B" "${ib}"
head -c $(( (ib_dwords - 1) * 4 )) "${ib}" > "${workdir}/truncated.bin"
expect REJECT "truncated packet (final dword dropped)" \
    "$T" "$B" "${workdir}/truncated.bin"
{ cat "${ib}"; printf '\0\0\0\0'; } > "${workdir}/extra.bin"
expect REJECT "extra packet payload appended" "$T" "$B" "${workdir}/extra.bin"
expect REJECT "wrong destination-cache register (0x1430)" \
    "$T" --set-dword "${dstcache_header}=0x0000050c" "$B" "${ib}"
expect REJECT "forbidden register in place of DP_CNTL (0x1430)" \
    "$T" --set-dword "${dp_cntl_header}=0x0000050c" "$B" "${ib}"

echo "kernel-transparent mutations (ACCEPT-NO-DRAW; Mesa-owned refusals):"
expect ACCEPT-NO-DRAW "wrong relocation target (completion object)" \
    "$T" --set-dword "${reloc_payload}=1" "$B" "${ib}"
{
    echo "family rs480"
    sed -n 's/^bo 1 /bo 0 /p' "$B"
    sed -n 's/^bo 0 /bo 1 /p' "$B"
} > "${workdir}/swapped-bundle.txt"
expect ACCEPT-NO-DRAW "wrong relocation order (swapped bundle)" \
    "$T" "${workdir}/swapped-bundle.txt" "${ib}"
expect ACCEPT-NO-DRAW "wrong pitch field (pitch 0)" \
    "$T" --set-reg "${dst_pitch_offset}=0x00000000" "$B" "${ib}"
expect ACCEPT-NO-DRAW "wrong destination base (offset past the object)" \
    "$T" --set-reg "${dst_pitch_offset}=0x0100ffff" "$B" "${ib}"
expect ACCEPT-NO-DRAW "rectangle past the safe scissor (y 0x2000)" \
    "$T" --set-dword "${second_rect_y_x}=0x20000000" "$B" "${ib}"
expect ACCEPT-NO-DRAW "rectangle past the object (height 0x1fff)" \
    "$T" --set-dword "${second_rect_size}=0x00401fff" "$B" "${ib}"
expect ACCEPT-NO-DRAW "scissor widened past the safe bound" \
    "$T" --set-reg "${sc_bottom_right}=0x20002000" "$B" "${ib}"
expect ACCEPT-NO-DRAW "bad WAIT_UNTIL (0xffffffff)" \
    "$T" --set-reg "${wait_until}=0xffffffff" "$B" "${ib}"
head -c $(( last_wait * 4 )) "${ib}" > "${workdir}/no-wait.bin"
expect ACCEPT-NO-DRAW "stream truncated before the final wait" \
    "$T" "$B" "${workdir}/no-wait.bin"
expect ACCEPT-NO-DRAW "destination object undersized (1024 bytes)" \
    "$T" --set-bo-size 0=1024 "$B" "${ib}"

echo "rb2d fill submit-object replay: retained bytes ACCEPT-NO-DRAW;" \
     "parser-owned mutations reject; the 2D destination geometry passes" \
     "the kernel unchecked"
