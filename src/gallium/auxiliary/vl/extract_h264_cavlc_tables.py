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


class ExtractionError(ValueError):
    """The specification text does not contain a complete VLC domain."""


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


def is_table_caption(line, start_marker):
    """Identify a table caption while ignoring contents and TOC references."""
    caption = line.lstrip("\f ")
    if not caption.startswith(start_marker):
        return False
    remainder = caption[len(start_marker) :]
    if re.search(r"\.{4,}", remainder):
        return False
    return any(marker in remainder for marker in ("-", "–", "—")) or (
        "continued" in remainder.lower()
    )


def section(lines, start_marker, stop_markers):
    """Return every page of the table between its caption and section end.

    A PDF can repeat a table caption on continuation pages.  Caption
    detection skips the table of contents and prose references, then keeps
    collecting across each continuation until the syntax section that follows
    the complete table.
    """
    collecting = False
    out = []
    for line in lines:
        if is_table_caption(line, start_marker):
            collecting = True
            continue
        if not collecting:
            continue
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


def validate_table_cardinalities(
    coeff, total_zeros_1_7, total_zeros_8_15, total_zeros_chroma, run_before
):
    """Reject a partial table before emitting a silently incomplete header."""
    expected_coeff = {"nc0": 62, "nc2": 62, "nc4": 62, "chroma": 14}
    for context, expected in expected_coeff.items():
        actual = len(coeff.get(context, ()))
        if actual != expected:
            raise ExtractionError(
                f"coeff_token {context} has {actual} rows; expected {expected}"
            )

    for total_coeff in range(1, 8):
        expected = 17 - total_coeff
        actual = len(total_zeros_1_7.get(total_coeff - 1, ()))
        if actual != expected:
            raise ExtractionError(
                f"total_zeros 4x4 TotalCoeff {total_coeff} has {actual} rows; "
                f"expected {expected}"
            )

    for total_coeff in range(8, 16):
        expected = 17 - total_coeff
        actual = len(total_zeros_8_15.get(total_coeff - 8, ()))
        if actual != expected:
            raise ExtractionError(
                f"total_zeros 4x4 TotalCoeff {total_coeff} has {actual} rows; "
                f"expected {expected}"
            )

    for total_coeff in range(1, 4):
        expected = 5 - total_coeff
        actual = len(total_zeros_chroma.get(total_coeff - 1, ()))
        if actual != expected:
            raise ExtractionError(
                f"total_zeros chroma TotalCoeff {total_coeff} has {actual} rows; "
                f"expected {expected}"
            )

    for zeros_left in range(1, 7):
        expected = zeros_left + 1
        actual = len(run_before.get(zeros_left - 1, ()))
        if actual != expected:
            raise ExtractionError(
                f"run_before zerosLeft {zeros_left} has {actual} rows; "
                f"expected {expected}"
            )
    actual = len(run_before.get(6, ()))
    if actual != 15:
        raise ExtractionError(
            f"run_before zerosLeft > 6 has {actual} rows; expected 15"
        )


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
    validate_table_cardinalities(
        coeff,
        total_zeros_1_7,
        total_zeros_8_15,
        total_zeros_chroma,
        run_before,
    )

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


def selftest():
    """Exercise continuation-caption accumulation and cardinality refusal."""
    repeated_caption = [
        "Table 9-5 - contents................................................154",
        "\f Table 9-5 - coeff_token mapping",
        "first-page-row",
        "\f Table 9-5 (continued)",
        "second-page-row",
        "9.2.2    Parsing process for level information",
    ]
    rows = section(repeated_caption, "Table 9-5", ["9.2.2"])
    if rows != ["first-page-row", "second-page-row"]:
        raise ExtractionError(
            f"continuation-caption fixture returned unexpected rows: {rows!r}"
        )

    def synthetic_rows(count):
        return [(0, 1, value) for value in range(count)]

    coeff = {
        context: synthetic_rows(count)
        for context, count in (("nc0", 62), ("nc2", 62), ("nc4", 62), ("chroma", 14))
    }
    total_zeros_1_7 = {index: synthetic_rows(16 - index) for index in range(7)}
    total_zeros_8_15 = {index: synthetic_rows(9 - index) for index in range(8)}
    total_zeros_chroma = {index: synthetic_rows(4 - index) for index in range(3)}
    run_before = {index: synthetic_rows(index + 2) for index in range(6)}
    run_before[6] = synthetic_rows(15)
    validate_table_cardinalities(
        coeff,
        total_zeros_1_7,
        total_zeros_8_15,
        total_zeros_chroma,
        run_before,
    )
    coeff["nc0"].pop()
    try:
        validate_table_cardinalities(
            coeff,
            total_zeros_1_7,
            total_zeros_8_15,
            total_zeros_chroma,
            run_before,
        )
    except ExtractionError as error:
        if "coeff_token nc0" not in str(error):
            raise
    else:
        raise ExtractionError("partial coeff_token fixture was accepted")
    print(
        "selftest: continuation captions accumulate and incomplete table "
        "cardinalities fail"
    )
    return 0


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        return selftest()
    if len(sys.argv) == 2 and sys.argv[1] in {"-h", "--help"}:
        print(f"usage: {sys.argv[0]} <spec.pdf>")
        return 0
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <spec.pdf>", file=sys.stderr)
        return 2
    try:
        emit_header(pdf_lines(sys.argv[1]))
    except ExtractionError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
