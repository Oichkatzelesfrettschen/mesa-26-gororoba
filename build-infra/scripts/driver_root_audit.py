#!/usr/bin/env python3
"""Verify the AMD Vulkan driver source roots and their build-graph entry points.

Within an AMD GPU-family namespace, the Vulkan driver's source root is that
family's `vulkan/` directory.  A driver-named leaf beneath it (the retired
`src/amd/r300/vulkan/r3v/`) leaves the family and `vulkan/` levels as empty
pass-through directories that carry no meson.build, so a reader resolving the
path by analogy with a sibling family reaches a directory holding no sources.

The audit pins each family's root, the `subdir()` call that enters it, and the
installed artifact names, which are driver identity rather than directory
placement and stay spelled as the ICD manifests spell them.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# Family namespace -> the meson.build that enters that family's vulkan/ root.
AMD_VULKAN_FAMILIES = {
    "src/amd/vulkan": "src/amd/meson.build",
    "src/amd/terascale/vulkan": "src/amd/meson.build",
    "src/amd/r300/vulkan": "src/meson.build",
}

# Driver identity survives a directory move: these names are spelled by the
# installed DSO and the ICD manifests, and the audit holds them fixed.
DRIVER_ARTIFACT_NAMES = {
    "src/amd/r300/vulkan/meson.build": ["vulkan_r3v", "r3v_icd"],
    "src/amd/terascale/vulkan/meson.build": ["vulkan_terascale", "terascale_icd"],
}

RETIRED_PATHS = ["src/amd/r300/vulkan/r3v"]

# Spellings of the retired leaf, absolute and relative.  A comment naming
# `r3v/meson.build` drifts the same way the directory did, and the relative
# form survives a sweep that only looks for the full path.
RETIRED_PATH_STRINGS = ["src/amd/r300/vulkan/r3v/", "r3v/meson.build"]

# Retained evidence records the path that existed when it was produced.  The
# allowlist and the migration docs are the two current-authority files that
# quote a retired path deliberately.
EVIDENCE_PREFIXES = (
    "docs/r3v-rename-allowlist.txt",
)
EVIDENCE_DIR_PARTS = frozenset({"results", "bundles", ".git", "build", "builddir"})

TEXT_SUFFIXES = frozenset(
    {".c", ".h", ".cpp", ".hpp", ".py", ".build", ".options", ".md",
     ".txt", ".rst", ".sh", ".ini", ".toml", ".yml", ".yaml", ".meson"}
)


def strip_meson_comments(text):
    """Drop `#` comments so a commented-out subdir() never satisfies the gate."""
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def check_tree(root):
    """Return the list of failure strings for the tree rooted at `root`."""
    failures = []

    for family_root, entry_file in AMD_VULKAN_FAMILIES.items():
        meson = root / family_root / "meson.build"
        if not meson.is_file():
            failures.append(f"missing driver root meson.build: {family_root}/meson.build")

        entry = root / entry_file
        if not entry.is_file():
            failures.append(f"missing build entry point: {entry_file}")
            continue
        # subdir() paths are relative to the entering file's directory.
        relative = str(Path(family_root).relative_to(Path(entry_file).parent))
        body = strip_meson_comments(entry.read_text(encoding="utf-8", errors="replace"))
        if not re.search(r"subdir\s*\(\s*['\"]%s['\"]" % re.escape(relative), body):
            failures.append(f"{entry_file} does not enter {relative} via subdir()")

    for retired in RETIRED_PATHS:
        if (root / retired).exists():
            failures.append(f"retired driver-named leaf present: {retired}")

    for meson_rel, names in DRIVER_ARTIFACT_NAMES.items():
        meson = root / meson_rel
        if not meson.is_file():
            continue
        body = meson.read_text(encoding="utf-8", errors="replace")
        for name in names:
            if name not in body:
                failures.append(f"{meson_rel} no longer names the artifact {name}")

    failures.extend(scan_retired_path_strings(root))
    return failures


def candidate_files(root):
    """Yield the tracked text files that carry current authority.

    Tracked files are the repository's own statements; build directories and
    retained logs sit outside the index and record what ran at the time.
    """
    try:
        listing = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            check=True, capture_output=True, text=True).stdout.split("\0")
        rels = [Path(name) for name in listing if name]
    except (OSError, subprocess.CalledProcessError):
        rels = [p.relative_to(root) for p in root.rglob("*") if p.is_file()]
    for rel in rels:
        path = root / rel
        if path.suffix in TEXT_SUFFIXES and path.is_file():
            yield rel, path


def scan_retired_path_strings(root):
    """Report current-authority files that still spell a retired path."""
    failures = []
    this_file = Path(__file__).resolve()
    for needle in RETIRED_PATH_STRINGS:
        for rel, path in candidate_files(root):
            if EVIDENCE_DIR_PARTS.intersection(rel.parts):
                continue
            if str(rel).startswith(EVIDENCE_PREFIXES):
                continue
            # The audit names the retired path to detect it.
            if path.resolve() == this_file:
                continue
            try:
                body = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if needle in body:
                failures.append(f"{rel} references the retired path {needle}")
    return failures


def self_test():
    """Calibrate the verdict against a known-good and a known-bad tree.

    The known-bad reproduces the exact pre-move defect: the driver-named leaf
    holds the sources and the entry point spells the leaf in its subdir() call.
    """
    results = []
    with tempfile.TemporaryDirectory() as tmp:
        good = Path(tmp) / "good"
        for family in AMD_VULKAN_FAMILIES:
            (good / family).mkdir(parents=True)
            body = "meson.build\n"
            for name in DRIVER_ARTIFACT_NAMES.get(family + "/meson.build", []):
                body += f"# {name}\n"
            (good / family / "meson.build").write_text(body)
        (good / "src/amd/meson.build").write_text(
            "subdir('vulkan')\nsubdir('terascale/vulkan')\n")
        (good / "src/meson.build").write_text("subdir('amd/r300/vulkan')\n")
        results.append(("known-good tree", check_tree(good), False))

        bad = Path(tmp) / "bad"
        for family in AMD_VULKAN_FAMILIES:
            (bad / family).mkdir(parents=True)
            if family != "src/amd/r300/vulkan":
                body = "meson.build\n"
                for name in DRIVER_ARTIFACT_NAMES.get(family + "/meson.build", []):
                    body += f"# {name}\n"
                (bad / family / "meson.build").write_text(body)
        leaf = bad / "src/amd/r300/vulkan/r3v"
        leaf.mkdir(parents=True)
        (leaf / "meson.build").write_text("# vulkan_r3v r3v_icd\n")
        (bad / "src/amd/meson.build").write_text(
            "subdir('vulkan')\nsubdir('terascale/vulkan')\n")
        (bad / "src/meson.build").write_text("subdir('amd/r300/vulkan/r3v')\n")
        results.append(("known-bad tree", check_tree(bad), True))

    ok = True
    for label, failures, expect_failures in results:
        detected = bool(failures)
        verdict = "OK" if detected == expect_failures else "MISCALIBRATED"
        if detected != expect_failures:
            ok = False
        print(f"{verdict}  {label}: {len(failures)} failure(s)")
        for line in failures:
            print(f"        {line}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", default=None,
                    help="repository root (default: git rev-parse --show-toplevel)")
    ap.add_argument("--self-test", action="store_true",
                    help="calibrate the verdict on synthetic known-good and known-bad trees")
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
