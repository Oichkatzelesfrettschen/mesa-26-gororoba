#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rename and identity ratchet over the r3v tree.

Two renames left deliberate survivors: r300vk -> r3v (driver, env-var, and
artifact names) and the dual-backend lane identity -> the one r3v ICD
(`libvulkan_r3v_native`, `r3v_native_icd`, the two lane options, the two
lane macros, the "r3v-native" driver identity).  The ledger
docs/r3v-rename-allowlist.txt names every surviving spelling as a row --
path, token, replacement, owner, removal condition -- and this ratchet
holds the tree to it: a token hit outside the ledger fails, a ledger row
with no remaining hit fails (the row leaves with the spelling), so the
row count only falls.  Internal mechanism names (`r3v_native_*` files,
functions, `R3V_NATIVE_*` environment variables, test names) are not
product identity and are not tokens here.
"""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

TOKENS = {
    "r300vk": re.compile(r"r300vk"),
    "R300VK": re.compile(r"R300VK"),
    "libvulkan_r300": re.compile(r"libvulkan_r300\b"),
    "r300_icd_manifest": re.compile(r"r300_icd\*?\.json"),
    "r300_devenv_icd": re.compile(r"r300_devenv_icd"),
    "libvulkan_r3v_native": re.compile(r"libvulkan_r3v_native|'vulkan_r3v_native'"),
    "r3v_native_icd": re.compile(r"r3v_native_icd"),
    "r3v_native_devenv_icd": re.compile(r"r3v_native_devenv"),
    "r3v-native-backend": re.compile(r"r3v-native-backend"),
    "r3v-gallium-backend": re.compile(r"r3v-gallium-backend"),
    "R3V_NATIVE_BACKEND": re.compile(r"R3V_NATIVE_BACKEND"),
    "R3V_GALLIUM_BACKEND": re.compile(r"R3V_GALLIUM_BACKEND"),
    "r3v-native-identity": re.compile(r'"r3v-native"|\(r3v-native\)'),
}

SCAN_ROOTS = ("meson.build", "meson.options", "src", "build-infra", "docs")
LEDGER = "docs/r3v-rename-allowlist.txt"
# The ratchet names the tokens it scans for.
SELF = "src/amd/r300/vulkan/tests/r3v_rename_ratchet.py"
SKIP_DIRS = {".git", "__pycache__"}
# Retained review-thread evidence: raw GraphQL responses recorded verbatim
# from the forge, which quote whatever spelling a pull request carried at
# the time.  The ratchet governs maintained source and documentation, so
# these bytes leave the token scan and take an integrity check instead --
# the sibling manifest.json pins each one by SHA-256, so a raw file that
# changed fails here rather than passing as evidence.
IMMUTABLE_EVIDENCE = re.compile(
    r"^build-infra/docs/review-thread-frontiers/[^/]+/raw/")
EVIDENCE_MANIFEST = "manifest.json"
TEXT_SUFFIXES = {
    ".build", ".options", ".c", ".h", ".py", ".sh", ".md", ".txt", ".rst",
    ".json", ".tsv", ".meson", ".install", ".in", ".cfg", ".yml", ".yaml",
    ".toml", ".conf", ".pc", ".spvasm", ".vert", ".frag", ".glsl", "",
}
COLUMNS = ("path", "token", "replacement", "owner", "removal_condition")

STATUS_OK = 0
STATUS_FAIL = 1
STATUS_USAGE = 2


def scan_files(root: Path):
    """Every tracked text file under the scan roots.  The ratchet judges
    the repository's content, so the enumeration is git's index rather
    than the filesystem: ignored build directories, retained evidence
    residue, and other workspace-local files stay outside the verdict."""
    listing = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--", *SCAN_ROOTS],
        capture_output=True)
    if listing.returncode == 0:
        candidates = [root / rel
                      for rel in sorted(listing.stdout.decode().split("\0"))
                      if rel]
    else:
        # A root outside any repository (the selftest's fixture trees)
        # scans its filesystem under the same roots.
        candidates = []
        for entry in SCAN_ROOTS:
            start = root / entry
            if start.is_file():
                candidates.append(start)
            elif start.is_dir():
                candidates.extend(sorted(start.rglob("*")))
    for path in candidates:
        if not path.is_file():
            continue
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        if path.name == "PKGBUILD" or path.suffix in TEXT_SUFFIXES:
            yield path


def immutable_evidence(root: Path) -> list[Path]:
    return [path for path in scan_files(root)
            if IMMUTABLE_EVIDENCE.match(path.relative_to(root).as_posix())]


def check_evidence_integrity(root: Path) -> list[str]:
    """Every immutable raw file matches the digest its bundle manifest
    pins.  A file the manifest does not name cannot be shown immutable,
    so it fails the same way a modified one does."""
    failures: list[str] = []
    manifests: dict[Path, dict[str, str]] = {}
    for path in immutable_evidence(root):
        rel = path.relative_to(root).as_posix()
        bundle = path.parent.parent
        if bundle not in manifests:
            manifest = bundle / EVIDENCE_MANIFEST
            try:
                manifests[bundle] = json.loads(
                    manifest.read_text(encoding="utf-8")).get("files", {})
            except (OSError, ValueError):
                manifests[bundle] = {}
        key = f"raw/{path.name}"
        pinned = manifests[bundle].get(key)
        if pinned is None:
            failures.append(
                f"{rel}: immutable evidence outside the bundle manifest; "
                f"{EVIDENCE_MANIFEST} pins no {key}")
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != pinned:
            failures.append(
                f"{rel}: immutable evidence modified; manifest pins "
                f"{pinned[:16]} and the file hashes {digest[:16]}")
    return failures


def find_hits(root: Path) -> dict[tuple[str, str], list[int]]:
    hits: dict[tuple[str, str], list[int]] = {}
    for path in scan_files(root):
        rel = path.relative_to(root).as_posix()
        if rel in (LEDGER, SELF) or IMMUTABLE_EVIDENCE.match(rel):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for number, line in enumerate(text.splitlines(), start=1):
            for name, pattern in TOKENS.items():
                if pattern.search(line):
                    hits.setdefault((rel, name), []).append(number)
    return hits


def read_ledger(text: str) -> list[dict[str, str]]:
    rows = []
    for number, raw in enumerate(text.splitlines(), start=1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        fields = raw.split("\t")
        if len(fields) != len(COLUMNS) or any(not f.strip() for f in fields):
            raise ValueError(
                f"ledger line {number}: expected {len(COLUMNS)} tab-separated "
                f"fields {COLUMNS}, got {raw!r}")
        row = dict(zip(COLUMNS, (f.strip() for f in fields)))
        if row["token"] not in TOKENS:
            raise ValueError(
                f"ledger line {number}: unknown token {row['token']!r}; "
                f"tokens: {sorted(TOKENS)}")
        rows.append(row)
    return rows


def check(root: Path, ledger_text: str) -> list[str]:
    failures: list[str] = []
    try:
        rows = read_ledger(ledger_text)
    except ValueError as error:
        return [str(error)]
    failures.extend(check_evidence_integrity(root))
    hits = find_hits(root)
    allowed = {(row["path"], row["token"]) for row in rows}
    seen_rows: set[tuple[str, str]] = set()
    for (path, token), lines in sorted(hits.items()):
        if (path, token) in allowed:
            seen_rows.add((path, token))
        else:
            failures.append(
                f"{path}:{lines[0]}: {token} spelling outside the rename "
                f"ledger ({len(lines)} line(s))")
    for row in rows:
        key = (row["path"], row["token"])
        if key not in seen_rows:
            failures.append(
                f"ledger row {row['path']} {row['token']} has no remaining "
                f"hit; the row leaves with the spelling")
    duplicates = {k for k in allowed if sum(
        1 for r in rows if (r["path"], r["token"]) == k) > 1}
    for path, token in sorted(duplicates):
        failures.append(f"ledger row {path} {token} appears more than once")
    return failures


def run(root: Path) -> int:
    ledger = root / LEDGER
    if not ledger.is_file():
        print(f"rename ledger missing: {ledger}", file=sys.stderr)
        return STATUS_FAIL
    failures = check(root, ledger.read_text(encoding="utf-8"))
    if failures:
        print("\n".join(failures))
        return STATUS_FAIL
    rows = read_ledger(ledger.read_text(encoding="utf-8"))
    print(f"r3v_rename_ratchet: {len(rows)} ledger rows, every token "
          f"spelling ledgered, every row live")
    return STATUS_OK


def list_hits(root: Path) -> int:
    for (path, token), lines in sorted(find_hits(root).items()):
        print(f"{path}\t{token}\t{len(lines)}\t{lines[:6]}")
    return STATUS_OK


def selftest() -> int:
    checks = []

    def expect(label: str, failures: list[str], want: bool,
               marker: str = "") -> None:
        ok = bool(failures) == want and (
            not marker or any(marker in f for f in failures))
        checks.append((label, ok))
        if not ok:
            print(f"selftest {label}: got {failures!r}")

    with tempfile.TemporaryDirectory(prefix="r3v-rename-ratchet-") as tmp:
        root = Path(tmp)
        (root / "src").mkdir()
        (root / "docs").mkdir()
        (root / "src/a.c").write_text(
            'getenv("R300VK_DEBUG");\n', encoding="utf-8")
        (root / "docs/b.md").write_text("plain\n", encoding="utf-8")
        good = ("# ledger\n"
                "src/a.c\tR300VK\tR3V_DEBUG\tenv-compat retirement\t"
                "fallback reader leaves\n")
        expect("known-good", check(root, good), False)
        expect("unlisted-hit", check(root, "# empty\n"), True,
               "outside the rename ledger")
        stale = good + ("docs/b.md\tr300vk\tr3v\towner\tcondition\n")
        expect("stale-row", check(root, stale), True, "no remaining hit")
        expect("malformed-row", check(root, "src/a.c\tR300VK\n"), True,
               "tab-separated")
        expect("unknown-token", check(root, "src/a.c\tBOGUS\tx\ty\tz\n"),
               True, "unknown token")
        (root / "src/c.c").write_text(
            "struct x; /* libvulkan_r3v_native */\n", encoding="utf-8")
        expect("identity-hit", check(root, good), True,
               "libvulkan_r3v_native spelling outside")
        (root / "src/c.c").unlink()

        # Immutable retained evidence: the same forbidden spelling that
        # fails in maintained source passes here, and the manifest is
        # what decides the file is the evidence it claims to be.
        bundle = root / ("build-infra/docs/review-thread-frontiers/"
                         "merged-thread-frontier-fixture")
        raw = bundle / "raw"
        raw.mkdir(parents=True)
        evidence = raw / "merged-prs-0001.json"
        evidence.write_text('{"body": "r300vk"}\n', encoding="utf-8")
        manifest = bundle / EVIDENCE_MANIFEST

        def pin(text: str) -> None:
            manifest.write_text(json.dumps({"files": {
                "raw/merged-prs-0001.json":
                    hashlib.sha256(text.encode()).hexdigest()}}),
                encoding="utf-8")

        pin(evidence.read_text(encoding="utf-8"))
        expect("immutable-evidence-ignored", check(root, good), False)
        pin("different bytes\n")
        expect("immutable-evidence-modified", check(root, good), True,
               "immutable evidence modified")
        manifest.write_text(json.dumps({"files": {}}), encoding="utf-8")
        expect("immutable-evidence-unpinned", check(root, good), True,
               "outside the bundle manifest")

    failed = [label for label, ok in checks if not ok]
    for label, ok in checks:
        print(f"  {'ok  ' if ok else 'FAIL'} {label}")
    print(f"{len(checks) - len(failed)}/{len(checks)} checks pass")
    return STATUS_FAIL if failed else STATUS_OK


def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) == 3 and argv[1] == "--hits":
        return list_hits(Path(argv[2]))
    if len(argv) == 2:
        return run(Path(argv[1]))
    print("usage: r3v_rename_ratchet.py <repo-root> | --hits <repo-root> | "
          "--selftest", file=sys.stderr)
    return STATUS_USAGE


if __name__ == "__main__":
    sys.exit(main(sys.argv))
