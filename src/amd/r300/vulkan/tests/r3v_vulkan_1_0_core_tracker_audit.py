# SPDX-License-Identifier: MIT
"""Audit the R3V Vulkan 1.0 core specification sheet and tracker.

The Vulkan XML registry supplies the API inventory, not every normative
semantic rule in the prose specification.  The sheet pins the final 1.0 core
registry revision and preserves its feature requirement blocks.  The tracker
maps each block exactly once to an R3V review row.  A row can describe only
source-audited bounded execution, explicit refusal, or an unassessed surface;
it cannot turn source presence into a conformance verdict.

Usage:
  r3v_vulkan_1_0_core_tracker_audit.py --sheet PATH --tracker PATH \
      --source-root PATH
  r3v_vulkan_1_0_core_tracker_audit.py --selftest
"""

import argparse
import copy
import hashlib
import json
import shlex
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as element_tree
from pathlib import Path

SOURCE_COMMIT = "ab08f0951ef1ad9b84db93f971e113c1d9d55609"
SOURCE_SHA256 = "820d7e3f6fb54d955b0bbd89a0c96be3e66f9a7cc60a7eb28d820ad57c6c2b4e"
PROJECTION_SHA256 = "ddd64cfa307fc9c00561970900396fc45ccfcb543e1979419e27a9452a9e92c7"
SOURCE_IDENTITY = {
    "publisher": "Khronos Group",
    "repository": "https://github.com/KhronosGroup/Vulkan-Docs",
    "tag": "v1.0.69-core",
    "commit": SOURCE_COMMIT,
    "tag_object": "ff4049b28854a0a7472623f3c037dee800fb4c58",
    "tagger_date": "2018-02-19T23:33:22Z",
    "xml_path": "src/spec/vk.xml",
    "xml_url": (
        "https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/"
        f"{SOURCE_COMMIT}/src/spec/vk.xml"
    ),
    "xml_sha256": SOURCE_SHA256,
    "registry_landing_page": "https://registry.khronos.org/vulkan/specs/1.0/README.md",
}
ALLOWED_STATUSES = frozenset({"bounded", "refused", "unassessed"})


class AuditFailure(Exception):
    """The projection, tracker, or their coverage contract is invalid."""


def read_json(path):
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AuditFailure(f"cannot read {path}: {error}") from error
    if not isinstance(document, dict):
        raise AuditFailure(f"{path} must contain a JSON object")
    return document


def projection_digest(blocks):
    canonical = json.dumps(
        blocks, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def sheet_blocks(sheet, expected_projection_sha256=PROJECTION_SHA256):
    if sheet.get("schema_version") != 1:
        raise AuditFailure("sheet schema_version must equal 1")
    source = sheet.get("source")
    if source != SOURCE_IDENTITY:
        raise AuditFailure(
            "sheet source identity does not pin the official Vulkan-Docs release"
        )
    projection = sheet.get("projection")
    if not isinstance(projection, dict):
        raise AuditFailure("sheet projection must be an object")
    if projection.get("feature") != "VK_VERSION_1_0":
        raise AuditFailure("sheet must project VK_VERSION_1_0")
    if projection.get("api") != "vulkan" or projection.get("version") != "1.0":
        raise AuditFailure("sheet must project the Vulkan 1.0 API")
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
    if projection_digest(blocks) != expected_projection_sha256:
        raise AuditFailure(
            "sheet requirement blocks differ from the pinned registry projection"
        )
    counts = projection.get("counts")
    if not isinstance(counts, dict):
        raise AuditFailure("sheet counts must be an object")
    expected_counts = {
        "requirement_blocks": len(blocks),
        "commands": len(commands),
        "types": sum(len(block["types"]) for block in blocks),
        "enums": sum(len(block["enums"]) for block in blocks),
    }
    if counts != expected_counts:
        raise AuditFailure("sheet counts disagree with its requirement blocks")
    return set(names)


def validate_evidence_locator(locator, source_root):
    """Resolve one recorded rg command against the declared source tree."""
    path = locator["path"]
    discovery = locator["discovery"]
    try:
        command = shlex.split(discovery)
    except ValueError as error:
        raise AuditFailure(f"evidence locator has malformed shell syntax: {error}") from error
    if (
        len(command) != 4
        or command[0] != "rg"
        or command[1] != "--fixed-strings"
        or command[3] != path
    ):
        raise AuditFailure(
            f"evidence locator for {path!r} is not an exact rg command"
        )
    relative_path = Path(path)
    if relative_path.is_absolute() or ".." in relative_path.parts:
        raise AuditFailure(f"evidence locator escapes the source root: {path!r}")
    root = source_root.resolve()
    source_path = (root / relative_path).resolve()
    try:
        source_path.relative_to(root)
    except ValueError as error:
        raise AuditFailure(f"evidence locator escapes the source root: {path!r}") from error
    if not source_path.is_file():
        raise AuditFailure(f"evidence locator names no source file: {path!r}")
    result = subprocess.run(
        command,
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise AuditFailure(
            f"evidence locator does not resolve in the source tree: {discovery}"
        )


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


def audit(
    sheet,
    tracker,
    registry_xml=None,
    expected_projection_sha256=PROJECTION_SHA256,
    source_root=None,
):
    block_names = sheet_blocks(sheet, expected_projection_sha256)
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
        if not isinstance(evidence, list) or not evidence:
            raise AuditFailure(f"tracker row {identifier} lacks evidence locators")
        for locator in evidence:
            if not isinstance(locator, dict) or set(locator) != {
                "path",
                "discovery",
            }:
                raise AuditFailure(
                    f"tracker row {identifier} has a malformed evidence locator"
                )
            path = locator["path"]
            discovery = locator["discovery"]
            if (
                not isinstance(path, str)
                or not path
                or not isinstance(discovery, str)
                or not discovery.startswith("rg --fixed-strings ")
                or path not in discovery
            ):
                raise AuditFailure(
                    f"tracker row {identifier} has a non-reproducible evidence locator"
                )
            if source_root is not None:
                validate_evidence_locator(locator, source_root)
        if not isinstance(falsifier, str) or not falsifier:
            raise AuditFailure(f"tracker row {identifier} lacks a falsifier")
        blocks = row.get("requirement_blocks")
        if (
            not isinstance(blocks, list)
            or not blocks
            or not all(isinstance(value, str) and value for value in blocks)
        ):
            raise AuditFailure(f"tracker row {identifier} lacks requirement blocks")
        if len(blocks) != len(set(blocks)):
            raise AuditFailure(
                f"tracker row {identifier} maps a requirement block more than once"
            )
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
        "source": copy.deepcopy(SOURCE_IDENTITY),
        "projection": {
            "feature": "VK_VERSION_1_0",
            "api": "vulkan",
            "version": "1.0",
            "requirement_blocks": [
                {
                    "name": "memory",
                    "commands": ["vkMapMemory"],
                    "types": [],
                    "enums": [],
                }
            ],
            "counts": {
                "requirement_blocks": 1,
                "commands": 1,
                "types": 0,
                "enums": 0,
            },
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
                "evidence": [
                    {
                        "path": "source.c",
                        "discovery": "rg --fixed-strings 'vkMapMemory' source.c",
                    }
                ],
                "falsifier": "remove source",
                "requirement_blocks": ["memory"],
            }
        ],
    }
    expected_digest = projection_digest(sheet["projection"]["requirement_blocks"])
    audit(sheet, tracker, expected_projection_sha256=expected_digest)
    mutated_source = copy.deepcopy(sheet)
    mutated_source["source"]["tag_object"] = "0" * 40
    try:
        audit(mutated_source, tracker, expected_projection_sha256=expected_digest)
    except AuditFailure:
        pass
    else:
        raise AuditFailure("selftest admitted a mutated registry source identity")
    tracker["rows"][0]["status"] = "conformant"
    try:
        audit(sheet, tracker, expected_projection_sha256=expected_digest)
    except AuditFailure:
        pass
    else:
        raise AuditFailure("selftest admitted a conformance status")
    tracker["rows"][0]["status"] = "bounded"
    tracker["rows"][0]["requirement_blocks"] = ["memory", "memory"]
    try:
        audit(sheet, tracker, expected_projection_sha256=expected_digest)
    except AuditFailure:
        pass
    else:
        raise AuditFailure("selftest admitted duplicate coverage within one row")
    tracker["rows"][0]["requirement_blocks"] = ["memory"]
    tracker["rows"].append(dict(tracker["rows"][0]))
    try:
        audit(sheet, tracker, expected_projection_sha256=expected_digest)
    except AuditFailure:
        pass
    else:
        raise AuditFailure("selftest admitted duplicate block coverage")
    tracker["rows"].pop()
    mutated_sheet = copy.deepcopy(sheet)
    mutated_sheet["projection"]["requirement_blocks"][0]["commands"][0] = (
        "vkUnmapMemory"
    )
    try:
        audit(mutated_sheet, tracker, expected_projection_sha256=expected_digest)
    except AuditFailure:
        pass
    else:
        raise AuditFailure("selftest admitted a mutated registry projection")
    with tempfile.TemporaryDirectory() as temporary_directory:
        source_root = Path(temporary_directory)
        Path(source_root, "source.c").write_text(
            "void vkMapMemory(void) {}\n", encoding="utf-8"
        )
        audit(
            sheet,
            tracker,
            expected_projection_sha256=expected_digest,
            source_root=source_root,
        )
        bad_locator = copy.deepcopy(tracker)
        bad_locator["rows"][0]["evidence"][0]["discovery"] = (
            "rg --fixed-strings 'missing' source.c"
        )
        try:
            audit(
                sheet,
                bad_locator,
                expected_projection_sha256=expected_digest,
                source_root=source_root,
            )
        except AuditFailure:
            pass
        else:
            raise AuditFailure("selftest admitted an unresolved evidence locator")
        scalar_json = Path(temporary_directory) / "scalar.json"
        scalar_json.write_text("[]", encoding="utf-8")
        try:
            read_json(scalar_json)
        except AuditFailure:
            pass
        else:
            raise AuditFailure("selftest admitted a non-object JSON document")
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
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.selftest:
            return selftest()
        if args.sheet is None or args.tracker is None:
            parser.error("--sheet and --tracker are required")
        if args.source_root is None:
            parser.error("--source-root is required")
        rows, blocks = audit(
            read_json(args.sheet),
            read_json(args.tracker),
            args.registry_xml,
            source_root=args.source_root,
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
