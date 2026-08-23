# SPDX-License-Identifier: MIT
"""Verify the R300 carrier-contract liveness census.

The common consumer census answers who can compile against an API-neutral
contract.  This checker records a separate set of facts: Meson registration,
archive definitions, non-owner production references, route selection,
hardware execution, and evidence-only references.  A static archive can
define a symbol without a driver route selecting or executing it, so the
checker keeps those facts orthogonal.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from r300_common_boundary_audit import strip_comments, strip_literals


README_PATH = "src/amd/r300/common/README.md"
COMMON_MESON_PATH = "src/amd/r300/common/meson.build"
COMPILER_MESON_PATH = "src/amd/r300/compiler/meson.build"
GALLIUM_MESON_PATH = "src/gallium/drivers/r300/meson.build"
CPU_MESON_PATH = "src/amd/r300/cpu/meson.build"
VULKAN_MESON_PATH = "src/amd/r300/vulkan/meson.build"
POLICY_SOURCE_PATH = "src/amd/r300/common/r300_carrier_policy.c"
POLICY_HEADER_PATH = "src/amd/r300/common/r300_carrier_policy.h"
PACKER_SOURCE_PATH = "src/amd/r300/compiler/r300_nir.c"
PACKER_HEADER_PATH = "src/amd/r300/compiler/r300_nir.h"

TABLE_HEADER = (
    "Subject",
    "Build registration",
    "Archive definition",
    "Contract reach",
    "Production references",
    "Route selected",
    "Hardware executed",
    "Evidence-only references",
)
CODE_SPAN = re.compile(r"`([^`]+)`")
INCLUDE = re.compile(r'\s*#\s*include\s+["<]([^">]+)[">]')


@dataclass(frozen=True)
class LivenessRow:
    subjects: tuple[str, ...]
    build_registration: str
    archive_definition: str
    contract_reach: frozenset[str]
    production_references: frozenset[str]
    route_selections: frozenset[str]
    hardware_executions: frozenset[str]
    evidence_references: frozenset[str]


@dataclass(frozen=True)
class SubjectInfo:
    name: str
    provider: str
    declaration: str
    archive: str
    reach_header: str


def code_text(text: str) -> str:
    """Blank comments and literals before lexical symbol matching."""
    return "\n".join(
        strip_literals(line) for line in strip_comments(text).splitlines())


def _table_lines(text: str) -> list[str]:
    marker = "## Liveness census"
    start = text.find(marker)
    if start < 0:
        raise ValueError("missing Liveness census heading")
    table: list[str] = []
    entered = False
    for line in text[start + len(marker):].splitlines():
        if line.startswith("## "):
            break
        if line.startswith("|"):
            entered = True
            table.append(line)
        elif entered and line.strip():
            raise ValueError("non-table content interrupts liveness census")
    if len(table) < 3:
        raise ValueError("liveness census table is empty")
    return table


def _cell_tokens(cell: str, label: str) -> frozenset[str]:
    values = frozenset(CODE_SPAN.findall(cell))
    if not values:
        raise ValueError(f"{label} has no code-span values")
    if "none" in values:
        if len(values) != 1:
            raise ValueError(f"{label} mixes none with values")
        return frozenset()
    return values


def parse_liveness(text: str) -> list[LivenessRow]:
    lines = _table_lines(text)
    header = tuple(cell.strip() for cell in lines[0].strip("|").split("|"))
    if header != TABLE_HEADER:
        raise ValueError(f"unexpected liveness header: {header!r}")
    separator = tuple(
        cell.strip() for cell in lines[1].strip("|").split("|"))
    if len(separator) != len(TABLE_HEADER) or not all(
            re.fullmatch(r":?-{3,}:?", cell) for cell in separator):
        raise ValueError("invalid liveness separator")

    rows: list[LivenessRow] = []
    for number, line in enumerate(lines[2:], 1):
        cells = tuple(cell.strip() for cell in line.strip("|").split("|"))
        if len(cells) != len(TABLE_HEADER):
            raise ValueError(
                f"liveness row {number} has {len(cells)} columns")
        subjects = tuple(CODE_SPAN.findall(cells[0]))
        if not subjects:
            raise ValueError(f"liveness row {number} names no subject")
        build = _cell_tokens(cells[1], f"liveness row {number} build")
        archive = _cell_tokens(cells[2], f"liveness row {number} archive")
        if len(build) != 1 or len(archive) != 1:
            raise ValueError(f"liveness row {number} needs one build target")
        rows.append(LivenessRow(
            subjects,
            next(iter(build)),
            next(iter(archive)),
            _cell_tokens(cells[3], f"liveness row {number} contract reach"),
            _cell_tokens(cells[4], f"liveness row {number} production refs"),
            _cell_tokens(cells[5], f"liveness row {number} route selected"),
            _cell_tokens(cells[6], f"liveness row {number} hardware executed"),
            _cell_tokens(cells[7], f"liveness row {number} evidence refs"),
        ))
    return rows


def meson_files(repo_root: Path, relative_path: str, variable: str) -> set[Path]:
    path = repo_root / relative_path
    if not path.is_file():
        raise ValueError(f"missing Meson registry: {path}")
    text = path.read_text(encoding="utf-8")
    match = re.search(
        rf"\b{re.escape(variable)}\s*=\s*files\s*\((.*?)\n?\s*\)",
        text, re.DOTALL)
    if match is None:
        raise ValueError(f"missing files assignment {variable}: {path}")
    values = re.findall(r"['\"]([^'\"]+\.[ch])['\"]", match.group(1))
    if not values:
        raise ValueError(f"empty files assignment {variable}: {path}")
    return {(path.parent / value).resolve() for value in values}


def source_universe(repo_root: Path) -> set[Path]:
    roots = (
        repo_root / "src/amd/r300",
        repo_root / "src/gallium/drivers/r300",
    )
    return {
        path.resolve()
        for root in roots
        if root.is_dir()
        for path in root.rglob("*")
        if path.is_file() and path.suffix in {".c", ".h"}
    }


def include_index(universe: set[Path]) -> dict[str, set[Path]]:
    index: dict[str, set[Path]] = {}
    for path in universe:
        index.setdefault(path.name, set()).add(path)
    return index


def resolve_include(repo_root: Path, source: Path, include: str,
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


def reachable_paths(repo_root: Path, roots: set[Path], universe: set[Path],
                    by_name: dict[str, set[Path]]) -> set[Path]:
    reached: set[Path] = set()
    queue = list(roots)
    while queue:
        path = queue.pop()
        if path in reached or path not in universe:
            continue
        reached.add(path)
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            raise ValueError(f"unreadable source: {path}: {error}") from error
        for line in strip_comments(text).splitlines():
            include = INCLUDE.fullmatch(line)
            if include is None:
                continue
            target = resolve_include(repo_root, path, include.group(1),
                                     universe, by_name)
            if target is not None and target not in reached:
                queue.append(target)
    return reached


def contract_reach(repo_root: Path) -> dict[str, frozenset[str]]:
    universe = source_universe(repo_root)
    by_name = include_index(universe)
    domains = {
        "compiler": meson_files(repo_root, COMPILER_MESON_PATH,
                                 "files_r300_compiler"),
        "r300g": meson_files(repo_root, GALLIUM_MESON_PATH, "files_r300"),
        "cpu": meson_files(repo_root, CPU_MESON_PATH, "r300_cpu_files"),
        "native": (
            meson_files(repo_root, VULKAN_MESON_PATH,
                        "r3v_loader_base_files") |
            meson_files(repo_root, VULKAN_MESON_PATH, "r3v_native_files")),
    }
    headers = {
        POLICY_HEADER_PATH: set(),
        PACKER_HEADER_PATH: set(),
    }
    for domain, registered in domains.items():
        # Meson source sets also list public headers.  Reach starts at compiled
        # translation units so registration alone cannot manufacture a
        # compilation path through an otherwise unused header.
        roots = {path for path in registered if path.suffix == ".c"}
        if not roots:
            raise ValueError(f"production domain has no C roots: {domain}")
        reached = reachable_paths(repo_root, roots, universe, by_name)
        for header in headers:
            if (repo_root / header).resolve() in reached:
                headers[header].add(domain)
    return {header: frozenset(domains) for header, domains in headers.items()}


def policy_symbols(repo_root: Path) -> tuple[str, ...]:
    source = repo_root / POLICY_SOURCE_PATH
    header = repo_root / POLICY_HEADER_PATH
    if not source.is_file() or not header.is_file():
        raise ValueError("carrier-policy source or header is missing")
    source_text = code_text(source.read_text(encoding="utf-8"))
    header_text = code_text(header.read_text(encoding="utf-8"))
    table = re.search(
        r"\bpolicies\s*\[\]\s*=\s*\{(.*?)\};", source_text, re.DOTALL)
    if table is None:
        raise ValueError("carrier-policy registry is missing")
    members = tuple(re.findall(r"&\s*(r300_carrier_[A-Za-z0-9_]+)\b",
                               table.group(1)))
    if not members or len(members) != len(set(members)):
        raise ValueError("carrier-policy registry has no members or duplicates")
    declarations = tuple(re.findall(
        r"\bextern\s+const\s+struct\s+r300_carrier_policy\s+"
        r"(r300_carrier_[A-Za-z0-9_]+)\s*;", header_text))
    if set(members) != set(declarations):
        raise ValueError(
            "carrier-policy public declarations and registry members disagree: "
            f"members={sorted(members)!r}, declarations={sorted(declarations)!r}")
    return members


def subject_info(repo_root: Path) -> dict[str, SubjectInfo]:
    policies = policy_symbols(repo_root)
    info = {
        name: SubjectInfo(name, POLICY_SOURCE_PATH, POLICY_HEADER_PATH,
                          "libr300_common", POLICY_HEADER_PATH)
        for name in policies
    }
    for name in ("r300_carrier_policies", "r300_carrier_dp4_select"):
        info[name] = SubjectInfo(name, POLICY_SOURCE_PATH, POLICY_HEADER_PATH,
                                 "libr300_common", POLICY_HEADER_PATH)
    packer = "r300_nir_build_carrier_pack"
    info[packer] = SubjectInfo(packer, PACKER_SOURCE_PATH, PACKER_HEADER_PATH,
                               "libr300_compiler", PACKER_HEADER_PATH)

    for item in info.values():
        provider = code_text((repo_root / item.provider).read_text(
            encoding="utf-8"))
        declaration = code_text((repo_root / item.declaration).read_text(
            encoding="utf-8"))
        if not re.search(rf"\b{re.escape(item.name)}\b", provider):
            raise ValueError(f"provider does not define {item.name}: {item.provider}")
        if not re.search(rf"\b{re.escape(item.name)}\b", declaration):
            raise ValueError(
                f"declaration header does not expose {item.name}: "
                f"{item.declaration}")
    return info


def _evidence_path(relative: Path) -> bool:
    return "tests" in relative.parts or relative.name.endswith("_manifest.c")


def symbol_references(
        repo_root: Path,
        subjects: dict[str, SubjectInfo]) -> dict[str, dict[str, set[str]]]:
    """Classify non-owner references after stripping comments and literals."""
    result = {
        name: {"production": set(), "evidence": set()}
        for name in subjects
    }
    for path in source_universe(repo_root):
        relative = path.relative_to(repo_root)
        text = code_text(path.read_text(encoding="utf-8"))
        for name, item in subjects.items():
            # A provider owns its registry and implementation dependencies.
            # The liveness row records references from other production units.
            if relative.as_posix() in (item.provider, item.declaration):
                continue
            if not re.search(rf"\b{re.escape(name)}\b", text):
                continue
            category = "evidence" if _evidence_path(relative) else "production"
            result[name][category].add(relative.as_posix())
    return result


def source_findings(repo_root: Path) -> list[str]:
    repo_root = repo_root.resolve()
    try:
        rows = parse_liveness((repo_root / README_PATH).read_text(encoding="utf-8"))
        subjects = subject_info(repo_root)
        reaches = contract_reach(repo_root)
        references = symbol_references(repo_root, subjects)
        common_files = meson_files(repo_root, COMMON_MESON_PATH,
                                   "r300_common_contract_files")
        compiler_files = meson_files(repo_root, COMPILER_MESON_PATH,
                                     "files_r300_compiler")
    except (OSError, UnicodeError, ValueError) as error:
        return [f"liveness census input error: {error}"]

    findings: list[str] = []
    if (repo_root / POLICY_SOURCE_PATH).resolve() not in common_files:
        findings.append(
            "BUILD_REGISTERED: r300_carrier_policy.c is absent from "
            "r300_common_contract_files")
    if (repo_root / PACKER_SOURCE_PATH).resolve() not in compiler_files:
        findings.append(
            "BUILD_REGISTERED: r300_nir.c is absent from files_r300_compiler")

    row_for_subject: dict[str, LivenessRow] = {}
    for row_number, row in enumerate(rows, 1):
        for name in row.subjects:
            if name in row_for_subject:
                findings.append(
                    f"liveness row {row_number}: duplicate subject {name}")
            row_for_subject[name] = row
    unknown = sorted(set(row_for_subject) - set(subjects))
    missing = sorted(set(subjects) - set(row_for_subject))
    for name in unknown:
        findings.append(f"liveness census names unknown subject {name}")
    for name in missing:
        findings.append(f"liveness census omits subject {name}")

    for name, item in subjects.items():
        row = row_for_subject.get(name)
        if row is None:
            continue
        if row.build_registration != item.archive:
            findings.append(
                f"{name}: declared build {row.build_registration}, expected "
                f"{item.archive}")
        if row.archive_definition != item.archive:
            findings.append(
                f"{name}: declared archive {row.archive_definition}, expected "
                f"{item.archive}")
        measured_reach = reaches[item.reach_header]
        if row.contract_reach != measured_reach:
            findings.append(
                f"{name}: declared contract reach "
                f"{sorted(row.contract_reach)!r}, measured "
                f"{sorted(measured_reach)!r}")
        production = frozenset(references[name]["production"])
        evidence = frozenset(references[name]["evidence"])
        if row.production_references != production:
            findings.append(
                f"{name}: declared production references "
                f"{sorted(row.production_references)!r}, measured "
                f"{sorted(production)!r}")
        if row.evidence_references != evidence:
            findings.append(
                f"{name}: declared evidence-only references "
                f"{sorted(row.evidence_references)!r}, measured "
                f"{sorted(evidence)!r}")
        # Every current subject is dormant.  Positive route or silicon claims
        # stay closed until an adapter validates an exact selector callsite or
        # typed external evidence record; a registered test name alone proves
        # neither fact.
        if row.route_selections:
            findings.append(
                f"{name}: ROUTE_SELECTED claim lacks an exact selector "
                f"adapter: {sorted(row.route_selections)!r}")
        if row.hardware_executions:
            findings.append(
                f"{name}: HARDWARE_EXECUTED claim lacks a typed evidence "
                f"adapter: {sorted(row.hardware_executions)!r}")
    return findings


def defined_symbols(nm: str, archive: Path,
                    run_command=subprocess.run) -> set[str]:
    if not archive.is_file():
        raise ValueError(f"archive is missing: {archive}")
    try:
        result = run_command(
            [nm, "-g", "--defined-only", str(archive)],
            check=False, capture_output=True, text=True)
    except (OSError, subprocess.SubprocessError, UnicodeError) as error:
        raise ValueError(f"nm failed for {archive}: {error}") from error
    if result.returncode != 0:
        diagnostic = result.stderr.strip()
        detail = f"nm failed for {archive}: status {result.returncode}"
        raise ValueError(f"{detail}: {diagnostic}" if diagnostic else detail)
    symbols: set[str] = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if not fields or line.rstrip().endswith(":"):
            continue
        symbols.add(fields[-1].lstrip("_"))
    return symbols


def archive_findings(repo_root: Path, nm: str, common_archive: Path,
                     compiler_archive: Path,
                     run_command=subprocess.run) -> list[str]:
    repo_root = repo_root.resolve()
    findings = source_findings(repo_root)
    if findings:
        return findings
    try:
        subjects = subject_info(repo_root)
        archives = {
            "libr300_common": defined_symbols(nm, common_archive, run_command),
            "libr300_compiler": defined_symbols(
                nm, compiler_archive, run_command),
        }
    except ValueError as error:
        return [f"ARCHIVE_DEFINED: {error}"]
    for name, item in subjects.items():
        if name not in archives[item.archive]:
            findings.append(
                f"ARCHIVE_DEFINED: {name} is absent from {item.archive}")
    return findings


def print_findings(findings: list[str]) -> int:
    if findings:
        print("\n".join(findings))
        return 1
    return 0


def _write_fixture(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _fixture_readme() -> str:
    policy_rows = []
    policy_test = "`src/amd/r300/common/tests/r300_carrier_policy_test.c`"
    ledger_test = "`src/amd/r300/common/tests/r300_operation_ledger_test.c`"
    evidence = {
        "r300_carrier_identity": policy_test,
        "r300_carrier_dp4_u7": f"{policy_test}, {ledger_test}",
        "r300_carrier_dp4_u8_boundary": policy_test,
        "r300_carrier_blend_acc": policy_test,
        "r300_carrier_zpass": policy_test,
        "r300_carrier_ieee16_classify": policy_test,
        "r300_carrier_ieee16_mul": policy_test,
        "r300_carrier_ieee16_result": f"{policy_test}, {ledger_test}",
        "r300_carrier_ieee16_debug": policy_test,
        "r300_carrier_policies": f"{policy_test}, {ledger_test}",
        "r300_carrier_dp4_select": policy_test,
    }
    for name, references in evidence.items():
        policy_rows.append(
            f"| `{name}` | `libr300_common` | `libr300_common` | "
            f"`compiler`, `r300g` | "
            f"`none` | `none` | `none` | {references} |")
    policy_rows.append(
        "| `r300_nir_build_carrier_pack` | `libr300_compiler` | "
        "`libr300_compiler` | `compiler`, `r300g` | `none` | `none` | "
        "`none` | `none` |")
    return (
        "# Fixture\n\n## Liveness census\n\n"
        "| Subject | Build registration | Archive definition | Contract reach | "
        "Production references | Route selected | Hardware executed | "
        "Evidence-only references |\n"
        "| --- | --- | --- | --- | --- | --- | --- | --- |\n" +
        "\n".join(policy_rows) + "\n\n## End\n")


def _write_fixture_tree(root: Path) -> None:
    policies = (
        "r300_carrier_identity",
        "r300_carrier_dp4_u7",
        "r300_carrier_dp4_u8_boundary",
        "r300_carrier_blend_acc",
        "r300_carrier_zpass",
        "r300_carrier_ieee16_classify",
        "r300_carrier_ieee16_mul",
        "r300_carrier_ieee16_result",
        "r300_carrier_ieee16_debug",
    )
    _write_fixture(root, README_PATH, _fixture_readme())
    _write_fixture(root, COMMON_MESON_PATH,
                   "r300_common_contract_files = files(\n"
                   "  'r300_carrier_policy.c',\n)\n"
                   "test('r300-carrier-policy', 'policy')\n")
    _write_fixture(root, COMPILER_MESON_PATH,
                   "files_r300_compiler = files(\n  'r300_nir.c',\n)\n")
    _write_fixture(root, GALLIUM_MESON_PATH,
                   "files_r300 = files(\n  'r300_driver.c',\n)\n")
    _write_fixture(root, CPU_MESON_PATH,
                   "r300_cpu_files = files(\n  'r300_cpu.c',\n)\n")
    _write_fixture(root, VULKAN_MESON_PATH,
                   "r3v_loader_base_files = files(\n  'r3v_loader.c',\n)\n"
                   "r3v_native_files = files(\n  'r3v_native.c',\n)\n")
    declarations = "\n".join(
        f"extern const struct r300_carrier_policy {name};" for name in policies)
    definitions = "\n".join(
        f"const struct r300_carrier_policy {name} = {{0}};" for name in policies)
    members = "\n".join(f"  &{name}," for name in policies)
    _write_fixture(root, POLICY_HEADER_PATH,
                   "struct r300_carrier_policy { int value; };\n" +
                   declarations + "\n"
                   "const struct r300_carrier_policy *const *\n"
                   "r300_carrier_policies(unsigned *count);\n"
                   "const struct r300_carrier_policy *\n"
                   "r300_carrier_dp4_select(unsigned value);\n")
    _write_fixture(root, POLICY_SOURCE_PATH,
                   '#include "r300_carrier_policy.h"\n' + definitions + "\n"
                   "static const struct r300_carrier_policy *const policies[] = {\n" +
                   members + "\n};\n"
                   "const struct r300_carrier_policy *const *\n"
                   "r300_carrier_policies(unsigned *count) { return policies; }\n"
                   "const struct r300_carrier_policy *\n"
                   "r300_carrier_dp4_select(unsigned value) {\n"
                   "  return policies[value];\n}\n")
    _write_fixture(root, PACKER_HEADER_PATH,
                   '#include "amd/r300/common/r300_carrier_policy.h"\n'
                   "struct nir_builder; struct nir_def;\n"
                   "struct nir_def *r300_nir_build_carrier_pack(\n"
                   "  struct nir_builder *builder,\n"
                   "  const struct r300_carrier_policy *policy,\n"
                   "  struct nir_def *value);\n")
    _write_fixture(root, PACKER_SOURCE_PATH,
                   '#include "r300_nir.h"\n'
                   "struct nir_def *r300_nir_build_carrier_pack(\n"
                   "  struct nir_builder *builder,\n"
                   "  const struct r300_carrier_policy *policy,\n"
                   "  struct nir_def *value) { return value; }\n")
    _write_fixture(root, "src/gallium/drivers/r300/r300_driver.c",
                   '#include "amd/r300/compiler/r300_nir.h"\n')
    _write_fixture(root, "src/amd/r300/cpu/r300_cpu.c", "int cpu_root;\n")
    _write_fixture(root, "src/amd/r300/vulkan/r3v_loader.c",
                   "int loader_root;\n")
    _write_fixture(root, "src/amd/r300/vulkan/r3v_native.c",
                   "int native_root;\n")
    _write_fixture(root, "src/amd/r300/common/tests/r300_carrier_policy_test.c",
                   '#include "../r300_carrier_policy.h"\n'
                   "void test(void) {\n" +
                   "  (void)r300_carrier_identity;\n"
                   "  (void)r300_carrier_dp4_u7;\n"
                   "  (void)r300_carrier_dp4_u8_boundary;\n"
                   "  (void)r300_carrier_blend_acc;\n"
                   "  (void)r300_carrier_zpass;\n"
                   "  (void)r300_carrier_ieee16_classify;\n"
                   "  (void)r300_carrier_ieee16_mul;\n"
                   "  (void)r300_carrier_ieee16_result;\n"
                   "  (void)r300_carrier_ieee16_debug;\n"
                   "  (void)r300_carrier_policies(0);\n"
                   "  (void)r300_carrier_dp4_select(0);\n}\n")
    _write_fixture(root, "src/amd/r300/common/tests/r300_operation_ledger_test.c",
                   '#include "../r300_carrier_policy.h"\n'
                   "void ledger(void) {\n"
                   "  (void)r300_carrier_dp4_u7;\n"
                   "  (void)r300_carrier_ieee16_result;\n"
                   "  (void)r300_carrier_policies(0);\n}\n")


def _fake_nm_result(arguments, **_kwargs):
    archive = Path(arguments[-1]).name
    common = {
        "r300_carrier_identity",
        "r300_carrier_dp4_u7",
        "r300_carrier_dp4_u8_boundary",
        "r300_carrier_blend_acc",
        "r300_carrier_zpass",
        "r300_carrier_ieee16_classify",
        "r300_carrier_ieee16_mul",
        "r300_carrier_ieee16_result",
        "r300_carrier_ieee16_debug",
        "r300_carrier_policies",
        "r300_carrier_dp4_select",
    }
    symbols = common if archive == "common.a" else {
        "r300_nir_build_carrier_pack"}
    output = "\n".join(f"00000000 T {symbol}" for symbol in sorted(symbols))
    return subprocess.CompletedProcess(arguments, 0, output, "")


def selftest() -> int:
    with tempfile.TemporaryDirectory(prefix="r300-common-liveness-") as tmp:
        root = Path(tmp)
        _write_fixture_tree(root)
        good = source_findings(root)
        if good:
            print(f"selftest good fixture failed: {good!r}")
            return 1

        comment = root / "src/gallium/drivers/r300/comment.c"
        comment.write_text(
            "/* r300_carrier_identity */\n"
            "const char *label = \"r300_carrier_identity\";\n",
            encoding="utf-8")
        if source_findings(root):
            print("selftest comment or literal became a symbol reference")
            return 1

        production = root / "src/gallium/drivers/r300/reference.c"
        production.write_text(
            '#include "amd/r300/common/r300_carrier_policy.h"\n'
            "const void *reference = &r300_carrier_identity;\n",
            encoding="utf-8")
        if not any("r300_carrier_identity: declared production references" in
                   finding for finding in source_findings(root)):
            print("selftest production reference did not fail")
            return 1
        production.unlink()

        common_meson = root / COMMON_MESON_PATH
        common_meson.write_text(
            "r300_common_contract_files = files('other.c')\n",
            encoding="utf-8")
        if not any("BUILD_REGISTERED" in finding for finding in source_findings(root)):
            print("selftest missing common build registration did not fail")
            return 1
        _write_fixture(root, COMMON_MESON_PATH,
                       "r300_common_contract_files = files(\n"
                       "  'r300_carrier_policy.c',\n)\n"
                       "test('r300-carrier-policy', 'policy')\n")

        native = root / "src/amd/r300/vulkan/r3v_native.c"
        native.write_text(
            '#include "amd/r300/common/r300_carrier_policy.h"\n',
            encoding="utf-8")
        if not any("declared contract reach" in finding and
                   "native" in finding for finding in source_findings(root)):
            print("selftest native contract-reach drift did not fail")
            return 1
        native.write_text("int native_root;\n", encoding="utf-8")

        readme = root / README_PATH
        readme.write_text(readme.read_text(encoding="utf-8").replace(
            "`none` | `none` | `none` | "
            "`src/amd/r300/common/tests/r300_carrier_policy_test.c` |",
            "`none` | `unbound-selector` | `none` | "
            "`src/amd/r300/common/tests/r300_carrier_policy_test.c` |", 1),
            encoding="utf-8")
        if not any("ROUTE_SELECTED claim lacks an exact selector adapter" in
                   finding for finding in source_findings(root)):
            print("selftest unbound route-selection claim did not fail")
            return 1
        _write_fixture(root, README_PATH, _fixture_readme())

        readme.write_text(readme.read_text(encoding="utf-8").replace(
            "`none` | `none` | `none` | "
            "`src/amd/r300/common/tests/r300_carrier_policy_test.c` |",
            "`none` | `none` | `unbound-silicon-cell` | "
            "`src/amd/r300/common/tests/r300_carrier_policy_test.c` |", 1),
            encoding="utf-8")
        if not any("HARDWARE_EXECUTED claim lacks a typed evidence adapter" in
                   finding for finding in source_findings(root)):
            print("selftest unbound hardware-execution claim did not fail")
            return 1
        _write_fixture(root, README_PATH, _fixture_readme())

        duplicate = (
            "| `r300_carrier_identity` | `libr300_common` | "
            "`libr300_common` | `compiler`, `r300g` | `none` | `none` | "
            "`none` | "
            "`src/amd/r300/common/tests/r300_carrier_policy_test.c` |\n")
        readme.write_text(readme.read_text(encoding="utf-8").replace(
            "\n## End\n", "\n" + duplicate + "\n## End\n"),
            encoding="utf-8")
        if not any("duplicate subject r300_carrier_identity" in finding
                   for finding in source_findings(root)):
            print("selftest duplicate liveness subject did not fail")
            return 1
        _write_fixture(root, README_PATH, _fixture_readme())

        common_archive = root / "common.a"
        compiler_archive = root / "compiler.a"
        common_archive.touch()
        compiler_archive.touch()
        if archive_findings(root, "nm", common_archive, compiler_archive,
                            _fake_nm_result):
            print("selftest complete archive fixture failed")
            return 1

        def missing_packer(arguments, **_kwargs):
            result = _fake_nm_result(arguments, **_kwargs)
            if Path(arguments[-1]).name == "compiler.a":
                result.stdout = ""
            return result

        if not any("ARCHIVE_DEFINED: r300_nir_build_carrier_pack" in finding
                   for finding in archive_findings(
                       root, "nm", common_archive, compiler_archive,
                       missing_packer)):
            print("selftest missing archive definition did not fail")
            return 1

    print("r300_common_liveness_census: source, archive, route, and hardware "
          "calibrations passed")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo_root", nargs="?", type=Path)
    parser.add_argument("--archives", action="store_true",
                        help="also verify compiled archive definitions")
    parser.add_argument("--nm", help="nm executable for --archives")
    parser.add_argument("--common-archive", type=Path)
    parser.add_argument("--compiler-archive", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        if args.repo_root is not None or args.archives or args.nm or \
                args.common_archive or args.compiler_archive:
            parser.error("--selftest takes no other arguments")
        return selftest()
    if args.repo_root is None:
        parser.error("repo_root is required")
    if args.archives:
        if not (args.nm and args.common_archive and args.compiler_archive):
            parser.error("--archives requires --nm, --common-archive, and "
                         "--compiler-archive")
        findings = archive_findings(args.repo_root, args.nm,
                                    args.common_archive, args.compiler_archive)
        if print_findings(findings) != 0:
            return 1
        print("r300_common_liveness_census: build registration, contract "
              "reach, references, dormant route/hardware cells, and archive "
              "definitions checked")
        return 0
    if any((args.nm, args.common_archive, args.compiler_archive)):
        parser.error("archive options require --archives")
    findings = source_findings(args.repo_root)
    if print_findings(findings) != 0:
        return 1
    print("r300_common_liveness_census: build registration, contract reach, "
          "reference provenance, and dormant route/hardware cells checked")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
