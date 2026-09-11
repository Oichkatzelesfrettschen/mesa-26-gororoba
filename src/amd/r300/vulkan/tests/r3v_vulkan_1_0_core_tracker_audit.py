# SPDX-License-Identifier: MIT
"""Audit the R3V Vulkan 1.0 core specification sheet and tracker.

The Vulkan XML registry supplies the API inventory, not every normative
semantic rule in the prose specification.  The sheet pins the final 1.0 core
registry revision and preserves its feature requirement blocks.  The tracker
maps each block exactly once to an R3V review row.  A row can describe only
source-audited bounded execution, explicit refusal, or an unassessed surface;
it cannot turn source presence into a conformance verdict.

Usage:
  r3v_vulkan_1_0_core_tracker_audit.py --sheet PATH --tracker PATH
  r3v_vulkan_1_0_core_tracker_audit.py --selftest
"""

import argparse
import hashlib
import json
import sys
import tempfile
import xml.etree.ElementTree as element_tree
from pathlib import Path


SOURCE_COMMIT = "ab08f0951ef1ad9b84db93f971e113c1d9d55609"
SOURCE_SHA256 = "820d7e3f6fb54d955b0bbd89a0c96be3e66f9a7cc60a7eb28d820ad57c6c2b4e"
ALLOWED_STATUSES = frozenset({"bounded", "refused", "unassessed"})


class AuditFailure(Exception):
    """The projection, tracker, or their coverage contract is invalid."""


def read_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AuditFailure(f"cannot read {path}: {error}") from error


def sheet_blocks(sheet):
    if sheet.get("schema_version") != 1:
        raise AuditFailure("sheet schema_version must equal 1")
    source = sheet.get("source")
    if not isinstance(source, dict):
        raise AuditFailure("sheet source must be an object")
    if source.get("commit") != SOURCE_COMMIT:
        raise AuditFailure("sheet source commit does not pin Vulkan-Docs v1.0.69-core")
    if source.get("xml_sha256") != SOURCE_SHA256:
        raise AuditFailure("sheet XML digest does not pin the official registry input")
    projection = sheet.get("projection")
    if not isinstance(projection, dict):
        raise AuditFailure("sheet projection must be an object")
    if projection.get("feature") != "VK_VERSION_1_0":
        raise AuditFailure("sheet must project VK_VERSION_1_0")
    blocks = projection.get("requirement_blocks")
    if not isinstance(blocks, list) or not blocks:
        raise AuditFailure("sheet requirement_blocks must be a nonempty array")
    names = []
    commands = []
    for block in blocks:
        if not isinstance(block, dict):
            raise AuditFailure("sheet requirement block must be an object")
        name = block.get("name")
        if not isinstance(name, str) or not name:
            raise AuditFailure("sheet requirement block lacks a name")
        names.append(name)
        for key in ("commands", "types", "enums"):
            values = block.get(key)
            if not isinstance(values, list) or not all(
                isinstance(value, str) and value for value in values
            ):
                raise AuditFailure(f"sheet block {name!r} has malformed {key}")
        commands.extend(block["commands"])
    if len(names) != len(set(names)):
        raise AuditFailure("sheet repeats a requirement-block name")
    if len(commands) != len(set(commands)):
        raise AuditFailure("sheet repeats a core command")
    counts = projection.get("counts")
    if not isinstance(counts, dict):
        raise AuditFailure("sheet counts must be an object")
    if counts.get("commands") != len(commands):
        raise AuditFailure("sheet command count disagrees with its blocks")
    return set(names)


def registry_blocks(path):
    try:
        payload = path.read_bytes()
        root = element_tree.fromstring(payload)
    except (OSError, element_tree.ParseError) as error:
        raise AuditFailure(
            f"cannot read Vulkan registry XML {path}: {error}"
        ) from error
    digest = hashlib.sha256(payload).hexdigest()
    if digest != SOURCE_SHA256:
        raise AuditFailure(
            "registry XML digest disagrees with the pinned official input"
        )
    feature = next(
        (
            item
            for item in root.findall("feature")
            if item.get("name") == "VK_VERSION_1_0"
        ),
        None,
    )
    if feature is None:
        raise AuditFailure("registry XML contains no VK_VERSION_1_0 feature")
    return [
        {
            "name": requirement.get("comment", "unnamed requirement block"),
            "commands": [item.get("name") for item in requirement.findall("command")],
            "types": [item.get("name") for item in requirement.findall("type")],
            "enums": [item.get("name") for item in requirement.findall("enum")],
        }
        for requirement in feature.findall("require")
    ]


def audit_registry_projection(sheet, path):
    blocks = registry_blocks(path)
    if sheet["projection"]["requirement_blocks"] != blocks:
        raise AuditFailure("sheet projection differs from the pinned registry XML")
    expected_counts = {
        "requirement_blocks": len(blocks),
        "commands": sum(len(block["commands"]) for block in blocks),
        "types": sum(len(block["types"]) for block in blocks),
        "enums": sum(len(block["enums"]) for block in blocks),
    }
    if sheet["projection"]["counts"] != expected_counts:
        raise AuditFailure("sheet counts disagree with the pinned registry XML")


def audit(sheet, tracker, registry_xml=None):
    block_names = sheet_blocks(sheet)
    if registry_xml is not None:
        audit_registry_projection(sheet, registry_xml)
    if tracker.get("schema_version") != 1:
        raise AuditFailure("tracker schema_version must equal 1")
    if tracker.get("sheet") != "docs/hardware/r3v-vulkan-1-0-core-sheet.json":
        raise AuditFailure("tracker must name its canonical specification sheet")
    boundary = tracker.get("conformance_boundary")
    if not isinstance(boundary, str) or "conformance" not in boundary.lower():
        raise AuditFailure("tracker must state its conformance boundary")
    rows = tracker.get("rows")
    if not isinstance(rows, list) or not rows:
        raise AuditFailure("tracker rows must be a nonempty array")
    seen = set()
    for row in rows:
        if not isinstance(row, dict):
            raise AuditFailure("tracker row must be an object")
        identifier = row.get("id")
        if not isinstance(identifier, str) or not identifier:
            raise AuditFailure("tracker row lacks an id")
        status = row.get("status")
        if status not in ALLOWED_STATUSES:
            raise AuditFailure(
                f"tracker row {identifier} has invalid status {status!r}"
            )
        evidence = row.get("evidence")
        falsifier = row.get("falsifier")
        if (
            not isinstance(evidence, list)
            or not evidence
            or not all(isinstance(value, str) and value for value in evidence)
        ):
            raise AuditFailure(f"tracker row {identifier} lacks evidence locators")
        if not isinstance(falsifier, str) or not falsifier:
            raise AuditFailure(f"tracker row {identifier} lacks a falsifier")
        blocks = row.get("requirement_blocks")
        if (
            not isinstance(blocks, list)
            or not blocks
            or not all(isinstance(value, str) and value for value in blocks)
        ):
            raise AuditFailure(f"tracker row {identifier} lacks requirement blocks")
        overlap = seen.intersection(blocks)
        if overlap:
            raise AuditFailure(
                f"tracker maps requirement blocks more than once: {sorted(overlap)}"
            )
        seen.update(blocks)
    unknown = seen - block_names
    missing = block_names - seen
    if unknown:
        raise AuditFailure(
            f"tracker names unknown requirement blocks: {sorted(unknown)}"
        )
    if missing:
        raise AuditFailure(
            f"tracker leaves requirement blocks untracked: {sorted(missing)}"
        )
    return len(rows), len(block_names)


def selftest():
    sheet = {
        "schema_version": 1,
        "source": {"commit": SOURCE_COMMIT, "xml_sha256": SOURCE_SHA256},
        "projection": {
            "feature": "VK_VERSION_1_0",
            "requirement_blocks": [
                {
                    "name": "memory",
                    "commands": ["vkMapMemory"],
                    "types": [],
                    "enums": [],
                }
            ],
            "counts": {"commands": 1},
        },
    }
    tracker = {
        "schema_version": 1,
        "sheet": "docs/hardware/r3v-vulkan-1-0-core-sheet.json",
        "conformance_boundary": "Source review does not establish conformance.",
        "rows": [
            {
                "id": "memory",
                "status": "bounded",
                "evidence": ["source"],
                "falsifier": "remove source",
                "requirement_blocks": ["memory"],
            }
        ],
    }
    audit(sheet, tracker)
    tracker["rows"][0]["status"] = "conformant"
    try:
        audit(sheet, tracker)
    except AuditFailure:
        pass
    else:
        raise AuditFailure("selftest admitted a conformance status")
    tracker["rows"][0]["status"] = "bounded"
    tracker["rows"].append(dict(tracker["rows"][0]))
    try:
        audit(sheet, tracker)
    except AuditFailure:
        pass
    else:
        raise AuditFailure("selftest admitted duplicate block coverage")
    with tempfile.TemporaryDirectory() as temporary_directory:
        wrong_registry = Path(temporary_directory) / "vk.xml"
        wrong_registry.write_text("<registry/>", encoding="utf-8")
        try:
            registry_blocks(wrong_registry)
        except AuditFailure:
            pass
        else:
            raise AuditFailure("selftest admitted an unpinned registry XML")
    print("r3v_vulkan_1_0_core_tracker_audit: selftest OK")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sheet", type=Path)
    parser.add_argument("--tracker", type=Path)
    parser.add_argument("--registry-xml", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.selftest:
            return selftest()
        if args.sheet is None or args.tracker is None:
            parser.error("--sheet and --tracker are required")
        rows, blocks = audit(
            read_json(args.sheet), read_json(args.tracker), args.registry_xml
        )
        print(
            "r3v_vulkan_1_0_core_tracker_audit: "
            f"{rows} tracker rows cover {blocks} requirement blocks"
        )
        return 0
    except AuditFailure as error:
        print(f"r3v_vulkan_1_0_core_tracker_audit: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
