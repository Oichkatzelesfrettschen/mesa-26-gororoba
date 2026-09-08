# SPDX-License-Identifier: MIT
"""Calibrate the stock package boundary against complete and broken staged payloads."""

from __future__ import annotations

import importlib.util
import json
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "mesa_package_layout", ROOT / "scripts/mesa_package_layout.py"
)
assert SPEC is not None and SPEC.loader is not None
layout = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(layout)


@pytest.fixture
def configured_build(tmp_path: Path) -> Path:
    builddir = tmp_path / "build"
    (builddir / "meson-info").mkdir(parents=True)
    options = dict(layout.REQUIRED_OPTIONS)
    options.update(
        buildtype="release",
        b_ndebug="true",
        b_sanitize=[],
        optimization="2",
        valgrind="disabled",
        libunwind="disabled",
    )
    (builddir / "meson-info/intro-buildoptions.json").write_text(
        json.dumps([{"name": name, "value": value} for name, value in options.items()])
    )
    (builddir / ".mesa-source-identity.json").write_text(
        json.dumps(
            {
                "state": "final",
                "package_layout": "stock-usr",
                "prefix": "/usr",
                "builddir": str(builddir),
                "build_root": str(tmp_path),
                "package_destdir": str(tmp_path / "package-root"),
                "profile": "4_r300_full_release_x86_64v1-clang22-distcc-cache",
                "source_commit": "1" * 40,
                "source_tree": "2" * 40,
                "control_commit": "3" * 40,
            }
        )
    )
    return builddir


@pytest.fixture
def staged_payload(configured_build: Path) -> Path:
    stage = configured_build.parent / "package-root"
    for relative in layout.REQUIRED_ARTIFACTS:
        path = stage / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("fixture\n")
    (stage / "usr/lib/pkgconfig/dri.pc").write_text(
        "prefix=/usr\ndridriverdir=/usr/lib/dri\n"
    )
    (stage / "usr/lib/pkgconfig/gbm.pc").write_text(
        "prefix=/usr\nlibdir=${prefix}/lib\n"
    )
    manifest = stage / "usr/share/vulkan/icd.d/r3v_icd.x86_64.json"
    manifest.parent.mkdir(parents=True)
    manifest.write_text('{"ICD": {"library_path": "/usr/lib/libvulkan_r3v.so"}}\n')
    return stage


def test_complete_stock_package(configured_build: Path, staged_payload: Path) -> None:
    layout.publish_stage(configured_build, staged_payload)
    layout.verify_stage(configured_build, staged_payload, None)
    assert (
        staged_payload / "usr/share/mesa-gororoba/vulkan/icd.d/r3v_icd.x86_64.json"
    ).is_file()
    assert list((staged_payload / "usr/share/vulkan/icd.d").glob("*.json")) == []


@pytest.mark.parametrize("relative", layout.REQUIRED_ARTIFACTS)
def test_missing_loader_artifact(staged_payload: Path, relative: str) -> None:
    (staged_payload / relative).unlink()
    with pytest.raises((ValueError, OSError)):
        layout.check_stage(staged_payload)


@pytest.mark.parametrize(
    "prefix", ("/opt/mesa-gororoba-debug-optimized", "/usr/local/mesa")
)
def test_pkgconfig_prefix_leak(staged_payload: Path, prefix: str) -> None:
    (staged_payload / "usr/lib/pkgconfig/gbm.pc").write_text(f"prefix={prefix}\n")
    with pytest.raises(ValueError, match="pkg-config"):
        layout.check_stage(staged_payload)


@pytest.mark.parametrize(
    "target",
    ("/usr/lib/libGLX_mesa.so.0", "/opt/mesa/libGLX_mesa.so.0", "../../../../outside"),
)
def test_symlink_escape(staged_payload: Path, target: str) -> None:
    path = staged_payload / "usr/lib/libGLX_mesa.so.0"
    path.unlink()
    path.symlink_to(target)
    with pytest.raises((ValueError, OSError)):
        layout.check_stage(staged_payload)


@pytest.mark.parametrize(
    "option,value",
    (
        ("prefix", "/opt/mesa"),
        ("build-tests", False),
        ("werror", False),
        ("vulkan-layers", ["device-select"]),
        ("gallium-drivers", ["r300"]),
        ("b_sanitize", ["address"]),
        ("valgrind", "auto"),
        ("libunwind", "auto"),
        ("optimization", "3"),
        ("buildtype", "debug"),
        ("b_ndebug", "false"),
        ("dri-drivers-path", "/opt/mesa/lib/dri"),
    ),
)
def test_wrong_configuration(
    configured_build: Path, option: str, value: object
) -> None:
    path = configured_build / "meson-info/intro-buildoptions.json"
    rows = json.loads(path.read_text())
    for row in rows:
        if row["name"] == option:
            row["value"] = value
    path.write_text(json.dumps(rows))
    with pytest.raises(ValueError):
        layout.check_configuration(configured_build)


def test_payload_mutation_after_qualification(
    configured_build: Path, staged_payload: Path
) -> None:
    layout.publish_stage(configured_build, staged_payload)
    (staged_payload / "usr/lib/libgbm.so.1").write_text("changed\n")
    with pytest.raises(ValueError, match="changed after qualification"):
        layout.verify_stage(configured_build, staged_payload, None)


def test_recipe_profile_mismatch(configured_build: Path, staged_payload: Path) -> None:
    layout.publish_stage(configured_build, staged_payload)
    with pytest.raises(ValueError, match="recipe"):
        layout.verify_stage(
            configured_build,
            staged_payload,
            "2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache",
        )


def test_elf_loader_prefix_leak(
    staged_payload: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    (staged_payload / "usr/lib/libgbm.so.1").write_bytes(b"\x7fELFfixture")
    monkeypatch.setattr(
        layout.subprocess,
        "run",
        lambda *args, **kwargs: subprocess.CompletedProcess(
            args, 0, "(RUNPATH) Library runpath: [/opt/mesa/lib]", ""
        ),
    )
    with pytest.raises(ValueError, match="ELF loader"):
        layout.check_stage(staged_payload)


def test_build_owned_default_and_unprivileged_install() -> None:
    makefile = (ROOT / "Makefile").read_text()
    assert "PREFIX := $(BUILD_ROOT)/prefix" in makefile
    assert "override SELECT_PROFILE_PREFIX = $(BUILD_ROOT)/prefix" in makefile
    install = makefile.split("\ninstall: source-root-check", 1)[1].split(
        "\n# The logical /usr", 1
    )[0]
    assert "$(SUDO)" not in install
    assert "install-experiment" in install


def test_mutually_exclusive_stock_variants() -> None:
    variants = (
        "mesa-gororoba",
        "mesa-gororoba-debug-optimized",
        "mesa-gororoba-debug-o0",
    )
    for variant in variants:
        metadata = subprocess.run(
            ["makepkg", "--printsrcinfo"],
            cwd=ROOT / "packaging" / variant,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        conflicts = {
            line.split(" = ", 1)[1]
            for line in metadata.splitlines()
            if line.startswith("\tconflicts = ")
        }
        assert set(variants) - {variant} <= conflicts
        assert "mesa-gororoba-debug-asan" in conflicts
        assert "lib32-vulkan-radeon" not in conflicts
    tools = (ROOT / "packaging/mesa-gororoba-debug-tools/PKGBUILD").read_text()
    assert '"mesa-gororoba-debug-asan>=' not in tools
    assert not (ROOT / "packaging/mesa-gororoba-debug-asan/PKGBUILD").exists()


def test_lease_fixture_stops_when_directory_creation_fails(tmp_path: Path) -> None:
    import os
    import shlex
    import sys

    binaries = tmp_path / "bin"
    binaries.mkdir()
    python = binaries / "python3"
    python.write_text(
        "#!/bin/sh\n"
        "case \"$*\" in *'create-test-directory --label=build-lease'*) "
        "echo 'forced fixture directory failure' >&2; exit 19 ;; esac\n"
        f'exec {shlex.quote(sys.executable)} "$@"\n'
    )
    python.chmod(0o755)
    marker = tmp_path / "unexpected-mutation"
    for command in ("mkdir", "flock"):
        executable = binaries / command
        executable.write_text(
            f"#!/bin/sh\nprintf attempted > {shlex.quote(str(marker))}\nexit 23\n"
        )
        executable.chmod(0o755)
    environment = dict(os.environ)
    environment["PATH"] = str(binaries) + os.pathsep + environment["PATH"]
    environment.update(
        GIT_CONFIG_COUNT="1",
        GIT_CONFIG_KEY_0="core.fsmonitor",
        GIT_CONFIG_VALUE_0="false",
    )
    result = subprocess.run(
        ["make", "-C", str(ROOT), "build-lease-test"],
        env=environment,
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "forced fixture directory failure" in result.stderr
    assert not marker.exists()


def test_experiment_launcher_selects_config_and_layers(tmp_path: Path) -> None:
    import os

    build_root = tmp_path / "experiment"
    prefix = build_root / "prefix"
    for relative in (
        "lib",
        "share/vulkan/explicit_layer.d",
        "share/vulkan/implicit_layer.d",
    ):
        (prefix / relative).mkdir(parents=True)
    opencl = build_root / "installation-root/etc/OpenCL/vendors"
    opencl.mkdir(parents=True)
    environment = dict(os.environ)
    environment.update(
        VK_ADD_LAYER_PATH="/fixture/explicit",
        VK_ADD_IMPLICIT_LAYER_PATH="/fixture/implicit",
    )
    result = subprocess.run(
        [
            "sh",
            str(ROOT / "scripts/mesa-experiment-run"),
            str(build_root),
            "sh",
            "-c",
            'printf "%s\\n" "$OCL_ICD_VENDORS" "$VK_ADD_LAYER_PATH" "$VK_ADD_IMPLICIT_LAYER_PATH"',
        ],
        env=environment,
        capture_output=True,
        text=True,
        check=True,
    )
    assert result.stdout.splitlines() == [
        str(opencl),
        str(prefix / "share/vulkan/explicit_layer.d") + ":/fixture/explicit",
        str(prefix / "share/vulkan/implicit_layer.d") + ":/fixture/implicit",
    ]
