#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Keep r3v compute route membership, synthesis, and replay in lockstep."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


FIELD_RE = re.compile(
    r"\bpl\s*->\s*([A-Za-z_][A-Za-z0-9_]*)\s*\.\s*is_\1\b"
)


def mask_c_source(source: str) -> str:
    """Mask comments and literals while preserving byte offsets and newlines."""

    out = list(source)
    i = 0
    state = "code"
    quote = ""

    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""

        if state == "code":
            if ch == "/" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "line_comment"
                continue
            if ch == "/" and nxt == "*":
                out[i] = out[i + 1] = " "
                i += 2
                state = "block_comment"
                continue
            if ch in {'"', "'"}:
                quote = ch
                out[i] = " "
                i += 1
                state = "literal"
                continue
            i += 1
            continue

        if state == "line_comment":
            if ch == "\n":
                state = "code"
            else:
                out[i] = " "
            i += 1
            continue

        if state == "block_comment":
            if ch == "*" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "code"
                continue
            if ch != "\n":
                out[i] = " "
            i += 1
            continue

        # String or character literal.
        if ch == "\\":
            out[i] = " "
            if i + 1 < len(source):
                if source[i + 1] != "\n":
                    out[i + 1] = " "
                i += 2
            else:
                i += 1
            continue
        if ch == quote:
            out[i] = " "
            i += 1
            state = "code"
            continue
        if ch != "\n":
            out[i] = " "
        i += 1

    if state in {"block_comment", "literal"}:
        raise ValueError(f"unterminated C lexical state: {state}")
    return "".join(out)


def matching_delimiter(masked: str, start: int, opening: str, closing: str) -> int:
    depth = 0
    for i in range(start, len(masked)):
        ch = masked[i]
        if ch == opening:
            depth += 1
        elif ch == closing:
            depth -= 1
            if depth == 0:
                return i
    raise ValueError(f"unmatched {opening!r} at byte offset {start}")


def extract_function(source: str, name: str) -> str:
    masked = mask_c_source(source)
    for match in re.finditer(rf"\b{re.escape(name)}\s*\(", masked):
        open_paren = masked.find("(", match.start())
        close_paren = matching_delimiter(masked, open_paren, "(", ")")
        cursor = close_paren + 1
        while cursor < len(masked) and masked[cursor].isspace():
            cursor += 1
        if cursor >= len(masked) or masked[cursor] != "{":
            continue  # prototype or call site
        close_brace = matching_delimiter(masked, cursor, "{", "}")
        return source[match.start() : close_brace + 1]
    raise ValueError(f"function definition not found: {name}")


def fields_in_function(source: str, name: str) -> set[str]:
    function = extract_function(source, name)
    return set(FIELD_RE.findall(mask_c_source(function)))


def format_set(values: set[str]) -> str:
    return "{" + ", ".join(sorted(values)) + "}"


def report_delta(left_name: str, left: set[str], right_name: str, right: set[str]) -> bool:
    if left == right:
        return False
    print(f"{left_name} != {right_name}", file=sys.stderr)
    print(
        f"  only in {left_name}: {format_set(left - right)}",
        file=sys.stderr,
    )
    print(
        f"  only in {right_name}: {format_set(right - left)}",
        file=sys.stderr,
    )
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "source_dir",
        type=Path,
        help="src/amd/r300/vulkan/r3v source directory",
    )
    args = parser.parse_args()

    root = args.source_dir.resolve()
    files = {
        "membership": (root / "r3v_pipeline.h", "r3v_pipeline_matched_raster_verb"),
        "synthesis": (root / "r3v_pipeline.c", "r3v_synthesize_compute_shaders"),
        "dispatch": (root / "r3v_queue.c", "r3v_replay_dispatch"),
    }

    try:
        routes = {
            label: fields_in_function(path.read_text(encoding="utf-8"), function)
            for label, (path, function) in files.items()
        }
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"r3v compute verb reachability: {exc}", file=sys.stderr)
        return 2

    failed = False
    failed |= report_delta(
        "membership", routes["membership"], "synthesis", routes["synthesis"]
    )
    failed |= report_delta(
        "membership", routes["membership"], "dispatch", routes["dispatch"]
    )
    failed |= report_delta(
        "synthesis", routes["synthesis"], "dispatch", routes["dispatch"]
    )

    if failed:
        return 1

    print(
        "r3v compute verb reachability: "
        f"{len(routes['membership'])} routes agree across membership, synthesis, and dispatch"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
