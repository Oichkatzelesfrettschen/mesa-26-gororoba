#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify the finite ownership disposition of the Gallium-backed R3V lane.

The fake ICD is a temporary Vulkan owner, not a second consumer domain.  This
verdict producer derives that lane's production and test inputs from Meson,
adds the one r300g-resident classifier reached only by the fake pipeline, and
requires one disposition for every resulting file.  A whole-file DELETE row
covers every function in that file; separately named function rows pin the
mechanisms most likely to be mistaken for reusable Gallium or common code.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from dataclasses import dataclass, replace
from pathlib import Path


SCHEMA = "r3v-gallium-ownership-disposition/v1"
HEADER = (
    "kind",
    "path",
    "symbol",
    "disposition",
    "future_owner",
    "basis",
    "reason",
)
DISPOSITIONS = {"MOVE_R300G", "EXTRACT_COMMON", "KEEP_FIXTURE", "DELETE"}

VULKAN_MESON = Path("src/amd/r300/vulkan/meson.build")
R300G_MESON = Path("src/gallium/drivers/r300/meson.build")
LEDGER = Path("src/amd/r300/vulkan/r3v_gallium_ownership_disposition.tsv")
FIXTURE_SPEC = Path(
    "src/amd/r300/vulkan/tests/fixtures/"
    "r3v_gallium_ownership_known_bad.json")

# This source physically sits in files_r300, but its public detectors are
# reached only by the fake Vulkan pipeline.  Keeping the root explicit makes
# the exceptional cross-directory ownership auditable without treating every
# ordinary r300g header included by the adapter as a migration candidate.
EXTERNAL_CANDIDATE_FILES = {
    Path("src/gallium/drivers/r300/r300_compute_admission.c"),
    Path("src/gallium/drivers/r300/r300_compute_admission.h"),
    Path("src/gallium/drivers/r300/r300_compute_admission_match.h"),
    Path("src/gallium/drivers/r300/tests/r300_nir_compute_admission_test.c"),
}

# These are the separable bodies whose names previously invited a move.  A
# full-file DELETE row already controls all other functions in the unit.
REQUIRED_FUNCTION_ROWS = {
    (Path("src/amd/r300/vulkan/r3v_image.c"), "r3v_split_image_axis"),
    (Path("src/amd/r300/vulkan/r3v_pipeline.c"),
     "r3v_prepare_shader_nir"),
    (Path("src/amd/r300/vulkan/r3v_queue.c"), "r3v_replay_dispatch"),
    (Path("src/amd/r300/vulkan/r3v_identity_map.c"),
     "r3v_identity_map_dispatch_replay"),
    (Path("src/amd/r300/vulkan/r3v_dp4_fs_nir.c"),
     "r3v_build_dp4_fs_nir"),
    (Path("src/gallium/drivers/r300/r300_compute_admission.c"),
     "r300_nir_classify_compute"),
}

MOVE_FORBIDDEN = (
    re.compile(r"\bVk[A-Z][A-Za-z0-9_]*\b"),
    re.compile(r"\bvk_[A-Za-z0-9_]+\b"),
    re.compile(r"\br3v_[A-Za-z0-9_]+\b"),
    re.compile(r"\b(?:vulkan|tgsi)[A-Za-z0-9_]*\b", re.IGNORECASE),
    re.compile(
        r"\b(?:descriptor_set|command_buffer|queue_submit|render_pass|"
        r"pipeline_layout)\b"),
)
COMMON_FORBIDDEN = MOVE_FORBIDDEN + (
    re.compile(r"\bpipe_[A-Za-z0-9_]+\b"),
    re.compile(r"\bnir_[A-Za-z0-9_]+\b"),
    re.compile(r"\b(?:vtn|spirv_to_nir)[A-Za-z0-9_]*\b"),
)


@dataclass(frozen=True)
class Row:
    kind: str
    path: Path
    symbol: str
    disposition: str
    future_owner: str
    basis: str
    reason: str

    @property
    def key(self) -> tuple[str, Path, str]:
        return (self.kind, self.path, self.symbol)


def strip_comments_and_strings(text: str) -> str:
    """Remove non-code tokens before dependency-vocabulary scans."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    return re.sub(r"'(?:\\.|[^'\\])*'", "''", text)


def conditional_blocks(text: str, condition: str) -> list[str]:
    """Return complete Meson if-blocks whose opening line has condition."""
    lines = text.splitlines()
    blocks: list[str] = []
    index = 0
    while index < len(lines):
        opening = lines[index].strip()
        if not opening.startswith("if ") or condition not in opening:
            index += 1
            continue
        depth = 0
        block: list[str] = []
        while index < len(lines):
            line = lines[index]
            directive = line.strip()
            if directive.startswith("if "):
                depth += 1
            if directive == "endif":
                depth -= 1
            block.append(line)
            index += 1
            if depth == 0:
                break
        blocks.append("\n".join(block))
    return blocks


def quoted_sources(text: str) -> set[Path]:
    """Collect repository-local C paths named by a Meson fragment."""
    paths: set[Path] = set()
    for raw in re.findall(r"['\"]([^'\"]+\.[ch])['\"]", text):
        path = Path(raw)
        if ".." in path.parts:
            continue
        paths.add(path)
    return paths


def resolve_vulkan_path(relative: Path) -> Path:
    return Path("src/amd/r300/vulkan") / relative


def resolve_local_header(root: Path, source: Path, include: str) -> Path | None:
    candidates = (source.parent / include,
                  Path("src/amd/r300/vulkan") / include)
    for candidate in candidates:
        absolute = (root / candidate).resolve()
        try:
            normalized = absolute.relative_to(root)
        except ValueError:
            continue
        if absolute.is_file():
            return normalized
    return None


def reachable_local_headers(root: Path, roots: set[Path]) -> set[Path]:
    """Follow only quoted headers owned by the Vulkan directory."""
    pending = list(roots)
    visited: set[Path] = set()
    headers: set[Path] = set()
    while pending:
        source = pending.pop()
        if source in visited or not (root / source).is_file():
            continue
        visited.add(source)
        text = (root / source).read_text(encoding="utf-8")
        for include in re.findall(r'^\s*#\s*include\s+"([^"]+\.h)"',
                                  text, flags=re.MULTILINE):
            header = resolve_local_header(root, source, include)
            if header is None or not str(header).startswith(
                    "src/amd/r300/vulkan/"):
                continue
            if header not in headers:
                headers.add(header)
                pending.append(header)
    return headers


def derive_scope(root: Path) -> tuple[set[Path], set[Path]]:
    """Return the required file rows and r300g production source set."""
    meson_text = (root / VULKAN_MESON).read_text(encoding="utf-8")
    fake_blocks = conditional_blocks(
        meson_text,
        "get_option('r3v-gallium-backend') and with_gallium_r300")
    if not fake_blocks:
        raise ValueError("no Gallium-backed R3V Meson block found")
    fake_roots = {
        resolve_vulkan_path(path)
        for block in fake_blocks
        for path in quoted_sources(block)
        if path.suffix == ".c"
    }

    loader_match = re.search(
        r"libr3v_files\s*=\s*files\((.*?)\)\s*\n",
        meson_text, flags=re.DOTALL)
    if loader_match is None:
        raise ValueError("R3V loader-base source list not found")
    native_roots = {
        resolve_vulkan_path(path)
        for path in quoted_sources(loader_match.group(1))
        if path.suffix == ".c"
    }
    native_blocks = conditional_blocks(
        meson_text, "get_option('r3v-native-backend')")
    if len(native_blocks) != 1:
        raise ValueError("native R3V Meson block is missing or ambiguous")
    native_roots.update(
        resolve_vulkan_path(path)
        for path in quoted_sources(native_blocks[0])
        if path.suffix == ".c" and not str(path).startswith("tests/"))

    fake_headers = reachable_local_headers(root, fake_roots)
    native_headers = reachable_local_headers(root, native_roots)
    fake_only_headers = fake_headers - native_headers
    required = fake_roots | fake_only_headers | EXTERNAL_CANDIDATE_FILES

    r300g_text = (root / R300G_MESON).read_text(encoding="utf-8")
    source_match = re.search(r"files_r300\s*=\s*files\((.*?)\)\s*\n",
                             r300g_text, flags=re.DOTALL)
    if source_match is None:
        raise ValueError("r300g production source list not found")
    r300g_sources = {
        Path("src/gallium/drivers/r300") / path
        for path in quoted_sources(source_match.group(1))
        if path.suffix in {".c", ".h"} and len(path.parts) == 1
    }
    return required, r300g_sources


def read_rows(path: Path) -> list[Row]:
    text = path.read_text(encoding="utf-8")
    lines_on_disk = text.splitlines(keepends=True)
    if not lines_on_disk or lines_on_disk[0].strip() != f"# schema={SCHEMA}":
        raise ValueError(f"ledger schema must be exactly {SCHEMA}")
    lines = (line for line in lines_on_disk if not line.startswith("#"))
    reader = csv.DictReader(lines, delimiter="\t")
    if tuple(reader.fieldnames or ()) != HEADER:
        raise ValueError(
            f"ledger header must be exactly {list(HEADER)!r}")
    rows = []
    for number, raw in enumerate(reader, start=2):
        if None in raw or any(value is None for value in raw.values()):
            raise ValueError(f"malformed ledger row {number}")
        rows.append(Row(
            raw["kind"], Path(raw["path"]), raw["symbol"],
            raw["disposition"], raw["future_owner"], raw["basis"],
            raw["reason"]))
    return rows


def symbol_exists(root: Path, path: Path, symbol: str) -> bool:
    if not (root / path).is_file():
        return False
    text = strip_comments_and_strings(
        (root / path).read_text(encoding="utf-8"))
    return re.search(rf"\b{re.escape(symbol)}\s*\(", text) is not None


def parse_basis(basis: str) -> list[tuple[Path, str]]:
    parsed = []
    for item in basis.split(";"):
        if "::" not in item:
            return []
        path, symbol = item.split("::", 1)
        parsed.append((Path(path), symbol))
    return parsed


def forbidden_tokens(root: Path, row: Row) -> list[str]:
    if row.disposition not in {"MOVE_R300G", "EXTRACT_COMMON"}:
        return []
    text = strip_comments_and_strings(
        (root / row.path).read_text(encoding="utf-8"))
    patterns = (MOVE_FORBIDDEN if row.disposition == "MOVE_R300G"
                else COMMON_FORBIDDEN)
    return sorted({match.group(0)
                   for pattern in patterns for match in pattern.finditer(text)})


def public_compute_symbols(root: Path) -> set[str]:
    header = root / "src/gallium/drivers/r300/r300_compute_admission.h"
    text = strip_comments_and_strings(header.read_text(encoding="utf-8"))
    return set(re.findall(r"\b(r300_nir_[A-Za-z0-9_]+)\s*\(", text))


def validate(root: Path, rows: list[Row], required: set[Path],
             r300g_sources: set[Path], *, enforce_compute_orphan: bool = True
             ) -> list[str]:
    errors: list[str] = []
    keys: set[tuple[str, Path, str]] = set()
    file_rows: dict[Path, Row] = {}
    for row in rows:
        if row.key in keys:
            errors.append(f"duplicate row: {row.key}")
            continue
        keys.add(row.key)
        if row.kind not in {"FILE", "FUNCTION"}:
            errors.append(f"{row.path}: invalid kind {row.kind}")
        if row.disposition not in DISPOSITIONS:
            errors.append(
                f"{row.path}: invalid disposition {row.disposition}")
        if not row.reason:
            errors.append(f"{row.path}: disposition reason is empty")
        if not (root / row.path).is_file():
            errors.append(f"{row.path}: disposition input missing")
            continue
        if row.kind == "FILE":
            if row.symbol != "-":
                errors.append(f"{row.path}: FILE row must use symbol '-'")
            file_rows[row.path] = row
        elif not symbol_exists(root, row.path, row.symbol):
            errors.append(f"{row.path}: function {row.symbol} not found")

    missing = sorted(required - set(file_rows), key=str)
    extra = sorted(set(file_rows) - required, key=str)
    errors.extend(f"missing disposition row: {path}" for path in missing)
    errors.extend(f"stale disposition row: {path}" for path in extra)

    for path, symbol in sorted(REQUIRED_FUNCTION_ROWS,
                               key=lambda item: (str(item[0]), item[1])):
        key = ("FUNCTION", path, symbol)
        if path in required and key not in keys:
            errors.append(f"missing function disposition: {path}::{symbol}")

    for row in rows:
        if row.kind == "FUNCTION":
            parent = file_rows.get(row.path)
            if parent is None:
                continue
            if parent.disposition == "DELETE" and row.disposition != "DELETE":
                errors.append(
                    f"{row.path}::{row.symbol}: function survives a deleted file")

        if row.disposition == "DELETE":
            if row.future_owner != "-" or row.basis != "-":
                errors.append(
                    f"{row.path}: DELETE row must have '-' owner and basis")
            continue

        if row.disposition == "KEEP_FIXTURE":
            if "/tests/" not in f"/{row.path}":
                errors.append(f"{row.path}: KEEP_FIXTURE is not a test file")
            basis = parse_basis(row.basis)
            if len(basis) != 1 or not symbol_exists(root, *basis[0]):
                errors.append(
                    f"{row.path}: fixture basis is not one present mechanism")
            if not row.future_owner.startswith("src/"):
                errors.append(f"{row.path}: fixture future owner is not explicit")
            continue

        basis = parse_basis(row.basis)
        if row.disposition == "MOVE_R300G":
            if not row.future_owner.startswith("src/gallium/drivers/r300/"):
                errors.append(f"{row.path}: MOVE_R300G owner is outside r300g")
            if len(basis) != 1:
                errors.append(f"{row.path}: MOVE_R300G needs one production caller")
            elif basis[0][0] not in r300g_sources or not symbol_exists(
                    root, *basis[0]):
                errors.append(
                    f"{row.path}: declared r300g production caller not found")
        elif row.disposition == "EXTRACT_COMMON":
            if not row.future_owner.startswith("src/amd/r300/common/"):
                errors.append(f"{row.path}: EXTRACT_COMMON owner is not common")
            if len(basis) != 2 or any(not symbol_exists(root, *item)
                                     for item in basis):
                errors.append(
                    f"{row.path}: two present common consumer bases not found")

        forbidden = forbidden_tokens(root, row)
        if forbidden:
            errors.append(
                f"{row.path}: forbidden retained-interface vocabulary: "
                + ", ".join(forbidden[:8]))

    if enforce_compute_orphan:
        candidate = Path(
            "src/gallium/drivers/r300/r300_compute_admission.c")
        candidate_row = file_rows.get(candidate)
        if candidate_row is None or candidate_row.disposition == "DELETE":
            symbols = public_compute_symbols(root)
            callers = []
            for source in sorted(r300g_sources - {candidate}, key=str):
                text = strip_comments_and_strings(
                    (root / source).read_text(encoding="utf-8"))
                found = sorted(symbol for symbol in symbols
                               if re.search(rf"\b{re.escape(symbol)}\s*\(", text))
                if found:
                    callers.append(f"{source}:{','.join(found)}")
            if callers:
                errors.append(
                    "r300_compute_admission gained r300g production callers "
                    "while classified DELETE: " + "; ".join(callers))
            fake_callers = []
            for source in sorted(required, key=str):
                rendered = str(source)
                if (source.suffix != ".c" or "/tests/" in f"/{rendered}"
                        or not rendered.startswith("src/amd/r300/vulkan/")):
                    continue
                text = strip_comments_and_strings(
                    (root / source).read_text(encoding="utf-8"))
                found = sorted(symbol for symbol in symbols
                               if re.search(rf"\b{re.escape(symbol)}\s*\(", text))
                if found:
                    fake_callers.append(f"{source}:{','.join(found)}")
            if not fake_callers:
                errors.append(
                    "r300_compute_admission has no remaining fake-lane caller; "
                    "the recorded ownership reason is stale")
    return errors


def expect_one(label: str, errors: list[str], needle: str) -> None:
    matches = [error for error in errors if needle in error]
    if len(errors) != 1 or len(matches) != 1:
        raise AssertionError(
            f"{label}: expected one {needle!r} refusal, got {errors!r}")
    print(f"  ok - {label}: {matches[0]}")


def selftest(root: Path, rows: list[Row], required: set[Path],
             r300g_sources: set[Path]) -> int:
    spec = json.loads((root / FIXTURE_SPEC).read_text(encoding="utf-8"))
    if spec.get("schema") != "r3v-gallium-ownership-known-bad/v1":
        raise ValueError("known-bad fixture schema mismatch")
    cases = {case["name"]: case for case in spec.get("cases", [])}
    expected = {
        "missing-row", "false-gallium-caller", "false-common-consumer",
        "forbidden-vulkan-vocabulary",
    }
    if set(cases) != expected:
        raise ValueError("known-bad case inventory mismatch")

    baseline = validate(root, rows, required, r300g_sources)
    if baseline:
        raise AssertionError(f"live baseline is not valid: {baseline!r}")

    missing_target = Path(cases["missing-row"]["path"])
    missing_rows = [row for row in rows
                    if not (row.kind == "FILE" and row.path == missing_target)]
    expect_one("missing-row",
               validate(root, missing_rows, required, r300g_sources),
               "missing disposition row")

    caller_target = Path(cases["false-gallium-caller"]["path"])
    caller_rows = [
        replace(row, disposition="MOVE_R300G",
                future_owner=str(caller_target),
                basis=cases["false-gallium-caller"]["basis"])
        if row.kind == "FILE" and row.path == caller_target else row
        for row in rows
    ]
    expect_one("false-gallium-caller",
               validate(root, caller_rows, required, r300g_sources,
                        enforce_compute_orphan=False),
               "declared r300g production caller not found")

    neutral = Path(cases["false-common-consumer"]["path"])
    neutral_row = Row(
        "FILE", neutral, "-", "EXTRACT_COMMON",
        "src/amd/r300/common/neutral.c",
        cases["false-common-consumer"]["basis"], "calibration")
    expect_one("false-common-consumer",
               validate(root, [neutral_row], {neutral}, r300g_sources,
                        enforce_compute_orphan=False),
               "two present common consumer bases not found")

    forbidden = Path(cases["forbidden-vulkan-vocabulary"]["path"])
    forbidden_row = Row(
        "FILE", forbidden, "-", "MOVE_R300G",
        "src/gallium/drivers/r300/r300_gallium_candidate.c",
        cases["forbidden-vulkan-vocabulary"]["basis"], "calibration")
    expect_one("forbidden-vulkan-vocabulary",
               validate(root, [forbidden_row], {forbidden}, r300g_sources,
                        enforce_compute_orphan=False),
               "forbidden retained-interface vocabulary")

    print("r3v_gallium_ownership_disposition: 4 known-bad cases refused")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    root = (args.root or Path(__file__).resolve().parents[5]).resolve()
    try:
        required, r300g_sources = derive_scope(root)
        rows = read_rows(root / LEDGER)
        if args.selftest:
            return selftest(root, rows, required, r300g_sources)
        errors = validate(root, rows, required, r300g_sources)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"r3v_gallium_ownership_disposition: {error}", file=sys.stderr)
        return 2

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    counts = {name: 0 for name in sorted(DISPOSITIONS)}
    for row in rows:
        if row.kind == "FILE":
            counts[row.disposition] += 1
    rendered = " ".join(f"{name}={counts[name]}" for name in sorted(counts))
    print("r3v_gallium_ownership_disposition: "
          f"{len(required)} files and {len(REQUIRED_FUNCTION_ROWS)} named "
          f"functions covered; {rendered}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
