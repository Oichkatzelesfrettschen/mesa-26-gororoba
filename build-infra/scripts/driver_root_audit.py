#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify the AMD Vulkan driver source roots and their build-graph entry points.

Within an AMD GPU-family namespace, the Vulkan driver's source root is that
family's `vulkan/` directory.  A driver-named leaf beneath it (the retired
`src/amd/r300/vulkan/r3v/`) leaves the family and `vulkan/` levels as empty
pass-through directories that carry no meson.build, so a reader resolving the
path by analogy with a sibling family reaches a directory holding no sources.

The audit pins each family's root, the exact Meson predicate that enters it,
its driver-named pass-through leaf, and its installed artifact definitions.
Every family is entered from `src/amd/meson.build`; moving a call under a
different condition changes the set of configurations that build its ICD.

Artifact identity is read from the definitions rather than from the presence
of a name: an assignment target, a comment, or a stale reference keeps the
substring after the installed DSO or manifest output has been renamed.  The
audit matches the `shared_library()` name argument and the ICD
`custom_target()` output, which are what the loader and the ICD manifests
actually spell.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# Each family contract owns the entry predicate, direct-root identity, loader
# library, and installed manifest form.  Keeping these fields together prevents
# a new family from receiving only a subset of the structural gates.
AMD_VULKAN_FAMILIES = {
    "src/amd/vulkan": {
        "entry_points": {"src/amd/meson.build": "with_amd_vk"},
        "driver_leaf": "radv",
        "shared_library": "vulkan_radeon",
        "manifest_stem": "radeon_icd",
        "manifest_form": "suffix-variable",
        "installed_manifest": "radeon_icd.<platform>.json",
    },
    "src/amd/terascale/vulkan": {
        "entry_points": {"src/amd/meson.build": "with_amd_terascale_vk"},
        "driver_leaf": "terakan",
        "shared_library": "vulkan_terascale",
        "manifest_stem": "terascale_icd",
        "manifest_form": "cpu-format",
        "installed_manifest": "terascale_icd.<cpu>.json",
    },
    "src/amd/r300/vulkan": {
        "entry_points": {"src/amd/meson.build": "with_ati_r300_vk"},
        "driver_leaf": "r3v",
        "shared_library": "vulkan_r3v",
        "manifest_stem": "r3v_icd",
        "manifest_form": "cpu-format",
        "installed_manifest": "r3v_icd.<cpu>.json",
    },
}

# Spellings of the retired leaf, absolute and relative.  A comment naming
# `r3v/meson.build` drifts the same way the directory did, and the relative
# form survives a sweep that only looks for the full path.
RETIRED_PATH_STRINGS = ["src/amd/r300/vulkan/r3v/", "r3v/meson.build"]

# Retained evidence records the path identity governed by its source capture.
# Each prefix designates immutable evidence or scanner source with no build
# authority, and this script names the retired path in order to detect it.
RETAINED_EVIDENCE_PREFIXES = (
    "docs/r3v-rename-allowlist.txt",
    "build-infra/docs/review-thread-frontiers/",
    "build-infra/scripts/driver_root_audit.py",
)


def strip_meson_comment(line):
    """Remove a Meson comment while preserving hash characters in strings."""
    active_quote = None
    escaped = False
    for character_index, character in enumerate(line):
        if escaped:
            escaped = False
            continue
        if active_quote:
            if character == "\\":
                escaped = True
            elif character == active_quote:
                active_quote = None
            continue
        if character in ("'", '"'):
            active_quote = character
        elif character == "#":
            return line[:character_index]
    return line


def strip_meson_comments(text):
    """Drop `#` comments so a commented-out subdir() never satisfies the gate."""
    return "\n".join(strip_meson_comment(line) for line in text.splitlines())


def normalize_meson_expression(expression):
    """Collapse irrelevant whitespace while preserving expression tokens."""
    return " ".join(expression.split())


def parse_meson_subdir_calls(text):
    """Return subdir calls with the complete active Meson control stack."""
    active_blocks = []
    calls = []
    failures = []
    subdir_pattern = re.compile(r"subdir\s*\(\s*['\"]([^'\"]+)['\"]\s*\)")

    for line_number, source_line in enumerate(text.splitlines(), start=1):
        statement = strip_meson_comment(source_line).strip()
        if not statement:
            continue

        if_match = re.fullmatch(r"if\s+(.+)", statement)
        elif_match = re.fullmatch(r"elif\s+(.+)", statement)
        foreach_match = re.fullmatch(r"foreach\s+(.+)", statement)
        if if_match:
            condition = normalize_meson_expression(if_match.group(1))
            active_blocks.append(("if", f"if {condition}"))
            continue
        if elif_match:
            if not active_blocks or active_blocks[-1][0] != "if":
                failures.append(f"line {line_number}: elif without matching if")
            else:
                condition = normalize_meson_expression(elif_match.group(1))
                active_blocks[-1] = ("if", f"elif {condition}")
            continue
        if statement == "else":
            if not active_blocks or active_blocks[-1][0] != "if":
                failures.append(f"line {line_number}: else without matching if")
            else:
                active_blocks[-1] = ("if", "else")
            continue
        if statement == "endif":
            if not active_blocks or active_blocks[-1][0] != "if":
                failures.append(f"line {line_number}: endif without matching if")
            else:
                active_blocks.pop()
            continue
        if foreach_match:
            expression = normalize_meson_expression(foreach_match.group(1))
            active_blocks.append(("foreach", f"foreach {expression}"))
            continue
        if statement == "endforeach":
            if not active_blocks or active_blocks[-1][0] != "foreach":
                failures.append(
                    f"line {line_number}: endforeach without matching foreach"
                )
            else:
                active_blocks.pop()
            continue

        subdir_match = subdir_pattern.fullmatch(statement)
        if subdir_match:
            calls.append(
                {
                    "path": subdir_match.group(1),
                    "contexts": tuple(block[1] for block in active_blocks),
                }
            )

    if active_blocks:
        contexts = " -> ".join(block[1] for block in active_blocks)
        failures.append(f"unterminated Meson control stack: {contexts}")
    return calls, failures


def check_entry_points(root, failures):
    """Require one subdir call under each family's exact enable predicate."""
    parsed_entries = {}
    entry_files = sorted(
        {
            entry_file
            for contract in AMD_VULKAN_FAMILIES.values()
            for entry_file in contract["entry_points"]
        }
    )
    for entry_file in entry_files:
        entry = root / entry_file
        if not entry.is_file():
            failures.append(f"missing build entry point: {entry_file}")
            parsed_entries[entry_file] = None
            continue
        calls, parse_failures = parse_meson_subdir_calls(
            entry.read_text(encoding="utf-8", errors="replace")
        )
        parsed_entries[entry_file] = calls
        failures.extend(
            f"{entry_file}: {parse_failure}" for parse_failure in parse_failures
        )

    for family_root, contract in AMD_VULKAN_FAMILIES.items():
        meson = root / family_root / "meson.build"
        if not meson.is_file():
            failures.append(
                f"missing driver root meson.build: {family_root}/meson.build"
            )
        for entry_file, predicate in contract["entry_points"].items():
            calls = parsed_entries[entry_file]
            if calls is None:
                continue
            relative = str(Path(family_root).relative_to(Path(entry_file).parent))
            matching_calls = [call for call in calls if call["path"] == relative]
            expected_contexts = (f"if {predicate}",)
            if len(matching_calls) != 1:
                failures.append(
                    f"{entry_file} must enter {relative} exactly once under "
                    f"if {predicate}; found {len(matching_calls)} call(s)"
                )
                continue
            actual_contexts = matching_calls[0]["contexts"]
            if actual_contexts != expected_contexts:
                actual = " -> ".join(actual_contexts) or "top level"
                failures.append(
                    f"{entry_file} enters {relative} under {actual}; "
                    f"expected only if {predicate}"
                )


def installed_manifest_pattern(contract):
    """Build the complete installed-manifest output expression pattern."""
    stem = re.escape(contract["manifest_stem"])
    quote = r"(?P<quote>['\"])"
    if contract["manifest_form"] == "cpu-format":
        return re.compile(
            r"(?m)^[ \t]*output\s*:\s*" + quote + stem + r"\.@0@\.json(?P=quote)"
            r"\s*\.format\s*\(\s*host_machine\.cpu\s*\(\s*\)\s*\)\s*,"
        )
    if contract["manifest_form"] == "suffix-variable":
        return re.compile(
            r"(?m)^[ \t]*output\s*:\s*" + quote + stem + r"\.(?P=quote)"
            r"\s*\+\s*vulkan_manifest_suffix\s*,"
        )
    raise ValueError(f"unknown manifest form: {contract['manifest_form']}")


def check_artifacts(root, failures):
    """Require the DSO name argument and the ICD manifest output."""
    for family_root, contract in AMD_VULKAN_FAMILIES.items():
        meson_rel = f"{family_root}/meson.build"
        meson = root / meson_rel
        if not meson.is_file():
            continue
        body = strip_meson_comments(meson.read_text(encoding="utf-8", errors="replace"))
        # shared_library('<name>', ...): the first positional argument is the
        # library name the loader resolves, and it may sit on its own line.
        dso = contract["shared_library"]
        if not re.search(
            r"(?m)^[ \t]*(?:[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*)?"
            r"shared_library\s*\(\s*['\"]%s['\"]" % re.escape(dso),
            body,
        ):
            failures.append(f"{meson_rel} defines no shared_library named {dso}")
        if not installed_manifest_pattern(contract).search(body):
            failures.append(
                f"{meson_rel} emits no installed ICD manifest named "
                f"{contract['installed_manifest']}"
            )


def check_driver_named_leaves(root, failures):
    """Keep every mapped family driver directly in its Vulkan source root."""
    for family_root, contract in AMD_VULKAN_FAMILIES.items():
        leaf = f"{family_root}/{contract['driver_leaf']}"
        leaf_path = root / leaf
        if leaf_path.exists() or leaf_path.is_symlink():
            failures.append(f"driver-named pass-through leaf present: {leaf}")


def check_tree(root):
    """Return the list of failure strings for the tree rooted at `root`."""
    failures = []
    check_entry_points(root, failures)
    check_driver_named_leaves(root, failures)
    check_artifacts(root, failures)
    failures.extend(scan_retired_path_strings(root))
    return failures


def scan_retired_path_strings(root):
    """Report tracked files that still spell a retired build path.

    `git grep -I` searches every tracked file and skips the ones Git detects
    as binary, so a Makefile, a PKGBUILD, an extensionless script, a `.cc`
    source, or a JSON manifest is covered.  A suffix allowlist would exempt
    each of those, and the build system is written in exactly those formats.
    Untracked build directories and retained logs are outside the index by
    construction.  Tracked governance captures preserve immutable cited text
    under an exact retained-evidence prefix and carry no build authority.
    """
    failures = []
    for needle in RETIRED_PATH_STRINGS:
        result = subprocess.run(
            ["git", "-C", str(root), "grep", "-I", "-l", "-F", needle],
            capture_output=True,
            text=True,
        )
        # Exit 1 means no match; anything above that is a real grep failure.
        if result.returncode > 1:
            failures.append(
                f"git grep failed while scanning for {needle}: "
                f"{result.stderr.strip()}"
            )
            continue
        for rel in result.stdout.splitlines():
            if rel.startswith(RETAINED_EVIDENCE_PREFIXES):
                continue
            failures.append(f"{rel} references the retired path {needle}")
    return failures


def fixture_manifest_expression(contract):
    """Return the valid synthetic output expression for a family contract."""
    stem = contract["manifest_stem"]
    if contract["manifest_form"] == "cpu-format":
        return f"'{stem}.@0@.json'.format(host_machine.cpu())"
    if contract["manifest_form"] == "suffix-variable":
        return f"'{stem}.' + vulkan_manifest_suffix"
    raise ValueError(f"unknown manifest form: {contract['manifest_form']}")


def _write_family(base, family, artifacts_ok=True, manifest_expression=None):
    """Stage one family root with definitions the artifact check accepts."""
    (base / family).mkdir(parents=True, exist_ok=True)
    contract = AMD_VULKAN_FAMILIES.get(family)
    body = "project_files = []\n"
    if contract and artifacts_ok:
        output_expression = manifest_expression or fixture_manifest_expression(contract)
        body += (
            "lib = shared_library(\n"
            f"  '{contract['shared_library']}',\n"
            "  project_files,\n"
            ")\n"
            "icd = custom_target(\n"
            "  'icd',\n"
            f"  output : {output_expression},\n"
            ")\n"
        )
    elif contract:
        # The names survive as an assignment target and a comment while the
        # definitions they claim to pin have been renamed.
        body += (
            f"lib{contract['shared_library']} = shared_library(\n"
            "  'vulkan_renamed',\n"
            ")\n"
            f"# {contract['manifest_stem']}\n"
            "icd = custom_target(\n"
            "  output : 'renamed_icd.@0@.json',\n"
            ")\n"
        )
    (base / family / "meson.build").write_text(body, encoding="utf-8")


def _stage_good(base):
    """Stage every mapped family under its exact entry predicate."""
    entry_blocks = {}
    for family_root, contract in AMD_VULKAN_FAMILIES.items():
        _write_family(base, family_root)
        for entry_file, predicate in contract["entry_points"].items():
            relative = str(Path(family_root).relative_to(Path(entry_file).parent))
            entry_blocks.setdefault(entry_file, []).append(
                f"if {predicate}\n  subdir('{relative}')\nendif\n"
            )
    for entry_file, blocks in entry_blocks.items():
        entry_path = base / entry_file
        entry_path.parent.mkdir(parents=True, exist_ok=True)
        entry_path.write_text("".join(blocks), encoding="utf-8")
    src_entry = base / "src/meson.build"
    src_entry.parent.mkdir(parents=True, exist_ok=True)
    src_entry.write_text("subdir('amd')\n", encoding="utf-8")


def _replace_family_entry(base, family_root, replacement):
    """Replace one synthetic family's complete entry block exactly once."""
    contract = AMD_VULKAN_FAMILIES[family_root]
    if len(contract["entry_points"]) != 1:
        raise ValueError(f"fixture requires one entry point: {family_root}")
    entry_file, predicate = next(iter(contract["entry_points"].items()))
    relative = str(Path(family_root).relative_to(Path(entry_file).parent))
    original = f"if {predicate}\n  subdir('{relative}')\nendif\n"
    entry_path = base / entry_file
    body = entry_path.read_text(encoding="utf-8")
    if body.count(original) != 1:
        raise ValueError(f"fixture entry block is not unique: {family_root}")
    entry_path.write_text(body.replace(original, replacement, 1), encoding="utf-8")


def artifact_failures(family_root):
    """Return the exact two artifact failures for a synthetic family."""
    contract = AMD_VULKAN_FAMILIES[family_root]
    meson_rel = f"{family_root}/meson.build"
    return [
        f"{meson_rel} defines no shared_library named " f"{contract['shared_library']}",
        f"{meson_rel} emits no installed ICD manifest named "
        f"{contract['installed_manifest']}",
    ]


def self_test():
    """Calibrate every independent rejection against a named known-bad tree.

    Each known-bad isolates one check, and the calibration compares the
    failure set against the exact expectation.  Reducing a known-bad to
    `bool(failures)` lets two of three checks regress unnoticed as long as the
    third still fires, which is how a verdict producer goes quietly blind.
    """
    cases = []

    malformed_control_cases = (
        (
            "elif without matching if",
            "elif feature\n",
            "line 1: elif without matching if",
        ),
        ("else without matching if", "else\n", "line 1: else without matching if"),
        ("endif without matching if", "endif\n", "line 1: endif without matching if"),
        (
            "endforeach without matching foreach",
            "endforeach\n",
            "line 1: endforeach without matching foreach",
        ),
        (
            "unterminated if control",
            "if feature\n",
            "unterminated Meson control stack: if feature",
        ),
        (
            "unterminated foreach control",
            "foreach item : items\n",
            "unterminated Meson control stack: foreach item : items",
        ),
    )
    for case_name, source, expected_failure in malformed_control_cases:
        _, parse_failures = parse_meson_subdir_calls(source)
        cases.append(
            (
                f"known-bad: {case_name}",
                parse_failures,
                [expected_failure],
            )
        )

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        good = tmp / "good"
        _stage_good(good)
        cases.append(("known-good tree", check_tree(good), []))

        # The retired leaf holds the sources and the entry spells the leaf.
        bad = tmp / "retired-leaf"
        _stage_good(bad)
        (bad / "src/amd/r300/vulkan/meson.build").unlink()
        leaf = bad / "src/amd/r300/vulkan/r3v"
        leaf.mkdir(parents=True)
        _write_family(bad, "src/amd/r300/vulkan/r3v")
        _replace_family_entry(
            bad,
            "src/amd/r300/vulkan",
            "if with_ati_r300_vk\n" "  subdir('r300/vulkan/r3v')\n" "endif\n",
        )
        cases.append(
            (
                "known-bad: retired leaf holds the sources",
                check_tree(bad),
                [
                    "missing driver root meson.build: src/amd/r300/vulkan/meson.build",
                    "src/amd/meson.build must enter r300/vulkan exactly once under "
                    "if with_ati_r300_vk; found 0 call(s)",
                    "driver-named pass-through leaf present: src/amd/r300/vulkan/r3v",
                ],
            )
        )

        # A required entry is absent from its otherwise correct predicate.
        bad = tmp / "entry-dropped"
        _stage_good(bad)
        _replace_family_entry(
            bad, "src/amd/r300/vulkan", "if with_ati_r300_vk\nendif\n"
        )
        cases.append(
            (
                "known-bad: entry dropped",
                check_tree(bad),
                [
                    "src/amd/meson.build must enter r300/vulkan exactly once under "
                    "if with_ati_r300_vk; found 0 call(s)",
                ],
            )
        )

        # A commented call carries no Meson build authority.
        bad = tmp / "entry-commented"
        _stage_good(bad)
        _replace_family_entry(
            bad,
            "src/amd/r300/vulkan",
            "if with_ati_r300_vk\n  # subdir('r300/vulkan')\nendif\n",
        )
        cases.append(
            (
                "known-bad: entry commented out",
                check_tree(bad),
                [
                    "src/amd/meson.build must enter r300/vulkan exactly once under "
                    "if with_ati_r300_vk; found 0 call(s)",
                ],
            )
        )

        # A quoted spelling of the call carries no Meson build authority.
        bad = tmp / "entry-in-string"
        _stage_good(bad)
        _replace_family_entry(
            bad,
            "src/amd/r300/vulkan",
            "if with_ati_r300_vk\n" "  message(\"subdir('r300/vulkan')\")\n" "endif\n",
        )
        cases.append(
            (
                "known-bad: entry only in string",
                check_tree(bad),
                [
                    "src/amd/meson.build must enter r300/vulkan exactly once under "
                    "if with_ati_r300_vk; found 0 call(s)",
                ],
            )
        )

        # Every family rejects a call under a different enable condition.
        for family_root, contract in AMD_VULKAN_FAMILIES.items():
            bad = tmp / f"{contract['driver_leaf']}-predicate-false"
            _stage_good(bad)
            entry_file, expected_predicate = next(
                iter(contract["entry_points"].items())
            )
            relative = str(Path(family_root).relative_to(Path(entry_file).parent))
            _replace_family_entry(
                bad, family_root, f"if false\n  subdir('{relative}')\nendif\n"
            )
            cases.append(
                (
                    f"known-bad: {contract['driver_leaf']} false predicate",
                    check_tree(bad),
                    [
                        f"{entry_file} enters {relative} under if false; "
                        f"expected only if {expected_predicate}",
                    ],
                )
            )

        # A Gallium predicate cannot stand in for the R3V Vulkan predicate.
        bad = tmp / "r3v-gallium-predicate"
        _stage_good(bad)
        _replace_family_entry(
            bad,
            "src/amd/r300/vulkan",
            "if with_gallium_r300\n  subdir('r300/vulkan')\nendif\n",
        )
        cases.append(
            (
                "known-bad: R3V Gallium predicate",
                check_tree(bad),
                [
                    "src/amd/meson.build enters r300/vulkan under "
                    "if with_gallium_r300; expected only if with_ati_r300_vk",
                ],
            )
        )

        # An added nested condition narrows the exact family enable domain.
        bad = tmp / "r3v-nested-predicate"
        _stage_good(bad)
        _replace_family_entry(
            bad,
            "src/amd/r300/vulkan",
            "if with_ati_r300_vk\n"
            "  if false\n"
            "    subdir('r300/vulkan')\n"
            "  endif\n"
            "endif\n",
        )
        cases.append(
            (
                "known-bad: R3V nested predicate",
                check_tree(bad),
                [
                    "src/amd/meson.build enters r300/vulkan under "
                    "if with_ati_r300_vk -> if false; expected only if with_ati_r300_vk",
                ],
            )
        )

        # Alternate branches do not equal the direct family enable domain.
        bad = tmp / "r3v-elif-predicate"
        _stage_good(bad)
        _replace_family_entry(
            bad,
            "src/amd/r300/vulkan",
            "if false\n"
            "elif with_ati_r300_vk\n"
            "  subdir('r300/vulkan')\n"
            "endif\n",
        )
        cases.append(
            (
                "known-bad: R3V elif predicate",
                check_tree(bad),
                [
                    "src/amd/meson.build enters r300/vulkan under "
                    "elif with_ati_r300_vk; expected only if with_ati_r300_vk",
                ],
            )
        )

        bad = tmp / "r3v-else-predicate"
        _stage_good(bad)
        _replace_family_entry(
            bad,
            "src/amd/r300/vulkan",
            "if false\nelse\n  subdir('r300/vulkan')\nendif\n",
        )
        cases.append(
            (
                "known-bad: R3V else predicate",
                check_tree(bad),
                [
                    "src/amd/meson.build enters r300/vulkan under else; "
                    "expected only if with_ati_r300_vk",
                ],
            )
        )

        # A loop adds a configuration dimension beyond the family predicate.
        bad = tmp / "r3v-foreach-context"
        _stage_good(bad)
        _replace_family_entry(
            bad,
            "src/amd/r300/vulkan",
            "if with_ati_r300_vk\n"
            "  foreach driver : enabled_drivers\n"
            "    subdir('r300/vulkan')\n"
            "  endforeach\n"
            "endif\n",
        )
        cases.append(
            (
                "known-bad: R3V foreach context",
                check_tree(bad),
                [
                    "src/amd/meson.build enters r300/vulkan under "
                    "if with_ati_r300_vk -> foreach driver : enabled_drivers; "
                    "expected only if with_ati_r300_vk",
                ],
            )
        )

        # Duplicate calls make the build entry identity ambiguous.
        bad = tmp / "r3v-duplicate-entry"
        _stage_good(bad)
        duplicate_block = "if with_ati_r300_vk\n" "  subdir('r300/vulkan')\n" "endif\n"
        _replace_family_entry(
            bad, "src/amd/r300/vulkan", duplicate_block + duplicate_block
        )
        cases.append(
            (
                "known-bad: R3V duplicate entry",
                check_tree(bad),
                [
                    "src/amd/meson.build must enter r300/vulkan exactly once under "
                    "if with_ati_r300_vk; found 2 call(s)",
                ],
            )
        )

        # Artifact identity remains load-bearing in every mapped family.
        for family_root, contract in AMD_VULKAN_FAMILIES.items():
            bad = tmp / f"{contract['driver_leaf']}-artifacts-renamed"
            _stage_good(bad)
            _write_family(bad, family_root, artifacts_ok=False)
            cases.append(
                (
                    f"known-bad: {contract['driver_leaf']} artifacts renamed",
                    check_tree(bad),
                    artifact_failures(family_root),
                )
            )

        # Wrong extensions and arbitrary suffixes cannot satisfy an installed
        # manifest output contract.
        for family_root, contract in AMD_VULKAN_FAMILIES.items():
            for suffix in ("txt", "backup"):
                bad = tmp / f"{contract['driver_leaf']}-manifest-{suffix}"
                _stage_good(bad)
                wrong_expression = f"'{contract['manifest_stem']}.{suffix}'"
                _write_family(bad, family_root, manifest_expression=wrong_expression)
                cases.append(
                    (
                        f"known-bad: {contract['driver_leaf']} manifest {suffix}",
                        check_tree(bad),
                        [artifact_failures(family_root)[1]],
                    )
                )

        # An exact output expression preserved as inert string data cannot
        # stand in for the custom_target output keyword argument.
        for family_root, contract in AMD_VULKAN_FAMILIES.items():
            bad = tmp / f"{contract['driver_leaf']}-manifest-in-string"
            _stage_good(bad)
            meson_path = bad / family_root / "meson.build"
            valid_expression = fixture_manifest_expression(contract)
            meson_body = meson_path.read_text(encoding="utf-8")
            meson_path.write_text(
                meson_body.replace(valid_expression, "'renamed_icd.json'", 1)
                + f'stale_manifest = "output : {valid_expression},"\n',
                encoding="utf-8",
            )
            cases.append(
                (
                    f"known-bad: {contract['driver_leaf']} manifest only in string",
                    check_tree(bad),
                    [artifact_failures(family_root)[1]],
                )
            )

        # A mapped family root retains its own meson.build.
        bad = tmp / "root-missing"
        _stage_good(bad)
        (bad / "src/amd/r300/vulkan/meson.build").unlink()
        cases.append(
            (
                "known-bad: driver root meson.build removed",
                check_tree(bad),
                [
                    "missing driver root meson.build: src/amd/r300/vulkan/meson.build",
                ],
            )
        )

        # A driver-named leaf remains invalid beside a complete direct root.
        bad = tmp / "leaf-reappears"
        _stage_good(bad)
        (bad / "src/amd/r300/vulkan/r3v").mkdir(parents=True)
        cases.append(
            (
                "known-bad: retired leaf reappears",
                check_tree(bad),
                [
                    "driver-named pass-through leaf present: src/amd/r300/vulkan/r3v",
                ],
            )
        )

        # A broken symlink still recreates the prohibited driver-leaf shape.
        bad = tmp / "broken-driver-leaf-symlink"
        _stage_good(bad)
        (bad / "src/amd/vulkan/radv").symlink_to("missing-radv-source-root")
        cases.append(
            (
                "known-bad: broken driver leaf symlink",
                check_tree(bad),
                [
                    "driver-named pass-through leaf present: src/amd/vulkan/radv",
                ],
            )
        )

        # Every mapped family rejects its canonical driver as a pass-through
        # leaf, including sibling families with no historical rename path.
        for family_root, contract in AMD_VULKAN_FAMILIES.items():
            bad = tmp / f"{contract['driver_leaf']}-pass-through-leaf"
            _stage_good(bad)
            leaf_root = f"{family_root}/{contract['driver_leaf']}"
            _write_family(bad, leaf_root)
            (bad / family_root / "meson.build").write_text(
                f"subdir('{contract['driver_leaf']}')\n", encoding="utf-8"
            )
            cases.append(
                (
                    f"known-bad: {contract['driver_leaf']} pass-through leaf",
                    check_tree(bad),
                    [
                        f"driver-named pass-through leaf present: {leaf_root}",
                        *artifact_failures(family_root),
                    ],
                )
            )

        # A missing entry file produces one file-identity failure shared by all
        # family contracts that consume it.
        bad = tmp / "entry-file-missing"
        _stage_good(bad)
        (bad / "src/amd/meson.build").unlink()
        cases.append(
            (
                "known-bad: entry file absent",
                check_tree(bad),
                [
                    "missing build entry point: src/amd/meson.build",
                ],
            )
        )

    ok = True
    for label, failures, expected in cases:
        # The synthetic trees are not Git repositories, so the tracked-file
        # scan reports its own failure there; it is calibrated against the
        # real tree by the run this gate performs in the build.
        failures = [f for f in failures if not f.startswith("git grep failed")]
        if sorted(failures) == sorted(expected):
            print(f"OK    {label}: {len(failures)} failure(s) as expected")
        else:
            ok = False
            print(f"MISCALIBRATED  {label}")
            for line in sorted(set(expected) - set(failures)):
                print(f"        expected, absent: {line}")
            for line in sorted(set(failures) - set(expected)):
                print(f"        unexpected: {line}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--repo-root",
        default=None,
        help="repository root (default: git rev-parse --show-toplevel)",
    )
    ap.add_argument(
        "--self-test",
        action="store_true",
        help="calibrate every rejection on named known-bad trees",
    )
    args = ap.parse_args()

    if args.self_test:
        return 0 if self_test() else 1

    if args.repo_root:
        root = Path(args.repo_root).resolve()
    else:
        root = Path(
            subprocess.run(
                ["git", "rev-parse", "--show-toplevel"],
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()
        )

    failures = check_tree(root)
    if failures:
        print("FAIL  driver-root-audit: AMD Vulkan driver roots out of contract")
        for line in failures:
            print(f"        {line}")
        return 1
    roots = ", ".join(sorted(AMD_VULKAN_FAMILIES))
    print(f"OK    driver-root-audit: {roots}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
