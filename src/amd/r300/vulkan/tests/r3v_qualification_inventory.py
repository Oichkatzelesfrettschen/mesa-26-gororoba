# SPDX-License-Identifier: MIT
"""Qualification test-inventory gate for the R3V lane.

A qualification run proves the suite it names, so a build that silently
registered fewer tests must fail the run rather than return a smaller green
suite.  This tool reads Meson's introspection records from a build directory,
reports every dependency the qualification profile relies on, and in
qualification mode exits nonzero when any required test or dependency is
absent.  The triangle manifest integration test requires b3sum for its
independent digest oracle, so qualification also requires that host utility.

A build once registered zero native tests because the native backend and the
drm-shim tool were both missing, and the suite reported green; that build is
the permanent known-bad calibration, reproduced by the zero-native fixture.

A Gallium-enabled qualification also requires the real-nm known-bad separation
leg.  Native-only qualification keeps the synthetic separation selftest while
the Gallium DSO leg remains unavailable.

Replay qualification reads the loaded Radeon module srcversion and installed
driver-tree metadata, then compares both identities to the replay provenance.
Missing or changing deployment identity keeps the qualification gate closed.

Modes:
  <builddir>                  inventory report, exit 0 when readable
  <builddir> --require-tests  report plus fatal verdict on a required test
                              absent from its suite, or on an option that
                              removes the native tests wholesale; the
                              deployment identity and replay tooling a host
                              supplies stay reported rather than deciding
  <builddir> --qualification  report plus fatal verdict on any absence
  --fixture zero-native       the historic zero-native-test build: the real
                              required set evaluated against a test list with
                              every native suite entry removed; must fail
  --selftest                  zero-native fixture must fail and the complete
                              synthetic set must pass, through the same
                              evaluation the qualification mode runs
"""

import argparse
import ctypes.util
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPLAY_PROVENANCE_SCHEMA = "r3v-cs-track-replay-provenance/v1"
REPLAY_PROVENANCE_BUILDER = "build-infra/r3v/build_kernel_replay.py"
REPLAY_ARTIFACT = "replay_r300_cs_track"
FULL_SHA256 = set("0123456789abcdef")
MODULE_NAME = "radeon"
MODULE_SYSFS_SRCVERSION = Path("/sys/module/radeon/srcversion")
MODULE_DRIVER_TREE_FIELD = "gororoba_driver_tree"
MODULE_SRCVERSION_CHARS = set(
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")

R300_FLOAT2_TUPLE_REQUIRED_TESTS: tuple[str, ...] = (
    "r300-r2vb-float2-tuple-pass",
    "r300-r2vb-float2-tuple-replay",
)

R3V_FLOAT2_TUPLE_REQUIRED_TESTS: tuple[str, ...] = (
    "r3v-native-float2-tuple-cell",
    "r3v-native-float2-tuple-arming-runner",
    "r3v-native-float2-tuple-cell-closed",
    "r3v-native-float2-tuple-cell-open",
    "r3v-native-float2-tuple-cell-geometry",
    "r3v-native-float2-tuple-cell-external-manifest-ignored",
)

FLOAT2_TUPLE_REQUIRED_TESTS = (
    R300_FLOAT2_TUPLE_REQUIRED_TESTS + R3V_FLOAT2_TUPLE_REQUIRED_TESTS)

# The qualification-critical tests.  A name here is load-bearing evidence for
# the qualification verdict: transport admission, arming, dispatch closure,
# parser replay, or a known-bad calibration whose absence would let a broken
# verdict pass unchallenged.
REQUIRED_TESTS: tuple[str, ...] = (
    # r300 common: builder, cell, contract, staging, and parser replay.
    "r300-pm4-builder",
    "r300-fragment-binary",
    "r300-first-draw-state",
    "r300-tcl-bypass-triangle",
    "r300-delivery-route",
    "r300-r2vb-carrier-delivery",
    "r300-grid-fold",
    "r300-common-boundary-audit-selftest",
    "r300-common-boundary-audit-common-sources",
    "r300-common-boundary-audit-common-objects",
    "r300-common-boundary-audit-cpu-sources",
    "r300-common-boundary-audit-known-bad-source",
    "r300-common-boundary-audit-known-bad-object",
    "r300-common-consumer-census-selftest",
    "r300-common-consumer-census",
    "r300-r2vb-producer-pass",
    "r300-r2vb-producer-replay",
    "r300-r2vb-producer-fp24-sweep-replay",
    "r300-r2vb-producer-fp24-bisect-replay",
    *R300_FLOAT2_TUPLE_REQUIRED_TESTS,
    "r300-r2vb-reingest-pass",
    "r300-r2vb-reingest-replay",
    "r300-r2vb-public-route",
    "r300-r2vb-public-route-replay",
    "r300-zb-depth-state",
    "r300-zb-depth-control-cell",
    "r300-zb-depth-control-replay",
    "r300-tcl-bypass-fs-block-regeneration",
    "r300-tcl-bypass-offline-replay",
    "r300-cs-track-replay",
    "r300-staging-manifest",
    "r300-staging-manifest-runner-selftest",
    "r300-triangle-manifest-integration",
    # r3v native: arming, dispatch, recording, submit, lifecycle, WSI.
    "r3v-native-arming",
    "r3v-native-arming-positive",
    "r3v-native-zb-depth-control-closed",
    "r3v-native-zb-depth-control-open",
    "r3v-native-zb-depth-control-arming-runner",
    "r3v-native-arming-runner-refuses-undeclared",
    *R3V_FLOAT2_TUPLE_REQUIRED_TESTS,
    "r3v-native-entrypoint-audit-selftest",
    "r3v-native-entrypoint-audit-baseline",
    "r3v-native-entrypoint-audit-baseline-known-bad-extra",
    "r3v-native-entrypoint-closure",
    "r3v-native-entrypoint-closure-known-bad-BindBufferMemory2",
    "r3v-native-entrypoint-closure-known-bad-CreateImage",
    "r3v-native-public-surface-policy",
    "r3v-native-direct-table-sweep",
    "r3v-native-direct-table-sweep-known-bad-closed-extension",
    "r3v-native-direct-table-sweep-known-bad-higher-core",
    "r3v-native-direct-table-sweep-known-bad-promoted-alias",
    "r3v-native-format-features",
    "r3v-native-format-features-known-bad-query-disagreement",
    "r3v-native-format-features-known-bad-texel-buffer",
    # Native descriptor contract: layout/pool admission, pool capacity,
    # offset and VK_WHOLE_SIZE range semantics, poison-on-void-update.
    "r3v-native-descriptor",
    "r3v-native-descriptor-known-bad-unpoisoned-oversize-write",
    "r3v-native-descriptor-known-bad-pool-overflow-admits",
    "r3v-native-loader-sweep",
    "r3v-native-loader-application",
    "r3v-native-loader-application-known-bad-ib",
    "r3v-native-loader-application-known-bad-corrupt_footprint",
    "r3v-native-loader-application-known-bad-corrupt_tail",
    "r3v-native-loader-application-symbols",
    "r3v-native-loader-application-symbols-known-bad",
    "r3v-native-recording-poison",
    # Native prepared submission lifetime: commit, reset, shape refusals,
    # pre-ioctl failure, teardown, and one single-reason calibration per
    # releaser or binding guard.
    "r3v-native-submit-lifetime-commit",
    "r3v-native-submit-lifetime-reset",
    "r3v-native-submit-lifetime-different-command-buffer",
    "r3v-native-submit-lifetime-submit-shape",
    "r3v-native-submit-lifetime-trace-refusal",
    "r3v-native-submit-lifetime-teardown",
    "r3v-native-submit-lifetime-known-bad-reset-release",
    "r3v-native-submit-lifetime-known-bad-command-buffer-binding",
    "r3v-native-submit-lifetime-known-bad-teardown-release",
    # Vertex front ends: the native ICD's direct SPIR-V admission and
    # its parity with the NIR front end over the one job IR.
    "r3v-native-pipeline-frontend",
    "r300-vertex-front-end-parity",
    "r300-vertex-front-end-parity-known-bad-divergent-job",
    # R2VB typed-route oracle over the SPIR-V-derived vertex representation,
    # owned by the r300g planner lane.
    "r300-r2vb-typed-route-oracle",
    "r300-r2vb-producer-census",
    "r300-r2vb-producer-census-known-bad-missing-corpus-member",
    "r300-r2vb-producer-census-known-bad-extra-corpus-member",
    "r300-typed-carry-reference",
    "r300-typed-carry-reference-selftest",
    # Compute surface: the direct SPIR-V admission, the CPU executor,
    # and the end-to-end gate-off/gate-on dispatch under the shim.
    "r300-cpu-compute-job",
    "r3v-native-compute-frontend",
    "r3v-native-compute-dispatch",
    "r3v-native-submit-object-replay",
    "r3v-native-triangle-cell-closed",
    "r3v-native-triangle-cell-open",
    "r3v-native-triangle-cell-unattested",
    # Direct-write control: emitter, manifest, and native binding.
    "r300-cpu-vertex",
    "r3v-native-vertex-carrier",
    "r3v-native-public-surface",
    "r300-direct-write",
    "r300-direct-write-manifest-integration",
    "r300-direct-write-cs-track-replay",
    "r3v-native-direct-write-closed",
    "r3v-native-direct-write-open",
    "r3v-native-direct-write-submit-object-replay",
    "r3v-native-direct-write-arming-runner",
    "r3v-native-direct-write-arming-positive",
    "r3v-native-direct-write-authority-parity",
    "r3v-native-public-gpu-producer-arming-runner",
    "r3v-native-public-gpu-producer-record",
    "r3v-native-route-timing-digest",
    "r3v-native-route-timing-record-cpu",
    "r3v-native-route-timing-record-gpu",
    "r3v-native-gallium-separation",
    "r3v-native-gallium-separation-selftest",
    "r3v-native-gallium-separation-known-bad",
    "r3v-native-common-boundary-sources",
    "r3v-native-common-boundary-objects",
    "r3v-native-common-boundary-final-dso",
    "r3v-native-lifecycle-pair-complete",
    "r3v-native-lifecycle-pair-missing-create",
    "r3v-native-lifecycle-pair-missing-destroy",
    "r3v-xvfb-wrapper-signal-cleanup",
    "r3v-native-wsi-surface-contract",
    "r3v-native-wsi-surface-contract-known-bad-capabilities",
    "r3v-native-wsi-surface-contract-known-bad-capabilities-error",
    "r3v-native-wsi-surface-contract-known-bad-formats-full",
    "r3v-native-wsi-surface-contract-known-bad-formats-short",
    "r3v-native-wsi-surface-contract-known-bad-formats-error",
    "r3v-native-wsi-surface-contract-known-bad-modes-full",
    "r3v-native-wsi-surface-contract-known-bad-modes-short",
    "r3v-native-wsi-surface-contract-known-bad-modes-error",
    "r3v-source-header-audit",
    "r3v-source-header-audit-selftest",
    "r3v-source-header-audit-known-bad-missing-spdx",
    "r3v-source-header-audit-known-bad-invented-copyright",
    "r3v-source-header-audit-known-bad-invented-project-collective-copyright",
    "r3v-source-header-audit-known-bad-ai-disclosure",
    # Radeon DRM transport and shim admission.
    "radeon-drm-vk-bo",
    "radeon-drm-vk-cs",
    "radeon-drm-vk-reloc",
    "radeon-noop-drm-shim-default",
)

# Suites whose entries the zero-native fixture removes: the historic build
# lost exactly the native backend's registrations.
NATIVE_SUITES = ("r3v", "radeon-drm-vk", "drm-shim")


def load_tests(builddir: Path) -> list[dict]:
    path = builddir / "meson-info" / "intro-tests.json"
    with open(path) as f:
        return json.load(f)


def load_options(builddir: Path) -> dict[str, object]:
    path = builddir / "meson-info" / "intro-buildoptions.json"
    with open(path) as f:
        return {o["name"]: o["value"] for o in json.load(f)}


def suite_names(entry: dict) -> list[str]:
    # Meson prefixes suites with the project name ("mesa:r3v").
    return [s.split(":", 1)[-1] for s in entry.get("suite", [])]


def required_test_suite(name: str) -> str:
    if name.startswith("r300-"):
        return "r300"
    if name.startswith("r3v-"):
        return "r3v"
    if name.startswith("radeon-drm-vk"):
        return "radeon-drm-vk"
    if name.startswith("radeon-noop-drm-shim"):
        return "drm-shim"
    raise ValueError(f"required test has no declared home suite: {name}")


def required_tests(options: dict[str, object]) -> tuple[str, ...]:
    del options
    return REQUIRED_TESTS


def tool_row(name: str) -> tuple[str, bool]:
    return (name, shutil.which(name) is not None)


def read_runtime_authority(
    sysfs_path: Path = MODULE_SYSFS_SRCVERSION,
    run_command=subprocess.run,
) -> tuple[str, str] | None:
    try:
        running_srcversion = sysfs_path.read_text().strip()
    except OSError:
        return None
    if (not running_srcversion or
            any(char not in MODULE_SRCVERSION_CHARS
                for char in running_srcversion)):
        return None
    installed_values: dict[str, str] = {}
    for field in (MODULE_DRIVER_TREE_FIELD, "srcversion"):
        try:
            result = run_command(
                ["modinfo", "-F", field, MODULE_NAME],
                capture_output=True,
                text=True,
                check=False,
            )
        except OSError:
            return None
        if result.returncode != 0:
            return None
        installed_values[field] = result.stdout.strip()
    installed_driver_tree = installed_values[MODULE_DRIVER_TREE_FIELD]
    installed_srcversion = installed_values["srcversion"]
    if (len(installed_driver_tree) != 40 or
            any(char not in FULL_SHA256 for char in installed_driver_tree) or
            not installed_srcversion or
            any(char not in MODULE_SRCVERSION_CHARS
                for char in installed_srcversion) or
            installed_srcversion != running_srcversion):
        return None
    return installed_driver_tree, running_srcversion


def validate_replay_provenance(
    tool: str,
    record: str,
    runtime_authority: tuple[str, str] | None = None,
) -> tuple[str, bool]:
    """The replay tool qualifies only through its provenance record: the
    record parses, its correspondence gate passed rather than being skipped,
    the artifact identifies the in-tree builder and isolated snapshot, and
    its ELF SHA-256 matches the tool on disk, and the retained driver tree and
    module srcversion match the live deployed module."""
    if not record:
        return ("unset", False)
    tool_path = Path(tool)
    if not tool_path.is_file() or not os.access(tool_path, os.X_OK):
        return ("tool-not-executable", False)
    try:
        with open(record) as f:
            provenance = json.load(f)
    except (OSError, json.JSONDecodeError):
        return ("unreadable", False)
    if not isinstance(provenance, dict):
        return ("malformed", False)
    if provenance.get("correspondence_gate") != "pass":
        return ("correspondence-gate-not-passed", False)
    if provenance.get("schema") != REPLAY_PROVENANCE_SCHEMA:
        return ("schema-mismatch", False)
    if provenance.get("builder") != REPLAY_PROVENANCE_BUILDER:
        return ("builder-mismatch", False)
    if provenance.get("artifact") != REPLAY_ARTIFACT:
        return ("artifact-mismatch", False)
    if provenance.get("isolated_worktree") is not True:
        return ("non-isolated-build", False)

    kernel_commit = provenance.get("kernel_commit")
    if (not isinstance(kernel_commit, str) or len(kernel_commit) != 40 or
            any(char not in FULL_SHA256 for char in kernel_commit)):
        return ("unpinned-kernel-commit", False)
    if (provenance.get("kernel_tool_source_sha") != kernel_commit or
            provenance.get("kernel_driver_logic_sha") != kernel_commit):
        return ("kernel-authority-mismatch", False)
    source_driver_tree = provenance.get("kernel_driver_logic_tree")
    deployed_driver_tree = provenance.get("deployed_driver_tree")
    running_srcversion = provenance.get("running_module_srcversion")
    if (not isinstance(source_driver_tree, str) or
            len(source_driver_tree) != 40 or
            any(char not in FULL_SHA256 for char in source_driver_tree) or
            not isinstance(deployed_driver_tree, str) or
            len(deployed_driver_tree) != 40 or
            any(char not in FULL_SHA256 for char in deployed_driver_tree)):
        return ("driver-authority-missing", False)
    if deployed_driver_tree != source_driver_tree:
        return ("driver-authority-mismatch", False)
    if (not isinstance(running_srcversion, str) or not running_srcversion or
            any(char not in MODULE_SRCVERSION_CHARS
                for char in running_srcversion)):
        return ("module-srcversion-missing", False)
    if provenance.get("deployed_driver_authority_verified") is not True:
        return ("driver-authority-unverified", False)
    if runtime_authority is None:
        return ("runtime-authority-unavailable", False)
    runtime_driver_tree, runtime_srcversion = runtime_authority
    if deployed_driver_tree != runtime_driver_tree:
        return ("deployed-driver-tree-runtime-mismatch", False)
    if running_srcversion != runtime_srcversion:
        return ("module-srcversion-runtime-mismatch", False)
    if (not isinstance(provenance.get("sources"), dict) or
            not provenance["sources"]):
        return ("source-provenance-missing", False)
    if not isinstance(provenance.get("compile_argv"), list):
        return ("compile-provenance-missing", False)
    if not isinstance(provenance.get("compiler"), str):
        return ("compiler-provenance-missing", False)

    recorded_output = provenance.get("output")
    if not isinstance(recorded_output, str):
        return ("output-provenance-missing", False)
    if Path(recorded_output).resolve() != tool_path.resolve():
        return ("output-path-mismatch", False)

    expected_digest = provenance.get("output_sha256")
    if (not isinstance(expected_digest, str) or len(expected_digest) != 64 or
            any(char not in FULL_SHA256 for char in expected_digest)):
        return ("output-digest-malformed", False)
    try:
        digest = hashlib.sha256(tool_path.read_bytes()).hexdigest()
    except OSError:
        return ("tool-unreadable", False)
    if digest != expected_digest:
        return ("digest-mismatch", False)
    return ("verified", True)


def replay_provenance_state() -> tuple[str, bool]:
    runtime_authority = read_runtime_authority()
    return validate_replay_provenance(
        os.environ.get("R3V_CS_TRACK_REPLAY_TOOL", ""),
        os.environ.get("R3V_CS_TRACK_REPLAY_PROVENANCE", ""),
        runtime_authority,
    )


def replay_provenance_selftest() -> int:
    """Exercise one valid record and independent pinned-artifact refusals."""
    with tempfile.TemporaryDirectory(prefix="r3v-replay-provenance-") as tmp:
        root = Path(tmp)
        tool = root / REPLAY_ARTIFACT
        tool.write_bytes(b"qualified replay artifact")
        tool.chmod(0o755)
        digest = hashlib.sha256(tool.read_bytes()).hexdigest()
        kernel_commit = "a" * 40
        record = {
            "schema": REPLAY_PROVENANCE_SCHEMA,
            "builder": REPLAY_PROVENANCE_BUILDER,
            "artifact": REPLAY_ARTIFACT,
            "kernel_commit": kernel_commit,
            "kernel_tool_source_sha": kernel_commit,
            "kernel_driver_logic_sha": kernel_commit,
            "kernel_driver_logic_tree": "c" * 40,
            "deployed_driver_tree": "c" * 40,
            "running_module_srcversion": "FIXTURESRCVERSION0000000",
            "deployed_driver_authority_verified": True,
            "sources": {"scripts/replay_r300_cs_track.c": {
                "sha256": "b" * 64,
            }},
            "compiler": "cc",
            "compile_argv": ["cc", "-Werror"],
            "output": str(tool),
            "output_sha256": digest,
            "correspondence_gate": "pass",
            "isolated_worktree": True,
        }
        record_path = root / "provenance.json"

        def check(expected: str, candidate: dict) -> bool:
            record_path.write_text(json.dumps(candidate))
            state, ok = validate_replay_provenance(
                str(tool), str(record_path),
                ("c" * 40, "FIXTURESRCVERSION0000000"),
            )
            if ok or state != expected:
                print(f"replay provenance selftest expected {expected}, "
                      f"got {state}", file=sys.stderr)
                return False
            return True

        record_path.write_text(json.dumps(record))
        state, ok = validate_replay_provenance(
            str(tool), str(record_path),
            ("c" * 40, "FIXTURESRCVERSION0000000"),
        )
        if state != "verified" or not ok:
            print(f"replay provenance selftest valid record: {state}",
                  file=sys.stderr)
            return 1
        mutations = [
            ("correspondence-gate-not-passed", {"correspondence_gate":
                                                "skipped"}),
            ("non-isolated-build", {"isolated_worktree": False}),
            ("builder-mismatch", {"builder": "operator-supplied"}),
            ("unpinned-kernel-commit", {"kernel_commit": "deadbeef"}),
            ("driver-authority-missing", {"deployed_driver_tree": None}),
            ("driver-authority-mismatch", {"deployed_driver_tree": "d" * 40}),
            ("module-srcversion-missing", {"running_module_srcversion": ""}),
            ("driver-authority-unverified", {
                "deployed_driver_authority_verified": False}),
            ("digest-mismatch", {"output_sha256": "0" * 64}),
        ]
        for expected, mutation in mutations:
            candidate = dict(record)
            candidate.update(mutation)
            if not check(expected, candidate):
                return 1
        candidate = dict(record)
        candidate["kernel_driver_logic_tree"] = "d" * 40
        candidate["deployed_driver_tree"] = "d" * 40
        record_path.write_text(json.dumps(candidate))
        state, ok = validate_replay_provenance(
            str(tool), str(record_path),
            ("c" * 40, "FIXTURESRCVERSION0000000"),
        )
        if ok or state != "deployed-driver-tree-runtime-mismatch":
            print("replay provenance selftest expected runtime driver-tree "
                  f"refusal, got {state}", file=sys.stderr)
            return 1
        candidate = dict(record)
        candidate["running_module_srcversion"] = "OTHER"
        record_path.write_text(json.dumps(candidate))
        state, ok = validate_replay_provenance(
            str(tool), str(record_path),
            ("c" * 40, "FIXTURESRCVERSION0000000"),
        )
        if ok or state != "module-srcversion-runtime-mismatch":
            print("replay provenance selftest expected runtime srcversion "
                  f"refusal, got {state}", file=sys.stderr)
            return 1

    with tempfile.TemporaryDirectory(prefix="r3v-runtime-authority-") as tmp:
        sysfs_path = Path(tmp) / "srcversion"
        sysfs_path.write_text("FIXTURESRCVERSION0000000\n")
        driver_tree = "d" * 40

        def good_modinfo(command, **kwargs):
            field = command[2]
            value = (driver_tree if field == MODULE_DRIVER_TREE_FIELD else
                     "FIXTURESRCVERSION0000000")
            return subprocess.CompletedProcess(
                command, 0, stdout=value + "\n", stderr="")

        authority = read_runtime_authority(sysfs_path, good_modinfo)
        if authority != (driver_tree, "FIXTURESRCVERSION0000000"):
            print("runtime authority selftest expected valid tuple, "
                  f"got {authority}", file=sys.stderr)
            return 1

        def missing_modinfo(command, **kwargs):
            return subprocess.CompletedProcess(command, 1, stdout="", stderr="missing")

        if read_runtime_authority(sysfs_path, missing_modinfo) is not None:
            print("runtime authority selftest accepted modinfo failure",
                  file=sys.stderr)
            return 1

        def mismatched_modinfo(command, **kwargs):
            field = command[2]
            value = (driver_tree if field == MODULE_DRIVER_TREE_FIELD else
                     "OTHER")
            return subprocess.CompletedProcess(
                command, 0, stdout=value + "\n", stderr="")

        if read_runtime_authority(sysfs_path, mismatched_modinfo) is not None:
            print("runtime authority selftest accepted srcversion mismatch",
                  file=sys.stderr)
            return 1
    print(f"replay provenance selftest: {len(mutations)} refusal legs and "
          "one verified leg and runtime-authority legs OK")
    return 0


def env_tool_row(var: str) -> tuple[str, str, bool]:
    """A replay or controls program named by environment: the row reports
    unset, set-but-not-executable, and executable distinctly, because the
    wrapper treats the first as SKIP and qualification treats all but the
    last as failure."""
    value = os.environ.get(var, "")
    if not value:
        return (var, "unset", False)
    if os.access(value, os.X_OK) and Path(value).is_file():
        return (var, "executable", True)
    return (var, "not-executable", False)


class HostProbes:
    """The host-facing lookups evaluate() consumes.  The default instance
    reads the real machine; the selftest substitutes deterministic results
    so both verdict directions travel the same evaluation and the fixture's
    failure is attributable to its test-list mutation alone."""

    def tool_present(self, name: str) -> bool:
        return shutil.which(name) is not None

    def loader(self) -> str | None:
        return ctypes.util.find_library("vulkan")

    def libc(self) -> str:
        return "glibc" if "glibc" in (os.confstr("CS_GNU_LIBC_VERSION")
                                      or "") else "other"

    def env_tool(self, var: str) -> tuple[str, bool]:
        return env_tool_row(var)[1:]

    def replay_provenance(self) -> tuple[str, bool]:
        return replay_provenance_state()


class GoodProbes(HostProbes):
    def tool_present(self, name: str) -> bool:
        return True

    def loader(self) -> str | None:
        return "libvulkan.so.1"

    def libc(self) -> str:
        return "glibc"

    def env_tool(self, var: str) -> tuple[str, bool]:
        return ("executable", True)

    def replay_provenance(self) -> tuple[str, bool]:
        return ("verified", True)


class MissingB3sumProbes(GoodProbes):
    """Known-bad host calibration for the required digest utility."""

    def tool_present(self, name: str) -> bool:
        return name != "b3sum"


class MissingKernelReplayProbes(GoodProbes):
    def env_tool(self, var: str) -> tuple[str, bool]:
        if var == "R3V_KERNEL_REPLAY_TOOL":
            return ("unset", False)
        return super().env_tool(var)


def evaluate(registered: set[str], options: dict[str, object],
             qualification: bool,
             probes: HostProbes | None = None,
             require_tests: bool = False) -> int:
    """Report the inventory; in qualification mode, any absence is fatal.
    Every row prints before the verdict so a failing run names each missing
    item rather than the first.

    require_tests decides on the registered test set alone, which is the
    claim a build can make about itself: a package boundary can prove the
    suite it ships and cannot supply the replay tooling and deployment
    identity a qualification run reads from the host."""
    if probes is None:
        probes = HostProbes()
    failures: list[str] = []
    registration_failures: list[str] = []

    native = options.get("r3v-native-backend")
    native_on = str(native) in ("enabled", "auto", "True", "true")
    print(f"option r3v-native-backend: {native}")
    if not native_on:
        failures.append("r3v-native-backend not enabled")

    build_tests = options.get("build-tests")
    print(f"option build-tests: {build_tests}")
    if str(build_tests) not in ("True", "true"):
        failures.append("build-tests disabled")

    gallium = options.get("gallium-drivers")
    print(f"option gallium-drivers: {gallium}")

    for name in ("nm", "Xvfb", "b3sum"):
        present = probes.tool_present(name)
        print(f"tool {name}: {'present' if present else 'absent'}")
        if not present:
            failures.append(f"tool {name} absent")

    loader = probes.loader()
    print(f"vulkan loader: {loader or 'absent'}")
    if loader is None:
        failures.append("vulkan loader absent")

    # The drm-shim preload rides LD_PRELOAD against glibc's loader.
    glibc = probes.libc()
    print(f"libc: {glibc}")
    if glibc != "glibc":
        failures.append("non-glibc host; drm-shim preload unavailable")

    for var in ("R3V_CS_TRACK_REPLAY_TOOL", "R3V_CS_TRACK_CONTROLS",
                "R3V_KERNEL_REPLAY_TOOL"):
        state, ok = probes.env_tool(var)
        print(f"env {var}: {state}")
        if not ok:
            failures.append(f"{var} {state}")

    state, ok = probes.replay_provenance()
    print(f"replay provenance: {state}")
    if not ok:
        failures.append(f"replay provenance {state}")

    by_suite: dict[str, int] = {}
    for suite in ("r300", "r3v", "radeon-drm-vk", "drm-shim"):
        count = sum(1 for n in registered_by_suite.get(suite, ()))
        by_suite[suite] = count
    for suite, count in by_suite.items():
        print(f"suite {suite}: {count} registered")

    expected_tests = required_tests(options)
    missing = [
        (name, required_test_suite(name))
        for name in expected_tests
        if name not in registered_by_suite.get(required_test_suite(name), ())
    ]
    print(f"required tests: {len(expected_tests) - len(missing)}"
          f"/{len(expected_tests)} registered")
    for name, suite in missing:
        registered_suites = sorted(
            registered_suite
            for registered_suite, names in registered_by_suite.items()
            if name in names
        )
        if name in registered:
            print(f"missing required test: {name} from suite {suite}; "
                  f"registered under {','.join(registered_suites)}")
        else:
            print(f"missing required test: {name} from suite {suite}")
        failures.append(f"required test {name} not registered in suite {suite}")
        registration_failures.append(name)

    for absence in ("r3v-native-backend not enabled", "build-tests disabled"):
        if absence in failures:
            registration_failures.append(absence)

    return verdict(qualification, require_tests, failures,
                   registration_failures)


def verdict(qualification: bool, require_tests: bool, failures: list[str],
            registration_failures: list[str]) -> int:
    """The mode's verdict over the absences the report already named.

    Each mode reads one list and prints one line: the qualification gate
    judges every absence, the registration gate judges the absences a
    build decides for itself, and the report mode judges none.  The two
    gates are mutually exclusive at the argument parser, so no call
    reaches here asking for both."""
    if qualification:
        if failures:
            print(f"verdict: QUALIFICATION FAILURE ({len(failures)} absences)",
                  file=sys.stderr)
            return 1
        print("verdict: qualification inventory complete")
        return 0
    if require_tests:
        if registration_failures:
            print("verdict: REGISTRATION FAILURE "
                  f"({len(registration_failures)} absences)", file=sys.stderr)
            return 1
        print("verdict: every required test is registered")
        return 0
    print("verdict: inventory (qualification gate not requested)")
    return 0


# evaluate() reads the per-suite registration counts through this module
# state so the fixture and the real run report through one code path.
registered_by_suite: dict[str, list[str]] = {}


def collect(entries: list[dict]) -> set[str]:
    registered_by_suite.clear()
    names: set[str] = set()
    for entry in entries:
        names.add(entry["name"])
        for suite in suite_names(entry):
            registered_by_suite.setdefault(suite, []).append(entry["name"])
    return names


def synthetic_complete(options: dict[str, object] | None = None) -> list[dict]:
    """A test list carrying every required name under its home suite, which
    calibrates the passing direction of the same evaluation."""
    entries = []
    for name in required_tests(options or {}):
        suite = required_test_suite(name)
        entries.append({"name": name, "suite": [f"mesa:{suite}"]})
    return entries


def zero_native(entries: list[dict]) -> list[dict]:
    return [e for e in entries
            if not any(s in NATIVE_SUITES for s in suite_names(e))]


QUALIFYING_OPTIONS = {"r3v-native-backend": "enabled", "build-tests": True,
                      "gallium-drivers": ["r300"]}

NATIVE_ONLY_OPTIONS = dict(QUALIFYING_OPTIONS)
NATIVE_ONLY_OPTIONS["gallium-drivers"] = []


def run_fixture() -> int:
    # Good probes isolate the mutation: the fixture's refusal comes from the
    # removed native registrations alone, not from this host's environment.
    entries = zero_native(synthetic_complete(QUALIFYING_OPTIONS))
    return evaluate(collect(entries), QUALIFYING_OPTIONS, qualification=True,
                    probes=GoodProbes())


def run_selftest() -> int:
    if replay_provenance_selftest() != 0:
        return 1
    probes = GoodProbes()
    complete_entries = synthetic_complete(QUALIFYING_OPTIONS)
    bad = evaluate(collect(zero_native(complete_entries)),
                   QUALIFYING_OPTIONS, qualification=True, probes=probes)
    if bad == 0:
        print("selftest: zero-native fixture passed the gate", file=sys.stderr)
        return 1
    missing_kernel_replay = evaluate(
        collect(complete_entries), QUALIFYING_OPTIONS, qualification=True,
        probes=MissingKernelReplayProbes())
    if missing_kernel_replay == 0:
        print("selftest: missing kernel replay passed the gate",
              file=sys.stderr)
        return 1
    good = evaluate(collect(complete_entries), QUALIFYING_OPTIONS,
                    qualification=True, probes=probes)
    if good != 0:
        print("selftest: complete set failed the gate", file=sys.stderr)
        return 1
    for missing_test in FLOAT2_TUPLE_REQUIRED_TESTS:
        missing_tuple_test = [
            entry for entry in complete_entries
            if entry["name"] != missing_test
        ]
        missing_tuple = evaluate(
            collect(missing_tuple_test), QUALIFYING_OPTIONS,
            qualification=True, probes=probes)
        if missing_tuple == 0:
            print("selftest: missing FLOAT_2 tuple test passed the gate: " +
                  missing_test, file=sys.stderr)
            return 1
        home_suite = required_test_suite(missing_test)
        wrong_suite = "r3v" if home_suite == "r300" else "r300"
        misplaced_tuple_test = [
            ({**entry, "suite": [f"mesa:{wrong_suite}"]}
             if entry["name"] == missing_test else entry)
            for entry in complete_entries
        ]
        misplaced_tuple = evaluate(
            collect(misplaced_tuple_test), QUALIFYING_OPTIONS,
            qualification=True, probes=probes)
        if misplaced_tuple == 0:
            print("selftest: misplaced FLOAT_2 tuple test passed the gate: " +
                  missing_test, file=sys.stderr)
            return 1
    native_only = evaluate(collect(synthetic_complete(NATIVE_ONLY_OPTIONS)),
                           NATIVE_ONLY_OPTIONS, qualification=True,
                           probes=probes)
    if native_only != 0:
        print("selftest: native-only set failed the gate", file=sys.stderr)
        return 1
    missing_b3sum = evaluate(collect(complete_entries),
                             QUALIFYING_OPTIONS, qualification=True,
                             probes=MissingB3sumProbes())
    if missing_b3sum == 0:
        print("selftest: missing b3sum passed the gate", file=sys.stderr)
        return 1
    # The require-tests mode decides on registration alone, so it refuses
    # a build missing a required test while a host that supplies neither
    # the replay tooling nor a module identity still passes: those
    # absences are the qualification gate's to judge, not the package
    # boundary's.
    require_bad = evaluate(collect(zero_native(complete_entries)),
                           QUALIFYING_OPTIONS, qualification=False,
                           probes=probes, require_tests=True)
    if require_bad == 0:
        print("selftest: zero-native fixture passed the require-tests gate",
              file=sys.stderr)
        return 1
    require_good = evaluate(collect(complete_entries), QUALIFYING_OPTIONS,
                            qualification=False,
                            probes=MissingKernelReplayProbes(),
                            require_tests=True)
    if require_good != 0:
        print("selftest: the complete set failed the require-tests gate over "
              "a host absence", file=sys.stderr)
        return 1
    print("selftest: gate refuses zero-native, each missing or misplaced "
          "FLOAT_2 tuple test, missing kernel replay, missing Gallium "
          "known-bad, and missing b3sum; admits dual-backend and native-only "
          "sets; require-tests refuses zero-native and admits a complete set "
          "under a host absence")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("builddir", nargs="?", type=Path)
    # The two gates judge different absence sets, so asking for both
    # names no verdict; the parser refuses rather than letting one win
    # silently.
    gate = parser.add_mutually_exclusive_group()
    gate.add_argument("--qualification", action="store_true")
    gate.add_argument("--require-tests", action="store_true")
    parser.add_argument("--fixture", choices=["zero-native"])
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    if args.selftest:
        return run_selftest()
    if args.fixture:
        return run_fixture()
    if args.builddir is None:
        parser.error("a build directory, --fixture, or --selftest is required")
    entries = load_tests(args.builddir)
    options = load_options(args.builddir)
    return evaluate(collect(entries), options, args.qualification,
                    require_tests=args.require_tests)


if __name__ == "__main__":
    sys.exit(main())
