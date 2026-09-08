#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate the configured and staged stock Mesa package boundary."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

REQUIRED_OPTIONS = {
    "prefix": "/usr",
    "libdir": "lib",
    "sysconfdir": "/etc",
    "gallium-drivers": ["r300", "zink"],
    "vulkan-drivers": ["ati_r300"],
    "vulkan-layers": ["anti-lag", "device-select"],
    "dri-drivers-path": "/usr/lib/dri",
    "gbm-backends-path": "/usr/lib/gbm",
    "build-tests": True,
    "werror": True,
    "llvm": "disabled",
    "draw-use-llvm": False,
    "glx": "dri",
    "egl": "enabled",
    "gbm": "enabled",
    "glvnd": "enabled",
}
REQUIRED_ARTIFACTS = (
    "usr/lib/libgbm.so.1",
    "usr/lib/libGLX_mesa.so.0",
    "usr/lib/libEGL_mesa.so.0",
    "usr/lib/gbm/dri_gbm.so",
    "usr/lib/dri/r300_dri.so",
    "usr/lib/dri/zink_dri.so",
    "usr/lib/dri/r300_drv_video.so",
    "usr/lib/libvulkan_r3v.so",
    "usr/lib/libVkLayer_MESA_anti_lag.so",
    "usr/lib/libVkLayer_MESA_device_select.so",
    "usr/share/vulkan/implicit_layer.d/VkLayer_MESA_anti_lag.json",
    "usr/share/vulkan/implicit_layer.d/VkLayer_MESA_device_select.json",
    "usr/share/glvnd/egl_vendor.d/50_mesa.json",
    "usr/lib/pkgconfig/dri.pc",
    "usr/lib/pkgconfig/gbm.pc",
)
SYSTEM_PROFILES = {
    "4_r300_full_release_x86_64v1-clang22-distcc-cache": ("release", "true"),
    "3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache": (
        "debugoptimized",
        "false",
    ),
    "2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache": ("debug", "false"),
}


def read_options(builddir: Path) -> dict[str, object]:
    rows = json.loads((builddir / "meson-info/intro-buildoptions.json").read_text())
    if not isinstance(rows, list):
        raise ValueError("Meson build options must be a list")
    options: dict[str, object] = {}
    for row in rows:
        if not isinstance(row, dict) or "name" not in row or "value" not in row:
            raise ValueError("Meson build option lacks its name or value")
        name = row["name"]
        if not isinstance(name, str) or name in options:
            raise ValueError("Meson build options contain invalid or duplicate names")
        options[name] = row["value"]
    return options


def check_configuration(builddir: Path) -> tuple[dict[str, object], dict[str, object]]:
    identity = json.loads((builddir / ".mesa-source-identity.json").read_text())
    if not isinstance(identity, dict):
        raise ValueError("build identity must be an object")
    if (
        identity.get("state") != "final"
        or identity.get("package_layout") != "stock-usr"
    ):
        raise ValueError("package requires a finalized stock-usr build identity")
    if identity.get("prefix") != "/usr" or identity.get("builddir") != str(builddir):
        raise ValueError("package identity disagrees with the selected build or prefix")
    stage = Path(str(identity.get("build_root", ""))) / "package-root"
    if identity.get("package_destdir") != str(stage):
        raise ValueError("package identity lacks its derived staging destination")
    profile = identity.get("profile")
    if not isinstance(profile, str) or profile not in SYSTEM_PROFILES:
        raise ValueError("system packages require a supported unsanitized r300 profile")
    options = read_options(builddir)
    for option, expected in REQUIRED_OPTIONS.items():
        if options.get(option) != expected:
            raise ValueError(
                f"package option {option}: expected {expected!r}, got {options.get(option)!r}"
            )
    expected_buildtype, expected_ndebug = SYSTEM_PROFILES[str(profile)]
    if (
        options.get("buildtype") != expected_buildtype
        or options.get("b_ndebug") != expected_ndebug
    ):
        raise ValueError(
            "package optimization/assertion options disagree with its profile"
        )
    if options.get("b_sanitize") not in ([], "none", ["none"]):
        raise ValueError("sanitizer drivers belong in build-owned experimental staging")
    return identity, options


def staged_file(stage: Path, relative: str) -> Path:
    path = stage / relative
    resolved = path.resolve(strict=True)
    if not resolved.is_relative_to(stage) or not resolved.is_file():
        raise ValueError(
            f"package artifact escapes staging or is not a file: {relative}"
        )
    return resolved


def check_stage(stage: Path) -> None:
    if stage.is_symlink() or stage.resolve(strict=True) != stage:
        raise ValueError("package staging root must be a physical directory")
    for relative in REQUIRED_ARTIFACTS:
        staged_file(stage, relative)
    for path in sorted(stage.rglob("*")):
        relative = path.relative_to(stage)
        if relative.parts[0] not in {"usr", "etc"}:
            raise ValueError(
                f"package payload has an alternate installation root: {relative}"
            )
        if path.is_symlink():
            target = path.readlink()
            if target.is_absolute():
                raise ValueError(
                    f"package symlink must stay relative to its stock directory: {relative}"
                )
            staged_file(stage, str(relative))
        if not path.is_file():
            continue
        if path.suffix == ".pc":
            content = path.read_text()
            if "/opt/" in content or "/usr/local" in content:
                raise ValueError(
                    f"pkg-config metadata leaks an alternate prefix: {relative}"
                )
            if path.name == "dri.pc" and "dridriverdir=/usr/lib/dri" not in content:
                raise ValueError(
                    "dri.pc must export the stock dridriverdir=/usr/lib/dri"
                )
        with path.open("rb") as artifact:
            is_elf = artifact.read(4) == b"\x7fELF"
        if is_elf:
            dynamic = subprocess.run(
                ["readelf", "--dynamic", str(path)],
                check=True,
                capture_output=True,
                text=True,
            ).stdout
            if re.search(
                r"\((?:RPATH|RUNPATH|NEEDED)\).*\[(?:[^\]]*/opt/|[^\]]*/usr/local)",
                dynamic,
            ):
                raise ValueError(
                    f"ELF loader metadata leaks an alternate prefix: {relative}"
                )


def publish_stage(builddir: Path, stage: Path) -> None:
    identity, options = check_configuration(builddir)
    if identity["package_destdir"] != str(stage):
        raise ValueError("staging destination disagrees with the build identity")
    check_stage(stage)
    manifests = sorted((stage / "usr/share/vulkan/icd.d").glob("r3v_icd*.json"))
    if len(manifests) != 1:
        raise ValueError(
            "r3v package requires exactly one staged architecture manifest"
        )
    destination = stage / "usr/share/mesa-gororoba/vulkan/icd.d"
    destination.mkdir(parents=True, exist_ok=True)
    manifests[0].rename(destination / manifests[0].name)
    record = {
        key: identity[key]
        for key in (
            "source_commit",
            "source_tree",
            "control_commit",
            "profile",
            "package_layout",
        )
    }
    record["options"] = options
    record["sha256"] = payload_hashes(stage)
    (stage / "usr/share/mesa-gororoba/build-identity.json").write_text(
        json.dumps(record, indent=2, sort_keys=True) + "\n"
    )


def payload_hashes(stage: Path) -> dict[str, str]:
    hashes: dict[str, str] = {}
    for path in sorted(stage.rglob("*")):
        relative = str(path.relative_to(stage))
        if relative == "usr/share/mesa-gororoba/build-identity.json":
            continue
        if path.is_symlink():
            hashes[relative] = "symlink:" + str(path.readlink())
        elif path.is_file():
            hashes[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    return hashes


def verify_stage(builddir: Path, stage: Path, expected_profile: str | None) -> None:
    identity, options = check_configuration(builddir)
    if identity["package_destdir"] != str(stage):
        raise ValueError("staging destination disagrees with the build identity")
    if expected_profile is not None and identity["profile"] != expected_profile:
        raise ValueError("staged package profile disagrees with the recipe")
    check_stage(stage)
    record = json.loads(
        (stage / "usr/share/mesa-gororoba/build-identity.json").read_text()
    )
    if not isinstance(record, dict):
        raise ValueError("staged package receipt must be an object")
    for field in (
        "source_commit",
        "source_tree",
        "control_commit",
        "profile",
        "package_layout",
    ):
        if record.get(field) != identity.get(field):
            raise ValueError(
                f"staged package receipt differs from build identity: {field}"
            )
    if record.get("options") != options or record.get("sha256") != payload_hashes(
        stage
    ):
        raise ValueError(
            "staged package payload or configuration changed after qualification"
        )
    manifests = list(
        (stage / "usr/share/mesa-gororoba/vulkan/icd.d").glob("r3v_icd*.json")
    )
    if len(manifests) != 1 or list((stage / "usr/share/vulkan/icd.d").glob("*.json")):
        raise ValueError(
            "experimental R3V selection must remain scoped to its launcher"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("config", "stage", "verify"))
    parser.add_argument("--builddir", type=Path, required=True)
    parser.add_argument("--stage", type=Path)
    parser.add_argument("--profile")
    parser.add_argument("--source-commit")
    args = parser.parse_args()
    try:
        if args.operation == "config":
            check_configuration(args.builddir)
        elif args.stage is None:
            raise ValueError("stage requires --stage")
        elif args.operation == "verify":
            verify_stage(args.builddir, args.stage, args.profile)
            if args.source_commit is not None:
                identity, _ = check_configuration(args.builddir)
                if identity["source_commit"] != args.source_commit:
                    raise ValueError(
                        "staged package source disagrees with the fetched revision"
                    )
        else:
            publish_stage(args.builddir, args.stage)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Mesa package layout: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
