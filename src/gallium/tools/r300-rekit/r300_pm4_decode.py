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
}


def c_string(raw):
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def parse_header(blob):
    if len(blob) < HEADER_STRUCT.size:
        raise ValueError("file is smaller than the r300-rekit IB header")
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
    reg_path = mesa_root / "src/gallium/drivers/r300/r300_reg.h"
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


def decode_packet0(index, word, dwords, register_names):
    count = ((word >> 16) & 0x3FFF) + 1
    one_reg = bool(word & (1 << 15))
    start_reg = (word & 0x7FFF) << 2
    end = index + 1 + count
    values = dwords[index + 1:min(end, len(dwords))]
    writes = []
    for offset, value in enumerate(values):
        reg = start_reg if one_reg else start_reg + offset * 4
        writes.append({
            "reg": f"0x{reg:04x}",
            "name": register_names.get(reg, ""),
            "value": f"0x{value:08x}",
        })
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


def decode_packet3(index, word, dwords):
    count = ((word >> 16) & 0x3FFF) + 1
    opcode = word & 0xFF00
    end = index + 1 + count
    payload = dwords[index + 1:min(end, len(dwords))]
    return {
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


def decode_ib(dwords, register_names):
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
            item = decode_packet0(index, word, dwords, register_names)
            decoded.append(item)
            index = max(item["next_index"], index + 1)
        elif packet_type == 0xC0000000:
            item = decode_packet3(index, word, dwords)
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


def print_text(header, decoded):
    print(f"run_id {header['run_id']}")
    print(f"mesa {header['mesa_commit']}")
    print(f"kernel {header['kernel_release']}")
    print(f"pci_id 0x{header['pci_id']:04x}")
    print(f"dwords {header['dword_count']}")
    for item in decoded:
        if item["packet"] == "PACKET0":
            trunc = " truncated=1" if item["truncated"] else ""
            print(f"{item['index']:05d}: PACKET0 count={item['count']} one_reg={int(item['one_reg'])}{trunc}")
            for write in item["writes"]:
                suffix = f" {write['name']}" if write["name"] else ""
                print(f"         {write['reg']}{suffix} = {write['value']}")
        elif item["packet"] == "PACKET3":
            suffix = f" {item['name']}" if item["name"] else ""
            trunc = " truncated=1" if item["truncated"] else ""
            print(f"{item['index']:05d}: PACKET3 {item['opcode']}{suffix} count={item['count']}{trunc}")
            for offset, value in enumerate(item["payload"]):
                print(f"         +{offset:02d} {value}")
        else:
            print(f"{item['index']:05d}: {item['packet']} {item['header']}")


def find_mesa_root(script_path):
    for parent in script_path.resolve().parents:
        if (parent / "src/gallium/drivers/r300/r300_reg.h").exists():
            return parent
    return Path.cwd()


def main():
    parser = argparse.ArgumentParser(description="Decode r300-rekit IB dumps")
    parser.add_argument("ib", type=Path, help="pre_ib.bin or patched_ib.bin")
    parser.add_argument("--jsonl", action="store_true", help="emit one JSON object per packet")
    parser.add_argument("--mesa-root", type=Path, default=None)
    args = parser.parse_args()

    mesa_root = args.mesa_root or find_mesa_root(Path(__file__))
    header, dwords = read_ib(args.ib)
    decoded = decode_ib(dwords, load_register_names(mesa_root))

    if args.jsonl:
        print(json.dumps({"type": "header", **header}, sort_keys=True))
        for item in decoded:
            item = dict(item)
            item.pop("next_index", None)
            print(json.dumps(item, sort_keys=True))
    else:
        print_text(header, decoded)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"r300_pm4_decode.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
