#!/usr/bin/env python3
# Copyright 2026 Oichkatzelesfrettschen
# SPDX-License-Identifier: MIT
"""Calibrate mutation-target rejection in a private mount namespace."""

from __future__ import annotations

import argparse
import importlib.util
import os
import subprocess
import tempfile
from pathlib import Path
from types import ModuleType


def fail(message: str) -> None:
    raise SystemExit(f"mount-boundary-calibration: {message}")


def require_private_user_mount_namespace() -> None:
    if os.environ.get("GOROROBA_MOUNT_BOUNDARY_CALIBRATION") != "1":
        fail("exact calibration consent is missing")
    if os.getuid() != 0:
        fail("calibration user namespace does not map the caller to uid 0")
    try:
        uid_fields = Path("/proc/self/uid_map").read_text(encoding="ascii").split()
        process_mount_namespace = os.readlink("/proc/self/ns/mnt")
        init_mount_namespace = os.readlink("/proc/1/ns/mnt")
    except OSError as error:
        fail(f"cannot verify namespace isolation: {error}")
    if (
        len(uid_fields) < 3
        or uid_fields[0] != "0"
        or uid_fields[1] == "0"
        or process_mount_namespace == init_mount_namespace
    ):
        fail("calibration is outside a private user and mount namespace")


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
        "build_root": build_root,
        "builddir": build_root / builddir_name,
        "prefix": build_root / "prefix",
        "sysconfdir": Path("/etc"),
    }


def require_mount_rejection(
    module: ModuleType,
    values: dict[str, Path | str],
    victim: Path,
    *,
    mount_point: Path,
    operation: str,
    expected_label: str,
) -> None:
    mount_point.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["mount", "--bind", str(victim), str(mount_point)],
        check=True,
        shell=False,
    )
    if mount_point.stat().st_dev != victim.stat().st_dev:
        fail("bind-mount calibration did not retain one device number")
    try:
        module.validate_layout(operation, values)
    except module.ControlError as error:
        expected_diagnostic = f"refusing {expected_label} crossing a mount point"
        if expected_diagnostic not in str(error):
            fail(f"bind mount returned the wrong diagnostic: {error}")
    else:
        fail(f"{operation} accepted a mount under {expected_label}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control-root", type=Path, required=True)
    arguments = parser.parse_args()
    require_private_user_mount_namespace()

    control_root = arguments.control_root.resolve(strict=True)
    module = load_source_root_control(control_root)
    audit_root = Path(
        tempfile.mkdtemp(
            prefix="mesa-mount-boundary.",
            dir="/tmp",
        )
    )
    build_namespace = audit_root / "namespace"
    victim = audit_root / "victim"
    victim.mkdir()
    (victim / "sentinel").touch()

    module.owned_build_namespaces = lambda _repository_root: (build_namespace,)
    module.validate_owned_namespace = (
        lambda selected_namespace, _user_id: selected_namespace
    )
    clean_descendant_values = layout_values(
        control_root,
        build_namespace,
        "clean-descendant-build",
        "descendant-probe",
    )
    clean_exact_values = layout_values(
        control_root,
        build_namespace,
        "clean-exact-build",
        "exact-probe",
    )
    install_builddir_values = layout_values(
        control_root,
        build_namespace,
        "install-builddir-build",
        "probe",
    )
    install_prefix_values = layout_values(
        control_root,
        build_namespace,
        "install-prefix-build",
        "probe",
    )
    clean_ancestor_values = layout_values(
        control_root,
        build_namespace,
        "clean-ancestor-build",
        "probe",
    )
    artifact_ancestor_values = layout_values(
        control_root,
        build_namespace,
        "artifact-ancestor-build",
        "probe",
    )
    module.validate_layout("clean", clean_descendant_values)
    module.validate_layout("clean", clean_exact_values)
    module.validate_layout("install", install_builddir_values)
    module.validate_layout("install", install_prefix_values)

    clean_descendant_builddir = clean_descendant_values["builddir"]
    clean_exact_builddir = clean_exact_values["builddir"]
    install_builddir = install_builddir_values["builddir"]
    install_prefix = install_prefix_values["prefix"]
    clean_ancestor_build_root = clean_ancestor_values["build_root"]
    artifact_ancestor_build_root = artifact_ancestor_values["build_root"]
    assert isinstance(clean_descendant_builddir, Path)
    assert isinstance(clean_exact_builddir, Path)
    assert isinstance(install_builddir, Path)
    assert isinstance(install_prefix, Path)
    assert isinstance(clean_ancestor_build_root, Path)
    assert isinstance(artifact_ancestor_build_root, Path)
    require_mount_rejection(
        module,
        clean_descendant_values,
        victim,
        mount_point=clean_descendant_builddir / "mounted-victim",
        operation="clean",
        expected_label="BUILDDIR",
    )
    require_mount_rejection(
        module,
        clean_exact_values,
        victim,
        mount_point=clean_exact_builddir,
        operation="clean",
        expected_label="BUILDDIR",
    )
    require_mount_rejection(
        module,
        install_builddir_values,
        victim,
        mount_point=install_builddir / "meson-logs" / "mounted-victim",
        operation="install",
        expected_label="BUILDDIR",
    )
    require_mount_rejection(
        module,
        install_prefix_values,
        victim,
        mount_point=install_prefix / "mounted-victim",
        operation="install",
        expected_label="PREFIX",
    )
    require_mount_rejection(
        module,
        clean_ancestor_values,
        victim,
        mount_point=clean_ancestor_build_root,
        operation="clean",
        expected_label="BUILDDIR",
    )
    require_mount_rejection(
        module,
        artifact_ancestor_values,
        victim,
        mount_point=artifact_ancestor_build_root,
        operation="artifact",
        expected_label="PREFIX",
    )

    control_prefix_values = {
        "source_root": control_root,
        "source_commit": "1" * 40,
        "source_tree": "2" * 40,
        "control_root": control_root,
        "control_commit": "3" * 40,
        "build_root": control_root / "build",
        "builddir": control_root / "build" / "mount-boundary-probe",
        "prefix": Path("/opt/local/mesa-26-gororoba"),
        "sysconfdir": Path("/etc"),
    }
    require_mount_rejection(
        module,
        control_prefix_values,
        victim,
        mount_point=Path("/opt/local"),
        operation="artifact",
        expected_label="PREFIX",
    )

    trusted_boundary = audit_root / "trusted-boundary"
    trusted_boundary.mkdir()
    trusted_victim = audit_root / "trusted-victim"
    trusted_victim.mkdir()
    subprocess.run(
        ["mount", "--bind", str(trusted_victim), str(trusted_boundary)],
        check=True,
        shell=False,
    )
    module.owned_build_namespaces = lambda _repository_root: (trusted_boundary,)
    module.validate_layout(
        "build",
        layout_values(
            control_root,
            trusted_boundary,
            "boundary-mount-build",
            "probe",
        ),
    )
    if not (victim / "sentinel").is_file():
        fail("bind-mount calibration changed the victim sentinel")
    print("OK    mutation paths reject crossing build and prefix bind mounts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
