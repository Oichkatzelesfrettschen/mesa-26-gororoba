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

Artifact identity is read from Meson's own syntax tree rather than from the
presence of a name: an assignment target, a comment, an inactive branch, or a
stale string carries no build authority.  The audit binds one top-level
installed `shared_library()` and one named installed ICD `custom_target()` to
each family contract, including the complete output expression and install
directory.
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from mesonbuild import mparser
except ImportError:
    mparser = None


class UnknownManifestFormError(ValueError):
    """Report a manifest-form value outside the finite family contract."""

    def __init__(self, manifest_form):
        super().__init__(f"unknown manifest form: {manifest_form}")


class FixtureContractError(ValueError):
    """Report a synthetic tree that cannot satisfy fixture ownership."""

    def __init__(self, family_root, reason):
        super().__init__(f"{reason}: {family_root}")


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


def parse_meson_source(text, filename, parser_module=mparser):
    """Parse one Meson file through the Meson version that builds the tree."""
    if parser_module is None:
        return None, [f"{filename}: Meson Python parser is unavailable"]
    try:
        return parser_module.Parser(text, filename).parse(), []
    except parser_module.ParseException as error:
        return None, [f"{filename}: Meson syntax error at line {error.lineno}"]


def unwrap_parenthesized(node):
    """Return the expression inside any syntactic parenthesis wrappers."""
    while isinstance(node, mparser.ParenthesizedNode):
        node = node.inner
    return node


def describe_meson_condition(node):
    """Return a stable description for a control-context diagnostic."""
    node = unwrap_parenthesized(node)
    if isinstance(node, mparser.IdNode):
        return node.value
    if isinstance(node, mparser.BooleanNode):
        return str(node.value).lower()
    return f"{type(node).__name__} at line {node.lineno}"


def iter_meson_statements(block, contexts=()):
    """Yield statements with the exact Meson AST control path that owns them."""
    for statement in block.lines:
        yield statement, contexts
        if isinstance(statement, mparser.IfClauseNode):
            for branch_index, branch in enumerate(statement.ifs):
                branch_name = "if" if branch_index == 0 else "elif"
                condition = describe_meson_condition(branch.condition)
                yield from iter_meson_statements(
                    branch.block, contexts + (f"{branch_name} {condition}",)
                )
            if not isinstance(statement.elseblock, mparser.EmptyNode):
                yield from iter_meson_statements(
                    statement.elseblock.block, contexts + ("else",)
                )
        elif isinstance(statement, mparser.ForeachClauseNode):
            variables = ", ".join(variable.value for variable in statement.varnames)
            yield from iter_meson_statements(
                statement.block, contexts + (f"foreach {variables}",)
            )


def statement_function(statement):
    """Return a direct function call made by one Meson statement."""
    if isinstance(statement, mparser.FunctionNode):
        return statement
    if isinstance(statement, mparser.AssignmentNode) and isinstance(
        statement.value, mparser.FunctionNode
    ):
        return statement.value
    return None


def regular_string(node):
    """Match a regular literal across Meson 1.4 and unified string nodes."""
    format_string_class = getattr(mparser, "FormatStringNode", ())
    return (
        isinstance(node, mparser.StringNode)
        and not isinstance(node, format_string_class)
        and not getattr(node, "is_fstring", False)
        and not getattr(node, "is_multiline", False)
    )


def literal_string(node, expected):
    """Match one regular, non-formatted Meson string literal exactly."""
    return regular_string(node) and node.value == expected


def first_literal_argument(call):
    """Return the first positional regular string argument, when present."""
    if not call.args.arguments:
        return None
    argument = call.args.arguments[0]
    if not regular_string(argument):
        return None
    return argument.value


def collect_meson_calls(tree, function_name):
    """Return direct calls to one function with their active control paths."""
    calls = []
    for statement, contexts in iter_meson_statements(tree):
        call = statement_function(statement)
        if call is not None and call.func_name.value == function_name:
            calls.append({"call": call, "contexts": contexts})
    return calls


def contracted_predicate_writes(tree, predicates):
    """Return contracted predicate names written or shadowed in an entry file."""
    writes = set()
    for statement, _ in iter_meson_statements(tree):
        if isinstance(statement, mparser.AssignmentNode):
            if statement.var_name.value in predicates:
                writes.add(statement.var_name.value)
        elif isinstance(statement, mparser.ForeachClauseNode):
            writes.update(
                variable.value
                for variable in statement.varnames
                if variable.value in predicates
            )
        call = statement_function(statement)
        if call is None or call.func_name.value not in {
            "set_variable",
            "unset_variable",
        }:
            continue
        predicate = first_literal_argument(call)
        if predicate in predicates:
            writes.add(predicate)
    return writes


def family_relative_path(family_root, entry_file):
    """Return the subdir argument from one entry file to a family root."""
    return str(Path(family_root).relative_to(Path(entry_file).parent))


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
        tree, parse_failures = parse_meson_source(
            entry.read_text(encoding="utf-8", errors="replace"), entry_file
        )
        parsed_entries[entry_file] = tree
        failures.extend(parse_failures)

        if tree is None:
            continue
        predicates = {
            contract["entry_points"][entry_file]
            for contract in AMD_VULKAN_FAMILIES.values()
            if entry_file in contract["entry_points"]
        }
        for predicate in sorted(contracted_predicate_writes(tree, predicates)):
            failures.append(f"{entry_file} writes contracted predicate {predicate}")

    for family_root, contract in AMD_VULKAN_FAMILIES.items():
        meson = root / family_root / "meson.build"
        if not meson.is_file():
            failures.append(
                f"missing driver root meson.build: {family_root}/meson.build"
            )
        for entry_file, predicate in contract["entry_points"].items():
            tree = parsed_entries[entry_file]
            if tree is None:
                continue
            relative = family_relative_path(family_root, entry_file)
            matching_calls = [
                call
                for call in collect_meson_calls(tree, "subdir")
                if first_literal_argument(call["call"]) == relative
                and len(call["call"].args.arguments) == 1
                and not call["call"].args.kwargs
            ]
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


def meson_keyword(call, name):
    """Return one named Meson function argument, when present."""
    for key, value in call.args.kwargs.items():
        if key.value == name:
            return value
    return None


def meson_true(node):
    """Return whether an AST node is the literal Meson boolean true."""
    return isinstance(node, mparser.BooleanNode) and node.value is True


def host_cpu_call(node):
    """Return whether a node is the exact host_machine.cpu() expression."""
    node = unwrap_parenthesized(node)
    return (
        isinstance(node, mparser.MethodNode)
        and isinstance(node.source_object, mparser.IdNode)
        and node.source_object.value == "host_machine"
        and node.name.value == "cpu"
        and not node.args.arguments
        and not node.args.kwargs
    )


def manifest_output_matches(node, contract):
    """Match one family's complete installed ICD output expression."""
    node = unwrap_parenthesized(node)
    stem = contract["manifest_stem"]
    if contract["manifest_form"] == "cpu-format":
        return (
            isinstance(node, mparser.MethodNode)
            and literal_string(node.source_object, f"{stem}.@0@.json")
            and node.name.value == "format"
            and len(node.args.arguments) == 1
            and not node.args.kwargs
            and host_cpu_call(node.args.arguments[0])
        )
    if contract["manifest_form"] == "suffix-variable":
        return (
            isinstance(node, mparser.ArithmeticNode)
            and node.operator.value == "+"
            and literal_string(node.left, f"{stem}.")
            and isinstance(unwrap_parenthesized(node.right), mparser.IdNode)
            and unwrap_parenthesized(node.right).value == "vulkan_manifest_suffix"
        )
    return False


def active_installed_library(call_record, library_name):
    """Match one top-level installed loader shared library."""
    call = call_record["call"]
    return (
        not call_record["contexts"]
        and first_literal_argument(call) == library_name
        and meson_true(meson_keyword(call, "install"))
    )


def active_installed_manifest(call_record, contract):
    """Match one top-level installed ICD manifest custom target."""
    call = call_record["call"]
    install_dir = unwrap_parenthesized(meson_keyword(call, "install_dir"))
    return (
        not call_record["contexts"]
        and first_literal_argument(call) == contract["manifest_stem"]
        and manifest_output_matches(meson_keyword(call, "output"), contract)
        and meson_true(meson_keyword(call, "install"))
        and isinstance(install_dir, mparser.IdNode)
        and install_dir.value == "with_vulkan_icd_dir"
    )


def check_artifacts(root, failures):
    """Require active installed loader and ICD manifest target definitions."""
    for family_root, contract in AMD_VULKAN_FAMILIES.items():
        meson_rel = f"{family_root}/meson.build"
        meson = root / meson_rel
        if not meson.is_file():
            continue
        tree, parse_failures = parse_meson_source(
            meson.read_text(encoding="utf-8", errors="replace"), meson_rel
        )
        failures.extend(parse_failures)
        if tree is None:
            continue

        dso = contract["shared_library"]
        library_matches = [
            call
            for call in collect_meson_calls(tree, "shared_library")
            if active_installed_library(call, dso)
        ]
        if len(library_matches) != 1:
            failures.append(
                f"{meson_rel} defines no unique active installed "
                f"shared_library named {dso}"
            )

        manifest_matches = [
            call
            for call in collect_meson_calls(tree, "custom_target")
            if active_installed_manifest(call, contract)
        ]
        if len(manifest_matches) != 1:
            failures.append(
                f"{meson_rel} emits no unique active installed ICD manifest named "
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
    raise UnknownManifestFormError(contract["manifest_form"])


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
            "  install : true,\n"
            ")\n"
            "icd = custom_target(\n"
            f"  '{contract['manifest_stem']}',\n"
            f"  output : {output_expression},\n"
            "  install_dir : with_vulkan_icd_dir,\n"
            "  install : true,\n"
            ")\n"
        )
    elif contract:
        # The names survive as an assignment target and a comment while the
        # definitions they claim to pin have been renamed.
        body += (
            f"lib{contract['shared_library']} = shared_library(\n"
            "  'vulkan_renamed',\n"
            "  install : true,\n"
            ")\n"
            f"# {contract['manifest_stem']}\n"
            "icd = custom_target(\n"
            "  'renamed_icd',\n"
            "  output : 'renamed_icd.@0@.json',\n"
            "  install_dir : with_vulkan_icd_dir,\n"
            "  install : true,\n"
            ")\n"
        )
    (base / family / "meson.build").write_text(body, encoding="utf-8")


def _stage_good(base):
    """Stage every mapped family under its exact entry predicate."""
    entry_blocks = {}
    for family_root, contract in AMD_VULKAN_FAMILIES.items():
        _write_family(base, family_root)
        for entry_file, predicate in contract["entry_points"].items():
            relative = family_relative_path(family_root, entry_file)
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
        raise FixtureContractError(family_root, "fixture requires one entry point")
    entry_file, predicate = next(iter(contract["entry_points"].items()))
    relative = family_relative_path(family_root, entry_file)
    original = f"if {predicate}\n  subdir('{relative}')\nendif\n"
    entry_path = base / entry_file
    body = entry_path.read_text(encoding="utf-8")
    if body.count(original) != 1:
        raise FixtureContractError(family_root, "fixture entry block is not unique")
    entry_path.write_text(body.replace(original, replacement, 1), encoding="utf-8")


def artifact_failures(family_root):
    """Return the exact two artifact failures for a synthetic family."""
    contract = AMD_VULKAN_FAMILIES[family_root]
    meson_rel = f"{family_root}/meson.build"
    return [
        f"{meson_rel} defines no unique active installed shared_library named "
        f"{contract['shared_library']}",
        f"{meson_rel} emits no unique active installed ICD manifest named "
        f"{contract['installed_manifest']}",
    ]


def append_parser_self_test_cases(cases):
    """Add Meson syntax and parser-availability calibration cases."""
    malformed_control_cases = (
        ("elif-without-if", "elif feature\n"),
        ("else-without-if", "else\n"),
        ("endif-without-if", "endif\n"),
        ("endforeach-without-foreach", "endforeach\n"),
        ("unterminated-if", "if feature\n"),
        ("unterminated-foreach", "foreach item : items\n"),
    )
    for case_name, source in malformed_control_cases:
        filename = f"{case_name}.build"
        _, parse_failures = parse_meson_source(source, filename)
        cases.append(
            (
                f"known-bad: {case_name}",
                parse_failures,
                [f"{filename}: Meson syntax error at line 1"],
            )
        )

    _, unavailable_parser_failures = parse_meson_source(
        "", "unavailable-parser.build", parser_module=None
    )
    cases.append(
        (
            "known-bad: unavailable Meson parser",
            unavailable_parser_failures,
            ["unavailable-parser.build: Meson Python parser is unavailable"],
        )
    )


def append_predicate_write_self_test_cases(cases, temporary_root):
    """Add every supported contracted-predicate write mechanism."""
    predicate_write_cases = (
        ("assignment", "with_ati_r300_vk = false\n"),
        ("plus assignment", "with_ati_r300_vk += true\n"),
        (
            "foreach shadow",
            "foreach with_ati_r300_vk : []\nendforeach\n",
        ),
        (
            "set_variable write",
            "set_variable('with_ati_r300_vk', false)\n",
        ),
        (
            "unset_variable write",
            "unset_variable('with_ati_r300_vk')\n",
        ),
    )
    canonical_r3v_entry = "if with_ati_r300_vk\n  subdir('r300/vulkan')\nendif\n"
    for case_name, write_statement in predicate_write_cases:
        bad = temporary_root / f"predicate-{case_name.replace(' ', '-')}"
        _stage_good(bad)
        _replace_family_entry(
            bad,
            "src/amd/r300/vulkan",
            write_statement + canonical_r3v_entry,
        )
        cases.append(
            (
                f"known-bad: contracted predicate {case_name}",
                check_tree(bad),
                ["src/amd/meson.build writes contracted predicate " "with_ati_r300_vk"],
            )
        )


def report_self_test_cases(cases):
    """Compare every calibrated failure set and print the exact differences."""
    all_cases_match = True
    for label, failures, expected in cases:
        # Synthetic trees are outside Git, so tracked-path scanning has no
        # authority there; the same path is calibrated by the live-tree run.
        failures = [
            failure for failure in failures if not failure.startswith("git grep failed")
        ]
        if sorted(failures) == sorted(expected):
            print(f"OK    {label}: {len(failures)} failure(s) as expected")
            continue
        all_cases_match = False
        print(f"MISCALIBRATED  {label}")
        for line in sorted(set(expected) - set(failures)):
            print(f"        expected, absent: {line}")
        for line in sorted(set(failures) - set(expected)):
            print(f"        unexpected: {line}")
    return all_cases_match


def self_test():
    """Calibrate every independent rejection against a named known-bad tree.

    Each known-bad isolates one check, and the calibration compares the
    failure set against the exact expectation.  Reducing a known-bad to
    `bool(failures)` lets two of three checks regress unnoticed as long as the
    third still fires, which is how a verdict producer goes quietly blind.
    """
    cases = []
    append_parser_self_test_cases(cases)

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

        # Meson's lexer owns comments and hash characters inside strings.
        good = tmp / "entry-comments-and-hash-string"
        _stage_good(good)
        _replace_family_entry(
            good,
            "src/amd/r300/vulkan",
            "if with_ati_r300_vk # exact family predicate\n"
            "  message('a#b')\n"
            "  subdir('r300/vulkan') # direct family root\n"
            "endif\n",
        )
        cases.append(("known-good: comments and hash string", check_tree(good), []))

        # Parentheses and explicit line continuation preserve the same AST
        # predicate identity as the canonical spelling.
        good = tmp / "entry-parenthesized-predicate"
        _stage_good(good)
        _replace_family_entry(
            good,
            "src/amd/r300/vulkan",
            "if(with_ati_r300_vk)\n  subdir('r300/vulkan')\nendif\n",
        )
        cases.append(("known-good: parenthesized predicate", check_tree(good), []))

        good = tmp / "entry-continued-predicate"
        _stage_good(good)
        _replace_family_entry(
            good,
            "src/amd/r300/vulkan",
            "if \\\n  with_ati_r300_vk\n  subdir('r300/vulkan')\nendif\n",
        )
        cases.append(("known-good: continued predicate", check_tree(good), []))

        # A multiline string containing complete control syntax and the exact
        # call carries no Meson build authority.
        bad = tmp / "entry-in-multiline-string"
        _stage_good(bad)
        _replace_family_entry(
            bad,
            "src/amd/r300/vulkan",
            "message('''\n"
            "if with_ati_r300_vk\n"
            "  subdir('r300/vulkan')\n"
            "endif\n"
            "''')\n",
        )
        cases.append(
            (
                "known-bad: entry only in multiline string",
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
            relative = family_relative_path(family_root, entry_file)
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

        append_predicate_write_self_test_cases(cases, tmp)

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
                    "if with_ati_r300_vk -> foreach driver; "
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

        # Complete target spellings inside a multiline string remain inert AST
        # data even when both active definitions have drifted.
        bad = tmp / "r3v-artifacts-only-in-multiline-string"
        _stage_good(bad)
        _write_family(bad, "src/amd/r300/vulkan", artifacts_ok=False)
        r3v_meson = bad / "src/amd/r300/vulkan/meson.build"
        r3v_meson.write_text(
            r3v_meson.read_text(encoding="utf-8")
            + "stale_targets = '''\n"
            + "lib = shared_library(\n"
            + "  'vulkan_r3v',\n"
            + "  install : true,\n"
            + ")\n"
            + "icd = custom_target(\n"
            + "  'r3v_icd',\n"
            + "  output : 'r3v_icd.@0@.json'.format(host_machine.cpu()),\n"
            + "  install_dir : with_vulkan_icd_dir,\n"
            + "  install : true,\n"
            + ")\n"
            + "'''\n",
            encoding="utf-8",
        )
        cases.append(
            (
                "known-bad: R3V artifacts only in multiline string",
                check_tree(bad),
                artifact_failures("src/amd/r300/vulkan"),
            )
        )

        # Loader identity belongs to one installed top-level target.  A
        # non-installed target or a spelling retained under an inactive block
        # carries no loader authority.
        for family_root, contract in AMD_VULKAN_FAMILIES.items():
            bad = tmp / f"{contract['driver_leaf']}-library-not-installed"
            _stage_good(bad)
            meson_path = bad / family_root / "meson.build"
            meson_body = meson_path.read_text(encoding="utf-8")
            meson_path.write_text(
                meson_body.replace("  install : true,\n", "  install : false,\n", 1),
                encoding="utf-8",
            )
            cases.append(
                (
                    f"known-bad: {contract['driver_leaf']} library not installed",
                    check_tree(bad),
                    [artifact_failures(family_root)[0]],
                )
            )

            bad = tmp / f"{contract['driver_leaf']}-library-only-inactive"
            _stage_good(bad)
            meson_path = bad / family_root / "meson.build"
            meson_body = meson_path.read_text(encoding="utf-8")
            meson_path.write_text(
                meson_body.replace(
                    f"'{contract['shared_library']}'", "'vulkan_renamed'", 1
                )
                + "if false\n"
                + "  disabled_lib = shared_library(\n"
                + f"    '{contract['shared_library']}',\n"
                + "    project_files,\n"
                + "    install : true,\n"
                + "  )\n"
                + "endif\n",
                encoding="utf-8",
            )
            cases.append(
                (
                    f"known-bad: {contract['driver_leaf']} library only inactive",
                    check_tree(bad),
                    [artifact_failures(family_root)[0]],
                )
            )

        # Manifest identity, output, and installation properties belong to one
        # named top-level custom target.
        for family_root, contract in AMD_VULKAN_FAMILIES.items():
            manifest_tail = (
                "  install_dir : with_vulkan_icd_dir,\n" "  install : true,\n" ")\n"
            )

            bad = tmp / f"{contract['driver_leaf']}-manifest-not-installed"
            _stage_good(bad)
            meson_path = bad / family_root / "meson.build"
            meson_body = meson_path.read_text(encoding="utf-8")
            meson_path.write_text(
                meson_body.replace(
                    manifest_tail,
                    "  install_dir : with_vulkan_icd_dir,\n"
                    "  install : false,\n"
                    ")\n",
                    1,
                ),
                encoding="utf-8",
            )
            cases.append(
                (
                    f"known-bad: {contract['driver_leaf']} manifest not installed",
                    check_tree(bad),
                    [artifact_failures(family_root)[1]],
                )
            )

            bad = tmp / f"{contract['driver_leaf']}-manifest-wrong-install-dir"
            _stage_good(bad)
            meson_path = bad / family_root / "meson.build"
            meson_body = meson_path.read_text(encoding="utf-8")
            meson_path.write_text(
                meson_body.replace(
                    "install_dir : with_vulkan_icd_dir",
                    "install_dir : get_option('datadir')",
                    1,
                ),
                encoding="utf-8",
            )
            cases.append(
                (
                    f"known-bad: {contract['driver_leaf']} manifest wrong install dir",
                    check_tree(bad),
                    [artifact_failures(family_root)[1]],
                )
            )

            bad = tmp / f"{contract['driver_leaf']}-manifest-helper-only"
            _stage_good(bad)
            meson_path = bad / family_root / "meson.build"
            meson_body = meson_path.read_text(encoding="utf-8")
            output_expression = fixture_manifest_expression(contract)
            meson_path.write_text(
                meson_body.replace(
                    f"  '{contract['manifest_stem']}',\n",
                    "  'renamed_icd',\n",
                    1,
                )
                + "helper_icd = custom_target(\n"
                + f"  '{contract['manifest_stem']}',\n"
                + f"  output : {output_expression},\n"
                + "  install_dir : with_vulkan_icd_dir,\n"
                + "  install : false,\n"
                + ")\n",
                encoding="utf-8",
            )
            cases.append(
                (
                    f"known-bad: {contract['driver_leaf']} manifest helper only",
                    check_tree(bad),
                    [artifact_failures(family_root)[1]],
                )
            )

        # Meson AST identity is independent of line layout and a trailing comma
        # on the final keyword argument.
        for family_root, contract in AMD_VULKAN_FAMILIES.items():
            good = tmp / f"{contract['driver_leaf']}-compact-artifacts"
            _stage_good(good)
            output_expression = fixture_manifest_expression(contract)
            (good / family_root / "meson.build").write_text(
                "project_files = []\n"
                + "lib = shared_library("
                + f"'{contract['shared_library']}', project_files, install : true)\n"
                + "icd = custom_target("
                + f"'{contract['manifest_stem']}', install : true, "
                + "install_dir : with_vulkan_icd_dir, "
                + f"output : {output_expression})\n",
                encoding="utf-8",
            )
            cases.append(
                (
                    f"known-good: {contract['driver_leaf']} compact artifacts",
                    check_tree(good),
                    [],
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
                + "stale_manifest = '''\n"
                + f"output : {valid_expression},\n"
                + "'''\n",
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

    return report_self_test_cases(cases)


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
