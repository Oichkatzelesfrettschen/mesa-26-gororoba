# SPDX-License-Identifier: MIT
"""Exercise default package identities and object-preserving repack callbacks."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

INFRA_ROOT = Path(__file__).resolve().parents[1]
PROFILE = "4_r300_full_release_x86_64v1-clang22-distcc-cache"
BUILD_ROOT_RELATIVE = Path("src/.mesa-26-gororoba-builds/objects")


@pytest.fixture
def package_fixture(tmp_path: Path) -> tuple[Path, dict[str, str]]:
    root = tmp_path
    environment = {
        name: value
        for name, value in os.environ.items()
        if not name.startswith(("GIT_", "MESA_"))
    }
    environment.update(
        GIT_CONFIG_GLOBAL="/dev/null",
        GIT_CONFIG_SYSTEM="/dev/null",
        GIT_CONFIG_NOSYSTEM="1",
        PYTHONDONTWRITEBYTECODE="1",
        COMPILER_CHAIN="direct",
        COMPILER_FAMILY="llvm",
        HOSTENV="package-fixture",
        BUILD_LOCK=str(root / "package.lock"),
        MESON_TEST_ARGS="",
        MAKEFLAGS="",
        MFLAGS="",
        MAKEOVERRIDES="",
        MESA_LLVM_VERSION="22",
        MESA_PACKAGE_BUILD_ROOT=str(root / BUILD_ROOT_RELATIVE),
    )
    source_root = root / "src/mesa-source"
    source_root.mkdir(parents=True)
    (root / BUILD_ROOT_RELATIVE).parent.mkdir(mode=0o700)
    for relative in (
        "Makefile",
        "scripts/source_root_control.py",
        "scripts/meson_profile_dflags.py",
        f"configs/alternates/{PROFILE}.meson",
        "packaging/stock-package.meson",
    ):
        destination = source_root / "build-infra" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(INFRA_ROOT / relative, destination)
    (source_root / "meson.build").write_text("project('package-identity')\n")
    (source_root / "meson.options").touch()
    for arguments in (
        ["init", "-q"],
        ["add", "."],
        [
            "-c",
            "user.name=package-test",
            "-c",
            "user.email=package-test.invalid",
            "commit",
            "-qm",
            "test: add package control source",
        ],
    ):
        subprocess.run(
            ["git", "-C", str(source_root), *arguments], check=True, env=environment
        )
    binaries = root / "bin"
    binaries.mkdir()
    for compiler in ("clang-22", "clang++-22"):
        executable = binaries / compiler
        executable.write_text("#!/bin/sh\nprintf '%s\\n' 'clang version 22.0.0'\n")
        executable.chmod(0o755)
    meson = binaries / "meson"
    meson.write_text(
        "#!/bin/sh\nset -eu\n"
        'case "$1" in\n'
        "setup) for argument do previous=${last:-}; last=$argument; done\n"
        '  test "$last" = "$MESA_BUILD_ROOT_INPUT/.mesa-source-view"\n'
        '  test "$last" != "$MESA_CONTROL_ROOT_INPUT"\n'
        '  test -f "$last/meson.build"\n'
        "  printf '%s\\n' 'qualified object' > \"$previous/object.o\" ;;\n"
        "test) case \" $* \" in *' --no-rebuild '*) : ;;\n"
        '  *) printf rebuilt > "$MESA_BUILDDIR_INPUT/object.o" ;; esac ;;\n'
        "*) exit 64 ;;\nesac\n"
    )
    meson.chmod(0o755)
    environment.update(
        PATH=str(binaries) + os.pathsep + environment["PATH"],
        MESON=str(meson),
        NINJA="true",
    )
    return root, environment


def run_callback(
    root: Path, environment: dict[str, str], body: str
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "bash",
            "-c",
            "set -euo pipefail; srcdir=$1/src; pkgname=mesa-gororoba; "
            f"_variant=release; _profile={PROFILE}; epoch=2; pkgver=26.2; pkgrel=1; "
            '. "$2"; ' + body,
            "_",
            str(root),
            str(INFRA_ROOT / "packaging/mesa-package-common.sh"),
        ],
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )


def test_default_package_build_records_source_control_identity(
    package_fixture: tuple[Path, dict[str, str]],
) -> None:
    root, environment = package_fixture
    result = run_callback(root, environment, "build")
    assert result.returncode == 0, result.stdout + result.stderr
    build_root = root / BUILD_ROOT_RELATIVE
    identity_path = build_root / f"mesa-{PROFILE}/.mesa-source-identity.json"
    identity = json.loads(identity_path.read_text())
    assert (
        identity["source_root"]
        == identity["control_root"]
        == str(root / "src/mesa-source")
    )
    assert identity["state"] == "final"
    assert identity["prefix"] == "/usr"
    assert identity["package_layout"] == "stock-usr"
    assert identity["source_view"] == str(build_root / ".mesa-source-view")
    assert (
        identity_path.read_bytes()
        == (build_root / ".mesa-external-source-identity.json").read_bytes()
    )
    result = run_callback(root, environment, "_package_paths; _package_make build")
    assert result.returncode == 0, result.stdout + result.stderr

    (build_root / ".mesa-source-view/meson.build").write_text("source drift\n")
    result = run_callback(root, environment, "_package_paths; _package_make build")
    assert result.returncode != 0
    assert "source view content drift" in result.stdout + result.stderr


def test_default_package_requires_clean_control_source(
    package_fixture: tuple[Path, dict[str, str]],
) -> None:
    root, environment = package_fixture
    (root / "src/mesa-source/meson.build").write_text("dirty control\n")
    result = run_callback(root, environment, "build")
    assert result.returncode != 0
    assert "package source/control worktree is dirty" in result.stdout + result.stderr
    assert not (
        root / BUILD_ROOT_RELATIVE / ".mesa-external-source-identity.json"
    ).exists()


def test_default_package_preserves_failed_configuration_identity(
    package_fixture: tuple[Path, dict[str, str]],
) -> None:
    root, environment = package_fixture
    environment["MESON"] = "false"
    result = run_callback(root, environment, "build")
    assert result.returncode != 0
    assert "Meson setup failed" in result.stdout + result.stderr
    build_root = root / BUILD_ROOT_RELATIVE
    identity = json.loads(
        (build_root / ".mesa-external-source-identity.json").read_text()
    )
    assert identity["state"] == "provisional"
    result = run_callback(root, environment, "_package_paths; _package_make clean")
    assert result.returncode == 0, result.stdout + result.stderr
    assert not (build_root / f"mesa-{PROFILE}").exists()


def test_default_package_rejects_build_directory_overlapping_source_view(
    package_fixture: tuple[Path, dict[str, str]],
) -> None:
    root, environment = package_fixture
    environment["MESA_PACKAGE_BUILDDIR"] = str(
        root / BUILD_ROOT_RELATIVE / ".mesa-source-view"
    )
    result = run_callback(root, environment, "_package_paths; _package_make configure")
    assert result.returncode != 0
    assert "BUILDDIR overlaps the source view" in result.stdout + result.stderr


@pytest.mark.parametrize("repack", (False, True))
def test_repack_callback_preserves_objects_during_meson_tests(
    package_fixture: tuple[Path, dict[str, str]], repack: bool
) -> None:
    root, environment = package_fixture
    result = run_callback(root, environment, "build")
    assert result.returncode == 0, result.stdout + result.stderr
    object_file = root / BUILD_ROOT_RELATIVE / f"mesa-{PROFILE}/object.o"
    before = object_file.read_bytes()
    if repack:
        environment["MESA_PACKAGE_BUILDDIR"] = str(object_file.parent)
    result = run_callback(
        root,
        environment,
        "eval \"$(declare -f _package_make | sed '1s/_package_make/_original_package_make/')\"; "
        '_package_make() { case "$1" in test) _original_package_make "$@" ;; esac; }; '
        'python3() { case "$1" in */source_root_control.py|*/meson_profile_dflags.py) '
        'command python3 "$@" ;; esac; }; check',
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert (object_file.read_bytes() == before) is repack


@pytest.mark.parametrize("operation", ("verify-identity", "verify-delete-identity"))
def test_default_package_rejects_missing_identity(
    package_fixture: tuple[Path, dict[str, str]], operation: str
) -> None:
    root, environment = package_fixture
    result = run_callback(root, environment, "build")
    assert result.returncode == 0, result.stdout + result.stderr
    (root / BUILD_ROOT_RELATIVE / ".mesa-external-source-identity.json").unlink()
    target = "build" if operation == "verify-identity" else "clean"
    result = run_callback(root, environment, f"_package_paths; _package_make {target}")
    assert result.returncode != 0
    assert "lacks source identity" in result.stdout + result.stderr
    assert (root / BUILD_ROOT_RELATIVE / f"mesa-{PROFILE}/object.o").is_file()
