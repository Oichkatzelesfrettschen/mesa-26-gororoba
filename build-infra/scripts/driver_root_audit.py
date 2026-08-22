#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify the AMD Vulkan driver source roots and their build-graph entry points.

Within an AMD GPU-family namespace, the Vulkan driver's source root is that
family's `vulkan/` directory.  A driver-named leaf beneath it (the retired
`src/amd/r300/vulkan/r3v/`) leaves the family and `vulkan/` levels as empty
pass-through directories that carry no meson.build, so a reader resolving the
path by analogy with a sibling family reaches a directory holding no sources.

The audit pins each family's root, every `subdir()` call that enters it, and
the installed artifact definitions.  Every family is entered from
`src/amd/meson.build`; the r300 Vulkan root is entered there under
`with_ati_r300_vk`, and losing that entry loses the ICD.

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

# Family namespace -> every meson.build that enters that family's vulkan/ root.
AMD_VULKAN_FAMILIES = {
    "src/amd/vulkan": ["src/amd/meson.build"],
    "src/amd/terascale/vulkan": ["src/amd/meson.build"],
    "src/amd/r300/vulkan": ["src/amd/meson.build"],
}

# Driver identity survives a directory move: the DSO name the loader resolves
# and the ICD manifest stem the runtime reads.  Both are held fixed.
DRIVER_ARTIFACTS = {
    "src/amd/r300/vulkan/meson.build": {
        "shared_library": "vulkan_r3v",
        "icd_output": "r3v_icd",
    },
    "src/amd/terascale/vulkan/meson.build": {
        "shared_library": "vulkan_terascale",
        "icd_output": "terascale_icd",
    },
}

RETIRED_PATHS = ["src/amd/r300/vulkan/r3v"]

# Spellings of the retired leaf, absolute and relative.  A comment naming
# `r3v/meson.build` drifts the same way the directory did, and the relative
# form survives a sweep that only looks for the full path.
RETIRED_PATH_STRINGS = ["src/amd/r300/vulkan/r3v/", "r3v/meson.build"]

# Retained evidence records the path that existed when it was produced.  The
# allowlist is the one current-authority file that quotes a retired path
# deliberately, and this script names the path in order to detect it.
EVIDENCE_PREFIXES = (
    "docs/r3v-rename-allowlist.txt",
    "build-infra/scripts/driver_root_audit.py",
)


def strip_meson_comments(text):
    """Drop `#` comments so a commented-out subdir() never satisfies the gate."""
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def check_entry_points(root, failures):
    """Require every configuration's subdir() into each family root."""
    for family_root, entry_files in AMD_VULKAN_FAMILIES.items():
        meson = root / family_root / "meson.build"
        if not meson.is_file():
            failures.append(
                f"missing driver root meson.build: {family_root}/meson.build")
        for entry_file in entry_files:
            entry = root / entry_file
            if not entry.is_file():
                failures.append(f"missing build entry point: {entry_file}")
                continue
            # subdir() paths are relative to the entering file's directory.
            relative = str(
                Path(family_root).relative_to(Path(entry_file).parent))
            body = strip_meson_comments(
                entry.read_text(encoding="utf-8", errors="replace"))
            if not re.search(r"subdir\s*\(\s*['\"]%s['\"]"
                             % re.escape(relative), body):
                failures.append(
                    f"{entry_file} does not enter {relative} via subdir()")


def check_artifacts(root, failures):
    """Require the DSO name argument and the ICD manifest output."""
    for meson_rel, artifacts in DRIVER_ARTIFACTS.items():
        meson = root / meson_rel
        if not meson.is_file():
            continue
        body = strip_meson_comments(
            meson.read_text(encoding="utf-8", errors="replace"))
        # shared_library('<name>', ...): the first positional argument is the
        # library name the loader resolves, and it may sit on its own line.
        dso = artifacts["shared_library"]
        if not re.search(r"shared_library\s*\(\s*['\"]%s['\"]"
                         % re.escape(dso), body):
            failures.append(
                f"{meson_rel} defines no shared_library named {dso}")
        # The install ICD target names its manifest through `output :`; the
        # devenv target's separate stem is deliberately not matched here.
        icd = artifacts["icd_output"]
        if not re.search(r"output\s*:\s*['\"]%s\." % re.escape(icd), body):
            failures.append(
                f"{meson_rel} emits no ICD manifest named {icd}.<cpu>.json")


def check_tree(root):
    """Return the list of failure strings for the tree rooted at `root`."""
    failures = []
    check_entry_points(root, failures)
    for retired in RETIRED_PATHS:
        if (root / retired).exists():
            failures.append(f"retired driver-named leaf present: {retired}")
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
    construction, so they record what ran without failing the gate.
    """
    failures = []
    for needle in RETIRED_PATH_STRINGS:
        result = subprocess.run(
            ["git", "-C", str(root), "grep", "-I", "-l", "-F", needle],
            capture_output=True, text=True)
        # Exit 1 means no match; anything above that is a real grep failure.
        if result.returncode > 1:
            failures.append(
                f"git grep failed while scanning for {needle}: "
                f"{result.stderr.strip()}")
            continue
        for rel in result.stdout.splitlines():
            if rel.startswith(EVIDENCE_PREFIXES):
                continue
            failures.append(f"{rel} references the retired path {needle}")
    return failures


def _write_family(base, family, artifacts_ok=True):
    """Stage one family root with definitions the artifact check accepts."""
    (base / family).mkdir(parents=True, exist_ok=True)
    artifacts = DRIVER_ARTIFACTS.get(family + "/meson.build")
    body = "project_files = []\n"
    if artifacts and artifacts_ok:
        body += ("lib = shared_library(\n  '%s',\n  project_files,\n)\n"
                 % artifacts["shared_library"])
        body += ("icd = custom_target(\n  'icd',\n"
                 "  output : '%s.@0@.json'.format(host_machine.cpu()),\n)\n"
                 % artifacts["icd_output"])
    elif artifacts:
        # The names survive as an assignment target and a comment while the
        # definitions they claim to pin have been renamed.
        body += ("lib%s = shared_library(\n  'vulkan_renamed',\n)\n"
                 % artifacts["shared_library"])
        body += ("# %s\nicd = custom_target(\n"
                 "  output : 'renamed_icd.@0@.json',\n)\n"
                 % artifacts["icd_output"])
    (base / family / "meson.build").write_text(body)


def self_test():
    """Calibrate every independent rejection against a named known-bad tree.

    Each known-bad isolates one check, and the calibration compares the
    failure set against the exact expectation.  Reducing a known-bad to
    `bool(failures)` lets two of three checks regress unnoticed as long as the
    third still fires, which is how a verdict producer goes quietly blind.
    """
    cases = []

    def stage_good(base):
        for family in AMD_VULKAN_FAMILIES:
            _write_family(base, family)
        (base / "src/amd/meson.build").write_text(
            "subdir('vulkan')\nsubdir('terascale/vulkan')\n"
            "subdir('r300/vulkan')\n")
        (base / "src/meson.build").write_text("subdir('amd')\n")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        good = tmp / "good"
        stage_good(good)
        cases.append(("known-good tree", check_tree(good), []))

        # 1. The pre-move defect in full: the leaf holds the sources and the
        #    entry spells the leaf.
        bad = tmp / "retired-leaf"
        stage_good(bad)
        (bad / "src/amd/r300/vulkan/meson.build").unlink()
        leaf = bad / "src/amd/r300/vulkan/r3v"
        leaf.mkdir(parents=True)
        _write_family(bad, "src/amd/r300/vulkan/r3v")
        (bad / "src/amd/meson.build").write_text(
            "subdir('vulkan')\nsubdir('terascale/vulkan')\n"
            "subdir('r300/vulkan/r3v')\n")
        cases.append(("known-bad: retired leaf holds the sources", check_tree(bad), [
            "missing driver root meson.build: src/amd/r300/vulkan/meson.build",
            "src/amd/meson.build does not enter r300/vulkan via subdir()",
            "retired driver-named leaf present: src/amd/r300/vulkan/r3v",
        ]))

        # 2. The entry is removed.
        bad = tmp / "entry-dropped"
        stage_good(bad)
        (bad / "src/amd/meson.build").write_text(
            "subdir('vulkan')\nsubdir('terascale/vulkan')\n")
        cases.append(("known-bad: entry dropped", check_tree(bad), [
            "src/amd/meson.build does not enter r300/vulkan via subdir()",
        ]))

        # 3. The entry is present but commented out.
        bad = tmp / "entry-commented"
        stage_good(bad)
        (bad / "src/amd/meson.build").write_text(
            "subdir('vulkan')\nsubdir('terascale/vulkan')\n"
            "# subdir('r300/vulkan')\n")
        cases.append(("known-bad: entry commented out", check_tree(bad), [
            "src/amd/meson.build does not enter r300/vulkan via subdir()",
        ]))

        # 5. The artifact names survive as an assignment target and a comment
        #    while the DSO and the manifest have both been renamed.
        bad = tmp / "artifacts-renamed"
        stage_good(bad)
        _write_family(bad, "src/amd/r300/vulkan", artifacts_ok=False)
        cases.append(("known-bad: artifact definitions renamed", check_tree(bad), [
            "src/amd/r300/vulkan/meson.build defines no shared_library named "
            "vulkan_r3v",
            "src/amd/r300/vulkan/meson.build emits no ICD manifest named "
            "r3v_icd.<cpu>.json",
        ]))

        # 6. The same drift on the sibling family.
        bad = tmp / "terascale-artifacts-renamed"
        stage_good(bad)
        _write_family(bad, "src/amd/terascale/vulkan", artifacts_ok=False)
        cases.append((
            "known-bad: sibling family artifacts renamed", check_tree(bad), [
                "src/amd/terascale/vulkan/meson.build defines no "
                "shared_library named vulkan_terascale",
                "src/amd/terascale/vulkan/meson.build emits no ICD manifest "
                "named terascale_icd.<cpu>.json",
            ]))

        # 7. The root meson.build is gone while both entries still call it.
        bad = tmp / "root-missing"
        stage_good(bad)
        (bad / "src/amd/r300/vulkan/meson.build").unlink()
        cases.append(("known-bad: driver root meson.build removed", check_tree(bad), [
            "missing driver root meson.build: src/amd/r300/vulkan/meson.build",
        ]))

        # 8. The retired leaf reappears beside a correct root.
        bad = tmp / "leaf-reappears"
        stage_good(bad)
        (bad / "src/amd/r300/vulkan/r3v").mkdir(parents=True)
        cases.append(("known-bad: retired leaf reappears", check_tree(bad), [
            "retired driver-named leaf present: src/amd/r300/vulkan/r3v",
        ]))

        # 9. An entry file the tree no longer carries.
        bad = tmp / "entry-file-missing"
        stage_good(bad)
        (bad / "src/amd/meson.build").unlink()
        cases.append(("known-bad: entry file absent", check_tree(bad), [
            "missing build entry point: src/amd/meson.build",
            "missing build entry point: src/amd/meson.build",
            "missing build entry point: src/amd/meson.build",
        ]))

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
    ap.add_argument("--repo-root", default=None,
                    help="repository root (default: git rev-parse --show-toplevel)")
    ap.add_argument("--self-test", action="store_true",
                    help="calibrate every rejection on named known-bad trees")
    args = ap.parse_args()

    if args.self_test:
        return 0 if self_test() else 1

    if args.repo_root:
        root = Path(args.repo_root).resolve()
    else:
        root = Path(subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            check=True, capture_output=True, text=True).stdout.strip())

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
