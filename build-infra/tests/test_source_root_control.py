# Copyright 2026 Oichkatzelesfrettschen
# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest

SCRIPT_PATH = (
    Path(__file__).resolve().parents[1]
    / "scripts"
    / "source_root_control.py"
)
MODULE_SPEC = importlib.util.spec_from_file_location(
    "source_root_control",
    SCRIPT_PATH,
)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
source_root_control = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(source_root_control)


def layout_values(tmp_path: Path) -> dict[str, Path | str]:
    return {
        "source_root": tmp_path / "source",
        "source_commit": "1" * 40,
        "source_tree": "2" * 40,
        "control_root": tmp_path / "control",
        "control_commit": "3" * 40,
        "build_root": tmp_path / "build-root",
        "builddir": tmp_path / "build-root" / "build",
        "prefix": tmp_path / "build-root" / "prefix",
    }


@pytest.mark.parametrize(
    "protected_path",
    (
        Path("/"),
        Path("/usr"),
        Path("/usr/local"),
        Path("/bin"),
        Path("/sbin"),
        Path("/lib"),
        Path("/lib64"),
    ),
)
def test_reject_protected_path_rejects_system_roots(
    protected_path: Path,
) -> None:
    with pytest.raises(source_root_control.ControlError):
        source_root_control.reject_protected_path(
            protected_path,
            "test path",
        )


def test_validate_layout_accepts_isolated_external_build(
    tmp_path: Path,
) -> None:
    source_root_control.validate_layout("build", layout_values(tmp_path))


def test_validate_layout_rejects_builddir_equal_to_build_root(
    tmp_path: Path,
) -> None:
    values = layout_values(tmp_path)
    values["builddir"] = values["build_root"]
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_layout("build", values)


def test_validate_layout_protects_control_worktree(
    tmp_path: Path,
) -> None:
    values = layout_values(tmp_path)
    control_root = values["control_root"]
    assert isinstance(control_root, Path)
    values["build_root"] = control_root / "external-build"
    values["builddir"] = control_root / "external-build" / "build"
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_layout("build", values)


def test_require_identity_fields_reports_value_drift(
    tmp_path: Path,
) -> None:
    expected = {"schema_version": 3, "source_commit": "1" * 40}
    recorded = {"schema_version": 3, "source_commit": "2" * 40}
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_identity_fields(
            recorded,
            expected,
            tmp_path / "identity.json",
        )


def test_require_identity_record_accepts_exact_final_record(
    tmp_path: Path,
) -> None:
    expected = {"schema_version": 3, "source_commit": "1" * 40}
    recorded = {
        **expected,
        "state": source_root_control.FINAL_STATE,
        "transaction_id": "a" * 32,
    }
    state, transaction_id = source_root_control.require_identity_record(
        recorded,
        expected,
        tmp_path / "identity.json",
        frozenset((source_root_control.FINAL_STATE,)),
    )
    assert state == source_root_control.FINAL_STATE
    assert transaction_id == "a" * 32


def test_require_identity_record_rejects_provisional_use(
    tmp_path: Path,
) -> None:
    expected = {"schema_version": 3, "source_commit": "1" * 40}
    recorded = {
        **expected,
        "state": source_root_control.PROVISIONAL_STATE,
        "transaction_id": "a" * 32,
    }
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_identity_record(
            recorded,
            expected,
            tmp_path / "identity.json",
            frozenset((source_root_control.FINAL_STATE,)),
        )


def test_require_identity_record_rejects_unknown_fields(
    tmp_path: Path,
) -> None:
    expected = {"schema_version": 3, "source_commit": "1" * 40}
    recorded = {
        **expected,
        "state": source_root_control.FINAL_STATE,
        "transaction_id": "a" * 32,
        "unbound_field": True,
    }
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_identity_record(
            recorded,
            expected,
            tmp_path / "identity.json",
            frozenset((source_root_control.FINAL_STATE,)),
        )
