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
    "libvulkan_r3v_native": re.compile(
        r"libvulkan_r3v_native|'vulkan_r3v_native'"),
    "r3v_native_icd": re.compile(r"r3v_native_icd"),
    "r3v_native_devenv_icd": re.compile(r"r3v_native_devenv"),
    "r3v-native-backend": re.compile(r"r3v-native-backend"),
    "r3v-gallium-backend": re.compile(r"r3v-gallium-backend"),
    "R3V_NATIVE_BACKEND": re.compile(r"R3V_NATIVE_BACKEND"),
    "R3V_GALLIUM_BACKEND": re.compile(r"R3V_GALLIUM_BACKEND"),
    "r3v-native-identity": re.compile(
        r'"r3v-native"|\'r3v-native\'|`r3v-native`|\(r3v-native\)'),
}

SCAN_ROOTS = ("meson.build", "meson.options", "src", "build-infra", "docs")
LEDGER = "docs/r3v-rename-allowlist.txt"
# The ratchet names the tokens it scans for.
SELF = "src/amd/r300/vulkan/tests/r3v_rename_ratchet.py"
SKIP_DIRS = {".git", "__pycache__"}
# Retained review evidence: forge responses recorded verbatim and the
# tables derived from them, which quote whatever spelling a pull request
# carried at the time.  The ratchet governs maintained source and
# documentation, so these bytes leave the token scan and take an
# integrity check instead.  Membership in a manifest decides, with no
# path prefix in the rule: the nearest ancestor directory holding a
# manifest.json with a files dict seals every file below it, and that
# dict is the whole definition of what the bundle contains.  A file the
# dict names is pinned by SHA-256 and reports as modified evidence when
# its bytes drift; a file below the seal the dict leaves out reports as
# unpinned evidence.  A bundle therefore covers a capture shape the tree
# has not used yet the moment its manifest names it, and a nested
# manifest (a generated/ directory inside a corpus) seals its own
# subtree while the files beside it stay maintained content.
# manifest.json is the seal rather than the evidence, so it stays in the
# token scan, and a directory carrying no manifest on any ancestor is
# unsealed and scanned the same way.
EVIDENCE_MANIFEST = "manifest.json"
COLUMNS = ("path", "token", "replacement", "owner", "removal_condition",
           "occurrence_count", "location_sha256")
# The baseline is intentionally an append-only boundary: a compatible spelling
# may leave the ledger after its removal, while a new path/token pair requires
# a reviewed ratchet change rather than a routine allowlist edit.
BASELINE_KEYS = frozenset({
    ("build-infra/docs/last-100-pr-review-comment-audit.md", "r300vk"),
    ("build-infra/docs/last-100-pr-review-comment-audit.md", "R300VK"),
    ("build-infra/docs/review-thread-classifications/"
     "merged-thread-frontier-after-WhUFS/action-frontier.tsv", "R300VK"),
    ("build-infra/docs/review-thread-classifications/"
     "merged-thread-frontier-after-WhUFS/assessments.tsv", "R300VK"),
    ("build-infra/docs/review-thread-classifications/"
     "merged-thread-frontier-after-WhUFS/"
     "pre-resolution-frontier.tsv", "R300VK"),
    ("build-infra/docs/review-thread-clusters/"
     "unresolved-review-thread-corpus-4b965810d471/"
     "review-status.tsv",
     "r3v-native-backend"),
})

STATUS_OK = 0
STATUS_FAIL = 1
STATUS_USAGE = 2


def scan_files(root: Path):
    """Every tracked regular file.  Token matching accepts only UTF-8 text,
    but repository paths enter the same scan whether or not their contents
    decode.  Git's index excludes ignored build products and local residue."""
    listing = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        capture_output=True)
    if listing.returncode == 0:
        candidates = [root / rel
                      for rel in sorted(listing.stdout.decode().split("\0"))
                      if rel]
    else:
        # The fixture has no index.  Its filesystem fallback uses the same
        # regular-file domain, so fixture results cover the production rule.
        candidates = sorted(root.rglob("*"))
    for path in candidates:
        if not path.is_file():
            continue
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        yield path


def bundle_seal(
        root: Path, rel: str,
        manifest_cache: dict[str, dict | None]
) -> tuple[str, str, dict] | None:
    """The sealed bundle holding rel, as (bundle, bundle-relative path,
    pinned files), or None when rel sits outside a sealed bundle.  The
    seal is the nearest ancestor manifest, so a file with no manifest on
    any ancestor holds no evidence and stays in the token scan."""
    parts = rel.split("/")
    if parts[-1] == EVIDENCE_MANIFEST:
        parts = parts[:-1]
    for depth in range(len(parts) - 1, 0, -1):
        bundle = "/".join(parts[:depth])
        if bundle not in manifest_cache:
            try:
                manifest_text = (root / bundle / EVIDENCE_MANIFEST).read_text(
                    encoding="utf-8")
                files = json.loads(manifest_text).get("files", {})
            except (OSError, ValueError):
                files = None
            manifest_cache[bundle] = files if isinstance(files, dict) else None
        files = manifest_cache[bundle]
        if files is None:
            continue
        inner = "/".join(parts[depth:])
        if inner == EVIDENCE_MANIFEST or not inner:
            return None
        return bundle, inner, files
    return None


def immutable_evidence(
        root: Path, manifest_cache: dict[str, dict | None]
) -> list[tuple[Path, str, dict]]:
    found = []
    for path in scan_files(root):
        seal = bundle_seal(
            root, path.relative_to(root).as_posix(), manifest_cache)
        if seal is not None:
            found.append((path, seal[1], seal[2]))
    return found


def check_evidence_integrity(
        root: Path, manifest_cache: dict[str, dict | None]
) -> list[str]:
    """Every file inside a sealed bundle matches the digest its manifest
    pins.  A file the manifest does not name cannot be shown immutable,
    so it fails the same way a modified one does."""
    failures: list[str] = []
    for path, inner, files in immutable_evidence(root, manifest_cache):
        rel = path.relative_to(root).as_posix()
        pinned = files.get(inner)
        if pinned is None:
            failures.append(
                f"{rel}: immutable evidence outside the bundle manifest; "
                f"{EVIDENCE_MANIFEST} pins no {inner}")
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != pinned:
            failures.append(
                f"{rel}: immutable evidence modified; manifest pins "
                f"{pinned[:16]} and the file hashes {digest[:16]}")
    return failures


def location_digest(locations: tuple[str, ...]) -> str:
    return hashlib.sha256("\n".join(locations).encode("utf-8")).hexdigest()


def find_hits(
        root: Path, manifest_cache: dict[str, dict | None] | None = None
) -> dict[tuple[str, str], tuple[str, ...]]:
    hits: dict[tuple[str, str], tuple[str, ...]] = {}
    if manifest_cache is None:
        manifest_cache = {}
    for path in scan_files(root):
        rel = path.relative_to(root).as_posix()
        if (rel in (LEDGER, SELF) or
                bundle_seal(root, rel, manifest_cache) is not None):
            continue
        text: str | None
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            text = None
        for name, pattern in TOKENS.items():
            locations = [f"path:{match.start() + 1}"
                         for match in pattern.finditer(rel)]
            if text is not None:
                for number, line in enumerate(text.splitlines(), start=1):
                    locations.extend(f"{number}:{match.start() + 1}"
                                     for match in pattern.finditer(line))
            if locations:
                hits[rel, name] = tuple(locations)
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
        try:
            row["occurrence_count"] = str(int(row["occurrence_count"]))
        except ValueError as error:
            raise ValueError(
                f"ledger line {number}: occurrence_count must be an integer") \
                from error
        if int(row["occurrence_count"]) < 1:
            raise ValueError(
                f"ledger line {number}: occurrence_count must be positive")
        if not re.fullmatch(r"[0-9a-f]{64}", row["location_sha256"]):
            raise ValueError(
                f"ledger line {number}: location_sha256 must be a "
                "SHA-256 digest")
        rows.append(row)
    return rows


def check(
        root: Path, ledger_text: str,
        baseline_keys: frozenset[tuple[str, str]] = BASELINE_KEYS
) -> list[str]:
    failures: list[str] = []
    try:
        rows = read_ledger(ledger_text)
    except ValueError as error:
        return [str(error)]
    manifest_cache: dict[str, dict | None] = {}
    failures.extend(check_evidence_integrity(root, manifest_cache))
    hits = find_hits(root, manifest_cache)
    allowed = {(row["path"], row["token"]): row for row in rows}
    seen_rows: set[tuple[str, str]] = set()
    for row in rows:
        key = (row["path"], row["token"])
        if key not in baseline_keys:
            failures.append(
                f"ledger row {row['path']} {row['token']} lies outside the "
                "fixed baseline")
    for (path, token), locations in sorted(hits.items()):
        row = allowed.get((path, token))
        if row is not None:
            seen_rows.add((path, token))
            expected_count = int(row["occurrence_count"])
            observed_digest = location_digest(locations)
            if (expected_count != len(locations) or
                    row["location_sha256"] != observed_digest):
                failures.append(
                    f"{path}: {token} locations changed; ledger expects "
                    f"{expected_count} occurrence(s) at "
                    f"{row['location_sha256']}, observed {len(locations)} at "
                    f"{observed_digest} ({', '.join(locations)})")
        else:
            failures.append(
                f"{path}:{locations[0]}: {token} spelling outside the rename "
                f"ledger ({len(locations)} occurrence(s))")
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
    for (path, token), locations in sorted(find_hits(root).items()):
        print(f"{path}\t{token}\t{len(locations)}\t"
              f"{location_digest(locations)}\t"
              f"{','.join(locations)}")
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
        fixture_baseline = frozenset({("src/a.c", "R300VK")})

        def ledger_row(path: str, token: str, replacement: str,
                       owner: str, removal_condition: str) -> str:
            locations = find_hits(root)[path, token]
            return "\t".join((path, token, replacement, owner,
                              removal_condition, str(len(locations)),
                              location_digest(locations))) + "\n"

        def check_fixture(ledger_text: str) -> list[str]:
            return check(root, ledger_text, fixture_baseline)

        good = "# ledger\n" + ledger_row(
            "src/a.c", "R300VK", "R3V_DEBUG", "env-compat retirement",
            "fallback reader leaves")
        expect("known-good", check_fixture(good), False)
        expect("unlisted-hit", check_fixture("# empty\n"), True,
               "outside the rename ledger")
        (root / "docs/b.md").write_text("r300vk\n", encoding="utf-8")
        expanded = good + ledger_row(
            "docs/b.md", "r300vk", "r3v", "owner", "condition")
        expect("added-row", check_fixture(expanded), True,
               "outside the fixed baseline")
        (root / "docs/b.md").write_text("plain\n", encoding="utf-8")
        stale = good + ("docs/b.md\tr300vk\tr3v\towner\tcondition\t1\t"
                        "00000000000000000000000000000000"
                        "00000000000000000000000000000000\n")
        expect("stale-row", check_fixture(stale), True, "no remaining hit")
        expect("malformed-row", check_fixture("src/a.c\tR300VK\n"), True,
               "tab-separated")
        expect("unknown-token", check_fixture(
            "src/a.c\tBOGUS\tx\ty\tz\t1\t"
            "00000000000000000000000000000000"
            "00000000000000000000000000000000\n"),
            True, "unknown token")
        (root / "src/a.c").write_text(
            'getenv("R300VK_DEBUG"); R300VK_NEW_REGRESSION;\n',
            encoding="utf-8")
        expect("additional-occurrence", check_fixture(good), True,
               "locations changed")
        (root / "src/a.c").write_text(
            "\ngetenv(\"R300VK_DEBUG\");\n", encoding="utf-8")
        expect("moved-occurrence", check_fixture(good), True,
               "locations changed")
        (root / "src/a.c").write_text(
            'getenv("R300VK_DEBUG");\n', encoding="utf-8")
        excluded_source = root / "src/excluded.cpp"
        excluded_source.write_text("R300VK_DEBUG\n", encoding="utf-8")
        expect("excluded-suffix-scanned", check_fixture(good), True,
               "excluded.cpp:1:1: R300VK spelling outside")
        excluded_source.unlink()
        path_only_source = root / "src/r300vk_regression.c"
        path_only_source.write_text("clean\n", encoding="utf-8")
        expect("path-name-scanned", check_fixture(good), True,
               "r300vk_regression.c:path:5: r300vk spelling outside")
        path_only_source.unlink()
        (root / "src/c.c").write_text(
            "struct x; /* libvulkan_r3v_native */\n", encoding="utf-8")
        expect("identity-hit", check_fixture(good), True,
               "libvulkan_r3v_native spelling outside")
        (root / "src/c.c").unlink()
        (root / "src/c.c").write_text(
            "'r3v-native' `r3v-native` r3v-native_helper\n",
            encoding="utf-8")
        expect("quoted-r3v-native-identity", check_fixture(good), True,
               "r3v-native-identity spelling outside")
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
        expect("immutable-evidence-ignored", check_fixture(good), False)
        pin("different bytes\n")
        expect("immutable-evidence-modified", check_fixture(good), True,
               "immutable evidence modified")
        manifest.write_text(json.dumps({"files": {}}), encoding="utf-8")
        expect("immutable-evidence-unpinned", check_fixture(good), True,
               "outside the bundle manifest")

        # Membership rather than a path prefix decides, so a capture
        # written beside raw/ is covered the moment the manifest pins it
        # and fails as maintained source until then.
        beside = bundle / "selected-threads.json"
        beside.write_text('{"body": "R300VK_DEBUG"}\n', encoding="utf-8")
        pin(evidence.read_text(encoding="utf-8"))
        expect("capture-beside-raw-unpinned", check_fixture(good), True,
               "outside the bundle manifest")
        manifest.write_text(json.dumps({"files": {
            "raw/merged-prs-0001.json": hashlib.sha256(
                evidence.read_bytes()).hexdigest(),
            "selected-threads.json": hashlib.sha256(
                beside.read_bytes()).hexdigest()}}), encoding="utf-8")
        expect("capture-beside-raw-pinned", check_fixture(good), False)

        # The manifest is the seal rather than the evidence, so its own
        # bytes stay in the token scan.
        sealed = json.loads(manifest.read_text(encoding="utf-8"))
        sealed["note"] = "R300VK_DEBUG"
        manifest.write_text(json.dumps(sealed), encoding="utf-8")
        expect("manifest-itself-scanned", check_fixture(good), True,
               "R300VK spelling outside")
        manifest.write_text(json.dumps({"files": {
            "raw/merged-prs-0001.json": hashlib.sha256(
                evidence.read_bytes()).hexdigest(),
            "selected-threads.json": hashlib.sha256(
                beside.read_bytes()).hexdigest()}}), encoding="utf-8")

        # A directory under the evidence root carrying no manifest is
        # unsealed, so it is maintained content and the scan reaches it.
        unsealed = (root / "build-infra/docs/review-thread-frontiers/"
                    "merged-pr-range-fixture")
        unsealed.mkdir(parents=True)
        (unsealed / "action-frontier.tsv").write_text(
            "row\tR300VK_DEBUG\n", encoding="utf-8")
        expect("unsealed-directory-scanned", check_fixture(good), True,
               "R300VK spelling outside")
        (unsealed / "action-frontier.tsv").unlink()

        # Membership rather than a path prefix seals: a corpus under any
        # other docs directory takes the same pinned, modified, and
        # unpinned verdicts on its tables.
        corpus = root / ("build-infra/docs/review-thread-corpus/"
                         "unresolved-review-thread-corpus-fixture")
        corpus.mkdir(parents=True)
        table = corpus / "work-groups.tsv"
        table.write_text("id\tr3v-native-backend\n", encoding="utf-8")
        expect("corpus-table-no-manifest-scanned", check_fixture(good), True,
               "r3v-native-backend spelling outside")
        (corpus / EVIDENCE_MANIFEST).write_text(json.dumps({"files": {
            "work-groups.tsv": hashlib.sha256(
                table.read_bytes()).hexdigest()}}), encoding="utf-8")
        expect("corpus-table-pinned", check_fixture(good), False)
        table.write_text("id\tr3v-native-backend\tedited\n",
                         encoding="utf-8")
        expect("corpus-table-modified", check_fixture(good), True,
               "immutable evidence modified")
        table.write_text("id\tr3v-native-backend\n", encoding="utf-8")
        (corpus / "group-members.tsv").write_text("id\tr300vk\n",
                                                  encoding="utf-8")
        expect("corpus-table-unpinned", check_fixture(good), True,
               "outside the bundle manifest")
        (corpus / "group-members.tsv").unlink()

        # A nested manifest seals its own subtree only: the generated
        # tables it pins pass, and a maintained table beside that
        # subtree stays in the token scan.
        clusters = root / ("build-infra/docs/review-thread-clusters/"
                           "unresolved-review-thread-corpus-fixture")
        generated = clusters / "generated"
        generated.mkdir(parents=True)
        derived = generated / "review-clusters.tsv"
        derived.write_text("id\tr3v-native-backend\n", encoding="utf-8")
        (generated / EVIDENCE_MANIFEST).write_text(json.dumps({"files": {
            "review-clusters.tsv": hashlib.sha256(
                derived.read_bytes()).hexdigest()}}), encoding="utf-8")
        expect("nested-generated-pinned", check_fixture(good), False)
        (clusters / "review-status.tsv").write_text(
            "id\tr3v-native-backend\n", encoding="utf-8")
        expect("beside-nested-seal-scanned", check_fixture(good), True,
               "review-status.tsv:1:4: r3v-native-backend spelling outside")

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
