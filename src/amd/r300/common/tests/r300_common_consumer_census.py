# SPDX-License-Identifier: MIT
"""Verify the R300 common component census and its ownership decisions."""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from r300_common_boundary_audit import source_lines

DECISIONS = {
    "KEEP_SHARED",
    "KEEP_SILICON_CONTRACT",
    "MOVE_TO_R3V_FRONTEND",
    "MOVE_TO_R3V_POLICY",
}
CONSUMERS = {
    "r300g",
    "compiler",
    "native",
    "legacy-r3v",
    "cpu",
    "none",
}
TABLE_HEADER = (
    "Files",
    "API-neutral contract",
    "Production consumers",
    "Standalone proof",
    "Owner decision",
)
CODE_SPAN = re.compile(r"`([^`]+)`")
TEST_REGISTRATION = re.compile(r"\btest\s*\(\s*['\"]([^'\"]+)['\"]")
INCLUDE = re.compile(r"\s*#\s*include\s+[\"<]([^\">]+)[\">]")


@dataclass(frozen=True)
class CensusRow:
    files: tuple[str, ...]
    consumers: frozenset[str]
    proofs: tuple[str, ...]
    decision: str


def _table_lines(text: str) -> list[str]:
    marker = "## Consumer census"
    start = text.find(marker)
    if start < 0:
        raise ValueError("missing Consumer census heading")
    lines = text[start + len(marker):].splitlines()
    table: list[str] = []
    entered = False
    for line in lines:
        if line.startswith("## "):
            break
        if line.startswith("|"):
            entered = True
            table.append(line)
        elif entered and line.strip():
            raise ValueError("non-table content interrupts consumer census")
    if len(table) < 3:
        raise ValueError("consumer census table is empty")
    return table


def parse_census(text: str) -> list[CensusRow]:
    lines = _table_lines(text)
    header = tuple(cell.strip() for cell in lines[0].strip("|").split("|"))
    if header != TABLE_HEADER:
        raise ValueError(f"unexpected census header: {header!r}")
    separator = tuple(
        cell.strip() for cell in lines[1].strip("|").split("|"))
    if len(separator) != len(TABLE_HEADER) or not all(
            re.fullmatch(r":?-{3,}:?", cell) for cell in separator):
        raise ValueError("invalid census separator")

    rows: list[CensusRow] = []
    for line_number, line in enumerate(lines[2:], 1):
        cells = tuple(cell.strip() for cell in line.strip("|").split("|"))
        if len(cells) != len(TABLE_HEADER):
            raise ValueError(
                f"census row {line_number} has {len(cells)} columns")
        files = tuple(CODE_SPAN.findall(cells[0]))
        consumers = frozenset(CODE_SPAN.findall(cells[2]))
        proofs = tuple(CODE_SPAN.findall(cells[3]))
        decisions = tuple(CODE_SPAN.findall(cells[4]))
        if not files:
            raise ValueError(f"census row {line_number} names no files")
        if not consumers or not consumers <= CONSUMERS:
            raise ValueError(
                f"census row {line_number} has invalid consumers: "
                f"{sorted(consumers)!r}")
        if "none" in consumers and len(consumers) != 1:
            raise ValueError(
                f"census row {line_number} mixes none with consumers")
        if len(decisions) != 1 or decisions[0] not in DECISIONS:
            raise ValueError(
                f"census row {line_number} has invalid decision: "
                f"{decisions!r}")
        rows.append(CensusRow(files, consumers, proofs, decisions[0]))
    return rows


def registered_tests(repo_root: Path) -> set[str]:
    meson_paths = (
        repo_root / "src/amd/r300/common/meson.build",
        repo_root / "src/amd/r300/cpu/meson.build",
        repo_root / "src/amd/r300/vulkan/meson.build",
        repo_root / "src/gallium/drivers/r300/meson.build",
    )
    tests: set[str] = set()
    for path in meson_paths:
        if not path.is_file():
            raise ValueError(f"missing Meson registry: {path}")
        tests.update(TEST_REGISTRATION.findall(path.read_text(encoding="utf-8")))
    return tests


def _meson_files(repo_root: Path, relative_path: str,
                 variable: str, operator: str = "=") -> set[Path]:
    path = repo_root / relative_path
    if not path.is_file():
        raise ValueError(f"missing production source registry: {path}")
    text = path.read_text(encoding="utf-8")
    match = re.search(
        rf"\b{re.escape(variable)}\s*{re.escape(operator)}\s*"
        rf"files\s*\((.*?)\n?\s*\)",
        text, re.DOTALL)
    if match is None:
        raise ValueError(
            f"missing files assignment {variable} {operator}: {path}")
    values = re.findall(r"['\"]([^'\"]+\.[ch])['\"]", match.group(1))
    if not values:
        raise ValueError(f"empty files assignment {variable}: {path}")
    return {(path.parent / value).resolve() for value in values}


def _source_universe(repo_root: Path) -> set[Path]:
    roots = (
        repo_root / "src/amd/r300",
        repo_root / "src/gallium/drivers/r300",
    )
    return {
        path.resolve()
        for root in roots
        for path in root.rglob("*")
        if path.is_file() and path.suffix in {".c", ".h"}
    }


def _resolve_include(repo_root: Path, source: Path, include: str,
                     universe: set[Path], by_name: dict[str, set[Path]]) -> Path | None:
    candidates = (
        (source.parent / include).resolve(),
        (repo_root / "src" / include).resolve(),
        (repo_root / include).resolve(),
    )
    for candidate in candidates:
        if candidate in universe:
            return candidate
    matches = by_name.get(Path(include).name, set())
    return next(iter(matches)) if len(matches) == 1 else None


def _domain_source_lines(domain: str, text: str):
    if domain == "native":
        return source_lines("native", text)
    if domain == "legacy-r3v":
        # Reuse the boundary audit's calibrated backend-expression walker.
        # Swapping the two known macro names makes its native truth table
        # describe the legacy Gallium-backed Vulkan compilation instead.
        placeholder = "R300_CONSUMER_CENSUS_NATIVE_PLACEHOLDER"
        if placeholder in text:
            raise ValueError("legacy backend predicate placeholder collision")
        legacy_text = text.replace("R3V_NATIVE_BACKEND", placeholder)
        legacy_text = legacy_text.replace(
            "R3V_GALLIUM_BACKEND", "R3V_NATIVE_BACKEND")
        legacy_text = legacy_text.replace(
            placeholder, "R3V_GALLIUM_BACKEND")
        return source_lines("native", legacy_text)
    return source_lines("common", text)


def _inside(path: Path, roots: tuple[Path, ...]) -> bool:
    for root in roots:
        try:
            path.relative_to(root)
        except ValueError:
            continue
        return True
    return False


def production_consumers(repo_root: Path) -> dict[str, frozenset[str]]:
    loader_base = _meson_files(
        repo_root, "src/amd/r300/vulkan/meson.build", "libr3v_files")
    legacy_r3v = _meson_files(
        repo_root, "src/amd/r300/vulkan/meson.build", "libr3v_files", "+=")
    domain_roots = {
        "r300g": _meson_files(
            repo_root, "src/gallium/drivers/r300/meson.build", "files_r300"),
        "compiler": _meson_files(
            repo_root, "src/amd/r300/compiler/meson.build",
            "files_r300_compiler"),
        "cpu": _meson_files(
            repo_root, "src/amd/r300/cpu/meson.build", "r300_cpu_files"),
        "native": (
            loader_base |
            _meson_files(repo_root, "src/amd/r300/vulkan/meson.build",
                         "r3v_native_files")),
        "legacy-r3v": loader_base | legacy_r3v,
    }
    common = (repo_root / "src/amd/r300/common").resolve()
    domain_boundaries = {
        "r300g": (
            (repo_root / "src/gallium/drivers/r300").resolve(), common),
        "compiler": (
            (repo_root / "src/amd/r300/compiler").resolve(), common),
        "cpu": ((repo_root / "src/amd/r300/cpu").resolve(), common),
        "native": (
            (repo_root / "src/amd/r300/vulkan").resolve(), common),
        "legacy-r3v": (
            (repo_root / "src/amd/r300/vulkan").resolve(), common),
    }
    universe = _source_universe(repo_root)
    by_name: dict[str, set[Path]] = {}
    for path in universe:
        by_name.setdefault(path.name, set()).add(path)

    consumers: dict[str, set[str]] = {
        path.name: set()
        for path in common.iterdir()
        if path.is_file() and path.suffix in {".c", ".h"}
    }
    for domain, roots in domain_roots.items():
        queue = list(roots)
        visited: set[Path] = set()
        while queue:
            path = queue.pop()
            if path in visited or path not in universe:
                continue
            visited.add(path)
            if path.parent == common:
                consumers[path.name].add(domain)
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as error:
                raise ValueError(f"unreadable production source: {path}: {error}")
            for _, line in _domain_source_lines(domain, text):
                include = INCLUDE.fullmatch(line)
                if include is None:
                    continue
                target = _resolve_include(
                    repo_root, path, include.group(1), universe, by_name)
                if (target is not None and target not in visited and
                        _inside(target, domain_boundaries[domain])):
                    queue.append(target)
    return {name: frozenset(domains) for name, domains in consumers.items()}


def validate_rows(rows: list[CensusRow], source_files: set[str],
                  test_names: set[str],
                  actual_consumers: dict[str, frozenset[str]]) -> list[str]:
    findings: list[str] = []
    seen: dict[str, int] = {}
    for row_number, row in enumerate(rows, 1):
        for name in row.files:
            seen[name] = seen.get(name, 0) + 1
            if name not in source_files:
                findings.append(
                    f"row {row_number}: unknown common source {name}")
        missing_proofs = sorted(set(row.proofs) - test_names)
        for proof in missing_proofs:
            findings.append(
                f"row {row_number}: proof is not a registered test: {proof}")

        measured_consumers = frozenset().union(
            *(actual_consumers.get(name, frozenset()) for name in row.files))
        declared_consumers = row.consumers - {"none"}
        if measured_consumers != declared_consumers:
            findings.append(
                f"row {row_number}: declared consumers "
                f"{sorted(declared_consumers)!r}, measured "
                f"{sorted(measured_consumers)!r}")

        concrete_consumers = row.consumers - {"none"}
        if row.decision == "KEEP_SHARED" and len(concrete_consumers) < 2:
            findings.append(
                f"row {row_number}: KEEP_SHARED needs two consumer domains")
        if row.decision == "KEEP_SILICON_CONTRACT" and not row.proofs:
            findings.append(
                f"row {row_number}: KEEP_SILICON_CONTRACT needs a proof")
        if row.decision.startswith("MOVE_TO_R3V_") and row.consumers != {
                "native"}:
            findings.append(
                f"row {row_number}: {row.decision} requires native alone")

    for name in sorted(source_files - set(seen)):
        findings.append(f"uncensused common source: {name}")
    for name in sorted(name for name, count in seen.items() if count > 1):
        findings.append(f"common source appears {seen[name]} times: {name}")
    return findings


def check(repo_root: Path) -> int:
    common = repo_root / "src/amd/r300/common"
    readme = common / "README.md"
    if not readme.is_file():
        print(f"consumer census missing: {readme}", file=sys.stderr)
        return 2
    source_files = {
        path.name for path in common.iterdir()
        if path.is_file() and path.suffix in {".c", ".h"}
    }
    if not source_files:
        print(f"common source census is empty: {common}", file=sys.stderr)
        return 2
    try:
        rows = parse_census(readme.read_text(encoding="utf-8"))
        tests = registered_tests(repo_root)
        consumers = production_consumers(repo_root)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"consumer census input error: {error}", file=sys.stderr)
        return 2
    findings = validate_rows(rows, source_files, tests, consumers)
    if findings:
        print("\n".join(findings))
        return 1
    print(
        "r300_common_consumer_census: "
        f"{len(source_files)} sources in {len(rows)} decisions; "
        f"{len(tests)} registered tests checked")
    return 0


def _write_fixture(root: Path, relative_path: str, text: str) -> None:
    path = root / relative_path
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _selftest_production_consumers() -> bool:
    with tempfile.TemporaryDirectory(
            prefix="r300-common-consumer-census-") as temporary:
        root = Path(temporary)
        _write_fixture(
            root, "src/gallium/drivers/r300/meson.build",
            "files_r300 = files('r300g.c')\n")
        _write_fixture(
            root, "src/gallium/drivers/r300/r300g.c",
            '#include "../../../amd/r300/common/shared.h"\n')
        _write_fixture(
            root, "src/amd/r300/compiler/meson.build",
            "files_r300_compiler = files('compiler.c')\n")
        _write_fixture(
            root, "src/amd/r300/compiler/compiler.c",
            '#include "foreign.h"\n#include "../common/shared.h"\n')
        _write_fixture(
            root, "src/amd/r300/compiler/foreign.h",
            '#include "../common/foreign_owned.h"\n')
        _write_fixture(
            root, "src/amd/r300/cpu/meson.build",
            "r300_cpu_files = files('cpu.c')\n")
        _write_fixture(
            root, "src/amd/r300/cpu/cpu.c",
            '#include "../common/cpu_only.h"\n')
        _write_fixture(
            root, "src/amd/r300/vulkan/meson.build",
            "libr3v_files = files('legacy.c')\n"
            "libr3v_files += files('legacy_extra.c')\n"
            "r3v_native_files = files('native.c')\n")
        _write_fixture(
            root, "src/amd/r300/vulkan/legacy.c",
            '#ifdef R3V_NATIVE_BACKEND\n'
            '#include "../common/native_arm.h"\n'
            '#else\n'
            '#include "../common/legacy_arm.h"\n'
            '#endif\n')
        _write_fixture(
            root, "src/amd/r300/vulkan/legacy_extra.c",
            '#include "../compiler/foreign.h"\n'
            '#include "../common/legacy_only.h"\n')
        _write_fixture(
            root, "src/amd/r300/vulkan/native.c",
            '#include "../common/shared.h"\n')
        for name in (
                "shared.h", "cpu_only.h", "foreign_owned.h",
                "native_arm.h", "legacy_arm.h", "legacy_only.h",
                "orphan.h"):
            _write_fixture(root, f"src/amd/r300/common/{name}", "")

        measured = production_consumers(root)
        expected = {
            "shared.h": frozenset(("r300g", "compiler", "native")),
            "cpu_only.h": frozenset(("cpu",)),
            "foreign_owned.h": frozenset(("compiler",)),
            "native_arm.h": frozenset(("native",)),
            "legacy_arm.h": frozenset(("legacy-r3v",)),
            "legacy_only.h": frozenset(("legacy-r3v",)),
            "orphan.h": frozenset(),
        }
        if measured != expected:
            print(
                "selftest production consumer discovery changed: "
                f"expected {expected!r}, got {measured!r}")
            return False
    return True


def selftest() -> int:
    good = CensusRow(
        ("shared.c",), frozenset(("r300g", "native")), (),
        "KEEP_SHARED")
    contract = CensusRow(
        ("contract.h",), frozenset(("none",)), ("contract-test",),
        "KEEP_SILICON_CONTRACT")
    sources = {"shared.c", "contract.h"}
    tests = {"contract-test"}
    consumers = {
        "shared.c": frozenset(("r300g", "native")),
        "contract.h": frozenset(),
    }
    cases = (
        ("good", [good, contract], sources, tests, consumers, 0),
        ("missing", [good], sources, tests, consumers, 1),
        ("duplicate", [good, good, contract], sources, tests, consumers, 1),
        ("unknown", [good, contract, CensusRow(
            ("ghost.c",), frozenset(("native",)), ("contract-test",),
            "KEEP_SILICON_CONTRACT")], sources, tests, consumers, 1),
        ("shared-one-consumer", [CensusRow(
            ("shared.c",), frozenset(("r300g",)), (), "KEEP_SHARED"),
            contract], sources, tests, consumers, 1),
        ("contract-without-proof", [good, CensusRow(
            ("contract.h",), frozenset(("none",)), (),
            "KEEP_SILICON_CONTRACT")], sources, tests, consumers, 1),
        ("unregistered-proof", [good, contract], sources, set(), consumers, 1),
        ("move-with-two-consumers", [CensusRow(
            ("shared.c",), frozenset(("native", "cpu")), (),
            "MOVE_TO_R3V_FRONTEND"), contract], sources, tests, consumers, 1),
        ("consumer-drift", [good, contract], sources, tests, {
            "shared.c": frozenset(("r300g",)),
            "contract.h": frozenset(),
        }, 1),
    )
    for name, rows, case_sources, case_tests, case_consumers, expected in cases:
        actual = 1 if validate_rows(
            rows, case_sources, case_tests, case_consumers) else 0
        if actual != expected:
            print(f"selftest {name}: expected {expected}, got {actual}")
            return 1

    sample = """# sample

## Consumer census

| Files | API-neutral contract | Production consumers | Standalone proof | Owner decision |
|---|---|---|---|---|
| `shared.c` | words | `r300g`, `native` | none | `KEEP_SHARED` |
| `contract.h` | words | `none` | `contract-test` | `KEEP_SILICON_CONTRACT` |

## End
"""
    try:
        parsed = parse_census(sample)
    except ValueError as error:
        print(f"selftest parser rejected known-good census: {error}")
        return 1
    if parsed != [good, contract]:
        print("selftest parser changed known-good census rows")
        return 1
    try:
        parse_census(sample.replace("|---|---|---|---|---|", "|--|---|---|---|---|"))
    except ValueError:
        pass
    else:
        print("selftest malformed separator did not refuse")
        return 1

    if not _selftest_production_consumers():
        return 1

    try:
        tuple(_domain_source_lines(
            "legacy-r3v", "R300_CONSUMER_CENSUS_NATIVE_PLACEHOLDER"))
    except ValueError:
        pass
    else:
        print("selftest legacy predicate placeholder collision did not refuse")
        return 1

    print(f"r300_common_consumer_census: {len(cases) + 4} checks passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("repo_root", nargs="?", type=Path)
    args = parser.parse_args()
    if args.selftest:
        if args.repo_root is not None:
            parser.error("--selftest takes no repository root")
        return selftest()
    if args.repo_root is None:
        args.repo_root = Path(__file__).resolve().parents[5]
    return check(args.repo_root.resolve())


if __name__ == "__main__":
    raise SystemExit(main())
