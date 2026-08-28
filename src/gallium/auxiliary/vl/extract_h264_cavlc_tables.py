#!/usr/bin/env python3
"""Extract the H.264 CAVLC VLC tables from an ITU-T specification PDF.

The Mesa-native CAVLC residual decoder consumes the coeff_token, total_zeros,
and run_before tables from ITU-T H.264.  This generator parses those tables
from the pinned specification text and emits the checked-in C header.  A
standalone Mesa checkout therefore regenerates the table without importing a
tool from another repository.

Usage: extract_h264_cavlc_tables.py <spec.pdf> > vl_h264_cavlc_tables.h
"""

import re
import subprocess
import sys


def pdf_lines(path):
    text = subprocess.run(
        ["pdftotext", "-layout", path, "-"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return text.splitlines()


def columns(line):
    """Split a table row while retaining spaces inside grouped codewords."""
    return [c.replace(" ", "") for c in re.split(r" {2,}", line.strip()) if c]


def bits_to_entry(bitstr, value):
    return int(bitstr, 2), len(bitstr), value


def section(lines, start_marker, stop_markers):
    """Return the last table section between the named markers."""
    start_index = None
    for index, line in enumerate(lines):
        if start_marker in line:
            start_index = index
    if start_index is None:
        return []

    out = []
    for line in lines[start_index + 1 :]:
        if any(stop in line for stop in stop_markers):
            break
        out.append(line)
    return out


def parse_coeff_token(lines):
    """Parse Table 9-5 into the four finite VLC context tables."""
    contexts = {"nc0": [], "nc2": [], "nc4": [], "chroma": []}
    # Column 5 is the fixed six-bit 8 <= nC form and stays in C logic.
    col_to_context = {2: "nc0", 3: "nc2", 4: "nc4", 6: "chroma"}
    for line in lines:
        cols = columns(line)
        if len(cols) < 7 or not cols[0].isdigit() or not cols[1].isdigit():
            continue
        trailing_ones, total_coeff = int(cols[0]), int(cols[1])
        if trailing_ones > 3 or total_coeff > 16:
            continue
        for column, context in col_to_context.items():
            code = cols[column]
            if code and set(code) <= {"0", "1"}:
                contexts[context].append(
                    bits_to_entry(code, (total_coeff << 2) | trailing_ones)
                )
    return contexts


def parse_total_zeros(lines, column_labels):
    """Parse one total_zeros table keyed by its TotalCoeff column."""
    tables = {}
    column_count = len(column_labels)
    for line in lines:
        cols = columns(line)
        if not cols or not cols[0].isdigit() or cols == column_labels:
            continue
        total_zeros = int(cols[0])
        if total_zeros > 15:
            continue
        for offset, code in enumerate(cols[1 : 1 + column_count]):
            if code and set(code) <= {"0", "1"}:
                tables.setdefault(offset, []).append(bits_to_entry(code, total_zeros))
    return tables


def parse_run_before(lines):
    """Parse Table 9-10 by aligning codewords with its seven headers."""
    labels = ["1", "2", "3", "4", "5", "6", ">6"]
    header = next(line for line in lines if line.split()[:7] == labels)
    positions, cursor = [], 0
    for label in labels:
        cursor = header.index(label, cursor)
        positions.append(cursor)
        cursor += len(label)

    tables = {}
    for line in lines:
        fields = line.split()
        if not fields or not fields[0].isdigit() or fields[:7] == labels:
            continue
        run_before = int(fields[0])
        if run_before > 14:
            continue
        for match in re.finditer(r"[01]+", line):
            if match.start() < positions[0] - 3:
                continue
            column = min(
                range(7), key=lambda index: abs(positions[index] - match.start())
            )
            tables.setdefault(column, []).append(
                bits_to_entry(match.group(), run_before)
            )
    return tables


def emit_entries(name, entries):
    print(f"static const struct vl_h264_vlc {name}[] = {{")
    for code, length, value in entries:
        print(f"   {{ 0x{code:04x}, {length:2d}, {value:3d} }},")
    print("};")


def emit_table_array(name, members):
    print(f"static const struct vl_h264_vlc_table {name}[] = {{")
    for entries_name, count in members:
        if entries_name is None:
            print("   { NULL, 0 },")
        else:
            print(f"   {{ {entries_name}, {count} }},")
    print("};")


def emit_header(lines):
    coeff = parse_coeff_token(
        section(lines, "Table 9-5", ["9.2.2", "Parsing process for level"])
    )
    total_zeros_1_7 = parse_total_zeros(
        section(lines, "Table 9-7", ["Table 9-8"]),
        [str(number) for number in range(1, 8)],
    )
    total_zeros_8_15 = parse_total_zeros(
        section(lines, "Table 9-8", ["Table 9-9"]),
        [str(number) for number in range(8, 16)],
    )
    total_zeros_chroma = parse_total_zeros(
        section(lines, "Table 9-9", ["Table 9-10"]),
        [str(number) for number in range(1, 4)],
    )
    run_before = parse_run_before(section(lines, "Table 9-10", ["9.2.4", "Combining"]))

    print("/*")
    print(" * SPDX-License-Identifier: MIT")
    print(" *")
    print(" * Generated by " "src/gallium/auxiliary/vl/extract_h264_cavlc_tables.py")
    print(" * from the ITU-T H.264 spec PDF (Tables 9-5, 9-7, 9-8, 9-9, 9-10).")
    print(" * Do not edit by hand; regenerate and diff.")
    print(" */")
    print()
    print("#ifndef vl_h264_cavlc_tables_h")
    print("#define vl_h264_cavlc_tables_h")
    print()
    print("#include <stdint.h>")
    print()
    print("struct vl_h264_vlc {")
    print("   uint16_t code;")
    print("   uint8_t len;")
    print("   int16_t value;")
    print("};")
    print()
    print("struct vl_h264_vlc_table {")
    print("   const struct vl_h264_vlc *entries;")
    print("   unsigned count;")
    print("};")
    print()

    for context in ("nc0", "nc2", "nc4", "chroma"):
        emit_entries(f"coeff_token_{context}", coeff[context])
    print()
    emit_table_array(
        "vl_h264_coeff_token_tables",
        [
            (f"coeff_token_{context}", len(coeff[context]))
            for context in ("nc0", "nc2", "nc4", "chroma")
        ],
    )
    print()

    members = [(None, 0)]
    for total_coeff in range(1, 16):
        entries = (
            total_zeros_1_7.get(total_coeff - 1)
            if total_coeff <= 7
            else total_zeros_8_15.get(total_coeff - 8)
        )
        name = f"total_zeros_4x4_{total_coeff}"
        emit_entries(name, entries)
        members.append((name, len(entries)))
    print()
    emit_table_array("vl_h264_total_zeros_4x4", members)
    print()

    chroma_members = [(None, 0)]
    for total_coeff in range(1, 4):
        entries = total_zeros_chroma.get(total_coeff - 1)
        name = f"total_zeros_chroma_{total_coeff}"
        emit_entries(name, entries)
        chroma_members.append((name, len(entries)))
    print()
    emit_table_array("vl_h264_total_zeros_chroma", chroma_members)
    print()

    run_members = [(None, 0)]
    for zeros_left in range(1, 8):
        entries = run_before.get(zeros_left - 1)
        name = f"run_before_{zeros_left}"
        emit_entries(name, entries)
        run_members.append((name, len(entries)))
    print()
    emit_table_array("vl_h264_run_before", run_members)
    print()
    print("#endif /* vl_h264_cavlc_tables_h */")


def main():
    if len(sys.argv) == 2 and sys.argv[1] in {"-h", "--help"}:
        print(f"usage: {sys.argv[0]} <spec.pdf>")
        return 0
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <spec.pdf>", file=sys.stderr)
        return 2
    emit_header(pdf_lines(sys.argv[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
