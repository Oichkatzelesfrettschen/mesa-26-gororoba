#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deploy the newest mesa-gororoba package through an explicit consent gate."""

from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

PACKAGE_GLOB = "mesa-gororoba-*.pkg.tar.zst"
PACKAGE_NAME_PATTERN = re.compile(r"^[A-Za-z0-9@._+:-]+[.]pkg[.]tar[.]zst$")
TARGET_HOST_PATTERN = re.compile(
    r"^(?:[A-Za-z0-9][A-Za-z0-9._-]*@)?[A-Za-z0-9][A-Za-z0-9._-]*$"
)


class DeployError(ValueError):
    """The deployment request failed a local admission invariant."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--target-host", required=True)
    parser.add_argument("--accepted", required=True)
    return parser.parse_args()


def select_package(package_dir: Path) -> Path:
    candidates = [path for path in package_dir.glob(PACKAGE_GLOB) if path.is_file()]
    if not candidates:
        raise DeployError("no package; run 'make pkg-mesa-gororoba' first")
    package = max(candidates, key=lambda path: (path.stat().st_mtime_ns, path.name))
    if not PACKAGE_NAME_PATTERN.fullmatch(package.name):
        raise DeployError(
            f"package name is unsafe for remote execution: {package.name!r}"
        )
    return package


def deploy(package_dir: Path, target_host: str, accepted: str) -> None:
    if accepted != "1":
        raise DeployError(
            "deployment remains locked; set MESA_GOROROBA_DEPLOY_ACCEPTED=1 "
            "for this invocation"
        )
    if not TARGET_HOST_PATTERN.fullmatch(target_host):
        raise DeployError(f"invalid target host: {target_host!r}")

    package = select_package(package_dir)
    remote_package = f"/tmp/{package.name}"
    print(f"deploy {package} -> {target_host}:{remote_package}")
    subprocess.run(["scp", str(package), f"{target_host}:{remote_package}"], check=True)
    install_command = shlex.join(
        ["sudo", "-n", "pacman", "-U", "--noconfirm", "--", remote_package]
    )
    subprocess.run(["ssh", target_host, install_command], check=True)


def main() -> int:
    arguments = parse_args()
    try:
        deploy(arguments.package_dir, arguments.target_host, arguments.accepted)
    except (DeployError, OSError, subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
