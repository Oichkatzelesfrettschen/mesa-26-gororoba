#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reject retired build names outside explicit compatibility boundaries."""

from __future__ import annotations

import argparse
import io
import re
import subprocess
import sys
import tokenize
from pathlib import Path

RETAINED_EVIDENCE_PREFIXES = (
    "build-infra/docs/review-thread-frontiers/",
    "build-infra/docs/review-thread-classifications/",
    "build-infra/docs/review-thread-corpus/",
    "build-infra/docs/review-thread-corpus-analysis/",
)
RETAINED_EVIDENCE_FILES = {
    "build-infra/docs/last-100-pr-review-comment-audit.md",
}
POLICY_IMPLEMENTATION_FILE = "build-infra/scripts/naming_policy.py"
IDENTIFIER_TOKEN = re.compile(
    r"(?<![A-Za-z0-9_-])([-A-Za-z0-9_]*[A-Za-z0-9_][-A-Za-z0-9_]*)(?![A-Za-z0-9_-])"
)
RETIRED_INTERNAL_ARTIFACT = re.compile(
    r"(?:\.gororoba-(?:source-identity\.json|"
    r"external-source-identity\.json|source-view)|"
    r"gororoba-toolchain\.meson|gororoba-review)",
    re.IGNORECASE,
)
RETIRED_QUALIFICATION_TERM = re.compile(
    r"\bdecision[-_\s]+grade\b",
    re.IGNORECASE,
)
# PCI 1002:5974 is shared by RS482 and RS485 and 1002:5975 is RS482M (the
# PCI ID repository), so a constant naming 5974 as RS482 alone or 5975 as
# RS485, and prose asserting either, misattribute the die; the current
# spellings are R300_PCI_DEVICE_RS48X_5974 and R300_PCI_DEVICE_RS482M_5975
# with the product resolved by r300_platform_identity_lookup.
RETIRED_CHIP_IDENTITY = re.compile(
    r"(?:\bR300_PCI_DEVICE_RS48[25]\b|\bR3V_PCI_DEVICE_ID_RS48[25]\b|"
    r"\b(?:0x)?5975\b[^\n]{0,24}?\bRS485(?!M)\b|"
    r"\bRS485(?!M)\b[^\n]{0,24}?\b(?:0x)?5975\b|"
    r"\b(?:0x)?5974\b[^\n]{0,40}?\b(?:alone|only|exclusively)\b[^\n]{0,40}?\bRS482\b)",
)
# The board's video BIOS names the part: the option-ROM string table of the
# Dell Vostro 1000 IGP carries "RS485/M BR#26605" and "ATI Radeon Xpress
# 1150".  1002:5974 is shared with the desktop RS482 (Xpress 1100), so
# naming the attended target RS482 names a chip this platform does not
# carry.  The die-class enumeration RS480/RS482/RS485 stays correct, RS482M
# stays the 1002:5975 part, and evidence sealed before the firmware read is
# exempt through retained_evidence().
MISATTRIBUTED_TARGET_CHIP = re.compile(
    r"(?:\bVostro[- ]?1000\b[^\n]{0,72}?\bRS482(?![M/])\b|"
    r"\bRS482(?![M/])\b[^\n]{0,72}?\bVostro[- ]?1000\b|"
    r"\bvostro1000[-_]rs482\b)",
)
HISTORICAL_LOG_NAMES = frozenset(
    (
        "mesa_gororoba_no_rusticl_build_20260426T010131Z",
        "mesa_gororoba_no_rusticl_install_norebuild_20260426T012346Z",
        "mesa_gororoba_pump_build_20260426T003804Z",
    )
)
ALLOWED_PRODUCT_NAMES = frozenset(
    (
        "00-mesa-gororoba-deps",
        "90-mesa-gororoba-r300",
        "linux-radeon-gororoba",
        "mesa-26-gororoba",
        "mesa-26-gororoba-builds",
        "mesa-gororoba",
        "mesa-gororoba-asan",
        "mesa-gororoba-debug",
        "mesa-gororoba-debug-asan",
        "mesa-gororoba-debug-asan-run",
        "mesa-gororoba-debug-o0",
        "mesa-gororoba-debug-o0-run",
        "mesa-gororoba-debug-optimized",
        "mesa-gororoba-debug-optimized-env",
        "mesa-gororoba-debug-optimized-run",
        "mesa-gororoba-debug-tools",
        "mesa-gororoba-env",
        "mesa-gororoba-gpu-debug-stack",
        "mesa-gororoba-newer",
        "mesa-gororoba-older",
        "mesa-gororoba-profile",
        "mesa-gororoba-run",
        "radeontool-gororoba",
        "radeontop-gororoba",
        "umr-gororoba",
        "xorg-mesa-gororoba",
    )
)
ALLOWED_LEGACY_NAMESPACE_NAMES_BY_LINE = {
    (
        "build-infra/Makefile",
        "$(if $(findstring GOROROBA_,$(variable)),$(variable))))",
    ): frozenset(("GOROROBA_",)),
    (
        "build-infra/Makefile",
        "$(filter GOROROBA_MESA_ENV,$(LEGACY_NAMESPACE_VARIABLE)),MESA_ENV_FILE,$(if \\",
    ): frozenset(("GOROROBA_MESA_ENV",)),
    (
        "build-infra/Makefile",
        "$(filter GOROROBA_MESA_PREFIX,$(LEGACY_NAMESPACE_VARIABLE)),MESA_INSTALL_PREFIX,$(if \\",
    ): frozenset(("GOROROBA_MESA_PREFIX",)),
    (
        "build-infra/Makefile",
        "$(filter MESA_GOROROBA_DEPLOY_ACCEPTED,$(LEGACY_NAMESPACE_VARIABLE)),MESA_DEPLOY_ACCEPTED,$(subst GOROROBA_,MESA_,$(LEGACY_NAMESPACE_VARIABLE)))))",
    ): frozenset(("GOROROBA_", "MESA_GOROROBA_DEPLOY_ACCEPTED")),
    (
        "build-infra/README.md",
        "contain `GOROROBA_` fail with their replacement instead of being ignored. The",
    ): frozenset(("GOROROBA_",)),
    (
        "build-infra/README.md",
        "two non-mechanical runner renames are `GOROROBA_MESA_PREFIX` to",
    ): frozenset(("GOROROBA_MESA_PREFIX",)),
    (
        "build-infra/README.md",
        "`MESA_INSTALL_PREFIX` and `GOROROBA_MESA_ENV` to `MESA_ENV_FILE`; deployment",
    ): frozenset(("GOROROBA_MESA_ENV",)),
    (
        "build-infra/README.md",
        "consent moved from `MESA_GOROROBA_DEPLOY_ACCEPTED` to `MESA_DEPLOY_ACCEPTED`.",
    ): frozenset(("MESA_GOROROBA_DEPLOY_ACCEPTED",)),
    (
        "build-infra/packaging/mesa-gororoba/PKGBUILD",
        "if [[ -v GOROROBA_DISTCC_JOBS ]]; then",
    ): frozenset(("GOROROBA_DISTCC_JOBS",)),
    (
        "build-infra/packaging/mesa-gororoba/PKGBUILD",
        'echo "GOROROBA_DISTCC_JOBS was renamed to MESA_DISTCC_JOBS" >&2',
    ): frozenset(("GOROROBA_DISTCC_JOBS",)),
    (
        "build-infra/packaging/mesa-gororoba/mesa-gororoba-env.sh",
        'if [ "${GOROROBA_MESA_PREFIX+x}" = x ]; then',
    ): frozenset(("GOROROBA_MESA_PREFIX",)),
    (
        "build-infra/packaging/mesa-gororoba/mesa-gororoba-env.sh",
        'echo "GOROROBA_MESA_PREFIX was renamed to MESA_INSTALL_PREFIX" >&2',
    ): frozenset(("GOROROBA_MESA_PREFIX",)),
    (
        "build-infra/packaging/mesa-gororoba/mesa-gororoba-run",
        'if [ "${GOROROBA_MESA_ENV+x}" = x ]; then',
    ): frozenset(("GOROROBA_MESA_ENV",)),
    (
        "build-infra/packaging/mesa-gororoba/mesa-gororoba-run",
        'echo "GOROROBA_MESA_ENV was renamed to MESA_ENV_FILE" >&2',
    ): frozenset(("GOROROBA_MESA_ENV",)),
    (
        "build-infra/packaging/mesa-gororoba-debug-asan/PKGBUILD",
        "for _legacy_name in GOROROBA_DEBUG_ASAN_BUILDDIR GOROROBA_DEBUG_ASAN_SRCROOT; do",
    ): frozenset(("GOROROBA_DEBUG_ASAN_BUILDDIR", "GOROROBA_DEBUG_ASAN_SRCROOT")),
    (
        "build-infra/packaging/mesa-gororoba-debug-asan/PKGBUILD",
        'echo "${_legacy_name} was renamed by replacing GOROROBA_ with MESA_" >&2',
    ): frozenset(("GOROROBA_",)),
    (
        "build-infra/packaging/mesa-gororoba-debug-o0/PKGBUILD",
        "for _legacy_name in GOROROBA_DEBUG_O0_BUILDDIR GOROROBA_DEBUG_O0_SRCROOT; do",
    ): frozenset(("GOROROBA_DEBUG_O0_BUILDDIR", "GOROROBA_DEBUG_O0_SRCROOT")),
    (
        "build-infra/packaging/mesa-gororoba-debug-o0/PKGBUILD",
        'echo "${_legacy_name} was renamed by replacing GOROROBA_ with MESA_" >&2',
    ): frozenset(("GOROROBA_",)),
    (
        "build-infra/packaging/mesa-gororoba-debug-optimized/PKGBUILD",
        "for _legacy_name in GOROROBA_DEBUG_BUILDDIR GOROROBA_DEBUG_SRCROOT; do",
    ): frozenset(("GOROROBA_DEBUG_BUILDDIR", "GOROROBA_DEBUG_SRCROOT")),
    (
        "build-infra/packaging/mesa-gororoba-debug-optimized/PKGBUILD",
        'echo "${_legacy_name} was renamed by replacing GOROROBA_ with MESA_" >&2',
    ): frozenset(("GOROROBA_",)),
    (
        "build-infra/packaging/mesa-gororoba-debug-optimized/mesa-gororoba-debug-optimized-env.sh",
        'if [ "${GOROROBA_MESA_PREFIX+x}" = x ]; then',
    ): frozenset(("GOROROBA_MESA_PREFIX",)),
    (
        "build-infra/packaging/mesa-gororoba-debug-optimized/mesa-gororoba-debug-optimized-env.sh",
        'echo "GOROROBA_MESA_PREFIX was renamed to MESA_INSTALL_PREFIX" >&2',
    ): frozenset(("GOROROBA_MESA_PREFIX",)),
    (
        "build-infra/packaging/mesa-gororoba-debug-optimized/mesa-gororoba-debug-optimized-run",
        'if [ "${GOROROBA_MESA_ENV+x}" = x ]; then',
    ): frozenset(("GOROROBA_MESA_ENV",)),
    (
        "build-infra/packaging/mesa-gororoba-debug-optimized/mesa-gororoba-debug-optimized-run",
        'echo "GOROROBA_MESA_ENV was renamed to MESA_ENV_FILE" >&2',
    ): frozenset(("GOROROBA_MESA_ENV",)),
    (
        "build-infra/packaging/umr-gororoba/PKGBUILD",
        "if [[ -v GOROROBA_UMR_SRC ]]; then",
    ): frozenset(("GOROROBA_UMR_SRC",)),
    (
        "build-infra/packaging/umr-gororoba/PKGBUILD",
        'echo "GOROROBA_UMR_SRC was renamed to MESA_UMR_SRC" >&2',
    ): frozenset(("GOROROBA_UMR_SRC",)),
    (
        "build-infra/scripts/source_root_control.py",
        'LEGACY_NAMESPACE_FRAGMENT = "GOROROBA_"',
    ): frozenset(("GOROROBA_",)),
    (
        "build-infra/scripts/source_root_control.py",
        '"GOROROBA_MESA_ENV": "MESA_ENV_FILE",',
    ): frozenset(("GOROROBA_MESA_ENV",)),
    (
        "build-infra/scripts/source_root_control.py",
        '"GOROROBA_MESA_PREFIX": "MESA_INSTALL_PREFIX",',
    ): frozenset(("GOROROBA_MESA_PREFIX",)),
    (
        "build-infra/scripts/source_root_control.py",
        '"MESA_GOROROBA_DEPLOY_ACCEPTED": "MESA_DEPLOY_ACCEPTED",',
    ): frozenset(("MESA_GOROROBA_DEPLOY_ACCEPTED",)),
    (
        "build-infra/tests/test_source_root_control.py",
        '("GOROROBA_TOPSRC_INPUT", "MESA_TOPSRC_INPUT"),',
    ): frozenset(("GOROROBA_TOPSRC_INPUT",)),
    (
        "build-infra/tests/test_source_root_control.py",
        '("GOROROBA_MESA_PREFIX", "MESA_INSTALL_PREFIX"),',
    ): frozenset(("GOROROBA_MESA_PREFIX",)),
    (
        "build-infra/tests/test_source_root_control.py",
        '("GOROROBA_MESA_ENV", "MESA_ENV_FILE"),',
    ): frozenset(("GOROROBA_MESA_ENV",)),
    (
        "build-infra/tests/test_source_root_control.py",
        '("MESA_GOROROBA_DEPLOY_ACCEPTED", "MESA_DEPLOY_ACCEPTED"),',
    ): frozenset(("MESA_GOROROBA_DEPLOY_ACCEPTED",)),
}
ALLOWED_LEGACY_ARTIFACT_NAMES_BY_LINE = {
    (
        "build-infra/scripts/source_root_control.py",
        'LEGACY_IDENTITY_FILENAME = ".gororoba-source-identity.json"',
    ): frozenset((".gororoba-source-identity.json",)),
    (
        "build-infra/scripts/source_root_control.py",
        'LEGACY_ROOT_IDENTITY_FILENAME = ".gororoba-external-source-identity.json"',
    ): frozenset((".gororoba-external-source-identity.json",)),
    (
        "build-infra/scripts/source_root_control.py",
        'LEGACY_SOURCE_VIEW_DIRECTORY = ".gororoba-source-view"',
    ): frozenset((".gororoba-source-view",)),
}
ALLOWED_HISTORICAL_NAMES_BY_LINE = {
    (
        "build-infra/LANE_CONSOLIDATION.md",
        "- `gororoba-terakan.meson` + `generic-x86-64-os.env` generic lane (superseded by numbered profiles)",
    ): frozenset(("gororoba-terakan",)),
}


def retired_qualification_compatibility(path: str, line: str) -> bool:
    return (
        path == "src/amd/r300/vulkan/tests/r3v_conformance_runner.py"
        and line.strip() == 'LEGACY_QUALIFICATION_VALIDITY_FIELD = "decision_grade"'
    )


def retained_evidence(path: str) -> bool:
    return path in RETAINED_EVIDENCE_FILES or path.startswith(
        RETAINED_EVIDENCE_PREFIXES
    )


RETAINED_BUNDLE_PATH = re.compile(r"results/[-\w./]*")


def inside_retained_bundle_path(line: str, offset: int) -> bool:
    """A retained bundle is cited by the name its seal fixed, so a chip
    token inside such a path is a citation rather than a claim."""
    return any(
        match.start() <= offset < match.end()
        for match in RETAINED_BUNDLE_PATH.finditer(line)
    )


def inside_double_quoted_string(line: str, offset: int) -> bool:
    in_double_quoted_string = False
    escaped = False
    for character in line[:offset]:
        if escaped:
            escaped = False
        elif character == "\\":
            escaped = True
        elif character == '"':
            in_double_quoted_string = not in_double_quoted_string
    return in_double_quoted_string


def repository_reference_allowed(
    path: str, line: str, match: re.Match[str], identifier_name: str
) -> bool:
    if identifier_name != "open_gororoba":
        return False
    if path == "AGENTS.md" or path.startswith("docs/"):
        return True
    return path == "src/amd/r300/common/r300_numeric_domain.c" and (
        inside_double_quoted_string(line, match.start())
        and inside_double_quoted_string(line, match.end() - 1)
    )


def identifier_name_for_policy(match: re.Match[str], line: str) -> str:
    identifier_name = match.group(1)
    if (
        identifier_name.endswith("-")
        and match.end() < len(line)
        and line[match.end()] in "*{<"
    ):
        return identifier_name[:-1]
    return identifier_name


def identifier_declares_function(match: re.Match[str], line: str) -> bool:
    prefix = line[: match.start()]
    suffix = line[match.end() :]
    return (
        bool(re.search(r"\bfunction\s*$", prefix))
        or suffix.startswith("(")
        or bool(re.match(r"\s+\([^)]*\)\s*(?:\{|;)", suffix))
    )


def allowed_identifier_names(path: str, line: str) -> frozenset[str]:
    stripped_line = line.strip()
    allowed_legacy_artifacts = ALLOWED_LEGACY_ARTIFACT_NAMES_BY_LINE.get(
        (path, stripped_line), frozenset()
    )
    allowed_legacy_artifact_identifiers = frozenset(
        match.group(1)
        for artifact_name in allowed_legacy_artifacts
        for match in IDENTIFIER_TOKEN.finditer(artifact_name)
        if "gororoba" in match.group(1).lower()
    )
    allowed = (
        ALLOWED_PRODUCT_NAMES
        | allowed_legacy_artifact_identifiers
        | ALLOWED_LEGACY_NAMESPACE_NAMES_BY_LINE.get((path, stripped_line), frozenset())
        | ALLOWED_HISTORICAL_NAMES_BY_LINE.get((path, stripped_line), frozenset())
    )
    if path == "build-infra/Makefile" and stripped_line == "MESA_REPO_NAME ?= gororoba":
        return allowed | frozenset(("gororoba",))
    if path in (
        "build-infra/r3v/build_kernel_replay.py",
        "src/amd/r300/vulkan/tests/r3v_qualification_inventory.py",
    ) and stripped_line == ('LEGACY_MODULE_DRIVER_TREE_FIELD = "gororoba_driver_tree"'):
        return allowed | frozenset(("gororoba_driver_tree",))
    if path == "build-infra/x130e-distcc-pump.md":
        historical_names = {
            name for name in HISTORICAL_LOG_NAMES if f"`~/logs/{name}.log`" in line
        }
        return allowed | frozenset(historical_names)
    return allowed


def policy_implementation_identifier_violations(
    text: str, starting_line_number: int
) -> list[str]:
    identifiers_by_line: dict[int, set[str]] = {}
    try:
        token_stream = tokenize.generate_tokens(io.StringIO(text).readline)
        for token_info in token_stream:
            if token_info.type == tokenize.NAME and (
                "gororoba" in token_info.string.lower()
                or RETIRED_QUALIFICATION_TERM.fullmatch(token_info.string)
            ):
                line_number = starting_line_number + token_info.start[0] - 1
                identifiers_by_line.setdefault(line_number, set()).add(
                    token_info.string
                )
    except (IndentationError, tokenize.TokenError) as error:
        return [
            f"{POLICY_IMPLEMENTATION_FILE}:{starting_line_number}: "
            f"cannot tokenize policy implementation: {error}"
        ]

    return [
        f"{POLICY_IMPLEMENTATION_FILE}:{line_number}: retired code identifier: "
        + ", ".join(sorted(identifier_names))
        for line_number, identifier_names in sorted(identifiers_by_line.items())
    ]


def violations(path: str, text: str, starting_line_number: int = 1) -> list[str]:
    if retained_evidence(path):
        return []
    if path == POLICY_IMPLEMENTATION_FILE:
        return policy_implementation_identifier_violations(text, starting_line_number)
    findings: list[str] = []
    for retired_match in RETIRED_QUALIFICATION_TERM.finditer(text):
        line_number = starting_line_number + text.count("\n", 0, retired_match.start())
        line_start = text.rfind("\n", 0, retired_match.start()) + 1
        line_end = text.find("\n", retired_match.end())
        if line_end == -1:
            line_end = len(text)
        containing_line = text[line_start:line_end]
        if not retired_qualification_compatibility(path, containing_line):
            findings.append(
                f"{path}:{line_number}: retired qualification wording: "
                f"{retired_match.group(0)!r}"
            )
    for identity_match in RETIRED_CHIP_IDENTITY.finditer(text):
        line_number = starting_line_number + text.count("\n", 0, identity_match.start())
        findings.append(
            f"{path}:{line_number}: retired chip identity claim: "
            f"{identity_match.group(0)!r}"
        )
    for target_match in MISATTRIBUTED_TARGET_CHIP.finditer(text):
        line_start = text.rfind("\n", 0, target_match.start()) + 1
        line_end = text.find("\n", target_match.end())
        if line_end == -1:
            line_end = len(text)
        if inside_retained_bundle_path(
            text[line_start:line_end], target_match.start() - line_start
        ):
            continue
        line_number = starting_line_number + text.count("\n", 0, target_match.start())
        findings.append(
            f"{path}:{line_number}: the attended target is RS485M per its "
            f"video BIOS, so this misattributes the chip: "
            f"{target_match.group(0)!r}"
        )
    for artifact_match in RETIRED_INTERNAL_ARTIFACT.finditer(text):
        line_start = text.rfind("\n", 0, artifact_match.start()) + 1
        line_end = text.find("\n", artifact_match.end())
        if line_end == -1:
            line_end = len(text)
        containing_line = text[line_start:line_end].strip()
        if artifact_match.group(0) in ALLOWED_LEGACY_ARTIFACT_NAMES_BY_LINE.get(
            (path, containing_line), frozenset()
        ):
            continue
        line_number = starting_line_number + text.count("\n", 0, artifact_match.start())
        findings.append(
            f"{path}:{line_number}: retired internal artifact name: "
            f"{artifact_match.group(0)!r}"
        )
    for line_number, line in enumerate(text.splitlines(), start=starting_line_number):
        allowed_names = allowed_identifier_names(path, line)
        identifier_names = set()
        for match in IDENTIFIER_TOKEN.finditer(line):
            identifier_name = identifier_name_for_policy(match, line)
            if "gororoba" not in identifier_name.lower():
                continue
            if identifier_declares_function(match, line):
                identifier_names.add(identifier_name)
                continue
            if identifier_name in allowed_names or repository_reference_allowed(
                path, line, match, identifier_name
            ):
                continue
            identifier_names.add(identifier_name)
        if identifier_names:
            findings.append(
                f"{path}:{line_number}: retired identifier: "
                + ", ".join(sorted(identifier_names))
            )
    return findings


def untracked_files(repository_root: Path) -> tuple[str, ...]:
    result = subprocess.run(
        [
            "git",
            "-C",
            str(repository_root),
            "ls-files",
            "-z",
            "--others",
            "--exclude-standard",
        ],
        check=True,
        capture_output=True,
    )
    return tuple(
        path.decode("utf-8", errors="surrogateescape")
        for path in result.stdout.split(b"\0")
        if path
    )


def tracked_candidate_files(repository_root: Path) -> tuple[str, ...]:
    result = subprocess.run(
        [
            "git",
            "-C",
            str(repository_root),
            "grep",
            "-l",
            "-I",
            "-i",
            "-E",
            "decision|gororoba",
            "--",
            ".",
            ":(exclude)build-infra/docs/review-thread-frontiers/**",
            ":(exclude)build-infra/docs/review-thread-classifications/**",
            ":(exclude)build-infra/docs/review-thread-corpus/**",
            ":(exclude)build-infra/docs/review-thread-corpus-analysis/**",
            ":(exclude)build-infra/docs/last-100-pr-review-comment-audit.md",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode not in (0, 1):
        raise subprocess.CalledProcessError(
            result.returncode,
            result.args,
            output=result.stdout,
            stderr=result.stderr,
        )
    return tuple(result.stdout.splitlines())


def candidate_file_violations(repository_root: Path, relative_path: str) -> list[str]:
    path = repository_root / relative_path
    if path.is_symlink():
        return []
    try:
        contents = path.read_bytes()
    except OSError as error:
        return [f"{relative_path}: cannot read candidate file: {error}"]
    if b"\0" in contents:
        return []
    return violations(relative_path, contents.decode("utf-8", errors="replace"))


def check_repository(repository_root: Path) -> list[str]:
    findings: list[str] = []
    for relative_path in tracked_candidate_files(repository_root):
        findings.extend(candidate_file_violations(repository_root, relative_path))
    for relative_path in untracked_files(repository_root):
        findings.extend(candidate_file_violations(repository_root, relative_path))
    return findings


def self_test() -> int:
    retired_identifier = "".join(("goro", "roba"))
    retired_mixed_case_identifier = "".join(("GoRo", "RoBa"))
    external_repository_identifier = f"open_{retired_identifier}"
    cases = (
        ("src/example.py", "def mesa_helper():\n    pass\n", False),
        (
            "src/example.py",
            f"def {retired_identifier}_helper():\n    pass\n",
            True,
        ),
        (
            POLICY_IMPLEMENTATION_FILE,
            f"def {retired_identifier}_helper():\n    pass\n",
            True,
        ),
        (
            POLICY_IMPLEMENTATION_FILE,
            f"{retired_mixed_case_identifier}_VALUE = 1\n",
            True,
        ),
        (POLICY_IMPLEMENTATION_FILE, "decision_grade = True\n", True),
        (
            POLICY_IMPLEMENTATION_FILE,
            f'RETIRED_NAME = "{retired_identifier}"\n'
            'RETIRED_QUALIFICATION_NAME = "decision-grade"\n'
            f"# {retired_mixed_case_identifier} remains test data.\n",
            False,
        ),
        ("src/example.py", f"def {retired_identifier}():\n    pass\n", True),
        (
            "src/example.py",
            f"def {retired_mixed_case_identifier}_helper():\n    pass\n",
            True,
        ),
        (
            "src/example.py",
            f"def deploy_mesa_{retired_identifier}():\n    pass\n",
            True,
        ),
        (
            "src/example.sh",
            f"{retired_identifier}-helper() {{ :; }}\n",
            True,
        ),
        (
            "src/example.sh",
            f"{retired_mixed_case_identifier}-helper() {{ :; }}\n",
            True,
        ),
        (
            "src/example.sh",
            f"{retired_identifier}--helper() {{ :; }}\n",
            True,
        ),
        (
            "src/example.sh",
            f"helper--{retired_identifier}() {{ :; }}\n",
            True,
        ),
        (
            "src/example.sh",
            f"mesa--{retired_identifier}--helper() {{ :; }}\n",
            True,
        ),
        ("src/example.sh", f"{retired_identifier}-() {{ :; }}\n", True),
        (
            "src/example.sh",
            f"function -{retired_identifier} {{ :; }}\n",
            True,
        ),
        ("docs/example.md", "fake-gororoba is not a repository.\n", True),
        ("docs/example.md", "fake-gororoba-* is not a product glob.\n", True),
        (
            "src/example.sh",
            f"mesa-{retired_identifier}-helper() {{ :; }}\n",
            True,
        ),
        (
            "src/example.sh",
            f"mesa-{retired_identifier}() {{ :; }}\n",
            True,
        ),
        ("src/example.c", f"{external_repository_identifier}();\n", True),
        (
            "docs/example.md",
            f"int {external_repository_identifier}(int value); "
            f"repository {external_repository_identifier}.\n",
            True,
        ),
        (
            "src/amd/r300/common/r300_numeric_domain.c",
            f"void {external_repository_identifier}(void) {{}}\n",
            True,
        ),
        (
            "src/amd/r300/common/r300_numeric_domain.c",
            'message = "proof source: open_gororoba Quaternion.v";\n',
            False,
        ),
        (
            "src/amd/r300/common/r300_numeric_domain.c",
            f"void {external_repository_identifier}(void) {{}} "
            f'/* "{external_repository_identifier}" */\n',
            True,
        ),
        ("src/example.c", f"MESA_{retired_identifier.upper()}();\n", True),
        ("src/example.py", 'PATH = ".gororoba-source-view"\n', True),
        ("build-infra/Makefile", "path := gororoba-toolchain.meson\n", True),
        ("docs/example.md", "The run is decision-grade.\n", True),
        ("docs/example.md", "The run is decision\n  grade.\n", True),
        ("build-infra/example.env", "GOROROBA_JOBS=6\n", True),
        (
            "build-infra/docs/review-thread-frontiers/raw/example.json",
            '"body": "GOROROBA_JOBS and decision-grade"\n',
            False,
        ),
        ("docs/example.md", "Proofs live in open_gororoba.\n", False),
        (
            "build-infra/tests/test_deploy_mesa.py",
            'SCRIPT_PATH = "deploy_mesa.py"\n',
            False,
        ),
        (
            "build-infra/r3v/build_kernel_replay.py",
            'LEGACY_MODULE_DRIVER_TREE_FIELD = "gororoba_driver_tree"\n',
            False,
        ),
        (
            "build-infra/r3v/build_kernel_replay.py",
            f"{retired_identifier}_driver_tree\n(\n)\n",
            True,
        ),
        (
            "src/amd/r300/vulkan/tests/r3v_conformance_runner.py",
            'LEGACY_QUALIFICATION_VALIDITY_FIELD = "decision_grade"\n',
            False,
        ),
        (
            "src/amd/r300/vulkan/tests/r3v_conformance_runner.py",
            'qualification_valid = receipt["decision_grade"]\n',
            True,
        ),
        (
            "src/example.c",
            f"int {retired_identifier} /* retired helper */ " "(void) { return 0; }\n",
            True,
        ),
        (
            "src/example.h",
            f"#define {retired_identifier} \\\n(value)\n",
            True,
        ),
        ("src/example.c", f"void (*{retired_identifier})(void);\n", True),
        (
            "src/example.c",
            f"int {retired_mixed_case_identifier}\n"
            "/* retired helper */\n(void) { return 0; }\n",
            True,
        ),
        ("src/example.py", f"{retired_identifier} = lambda: None\n", True),
        ("src/example.py", f"class {retired_identifier}:\n    pass\n", True),
        ("src/example.py", f"{retired_identifier}\n", True),
        (
            "src/example.py",
            f"from helpers import {retired_identifier}\n",
            True,
        ),
        ("src/example.py", f"{retired_identifier}, mesa = values\n", True),
        ("src/example.py", f"{retired_identifier} += 1\n", True),
        ("src/example.cpp", f"{retired_identifier}<int>();\n", True),
        (
            "src/example.c",
            f"extern void (* const {retired_identifier})(void);\n",
            True,
        ),
        ("src/example.h", "#define R300_PCI_DEVICE_RS48" "5 0x5975\n", True),
        ("src/example.h", "#define R300_PCI_DEVICE_RS48X_5974 0x5974\n", False),
        ("docs/example.md", "0x5975 is RS48" "5 (Radeon Xpress 1150).\n", True),
        ("docs/example.md", "1002:5975 is RS482M; 1002:5974 is RS482/RS485.\n", False),
        ("docs/example.md", "RS48" "5-marketed 1002:5975 refuses.\n", True),
        ("docs/example.md", "1002:5974 alone proves RS48" "2 here.\n", True),
        ("docs/example.md", "the Vostro 1000 (1002:5974, RS485M) target.\n", False),
        ("docs/example.md", "the Vostro 1000 RS482 target board.\n", True),
        ("docs/example.md", "RS482 is the chip in the Vostro 1000.\n", True),
        ("build-infra/docs/review-thread-corpus/x.md",
         "vostro1000-rs482 dump\n", False),
        ("docs/example.md", "vostro1000-rs482-capture\n", True),
        ("docs/example.md",
         "see `steinmarder-r300/results/cachyos-vostro1000-rs482-run/`\n",
         False),
        ("docs/example.md", "the Vostro 1000 RS480/RS482/RS485 family.\n", False),
        ("docs/example.md", "the Vostro 1000 carries RS485M silicon.\n", False),
        ("docs/example.md", "1002:5975 is RS482M, not the Vostro 1000 part.\n", False),
        ("build-infra/example.md", "Use mesa-26-gororoba.\n", False),
        ("build-infra/example.md", "Use mesa-26-gororoba-* worktrees.\n", False),
        ("build-infra/example.md", "Install mesa-gororoba-debug-optimized.\n", False),
        ("build-infra/example.md", "Install umr-gororoba.\n", False),
        ("build-infra/Makefile", "MESA_REPO_NAME ?= gororoba\n", False),
        (
            "build-infra/Makefile",
            "export GOROROBA_MESA_ENV := active\n",
            True,
        ),
        (
            "build-infra/scripts/source_root_control.py",
            'LEGACY_SOURCE_VIEW_DIRECTORY = ".gororoba-source-view"\n',
            False,
        ),
        (
            "build-infra/scripts/source_root_control.py",
            'CURRENT = ".gororoba-source-view"\n',
            True,
        ),
    )
    for path, text, expected_failure in cases:
        failed = bool(violations(path, text))
        if failed != expected_failure:
            print(
                f"naming policy self-test mismatch for {path}: "
                f"expected_failure={expected_failure}, failed={failed}",
                file=sys.stderr,
            )
            return 1
    print("naming policy self-test: known-good and known-bad inputs accepted")
    return 0


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path)
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if arguments.self_test and self_test() != 0:
        return 1
    if arguments.repo_root is None:
        if arguments.self_test:
            return 0
        print("--repo-root is required without --self-test", file=sys.stderr)
        return 2
    findings = check_repository(arguments.repo_root.resolve())
    if findings:
        print("\n".join(findings), file=sys.stderr)
        return 1
    print("naming policy: active files use plain Mesa and qualification names")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
