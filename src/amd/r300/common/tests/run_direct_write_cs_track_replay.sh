#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Full kernel CS-parser replay of the 2D solid-fill direct-write cell.
#
# R3V_CS_TRACK_REPLAY_TOOL names replay_r300_cs_track, built from the
# Linux radeon source tree.  The known-good arm proves the parser admits
# the exact control stream with its one DST_PITCH_OFFSET relocation; the
# known-bad arms prove the acceptance is decided by the stream rather than
# by the tool: a starved relocation, a relocation index past the chunk, a
# truncated final packet, and a register redirected onto a
# flagged-and-unnamed number each reject.  An unset variable is an absent
# configuration and skips.
#
# Usage: run_direct_write_cs_track_replay.sh <r300_direct_write_manifest>

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <r300_direct_write_manifest-binary>" >&2
    exit 1
fi

if [ -z "${R3V_CS_TRACK_REPLAY_TOOL:-}" ]; then
    echo "R3V_CS_TRACK_REPLAY_TOOL unset; direct-write replay not run" >&2
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

# One buffer object: the color destination the DST_PITCH_OFFSET relocation
# binds, in the domain and size the transport allocates.
cat > "${workdir}/bundle.txt" <<'BUNDLE'
family rs480
bo 0 role=color size=65536 read_domains=0x0 write_domain=0x2
BUNDLE

good=$("${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" \
    "${workdir}/ib.bin")
echo "${good}"
# The control carries no draw packet, so the parser's clean verdict for it
# is acceptance with the one destination relocation consumed.
case "${good}" in
    *"relocs=1"*ACCEPT*) ;;
    *)
        echo "control cell did not replay as accepted with one" \
             "relocation" >&2
        exit 1
        ;;
esac

# expect_reject NAME FILE: the mutated stream must be refused.
expect_reject() {
    if "${R3V_CS_TRACK_REPLAY_TOOL}" "${workdir}/bundle.txt" "$2" \
        >/dev/null 2>&1; then
        echo "known-bad arm $1 was accepted" >&2
        exit 1
    fi
    echo "  reject: $1"
}

ib_bytes=$(wc -c < "${workdir}/ib.bin")
ib_dwords=$((ib_bytes / 4))

# Starved relocation: drop the two-dword NOP that follows the
# DST_PITCH_OFFSET write (dwords 2 and 3 of the cell).
{
    dd if="${workdir}/ib.bin" bs=4 count=2 2>/dev/null
    dd if="${workdir}/ib.bin" bs=4 skip=4 2>/dev/null
} > "${workdir}/no-reloc.bin"
expect_reject "starved relocation" "${workdir}/no-reloc.bin"

# Relocation index past the chunk: the NOP payload at dword 3 names entry
# dword 64 where the one-BO chunk holds four.
{
    dd if="${workdir}/ib.bin" bs=4 count=3 2>/dev/null
    printf '\100\000\000\000'
    dd if="${workdir}/ib.bin" bs=4 skip=4 2>/dev/null
} > "${workdir}/reloc-past-chunk.bin"
expect_reject "relocation index past chunk" "${workdir}/reloc-past-chunk.bin"

# Truncated final packet: the last dword removed leaves the WAIT_UNTIL
# header claiming a payload past the chunk end.
dd if="${workdir}/ib.bin" of="${workdir}/truncated.bin" bs=4 \
    count=$((ib_dwords - 1)) 2>/dev/null
expect_reject "truncated final packet" "${workdir}/truncated.bin"

# Register redirected onto flagged-and-unnamed space: the DST_PITCH_OFFSET
# header (dword 0) rewritten to register 0x1430, DST_PITCH_OFFSET's
# neighbor, which the safe bitmap flags and r300_packet0_check does not
# name, so the default arm rejects.
{
    printf '\014\005\000\000'
    dd if="${workdir}/ib.bin" bs=4 skip=1 2>/dev/null
} > "${workdir}/unnamed-register.bin"
expect_reject "flagged-and-unnamed register" "${workdir}/unnamed-register.bin"

echo "run_direct_write_cs_track_replay: the control cell parses clean and" \
     "every known-bad arm rejects"
