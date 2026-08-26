# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest  # type: ignore[import-not-found]

SCRIPT_PATH = Path(__file__).resolve().parent / "mount_boundary_calibration.py"
MODULE_SPEC = importlib.util.spec_from_file_location(
    "mount_boundary_calibration", SCRIPT_PATH
)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
mount_boundary_calibration = importlib.util.module_from_spec(MODULE_SPEC)
sys.modules[MODULE_SPEC.name] = mount_boundary_calibration
MODULE_SPEC.loader.exec_module(mount_boundary_calibration)
del sys.modules[MODULE_SPEC.name]


def test_accepts_caller_mapped_private_mount_namespace() -> None:
    mount_boundary_calibration.validate_namespace_isolation(
        ["0", "1000", "1"],
        "mnt:[4026533000]",
        "mnt:[4026531832]",
    )


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]
    ("uid_fields", "process_namespace", "caller_namespace", "diagnostic"),
    (
        (["0", "0", "4294967295"], "mnt:[2]", "mnt:[1]", "user namespace"),
        (["0", "1000", "1"], "mnt:[1]", "mnt:[1]", "mount namespace"),
        (["0", "1000", "1"], "mnt:[2]", "", "caller mount namespace"),
        (["0", "1000", "1"], "mnt:[2]", "pid:[1]", "caller mount namespace"),
        (["0", "1000", "1"], "invalid", "mnt:[1]", "calibration mount namespace"),
    ),
)
def test_rejects_unproven_namespace_isolation(
    uid_fields: list[str],
    process_namespace: str,
    caller_namespace: str,
    diagnostic: str,
) -> None:
    with pytest.raises(SystemExit, match=diagnostic):
        mount_boundary_calibration.validate_namespace_isolation(
            uid_fields,
            process_namespace,
            caller_namespace,
        )
