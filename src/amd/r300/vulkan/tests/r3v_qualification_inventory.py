# SPDX-License-Identifier: MIT
"""Qualification test-inventory gate for the R3V lane.

A qualification run proves the suite it names, so a build that silently
registered fewer tests must fail the run rather than return a smaller green
suite.  This tool reads Meson's introspection records from a build directory,
reports every dependency the qualification profile relies on, and in
qualification mode exits nonzero when any required test or dependency is
absent.

A build once registered zero native tests because the native backend and the
drm-shim tool were both missing, and the suite reported green; that build is
the permanent known-bad calibration, reproduced by the zero-native fixture.

Modes:
  <builddir>                  inventory report, exit 0 when readable
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
import sys
from pathlib import Path

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
    "r300-r2vb-carrier-delivery",
    "r300-r2vb-producer-pass",
    "r300-r2vb-producer-replay",
    "r300-tcl-bypass-fs-block-regeneration",
    "r300-tcl-bypass-offline-replay",
    "r300-cs-track-replay",
    "r300-staging-manifest",
    # r3v native: arming, dispatch, recording, submit, lifecycle, WSI.
    "r3v-native-arming",
    "r3v-native-arming-positive",
    "r3v-native-arming-runner-refuses-undeclared",
    "r3v-native-entrypoint-audit-selftest",
    "r3v-native-entrypoint-closure",
    "r3v-native-entrypoint-closure-known-bad-BindBufferMemory2",
    "r3v-native-entrypoint-closure-known-bad-CreateImage",
    "r3v-native-public-surface-policy",
    "r3v-native-direct-table-sweep",
    "r3v-native-direct-table-sweep-known-bad-closed-extension",
    "r3v-native-direct-table-sweep-known-bad-higher-core",
    "r3v-native-direct-table-sweep-known-bad-promoted-alias",
    "r3v-native-loader-sweep",
    "r3v-native-loader-application",
    "r3v-native-loader-application-known-bad-ib",
    "r3v-native-loader-application-known-bad-corrupt_footprint",
    "r3v-native-loader-application-known-bad-corrupt_tail",
    "r3v-native-loader-application-symbols",
    "r3v-native-loader-application-symbols-known-bad",
    "r3v-native-recording-poison",
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
    "r3v-native-gallium-separation",
    "r3v-native-gallium-separation-known-bad",
    "r3v-native-lifecycle-pair-complete",
    "r3v-native-lifecycle-pair-missing-create",
    "r3v-native-lifecycle-pair-missing-destroy",
    "r3v-native-wsi-surface-contract",
    "r3v-native-wsi-surface-contract-known-bad-capabilities",
    "r3v-native-wsi-surface-contract-known-bad-formats",
    "r3v-native-wsi-surface-contract-known-bad-modes",
    "r3v-source-header-audit",
    "r3v-source-header-audit-selftest",
    "r3v-source-header-audit-known-bad-missing-spdx",
    "r3v-source-header-audit-known-bad-invented-copyright",
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


def tool_row(name: str) -> tuple[str, bool]:
    return (name, shutil.which(name) is not None)


def replay_provenance_state() -> tuple[str, bool]:
    """The replay tool qualifies only through its provenance record: the
    record parses, its correspondence gate passed rather than being skipped,
    and its ELF SHA-256 matches the tool on disk, which binds the running
    replay to the pinned kernel tree build_kernel_replay.py proved."""
    tool = os.environ.get("R3V_CS_TRACK_REPLAY_TOOL", "")
    record = os.environ.get("R3V_CS_TRACK_REPLAY_PROVENANCE", "")
    if not record:
        return ("unset", False)
    try:
        with open(record) as f:
            provenance = json.load(f)
    except (OSError, json.JSONDecodeError):
        return ("unreadable", False)
    if provenance.get("correspondence_gate") != "pass":
        return ("correspondence-gate-not-passed", False)
    try:
        digest = hashlib.sha256(Path(tool).read_bytes()).hexdigest()
    except OSError:
        return ("tool-unreadable", False)
    if digest != provenance.get("output_sha256"):
        return ("digest-mismatch", False)
    return ("verified", True)


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


def evaluate(registered: set[str], options: dict[str, object],
             qualification: bool,
             probes: HostProbes | None = None) -> int:
    """Report the inventory; in qualification mode, any absence is fatal.
    Every row prints before the verdict so a failing run names each missing
    item rather than the first."""
    if probes is None:
        probes = HostProbes()
    failures: list[str] = []

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

    for name in ("nm", "Xvfb"):
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

    for var in ("R3V_CS_TRACK_REPLAY_TOOL", "R3V_CS_TRACK_CONTROLS"):
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

    missing = [name for name in REQUIRED_TESTS if name not in registered]
    print(f"required tests: {len(REQUIRED_TESTS) - len(missing)}"
          f"/{len(REQUIRED_TESTS)} registered")
    for name in missing:
        print(f"missing required test: {name}")
        failures.append(f"required test {name} not registered")

    if not qualification:
        print("verdict: inventory (qualification gate not requested)")
        return 0
    if failures:
        print(f"verdict: QUALIFICATION FAILURE ({len(failures)} absences)",
              file=sys.stderr)
        return 1
    print("verdict: qualification inventory complete")
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


def synthetic_complete() -> list[dict]:
    """A test list carrying every required name under its home suite, which
    calibrates the passing direction of the same evaluation."""
    entries = []
    for name in REQUIRED_TESTS:
        if name.startswith("r300-"):
            suite = "r300"
        elif name.startswith("r3v-"):
            suite = "r3v"
        elif name.startswith("radeon-drm-vk"):
            suite = "radeon-drm-vk"
        else:
            suite = "drm-shim"
        entries.append({"name": name, "suite": [f"mesa:{suite}"]})
    return entries


def zero_native(entries: list[dict]) -> list[dict]:
    return [e for e in entries
            if not any(s in NATIVE_SUITES for s in suite_names(e))]


QUALIFYING_OPTIONS = {"r3v-native-backend": "enabled", "build-tests": True,
                      "gallium-drivers": ["r300"]}


def run_fixture() -> int:
    # Good probes isolate the mutation: the fixture's refusal comes from the
    # removed native registrations alone, not from this host's environment.
    entries = zero_native(synthetic_complete())
    return evaluate(collect(entries), QUALIFYING_OPTIONS, qualification=True,
                    probes=GoodProbes())


def run_selftest() -> int:
    probes = GoodProbes()
    bad = evaluate(collect(zero_native(synthetic_complete())),
                   QUALIFYING_OPTIONS, qualification=True, probes=probes)
    if bad == 0:
        print("selftest: zero-native fixture passed the gate", file=sys.stderr)
        return 1
    good = evaluate(collect(synthetic_complete()), QUALIFYING_OPTIONS,
                    qualification=True, probes=probes)
    if good != 0:
        print("selftest: complete set failed the gate", file=sys.stderr)
        return 1
    print("selftest: gate refuses zero-native and admits the complete set")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("builddir", nargs="?", type=Path)
    parser.add_argument("--qualification", action="store_true")
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
    return evaluate(collect(entries), options, args.qualification)


if __name__ == "__main__":
    sys.exit(main())
