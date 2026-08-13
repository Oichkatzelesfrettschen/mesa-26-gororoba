#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import argparse
import json
import re
import struct
import sys
from pathlib import Path


HEADER_STRUCT = struct.Struct("<8sIIIIIIIII64s64s64s16I")
MAGIC = b"R3RKIB1\0"
ENDIAN_MARKER = 0x01020304


PACKET3_NAMES = {
    0x1000: "PKT3_NOP",
    0x2800: "R300_PACKET3_3D_DRAW_VBUF",
    0x2900: "R300_PACKET3_3D_DRAW_IMMD",
    0x2A00: "R300_PACKET3_3D_DRAW_INDX",
    0x2F00: "R300_PACKET3_3D_LOAD_VBPNTR",
    0x3200: "R300_PACKET3_3D_CLEAR_ZMASK",
    0x3300: "R300_PACKET3_INDX_BUFFER",
    0x3400: "R300_PACKET3_3D_DRAW_VBUF_2",
    0x3500: "R300_PACKET3_3D_DRAW_IMMD_2",
    0x3600: "R300_PACKET3_3D_DRAW_INDX_2",
    0x3700: "R300_PACKET3_3D_CLEAR_HIZ",
    0x3800: "R300_PACKET3_3D_CLEAR_CMASK",
    0x3900: "R300_PACKET3_3D_DRAW_128",
    0x9B00: "R300_PACKET3_BITBLT_MULTI",
}


# r300 PACKET0 register writes that reference a BO the radeon kernel CS parser
# binds at DRM_RADEON_CS submit: the color and depth render-target offsets and
# the texture base offsets.  Matched by exact name family, not a blanket
# "OFFSET": R300_SE_VPORT_*OFFSET is a float viewport offset (0x1d9c+),
# R300_GA_OFFSET / R300_SU_DEPTH_OFFSET / R300_US_CODE_OFFSET are immediate
# pixel/poly/shader offsets -- none of those are BO relocation targets.
ADDRESS_REGISTER_RE = re.compile(
    r"^R300_(RB3D_COLOROFFSET[0-9]+|ZB_DEPTHOFFSET|RB3D_DEPTHOFFSET|TX_OFFSET_[0-9]+)$")

# PACKET3 opcodes whose payload carries BO base references the kernel binds:
# 3D_LOAD_VBPNTR (vertex-buffer bases) and INDX_BUFFER (index-buffer base).
ADDRESS_PACKET3_OPCODES = {0x2F00, 0x3300}


def address_view_note(view):
    """Capture-stage label for a relocation-target field.

    The field references a BO the kernel binds at DRM_RADEON_CS submit; view
    records which capture stage this IB is from -- cpu = before the submit
    ioctl, gpu = after.  The label does not assert the value's form: comparing
    the same field across the two stages is what shows whether the kernel
    rewrote the dword in place (whether the captured value is a BO-relative
    offset or an absolute VA is capture- and kernel-path-dependent)."""
    if view == "cpu":
        return "reloc_target/pre-submit"
    if view == "gpu":
        return "reloc_target/post-submit"
    return "reloc_target/view-unknown"


def c_string(raw):
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def parse_header(blob):
    if len(blob) < HEADER_STRUCT.size:
        raise ValueError("file is smaller than the r300 trace IB header")
    unpacked = HEADER_STRUCT.unpack_from(blob)
    magic = unpacked[0]
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r}")
    if unpacked[3] != ENDIAN_MARKER:
        raise ValueError("unsupported endian marker")
    return {
        "schema_version": unpacked[1],
        "header_size": unpacked[2],
        "endian_marker": unpacked[3],
        "dword_count": unpacked[4],
        "pci_id": unpacked[5],
        "family": unpacked[6],
        "drm_major": unpacked[7],
        "drm_minor": unpacked[8],
        "drm_patchlevel": unpacked[9],
        "mesa_commit": c_string(unpacked[10]),
        "kernel_release": c_string(unpacked[11]),
        "run_id": c_string(unpacked[12]),
    }


def read_ib(path):
    blob = path.read_bytes()
    header = parse_header(blob)
    start = header["header_size"]
    end = start + header["dword_count"] * 4
    if len(blob) < end:
        raise ValueError("file ended before all IB dwords were present")
    dwords = list(struct.unpack_from(f"<{header['dword_count']}I", blob, start))
    return header, dwords


def load_register_names(mesa_root):
    names = {}
    reg_path = mesa_root / "src/amd/r300/common/r300_reg.h"
    if not reg_path.exists():
        return names

    define_re = re.compile(r"^#\s*define\s+(R[0-9A-Z_]+)\s+(0x[0-9A-Fa-f]+|\d+)\b")
    for line in reg_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = define_re.match(line)
        if not match:
            continue
        name, value = match.groups()
        try:
            numeric = int(value, 0)
        except ValueError:
            continue
        names.setdefault(numeric, name)
    return names


def decode_packet0(index, word, dwords, register_names, view):
    count = ((word >> 16) & 0x3FFF) + 1
    one_reg = bool(word & (1 << 15))
    start_reg = (word & 0x7FFF) << 2
    end = index + 1 + count
    values = dwords[index + 1:min(end, len(dwords))]
    writes = []
    for offset, value in enumerate(values):
        reg = start_reg if one_reg else start_reg + offset * 4
        name = register_names.get(reg, "")
        write = {
            "reg": f"0x{reg:04x}",
            "name": name,
            "value": f"0x{value:08x}",
        }
        if ADDRESS_REGISTER_RE.match(name):
            write["address"] = True
            write["view_note"] = address_view_note(view)
        writes.append(write)
    return {
        "index": index,
        "packet": "PACKET0",
        "header": f"0x{word:08x}",
        "count": count,
        "truncated": end > len(dwords),
        "one_reg": one_reg,
        "writes": writes,
        "next_index": min(end, len(dwords)),
    }


def decode_packet3(index, word, dwords, view):
    count = ((word >> 16) & 0x3FFF) + 1
    opcode = word & 0xFF00
    end = index + 1 + count
    payload = dwords[index + 1:min(end, len(dwords))]
    item = {
        "index": index,
        "packet": "PACKET3",
        "header": f"0x{word:08x}",
        "opcode": f"0x{opcode:04x}",
        "name": PACKET3_NAMES.get(opcode, ""),
        "count": count,
        "truncated": end > len(dwords),
        "payload": [f"0x{value:08x}" for value in payload],
        "next_index": min(end, len(dwords)),
    }
    if opcode in ADDRESS_PACKET3_OPCODES:
        item["carries_reloc_addresses"] = True
        item["view_note"] = address_view_note(view)
    return item


def decode_ib(dwords, register_names, view):
    decoded = []
    index = 0
    while index < len(dwords):
        word = dwords[index]
        packet_type = word & 0xC0000000
        if word == 0x80000000:
            decoded.append({
                "index": index,
                "packet": "PACKET2_NOP",
                "header": f"0x{word:08x}",
                "next_index": index + 1,
            })
            index += 1
        elif packet_type == 0:
            item = decode_packet0(index, word, dwords, register_names, view)
            decoded.append(item)
            index = max(item["next_index"], index + 1)
        elif packet_type == 0xC0000000:
            item = decode_packet3(index, word, dwords, view)
            decoded.append(item)
            index = max(item["next_index"], index + 1)
        else:
            decoded.append({
                "index": index,
                "packet": "UNKNOWN",
                "header": f"0x{word:08x}",
                "next_index": index + 1,
            })
            index += 1
    return decoded


def print_text(header, decoded, view):
    print(f"run_id {header['run_id']}")
    print(f"mesa {header['mesa_commit']}")
    print(f"kernel {header['kernel_release']}")
    print(f"pci_id 0x{header['pci_id']:04x}")
    print(f"dwords {header['dword_count']}")
    print(f"view {view}")
    for item in decoded:
        if item["packet"] == "PACKET0":
            trunc = " truncated=1" if item["truncated"] else ""
            print(f"{item['index']:05d}: PACKET0 count={item['count']} one_reg={int(item['one_reg'])}{trunc}")
            for write in item["writes"]:
                suffix = f" {write['name']}" if write["name"] else ""
                note = f"  [{write['view_note']}]" if write.get("address") else ""
                print(f"         {write['reg']}{suffix} = {write['value']}{note}")
        elif item["packet"] == "PACKET3":
            suffix = f" {item['name']}" if item["name"] else ""
            trunc = " truncated=1" if item["truncated"] else ""
            reloc = f"  [reloc-addresses: {item['view_note']}]" if item.get("carries_reloc_addresses") else ""
            print(f"{item['index']:05d}: PACKET3 {item['opcode']}{suffix} count={item['count']}{trunc}{reloc}")
            for offset, value in enumerate(item["payload"]):
                print(f"         +{offset:02d} {value}")
        else:
            print(f"{item['index']:05d}: {item['packet']} {item['header']}")


def find_mesa_root(script_path):
    for parent in script_path.resolve().parents:
        if (parent / "src/amd/r300/common/r300_reg.h").exists():
            return parent
    return Path.cwd()


def main():
    parser = argparse.ArgumentParser(description="Decode r300 trace IB dumps")
    parser.add_argument("ib", type=Path, help="pre_ib.bin or patched_ib.bin")
    parser.add_argument("--jsonl", action="store_true", help="emit one JSON object per packet")
    parser.add_argument("--mesa-root", type=Path, default=None)
    parser.add_argument(
        "--view", choices=["cpu", "gpu", "auto"], default="auto",
        help="address-field view: cpu=pre-reloc BO offsets, gpu=post-reloc "
             "absolute VAs, auto=infer from the filename (pre_ib/patched_ib)")
    args = parser.parse_args()

    # The capture writes pre_ib.bin before DRM_RADEON_CS (CPU view: the driver's
    # BO-relative offsets) and patched_ib.bin after the kernel relocates it (GPU
    # view: absolute VAs).  auto reads that from the filename; an unrecognised
    # name leaves the view unknown so address fields are flagged but not asserted.
    view = args.view
    if view == "auto":
        name = args.ib.name
        if "pre_ib" in name:
            view = "cpu"
        elif "patched_ib" in name:
            view = "gpu"
        else:
            view = "unknown"

    mesa_root = args.mesa_root or find_mesa_root(Path(__file__))
    header, dwords = read_ib(args.ib)
    decoded = decode_ib(dwords, load_register_names(mesa_root), view)

    if args.jsonl:
        print(json.dumps({"type": "header", "view": view, **header}, sort_keys=True))
        for item in decoded:
            item = dict(item)
            item.pop("next_index", None)
            print(json.dumps(item, sort_keys=True))
    else:
        print_text(header, decoded, view)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"r300_pm4_decode.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
