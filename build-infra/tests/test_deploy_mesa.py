# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib.util
import os
import subprocess
from pathlib import Path

import pytest  # type: ignore[import-not-found]

BUILD_INFRA_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = BUILD_INFRA_ROOT / "scripts/deploy_mesa.py"
MODULE_SPEC = importlib.util.spec_from_file_location("deploy_mesa_package", SCRIPT_PATH)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
deploy_mesa_package = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(deploy_mesa_package)


def package(path: Path, timestamp_ns: int) -> Path:
    path.write_bytes(b"package fixture\n")
    os.utime(path, ns=(timestamp_ns, timestamp_ns))
    return path


def test_deploy_rejects_every_non_exact_consent(tmp_path: Path) -> None:
    for value in ("", "0", "true", "yes", "2"):
        with pytest.raises(
            deploy_mesa_package.DeployError, match="deployment remains locked"
        ):
            deploy_mesa_package.deploy(tmp_path, "cachyos-vostro1000", value)


def test_deploy_rejects_unsafe_target_host(tmp_path: Path) -> None:
    with pytest.raises(deploy_mesa_package.DeployError, match="invalid target host"):
        deploy_mesa_package.deploy(tmp_path, "host; reboot", "1")


def test_deploy_rejects_missing_package(tmp_path: Path) -> None:
    with pytest.raises(deploy_mesa_package.DeployError, match="no package"):
        deploy_mesa_package.deploy(tmp_path, "cachyos-vostro1000", "1")


def test_deploy_selects_newest_package_and_exact_remote_argv(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    older = package(tmp_path / "mesa-gororoba-older.pkg.tar.zst", 1)
    newer = package(tmp_path / "mesa-gororoba-newer.pkg.tar.zst", 2)
    observed: list[list[str]] = []

    def record(command: list[str], *, check: bool) -> subprocess.CompletedProcess[str]:
        assert check is True
        observed.append(command)
        return subprocess.CompletedProcess(command, 0)

    monkeypatch.setattr(deploy_mesa_package.subprocess, "run", record)
    deploy_mesa_package.deploy(tmp_path, "operator@cachyos-vostro1000", "1")

    assert older not in (Path(argument) for argument in observed[0])
    assert observed == [
        [
            "scp",
            str(newer),
            "operator@cachyos-vostro1000:/tmp/mesa-gororoba-newer.pkg.tar.zst",
        ],
        [
            "ssh",
            "operator@cachyos-vostro1000",
            "sudo -n pacman -U --noconfirm -- /tmp/mesa-gororoba-newer.pkg.tar.zst",
        ],
    ]
    assert all(
        "--overwrite" not in argument for command in observed for argument in command
    )
