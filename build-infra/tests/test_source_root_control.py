# Copyright 2026 Oichkatzelesfrettschen
# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib.util
import os
import pwd
import subprocess
from pathlib import Path

import pytest

SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts" / "source_root_control.py"
MODULE_SPEC = importlib.util.spec_from_file_location(
    "source_root_control",
    SCRIPT_PATH,
)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
source_root_control = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(source_root_control)


def layout_values(tmp_path: Path) -> dict[str, Path | str]:
    build_root = tmp_path / ".mesa-26-gororoba-builds" / "source-root-control-test"
    return {
        "source_root": tmp_path / "source",
        "source_commit": "1" * 40,
        "source_tree": "2" * 40,
        "control_root": tmp_path / "control",
        "control_commit": "3" * 40,
        "build_root": build_root,
        "builddir": build_root / "build",
        "prefix": build_root / "prefix",
    }


def set_captured_revisions(
    values: dict[str, Path | str],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    revision_names = {
        "source_commit": "GOROROBA_SOURCE_COMMIT_CAPTURED",
        "source_tree": "GOROROBA_SOURCE_TREE_CAPTURED",
        "control_commit": "GOROROBA_CONTROL_COMMIT_CAPTURED",
    }
    for field, variable_name in revision_names.items():
        revision = values[field]
        assert isinstance(revision, str)
        monkeypatch.setenv(variable_name, revision)


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


def test_validate_layout_rejects_peer_git_worktree(
    tmp_path: Path,
) -> None:
    values = layout_values(tmp_path)
    peer_root = tmp_path / ".mesa-26-gororoba-builds" / "peer"
    (peer_root / ".git").mkdir(parents=True)
    values["build_root"] = peer_root / "build-output"
    values["builddir"] = peer_root / "build-output" / "build"
    values["prefix"] = peer_root / "build-output" / "prefix"
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_layout("build", values)


def test_validate_layout_rejects_nested_git_worktree_before_clean(
    tmp_path: Path,
) -> None:
    values = layout_values(tmp_path)
    builddir = values["builddir"]
    assert isinstance(builddir, Path)
    nested_repository = builddir / "retained-repository"
    (nested_repository / ".git").mkdir(parents=True)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_layout("clean", values)


def test_validate_layout_rejects_nested_git_worktree_before_archive(
    tmp_path: Path,
) -> None:
    values = layout_values(tmp_path)
    prefix = values["prefix"]
    assert isinstance(prefix, Path)
    nested_repository = prefix / "retained-repository"
    (nested_repository / ".git").mkdir(parents=True)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_layout("distclean", values)


def test_owned_build_namespaces_ignore_home_environment(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("HOME", "/")
    account_home = Path(pwd.getpwuid(os.getuid()).pw_dir).resolve()
    namespaces = source_root_control.owned_build_namespaces(
        tmp_path / "control",
    )
    assert namespaces[0] == (
        account_home / ".cache" / "mesa-26-gororoba" / "external-builds"
    )


def test_owned_build_namespaces_reject_root(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(source_root_control.os, "getuid", lambda: 0)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.owned_build_namespaces(tmp_path / "control")


def test_validate_owned_namespace_rejects_symlink(
    tmp_path: Path,
) -> None:
    target = tmp_path / "target"
    target.mkdir()
    selector = tmp_path / "selector"
    selector.symlink_to(target, target_is_directory=True)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_owned_namespace(
            selector,
            os.getuid(),
        )


def test_validate_owned_namespace_rejects_writable_mode(
    tmp_path: Path,
) -> None:
    namespace = tmp_path / "namespace"
    namespace.mkdir(mode=0o777)
    namespace.chmod(0o777)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_owned_namespace(
            namespace,
            os.getuid(),
        )


def test_require_captured_inputs_rejects_replacement(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = layout_values(tmp_path)
    set_captured_revisions(values, monkeypatch)
    environment_names = {
        "source_root": "GOROROBA_TOPSRC_INPUT",
        "control_root": "GOROROBA_CONTROL_ROOT_INPUT",
        "build_root": "GOROROBA_BUILD_ROOT_INPUT",
        "builddir": "GOROROBA_BUILDDIR_INPUT",
        "prefix": "GOROROBA_PREFIX_INPUT",
    }
    anchor_names = {
        "source_root": "GOROROBA_SOURCE_ROOT_ANCHOR",
        "control_root": "GOROROBA_CONTROL_ROOT_ANCHOR",
        "build_root": "GOROROBA_BUILD_ROOT_ANCHOR",
        "builddir": "GOROROBA_BUILDDIR_ANCHOR",
        "prefix": "GOROROBA_PREFIX_ANCHOR",
    }
    for field, input_name in environment_names.items():
        path = values[field]
        assert isinstance(path, Path)
        path.mkdir(parents=True, exist_ok=True)
        monkeypatch.setenv(input_name, str(path))
        monkeypatch.setenv(
            anchor_names[field],
            source_root_control.path_anchor(path),
        )

    fields = tuple(environment_names)
    source_root_control.require_captured_inputs(values, fields)

    builddir = values["builddir"]
    assert isinstance(builddir, Path)
    moved_builddir = builddir.with_name("moved-build")
    builddir.rename(moved_builddir)
    builddir.mkdir()
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_captured_inputs(values, ("builddir",))


def test_require_captured_inputs_rejects_retargeted_selector(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    captured_path = tmp_path / "captured"
    selected_path = tmp_path / "selected"
    selected_path.mkdir()
    values = layout_values(tmp_path)
    values["builddir"] = selected_path
    set_captured_revisions(values, monkeypatch)
    monkeypatch.setenv("GOROROBA_BUILDDIR_INPUT", str(captured_path))
    monkeypatch.setenv(
        "GOROROBA_BUILDDIR_ANCHOR",
        source_root_control.path_anchor(selected_path),
    )
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_captured_inputs(values, ("builddir",))


@pytest.mark.parametrize(
    ("field", "variable_name"),
    (
        ("source_commit", "GOROROBA_SOURCE_COMMIT_CAPTURED"),
        ("source_tree", "GOROROBA_SOURCE_TREE_CAPTURED"),
        ("control_commit", "GOROROBA_CONTROL_COMMIT_CAPTURED"),
    ),
)
def test_require_captured_inputs_rejects_each_revision_change(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    field: str,
    variable_name: str,
) -> None:
    values = layout_values(tmp_path)
    set_captured_revisions(values, monkeypatch)
    monkeypatch.setenv(variable_name, "4" * 40)
    with pytest.raises(
        source_root_control.ControlError,
        match=f"build lease: {field}",
    ):
        source_root_control.require_captured_inputs(values, ())


def test_require_clean_worktree_detects_assume_unchanged(
    tmp_path: Path,
) -> None:
    repository = tmp_path / "repository"
    repository.mkdir()
    subprocess.run(
        ["git", "-C", str(repository), "init", "-q"],
        check=True,
    )
    source_file = repository / "meson.build"
    source_file.write_text("project('clean')\n", encoding="ascii")
    subprocess.run(
        ["git", "-C", str(repository), "add", "meson.build"],
        check=True,
    )
    subprocess.run(
        [
            "git",
            "-C",
            str(repository),
            "-c",
            "user.name=source-root-test",
            "-c",
            "user.email=source-root-test.invalid",
            "commit",
            "-qm",
            "test: add tracked source",
        ],
        check=True,
    )
    source_root_control.require_clean_worktree(
        repository,
        "test worktree",
    )
    subprocess.run(
        [
            "git",
            "-C",
            str(repository),
            "update-index",
            "--assume-unchanged",
            "meson.build",
        ],
        check=True,
    )
    source_file.write_text("project('dirty')\n", encoding="ascii")
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_clean_worktree(
            repository,
            "test worktree",
        )


def test_require_clean_worktree_detects_staged_only_change(
    tmp_path: Path,
) -> None:
    repository = tmp_path / "repository"
    repository.mkdir()
    subprocess.run(
        ["git", "-C", str(repository), "init", "-q"],
        check=True,
    )
    source_file = repository / "meson.build"
    source_file.write_text("project('clean')\n", encoding="ascii")
    subprocess.run(
        ["git", "-C", str(repository), "add", "meson.build"],
        check=True,
    )
    subprocess.run(
        [
            "git",
            "-C",
            str(repository),
            "-c",
            "user.name=source-root-test",
            "-c",
            "user.email=source-root-test.invalid",
            "commit",
            "-qm",
            "test: add tracked source",
        ],
        check=True,
    )
    source_file.write_text("project('staged')\n", encoding="ascii")
    subprocess.run(
        ["git", "-C", str(repository), "add", "meson.build"],
        check=True,
    )
    source_file.write_text("project('clean')\n", encoding="ascii")
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_clean_worktree(
            repository,
            "test worktree",
        )


def test_external_source_checks_source_and_control(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    source_root = tmp_path / "source"
    control_root = tmp_path / "control"
    checked: list[tuple[Path, str]] = []
    monkeypatch.setattr(
        source_root_control,
        "control_root",
        lambda: control_root,
    )
    monkeypatch.setattr(
        source_root_control,
        "require_clean_worktree",
        lambda root, label: checked.append((root, label)),
    )
    source_root_control.require_clean_external_source(source_root)
    assert checked == [
        (source_root, "external TOPSRC"),
        (control_root, "control worktree"),
    ]


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
