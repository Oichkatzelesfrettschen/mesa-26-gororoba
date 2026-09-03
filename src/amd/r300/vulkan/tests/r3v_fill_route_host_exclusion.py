#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prove the public fill route reaches no host store for its own result.

A hardware claim is a claim about who produced the bytes.  The route's
provenance asserts ``host_semantic_node == false``; this checks the source
that assertion describes, so the claim rests on the code rather than on the
field that reports it.

Three properties, each failing by name:

1. ``r3v_native_fill_route.c`` calls no byte-moving or mapping function.
   The map is the boundary, not the store loop: a routed record that
   reached a mapping has already put the destination bytes under host
   control whatever it did next.

2. ``r3v_native_transfer.c`` tests ``gpu_routed`` for the fill record before
   it reaches ``map_memory``.  A skip placed after the mapping would leave
   the host holding the destination on every routed fill.

3. The route marks the record only after installing a stream, so a marked
   record and an installed IB move together.  A marked record with no
   stream would be a fill nobody performs.

The checker is calibrated on synthetic known-bad sources before it judges
the tree, so a rule that cannot fail is reported as such.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROUTE = Path("src/amd/r300/vulkan/r3v_native_fill_route.c")
TRANSFER = Path("src/amd/r300/vulkan/r3v_native_transfer.c")

# Functions that move or expose destination bytes on the host.  calloc and
# free allocate the command stream itself, which is the driver's own
# memory and never the operation's result, so they are not listed.
FORBIDDEN = ("memcpy", "memmove", "memset", "map_memory",
             "radeon_drm_vk_bo_map", "r3v_native_cmd_buffer_execute_deferred_copies")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def route_calls_no_host_store(source: str) -> list[str]:
    body = strip_comments(source)
    found = []
    for name in FORBIDDEN:
        if re.search(r"\b" + re.escape(name) + r"\s*\(", body):
            found.append(f"fill route calls {name}")
    return found


def transfer_skips_before_mapping(source: str) -> list[str]:
    body = strip_comments(source)
    case = body.find("case R3V_NATIVE_COPY_FILL_BUFFER:")
    if case < 0:
        return ["transfer has no fill-buffer case"]
    skip = body.find("gpu_routed", case)
    mapping = body.find("map_memory", case)
    if skip < 0:
        return ["transfer never tests gpu_routed for the fill record"]
    if mapping < 0:
        return ["transfer's fill case no longer maps, so the order is unread"]
    if skip > mapping:
        return ["transfer tests gpu_routed after it maps the destination"]
    return []


def route_marks_after_install(source: str) -> list[str]:
    body = strip_comments(source)
    install = body.find("r3v_native_cmd_buffer_install_ib")
    mark = body.find("gpu_routed = true")
    if install < 0:
        return ["fill route installs no stream"]
    if mark < 0:
        return ["fill route marks no record as routed"]
    if mark < install:
        return ["fill route marks the record before installing its stream"]
    return []


CHECKS = (
    ("route calls no host store", route_calls_no_host_store, ROUTE),
    ("transfer skips before mapping", transfer_skips_before_mapping, TRANSFER),
    ("route marks after install", route_marks_after_install, ROUTE),
)

KNOWN_BAD = (
    ("route calls no host store",
     route_calls_no_host_store,
     "void f(void) { memcpy(a, b, 4); }"),
    ("transfer skips before mapping",
     transfer_skips_before_mapping,
     "case R3V_NATIVE_COPY_FILL_BUFFER: map_memory(d); if (op->gpu_routed) x;"),
    ("transfer skips before mapping",
     transfer_skips_before_mapping,
     "case R3V_NATIVE_COPY_FILL_BUFFER: map_memory(d);"),
    ("route marks after install",
     route_marks_after_install,
     "x->gpu_routed = true; r3v_native_cmd_buffer_install_ib(c);"),
)

KNOWN_GOOD = (
    ("route calls no host store",
     route_calls_no_host_store,
     "void f(void) { /* memcpy in a comment */ install(); }"),
    ("transfer skips before mapping",
     transfer_skips_before_mapping,
     "case R3V_NATIVE_COPY_FILL_BUFFER: if (op->gpu_routed) return 0;"
     " map_memory(d);"),
    ("route marks after install",
     route_marks_after_install,
     "r3v_native_cmd_buffer_install_ib(c); x->gpu_routed = true;"),
)


def calibrate() -> list[str]:
    """A rule that cannot fail proves nothing, and neither does one that
    always fails."""
    problems = []
    for label, rule, text in KNOWN_BAD:
        if not rule(text):
            problems.append(f"known-bad passed: {label}")
    for label, rule, text in KNOWN_GOOD:
        findings = rule(text)
        if findings:
            problems.append(f"known-good failed: {label}: {findings[0]}")
    return problems


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")

    problems = calibrate()
    if problems:
        for line in problems:
            print(f"calibration: {line}", file=sys.stderr)
        return 1
    print(f"fill-route host exclusion: calibration passed "
          f"({len(KNOWN_BAD)} known-bad, {len(KNOWN_GOOD)} known-good)")

    findings = []
    for label, rule, path in CHECKS:
        full = root / path
        if not full.is_file():
            findings.append(f"{label}: {path} is absent")
            continue
        for finding in rule(full.read_text(encoding="utf-8")):
            findings.append(f"{label}: {finding}")

    if findings:
        for line in findings:
            print(line, file=sys.stderr)
        return 1

    print(f"fill-route host exclusion: {len(CHECKS)} checks pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
