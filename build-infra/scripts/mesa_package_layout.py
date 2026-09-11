#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate the configured and staged stock Mesa package boundary."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import re
import shlex
import stat
import subprocess
import sys
from pathlib import Path, PurePosixPath

REQUIRED_OPTIONS = {
    "prefix": "/usr",
    "libdir": "lib",
    "sysconfdir": "/etc",
    "gallium-drivers": ["r300", "zink"],
    "vulkan-drivers": ["ati_r300"],
    "vulkan-layers": ["anti-lag", "device-select"],
    "tools": ["nir", "glsl", "dlclose-skip", "drm-shim"],
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
    instrumentation = "disabled" if expected_buildtype == "release" else "enabled"
    for option in ("valgrind", "libunwind"):
        if options.get(option) != instrumentation:
            raise ValueError(
                f"package {option} disagrees with its profile dependency closure"
            )
    optimization = "0" if expected_buildtype == "debug" else "2"
    if options.get("optimization") != optimization:
        raise ValueError("package optimization disagrees with its profile")
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


def stock_path(value: str, roots: tuple[str, ...], label: str) -> str:
    if not value.startswith("/") or "$" in value:
        raise ValueError(f"{label} requires an absolute resolved stock path: {value}")
    normalized = posixpath.normpath(value)
    path = PurePosixPath(normalized)
    if not any(path.is_relative_to(root) for root in roots):
        raise ValueError(f"{label} escapes its stock directories: {value}")
    return normalized


def check_pkgconfig(path: Path, installed_path: PurePosixPath) -> None:
    label = f"pkg-config metadata {installed_path}"
    variables = {"pcfiledir": str(installed_path.parent)}
    fields: dict[str, str] = {}
    content = path.read_text().replace("\\\n", "")
    for line in content.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        match = re.fullmatch(r"([A-Za-z0-9_.]+)\s*([=:])\s*(.*)", line)
        if match is None:
            raise ValueError(f"{label} has an invalid assignment or field: {line}")
        name, separator, value = match.groups()
        entries = variables if separator == "=" else fields
        if name in entries:
            raise ValueError(f"{label} repeats {name}")
        entries[name] = value

    resolved: dict[str, str] = {}

    def expand(value: str, active: tuple[str, ...] = ()) -> str:
        def replace(match: re.Match[str]) -> str:
            name = match.group(1)
            if name in active or name not in variables:
                raise ValueError(f"{label} has a cyclic or undefined variable: {name}")
            if name not in resolved:
                resolved[name] = expand(variables[name], (*active, name))
            return resolved[name]

        expanded = re.sub(r"\$\{([^{}]+)\}", replace, value)
        if "$" in expanded:
            raise ValueError(f"{label} has an unresolved variable: {value}")
        return expanded

    for name in variables:
        value = expand("${" + name + "}")
        if value.startswith("/") and posixpath.normpath(value) != "/usr":
            stock_path(value, ("/usr/lib", "/usr/include", "/usr/share"), label)
    expected_paths = {
        "prefix": "/usr",
        "exec_prefix": "/usr",
        "libdir": "/usr/lib",
        "includedir": "/usr/include",
    }
    if path.name == "dri.pc":
        expected_paths["dridriverdir"] = "/usr/lib/dri"
    if path.name == "gbm.pc":
        expected_paths["gbmbackendspath"] = "/usr/lib/gbm"
    required = {"prefix"}
    if path.name == "dri.pc":
        required.add("dridriverdir")
    if path.name == "gbm.pc":
        required.update(("libdir", "gbmbackendspath"))
    for name, expected in expected_paths.items():
        if name not in variables and name not in required:
            continue
        value = resolved.get(name, "")
        if stock_path(value, (expected,), label) != expected:
            raise ValueError(f"{label} requires {name}={expected}")

    for name, value in fields.items():
        expanded = expand(value)
        if name not in {"Libs", "Libs.private", "Cflags", "Cflags.private"}:
            continue
        arguments = shlex.split(expanded)
        position = 0
        while position < len(arguments):
            argument = arguments[position]
            roots = ("/usr/lib",) if name.startswith("Libs") else ("/usr/include",)
            prefixes = (
                ("-L",)
                if name.startswith("Libs")
                else ("-isystem", "-iquote", "-idirafter", "-I")
            )
            for prefix in prefixes:
                if argument.startswith(prefix):
                    directory = argument[len(prefix) :]
                    if not directory:
                        position += 1
                        if position == len(arguments):
                            raise ValueError(f"{label} lacks a path after {prefix}")
                        directory = arguments[position]
                    stock_path(directory, roots, label)
                    break
            else:
                # Mesa's generated metadata exports library names, include paths,
                # and pthread linkage. Other options need an explicit path rule.
                library = (
                    name.startswith("Libs")
                    and argument.startswith("-l")
                    and len(argument) > 2
                )
                if "/" in argument or not (library or argument == "-pthread"):
                    raise ValueError(f"{label} has an unsupported argument: {argument}")
            position += 1


def check_elf(path: Path, installed_path: PurePosixPath) -> None:
    result = subprocess.run(
        ["readelf", "--dynamic", str(path)],
        check=True,
        capture_output=True,
        text=True,
        env={**os.environ, "LC_ALL": "C"},
    )
    label = f"ELF loader metadata {installed_path}"
    if result.stderr:
        raise ValueError(f"{label}: {result.stderr.strip()}")
    for line in result.stdout.splitlines():
        if not re.search(r"\((?:RPATH|RUNPATH|NEEDED)\)", line):
            continue
        match = re.search(r"\((RPATH|RUNPATH|NEEDED)\).*\[([^\]]*)\]", line)
        if match is None:
            raise ValueError(f"{label} has an unreadable dynamic path: {line}")
        tag, value = match.groups()
        if tag == "NEEDED" and value and "/" not in value and "$" not in value:
            continue
        entries = [value] if tag == "NEEDED" else re.split("[:;]", value)
        for entry in entries:
            expanded = re.sub(
                r"\$(?:\{ORIGIN\}|ORIGIN\b)",
                lambda _match: str(installed_path.parent),
                entry,
            )
            stock_path(expanded, ("/usr/lib",), label)


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
        installed_path = PurePosixPath("/") / relative
        if path.suffix == ".pc":
            check_pkgconfig(path, installed_path)
        with path.open("rb") as artifact:
            is_elf = artifact.read(4) == b"\x7fELF"
        if is_elf:
            check_elf(path, installed_path)


def check_r3v_manifest(stage: Path) -> None:
    manifest_directory = stage / "usr/share/vulkan/icd.d"
    manifests = sorted(manifest_directory.glob("r3v_icd*.json"))
    private_manifests = list(
        (stage / "usr/share/mesa-gororoba/vulkan/icd.d").glob("r3v_icd*.json")
    )
    if len(manifests) != 1 or private_manifests:
        raise ValueError(
            "r3v package requires one manifest in the standard loader directory"
        )

    manifest = manifests[0]
    try:
        document = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError("r3v package manifest is unreadable") from error
    icd = document.get("ICD")
    if not isinstance(icd, dict):
        raise ValueError("r3v package manifest lacks its ICD object")
    library_path = icd.get("library_path")
    if not isinstance(library_path, str) or not library_path:
        raise ValueError("r3v package manifest lacks ICD.library_path")

    expected_library = (stage / "usr/lib/libvulkan_r3v.so").resolve(strict=True)
    if library_path.startswith("/"):
        resolved_library = (stage / library_path.lstrip("/")).resolve()
    elif "/" in library_path:
        resolved_library = (manifest.parent / library_path).resolve()
    else:
        resolved_library = (stage / "usr/lib" / library_path).resolve()
    if resolved_library != expected_library:
        raise ValueError(
            "r3v package manifest does not resolve to the packaged driver library"
        )


def publish_stage(builddir: Path, stage: Path) -> None:
    identity, options = check_configuration(builddir)
    if identity["package_destdir"] != str(stage):
        raise ValueError("staging destination disagrees with the build identity")
    check_stage(stage)
    check_r3v_manifest(stage)
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
    receipt = stage / "usr/share/mesa-gororoba/build-identity.json"
    receipt.parent.mkdir(parents=True, exist_ok=True)
    receipt.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")


def payload_hashes(stage: Path) -> dict[str, dict[str, str]]:
    hashes: dict[str, dict[str, str]] = {}
    for path in sorted(stage.rglob("*")):
        relative = str(path.relative_to(stage))
        if relative == "usr/share/mesa-gororoba/build-identity.json":
            continue
        mode = f"{stat.S_IMODE(path.lstat().st_mode):04o}"
        if path.is_symlink():
            hashes[relative] = {
                "mode": mode,
                "target": str(path.readlink()),
                "type": "symlink",
            }
        elif path.is_file():
            hashes[relative] = {
                "mode": mode,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "type": "file",
            }
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
    check_r3v_manifest(stage)


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
