#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Calibrate mutation-target rejection in a private mount namespace."""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import NoReturn

UID_MAP_FIELD_COUNT = 3
MOUNT_NAMESPACE_PATTERN = re.compile(r"^mnt:\[[1-9][0-9]*\]$")


@dataclass(frozen=True)
class MountRejectionCase:
    name: str
    values: dict[str, Path | str]
    mount_point: Path
    parent_directories: tuple[Path, ...]
    operation: str
    expected_label: str


@dataclass(frozen=True)
class FixturePath:
    field: str
    descendants: tuple[str, ...] = ()


@dataclass(frozen=True)
class MountRejectionSpec:
    name: str
    build_root_name: str
    builddir_name: str
    mount_path: FixturePath
    parent_paths: tuple[FixturePath, ...]
    operation: str
    expected_label: str


@dataclass(frozen=True)
class CalibrationContext:
    module: ModuleType
    control_root: Path
    victim: Path
    mount_executable: Path
    mounted_paths: list[Path]


MOUNT_REJECTION_SPECS = (
    MountRejectionSpec(
        name="clean descendant BUILDDIR",
        build_root_name="clean-descendant-build",
        builddir_name="descendant-probe",
        mount_path=FixturePath("builddir", ("mounted-victim",)),
        parent_paths=(FixturePath("build_root"), FixturePath("builddir")),
        operation="clean",
        expected_label="BUILDDIR",
    ),
    MountRejectionSpec(
        name="clean exact BUILDDIR",
        build_root_name="clean-exact-build",
        builddir_name="exact-probe",
        mount_path=FixturePath("builddir"),
        parent_paths=(FixturePath("build_root"),),
        operation="clean",
        expected_label="BUILDDIR",
    ),
    MountRejectionSpec(
        name="install descendant BUILDDIR",
        build_root_name="install-builddir-build",
        builddir_name="probe",
        mount_path=FixturePath(
            "builddir",
            ("meson-logs", "mounted-victim"),
        ),
        parent_paths=(
            FixturePath("build_root"),
            FixturePath("builddir"),
            FixturePath("builddir", ("meson-logs",)),
        ),
        operation="install",
        expected_label="BUILDDIR",
    ),
    MountRejectionSpec(
        name="install descendant PREFIX",
        build_root_name="install-prefix-build",
        builddir_name="probe",
        mount_path=FixturePath("prefix", ("mounted-victim",)),
        parent_paths=(FixturePath("build_root"), FixturePath("prefix")),
        operation="install",
        expected_label="PREFIX",
    ),
    MountRejectionSpec(
        name="clean ancestor BUILDDIR",
        build_root_name="clean-ancestor-build",
        builddir_name="probe",
        mount_path=FixturePath("build_root"),
        parent_paths=(),
        operation="clean",
        expected_label="BUILDDIR",
    ),
    MountRejectionSpec(
        name="artifact ancestor PREFIX",
        build_root_name="artifact-ancestor-build",
        builddir_name="probe",
        mount_path=FixturePath("build_root"),
        parent_paths=(),
        operation="artifact",
        expected_label="PREFIX",
    ),
)


def fail(message: str) -> NoReturn:
    raise SystemExit(f"mount-boundary-calibration: {message}")


def validate_namespace_isolation(
    uid_fields: list[str],
    process_mount_namespace: str,
    caller_mount_namespace: str,
) -> None:
    """Require a caller-mapped root user and a distinct mount namespace."""
    if (
        len(uid_fields) < UID_MAP_FIELD_COUNT
        or uid_fields[0] != "0"
        or uid_fields[1] == "0"
    ):
        fail("calibration is outside a private user namespace")
    if not MOUNT_NAMESPACE_PATTERN.fullmatch(caller_mount_namespace):
        fail("caller mount namespace identity is invalid")
    if not MOUNT_NAMESPACE_PATTERN.fullmatch(process_mount_namespace):
        fail("calibration mount namespace identity is invalid")
    if process_mount_namespace == caller_mount_namespace:
        fail("calibration is outside a private mount namespace")


def require_private_user_mount_namespace() -> None:
    if os.environ.get("MESA_MOUNT_BOUNDARY_CALIBRATION") != "1":
        fail("exact calibration consent is missing")
    if os.getuid() != 0:
        fail("calibration user namespace does not map the caller to uid 0")
    caller_mount_namespace = os.environ.get("MESA_CALLER_MOUNT_NAMESPACE", "")
    try:
        uid_fields = Path("/proc/self/uid_map").read_text(encoding="utf-8").split()
        process_mount_namespace = os.readlink("/proc/self/ns/mnt")
    except OSError as error:
        fail(f"cannot verify namespace isolation: {error}")
    validate_namespace_isolation(
        uid_fields,
        process_mount_namespace,
        caller_mount_namespace,
    )


def load_source_root_control(control_root: Path) -> ModuleType:
    script = control_root / "build-infra" / "scripts" / "source_root_control.py"
    module_spec = importlib.util.spec_from_file_location(
        "mount_boundary_source_root_control",
        script,
    )
    if module_spec is None or module_spec.loader is None:
        fail(f"cannot load source-root control: {script}")
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    return module


def layout_values(
    control_root: Path,
    build_namespace: Path,
    build_root_name: str,
    builddir_name: str,
) -> dict[str, Path | str]:
    build_root = build_namespace / build_root_name
    return {
        "source_root": control_root,
        "source_commit": "1" * 40,
        "source_tree": "2" * 40,
        "control_root": control_root,
        "control_commit": "3" * 40,
        "control_tree": "4" * 40,
        "build_root": build_root,
        "builddir": build_root / builddir_name,
        "prefix": build_root / "prefix",
        "sysconfdir": Path("/etc"),
    }


def require_path(values: dict[str, Path | str], field: str) -> Path:
    selected_path = values[field]
    if not isinstance(selected_path, Path):
        fail(f"{field} is not a path")
    return selected_path


def fixture_path(
    values: dict[str, Path | str],
    path_spec: FixturePath,
) -> Path:
    return require_path(values, path_spec.field).joinpath(*path_spec.descendants)


def create_fixture_directory(path: Path) -> None:
    if not path.parent.is_dir():
        fail(f"mount fixture parent does not exist: {path.parent}")
    try:
        path.mkdir()
    except OSError as error:
        fail(f"cannot create mount fixture directory {path}: {error}")


def require_existing_directory(path: Path, label: str) -> None:
    try:
        is_directory = path.is_dir()
    except OSError as error:
        fail(f"cannot inspect {label} {path}: {error}")
    if not is_directory:
        fail(f"{label} must be a pre-existing directory: {path}")


def require_layout_acceptance(
    module: ModuleType,
    operation: str,
    values: dict[str, Path | str],
    case_name: str,
) -> None:
    try:
        module.validate_layout(operation, values)
    except module.ControlError as error:
        fail(f"{case_name} known-good layout was rejected: {error}")


def resolve_mount_executable(name: str) -> Path:
    executable = shutil.which(name)
    if executable is None:
        fail(f"required mount executable is unavailable: {name}")
    try:
        return Path(executable).resolve(strict=True)
    except OSError as error:
        fail(f"cannot resolve mount executable {name}: {error}")


def bind_mount(
    mount_executable: Path,
    victim: Path,
    mount_point: Path,
    mounted_paths: list[Path],
) -> None:
    require_existing_directory(mount_point, "mount target")
    try:
        result = subprocess.run(
            [str(mount_executable), "--bind", str(victim), str(mount_point)],
            check=False,
            capture_output=True,
            text=True,
            shell=False,
        )
    except OSError as error:
        fail(f"cannot execute bind mount for {mount_point}: {error}")
    if result.returncode != 0:
        diagnostic = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        fail(f"bind mount failed for {mount_point}: {diagnostic}")
    mounted_paths.append(mount_point)


def require_mount_rejection(
    context: CalibrationContext,
    case: MountRejectionCase,
    *,
    create_target: bool = True,
) -> None:
    if create_target:
        create_fixture_directory(case.mount_point)
    else:
        require_existing_directory(case.mount_point, "mount target")
    require_layout_acceptance(
        context.module,
        case.operation,
        case.values,
        case.name,
    )
    bind_mount(
        context.mount_executable,
        context.victim,
        case.mount_point,
        context.mounted_paths,
    )
    try:
        same_device = case.mount_point.stat().st_dev == context.victim.stat().st_dev
    except OSError as error:
        fail(f"cannot inspect bind mount {case.mount_point}: {error}")
    if not same_device:
        fail("bind-mount calibration did not retain one device number")
    try:
        context.module.validate_layout(case.operation, case.values)
    except context.module.ControlError as error:
        expected_diagnostic = f"refusing {case.expected_label} crossing a mount point"
        if expected_diagnostic not in str(error):
            fail(f"bind mount returned the wrong diagnostic: {error}")
    else:
        fail(f"{case.operation} accepted a mount under {case.expected_label}")


def build_rejection_cases(
    control_root: Path,
    build_namespace: Path,
) -> tuple[MountRejectionCase, ...]:
    cases = []
    for spec in MOUNT_REJECTION_SPECS:
        values = layout_values(
            control_root,
            build_namespace,
            spec.build_root_name,
            spec.builddir_name,
        )
        cases.append(
            MountRejectionCase(
                name=spec.name,
                values=values,
                mount_point=fixture_path(values, spec.mount_path),
                parent_directories=tuple(
                    fixture_path(values, parent_path)
                    for parent_path in spec.parent_paths
                ),
                operation=spec.operation,
                expected_label=spec.expected_label,
            )
        )
    return tuple(cases)


def run_rejection_cases(
    context: CalibrationContext,
    build_namespace: Path,
) -> None:
    for case in build_rejection_cases(context.control_root, build_namespace):
        for parent_directory in case.parent_directories:
            create_fixture_directory(parent_directory)
        require_mount_rejection(context, case)


def run_control_prefix_case(
    context: CalibrationContext,
) -> None:
    control_prefix_values: dict[str, Path | str] = {
        "source_root": context.control_root,
        "source_commit": "1" * 40,
        "source_tree": "2" * 40,
        "control_root": context.control_root,
        "control_commit": "3" * 40,
        "control_tree": "4" * 40,
        "build_root": context.control_root / "build",
        "builddir": context.control_root / "build" / "mount-boundary-probe",
        "prefix": Path("/opt/local/mesa-26-gororoba"),
        "sysconfdir": Path("/etc"),
    }
    control_prefix_mount_point = Path("/opt/local")
    require_existing_directory(
        control_prefix_mount_point,
        "private namespace control-prefix mount target",
    )
    case = MountRejectionCase(
        name="control-prefix ancestor",
        values=control_prefix_values,
        mount_point=control_prefix_mount_point,
        parent_directories=(),
        operation="artifact",
        expected_label="PREFIX",
    )
    require_mount_rejection(
        context,
        case,
        create_target=False,
    )


def run_trusted_boundary_case(
    context: CalibrationContext,
    audit_root: Path,
) -> None:
    trusted_boundary = audit_root / "trusted-boundary"
    trusted_victim = audit_root / "trusted-victim"
    create_fixture_directory(trusted_victim)
    trusted_values = layout_values(
        context.control_root,
        trusted_boundary,
        "boundary-mount-build",
        "probe",
    )
    previous_owned_build_namespaces = getattr(  # noqa: B009
        context.module,
        "owned_build_namespaces",
    )
    setattr(  # noqa: B010
        context.module,
        "owned_build_namespaces",
        lambda _repository_root: (trusted_boundary,),
    )
    try:
        require_layout_acceptance(
            context.module,
            "build",
            trusted_values,
            "trusted boundary before bind mount",
        )
        create_fixture_directory(trusted_boundary)
        require_layout_acceptance(
            context.module,
            "build",
            trusted_values,
            "trusted boundary directory before bind mount",
        )
        bind_mount(
            context.mount_executable,
            trusted_victim,
            trusted_boundary,
            context.mounted_paths,
        )
        require_layout_acceptance(
            context.module,
            "build",
            trusted_values,
            "trusted boundary after bind mount",
        )
    finally:
        setattr(  # noqa: B010
            context.module,
            "owned_build_namespaces",
            previous_owned_build_namespaces,
        )


def run_calibration(
    module: ModuleType,
    control_root: Path,
    audit_root: Path,
    mount_executable: Path,
    mounted_paths: list[Path],
) -> Path:
    build_namespace = audit_root / "namespace"
    victim = audit_root / "victim"
    create_fixture_directory(build_namespace)
    create_fixture_directory(victim)
    sentinel = victim / "sentinel"
    try:
        sentinel.touch(exist_ok=False)
    except OSError as error:
        fail(f"cannot create victim sentinel {sentinel}: {error}")

    setattr(  # noqa: B010
        module,
        "owned_build_namespaces",
        lambda _repository_root: (build_namespace,),
    )
    setattr(  # noqa: B010
        module,
        "validate_owned_namespace",
        lambda selected_namespace, _user_id: selected_namespace,
    )
    setattr(  # noqa: B010
        module,
        "build_namespace_parent_boundary",
        lambda _namespace, _repository_root: audit_root,
    )
    context = CalibrationContext(
        module,
        control_root,
        victim,
        mount_executable,
        mounted_paths,
    )
    run_rejection_cases(context, build_namespace)
    run_control_prefix_case(context)
    run_trusted_boundary_case(context, audit_root)
    require_existing_directory(victim, "victim")
    if not sentinel.is_file():
        fail("bind-mount calibration changed the victim sentinel")
    print("OK    mutation paths reject crossing build and prefix bind mounts")
    return sentinel


def cleanup_calibration(
    audit_root: Path,
    sentinel: Path,
    mounted_paths: list[Path],
    umount_executable: Path,
) -> list[str]:
    diagnostics: list[str] = []
    if not sentinel.is_file():
        diagnostics.append("bind-mount calibration changed the victim sentinel")
    unmount_failed = False
    for mount_point in reversed(mounted_paths):
        try:
            result = subprocess.run(
                [str(umount_executable), str(mount_point)],
                check=False,
                capture_output=True,
                text=True,
                shell=False,
            )
        except OSError as error:
            diagnostics.append(f"cannot unmount {mount_point}: {error}")
            unmount_failed = True
            continue
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
            diagnostics.append(f"unmount failed for {mount_point}: {detail}")
            unmount_failed = True
    if not sentinel.is_file():
        sentinel_diagnostic = (
            "victim sentinel is absent after calibration mounts were released"
        )
        if sentinel_diagnostic not in diagnostics:
            diagnostics.append(sentinel_diagnostic)
    if unmount_failed:
        diagnostics.append(
            f"audit root remains for safe inspection after unmount failure: {audit_root}"
        )
    else:
        try:
            shutil.rmtree(audit_root)
        except OSError as error:
            diagnostics.append(f"cannot remove audit root {audit_root}: {error}")
    return diagnostics


def create_audit_root() -> Path:
    return Path(tempfile.mkdtemp(prefix="mesa-mount-boundary."))


def execute_calibration(
    module: ModuleType,
    control_root: Path,
    mount_executable: Path,
    umount_executable: Path,
) -> None:
    audit_root = create_audit_root()
    sentinel = audit_root / "victim" / "sentinel"
    mounted_paths: list[Path] = []
    try:
        sentinel = run_calibration(
            module,
            control_root,
            audit_root,
            mount_executable,
            mounted_paths,
        )
    except BaseException:
        for diagnostic in cleanup_calibration(
            audit_root,
            sentinel,
            mounted_paths,
            umount_executable,
        ):
            print(
                f"mount-boundary-calibration: cleanup: {diagnostic}",
                file=sys.stderr,
            )
        raise
    cleanup_diagnostics = cleanup_calibration(
        audit_root,
        sentinel,
        mounted_paths,
        umount_executable,
    )
    if cleanup_diagnostics:
        fail("; ".join(cleanup_diagnostics))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control-root", type=Path, required=True)
    arguments = parser.parse_args()
    require_private_user_mount_namespace()

    control_root = arguments.control_root.resolve(strict=True)
    module = load_source_root_control(control_root)
    mount_executable = resolve_mount_executable("mount")
    umount_executable = resolve_mount_executable("umount")
    execute_calibration(
        module,
        control_root,
        mount_executable,
        umount_executable,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
