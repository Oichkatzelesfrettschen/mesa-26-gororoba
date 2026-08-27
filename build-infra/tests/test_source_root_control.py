# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib.util
import os
import shlex
import tarfile
from pathlib import Path
from types import SimpleNamespace

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


@pytest.mark.parametrize(
    "identifier",
    (
        "",
        ".",
        "..",
        "bad/name",
        "bad value",
        "bad;value",
        "bad'value",
        'bad"value',
        "$(shell touch marker)",
        "$$(touch marker)",
        "bad\nvalue",
    ),
)
def test_input_identifier_rejects_non_leaf_values(
    identifier: str,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("GOROROBA_TEST_IDENTIFIER", identifier)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.input_identifier("GOROROBA_TEST_IDENTIFIER")


@pytest.mark.parametrize(
    "identifier",
    (
        "4_r300_full_release_x86_64v1-clang22-distcc-cache",
        "vostro1000-x86-64-v1-clang22-ccache-distcc",
        "profile.v1+audit",
    ),
)
def test_input_identifier_accepts_mechanism_leaf_values(
    identifier: str,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("GOROROBA_TEST_IDENTIFIER", identifier)
    assert (
        source_root_control.input_identifier("GOROROBA_TEST_IDENTIFIER") == identifier
    )


@pytest.mark.parametrize("value", ("", "stable"))
def test_input_enum_accepts_declared_values(
    value: str,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("GOROROBA_TEST_ENUM", value)
    assert (
        source_root_control.input_enum(
            "GOROROBA_TEST_ENUM",
            frozenset(("", "stable")),
        )
        == value
    )


@pytest.mark.parametrize("value", ("debug", "stable;id", "$(shell id)"))
def test_input_enum_rejects_undeclared_values(
    value: str,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("GOROROBA_TEST_ENUM", value)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.input_enum(
            "GOROROBA_TEST_ENUM",
            frozenset(("", "stable")),
        )


@pytest.mark.parametrize(
    ("value", "minimum", "expected"),
    (
        ("0", 0, "0"),
        ("1", 1, "1"),
        ("0006", 1, "6"),
        ("7200", 0, "7200"),
    ),
)
def test_input_decimal_accepts_bounded_integers(
    value: str,
    minimum: int,
    expected: str,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("GOROROBA_TEST_DECIMAL", value)
    assert (
        source_root_control.input_decimal(
            "GOROROBA_TEST_DECIMAL",
            minimum=minimum,
        )
        == expected
    )


@pytest.mark.parametrize(
    ("value", "minimum"),
    (
        ("", 0),
        ("-1", 0),
        ("0", 1),
        ("1.0", 0),
        ("1; touch marker", 0),
        ('1"; touch marker; #', 0),
    ),
)
def test_input_decimal_rejects_non_decimal_or_below_minimum(
    value: str,
    minimum: int,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("GOROROBA_TEST_DECIMAL", value)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.input_decimal(
            "GOROROBA_TEST_DECIMAL",
            minimum=minimum,
        )


@pytest.mark.parametrize("version", ("4.2", "4.3", "4.4.1", "5.0"))
def test_validate_make_version_accepts_supported_versions(version: str) -> None:
    source_root_control.validate_make_version(version)


@pytest.mark.parametrize(
    "version",
    ("", "4", "4.1", "4.1.9", "4.2; touch marker", '4"; touch marker; #.4'),
)
def test_validate_make_version_rejects_old_or_non_versions(version: str) -> None:
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_make_version(version)


def layout_values(tmp_path: Path) -> dict[str, Path | str]:
    build_root = tmp_path / ".mesa-26-gororoba-builds" / "source-root-control-test"
    return {
        "source_root": tmp_path / "source",
        "source_commit": "1" * 40,
        "source_tree": "2" * 40,
        "control_root": tmp_path / "control",
        "control_commit": "3" * 40,
        "control_tree": "4" * 40,
        "build_root": build_root,
        "builddir": build_root / "build",
        "prefix": build_root / "prefix",
        "sysconfdir": tmp_path / "etc",
        "profile": "test-profile",
        "hostenv": "test-hostenv",
        "mode": "default",
        "compiler_chain": "direct",
        "compiler_family": "llvm",
    }


def set_captured_revisions(
    values: dict[str, Path | str],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    revision_names = {
        "source_commit": "GOROROBA_SOURCE_COMMIT_CAPTURED",
        "source_tree": "GOROROBA_SOURCE_TREE_CAPTURED",
        "control_commit": "GOROROBA_CONTROL_COMMIT_CAPTURED",
        "control_tree": "GOROROBA_CONTROL_TREE_CAPTURED",
    }
    for field, variable_name in revision_names.items():
        revision = values[field]
        assert isinstance(revision, str)
        monkeypatch.setenv(variable_name, revision)


def commit_repository(repository: Path, message: str) -> None:
    source_root_control.run_git(repository, "add", "--all")
    source_root_control.run_git(
        repository,
        "-c",
        "user.name=source-root-test",
        "-c",
        "user.email=source-root-test.invalid",
        "-c",
        "commit.gpgsign=false",
        "commit",
        "-qm",
        message,
    )


def committed_repository(repository: Path, object_format: str = "sha1") -> Path:
    repository.mkdir(parents=True)
    source_root_control.run_git(
        repository,
        "init",
        "-q",
        "-b",
        "main",
        f"--object-format={object_format}",
    )
    source_file = repository / "meson.build"
    source_file.write_text("project('clean')\n", encoding="utf-8")
    (repository / "meson.options").write_text("", encoding="utf-8")
    commit_repository(repository, "test: add tracked source")
    return source_file


def source_view_values(tmp_path: Path) -> dict[str, Path | str]:
    values = layout_values(tmp_path)
    source_root = values["source_root"]
    build_root = values["build_root"]
    assert isinstance(source_root, Path)
    assert isinstance(build_root, Path)
    committed_repository(source_root)
    (source_root / "tracked-source").write_text("archive input\n", encoding="utf-8")
    commit_repository(source_root, "test: add source view input")
    values["source_commit"] = source_root_control.run_git(
        source_root,
        "rev-parse",
        "HEAD",
    )
    values["source_tree"] = source_root_control.run_git(
        source_root,
        "rev-parse",
        "HEAD^{tree}",
    )
    build_root.mkdir(parents=True)
    return values


def write_provisional_identity(values: dict[str, Path | str]) -> None:
    base_payload = source_root_control.base_identity_payload(values)
    source_root_control.write_json_atomic(
        source_root_control.root_identity_path(values),
        source_root_control.identity_record(
            base_payload,
            source_root_control.PROVISIONAL_STATE,
            "a" * 32,
            source_root_control.PENDING_SOURCE_VIEW_DIGEST,
        ),
    )


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
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    build_namespace = tmp_path / ".mesa-26-gororoba-builds"
    monkeypatch.setattr(
        source_root_control,
        "owned_build_namespaces",
        lambda _repository_root: (build_namespace,),
    )
    source_root_control.validate_layout("build", layout_values(tmp_path))


@pytest.mark.parametrize("operation", ("build", "clean", "configure", "test"))
def test_validate_layout_rejects_builddir_inside_source_view(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    operation: str,
) -> None:
    build_namespace = tmp_path / ".mesa-26-gororoba-builds"
    monkeypatch.setattr(
        source_root_control,
        "owned_build_namespaces",
        lambda _repository_root: (build_namespace,),
    )
    values = layout_values(tmp_path)
    values["builddir"] = source_root_control.source_view_path(values) / "nested-build"
    with pytest.raises(
        source_root_control.ControlError,
        match="BUILDDIR overlaps the source view",
    ):
        source_root_control.validate_layout(operation, values)


def test_validate_layout_rejects_prefix_equal_to_source_view(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    build_namespace = tmp_path / ".mesa-26-gororoba-builds"
    monkeypatch.setattr(
        source_root_control,
        "owned_build_namespaces",
        lambda _repository_root: (build_namespace,),
    )
    values = layout_values(tmp_path)
    values["prefix"] = source_root_control.source_view_path(values)
    with pytest.raises(
        source_root_control.ControlError,
        match="PREFIX aliases the source view",
    ):
        source_root_control.validate_layout("configure", values)


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


def test_validate_layout_rejects_bare_repository_ancestor(
    tmp_path: Path,
) -> None:
    values = layout_values(tmp_path)
    namespace = tmp_path / ".mesa-26-gororoba-builds"
    bare_repository = namespace / "retained.git"
    bare_repository.mkdir(parents=True)
    source_root_control.run_git(
        bare_repository,
        "init",
        "--bare",
        "-q",
        ".",
    )
    values["build_root"] = bare_repository
    values["builddir"] = bare_repository / "objects"
    values["prefix"] = bare_repository / "prefix"
    with pytest.raises(
        source_root_control.ControlError,
        match="inside a Git directory",
    ):
        source_root_control.validate_layout("clean", values)


def test_validate_layout_rejects_nested_bare_repository(
    tmp_path: Path,
) -> None:
    values = layout_values(tmp_path)
    builddir = values["builddir"]
    assert isinstance(builddir, Path)
    bare_repository = builddir / "retained.git"
    bare_repository.mkdir(parents=True)
    source_root_control.run_git(
        bare_repository,
        "init",
        "--bare",
        "-q",
        ".",
    )
    with pytest.raises(
        source_root_control.ControlError,
        match="containing a Git repository marker",
    ):
        source_root_control.validate_layout("clean", values)


def test_is_git_directory_accepts_linked_worktree_metadata(
    tmp_path: Path,
) -> None:
    git_directory = tmp_path / "linked-git-directory"
    git_directory.mkdir()
    for filename in ("HEAD", "commondir", "gitdir"):
        (git_directory / filename).touch()
    assert source_root_control.is_git_directory(git_directory)


def test_parse_mountinfo_decodes_kernel_path_escapes() -> None:
    mountinfo = (
        b"1 0 0:1 / / rw - rootfs rootfs rw\n"
        b"2 1 0:1 / /tmp/space\\040tab\\011line\\012slash\\134end "
        b"rw - tmpfs tmpfs rw\n"
    )
    assert source_root_control.parse_mountinfo(mountinfo) == (
        Path("/"),
        Path("/tmp/space tab\tline\nslash\\end"),
    )


@pytest.mark.parametrize(
    "mountinfo",
    (
        b"",
        b"1 0 0:1 / / rw rootfs rootfs rw\n",
        b"1 0 0:1 / relative rw - rootfs rootfs rw\n",
        b"1 0 0:1 / /tmp/bad\\09x rw - tmpfs tmpfs rw\n",
        b"1 0 0:1 / /tmp/bad\\000 rw - tmpfs tmpfs rw\n",
        b"1 0 0:1 / /tmp/bad\\400 rw - tmpfs tmpfs rw\n",
    ),
)
def test_parse_mountinfo_rejects_malformed_records(
    mountinfo: bytes,
) -> None:
    with pytest.raises(source_root_control.ControlError):
        source_root_control.parse_mountinfo(mountinfo)


def test_crossing_mount_point_rejects_ancestor_and_descendant_mounts() -> None:
    mount_points = (
        Path("/"),
        Path("/tmp/namespace/build-root"),
        Path("/tmp/namespace/build-root/probe/mounted"),
        Path("/tmp/namespace-sibling"),
    )
    assert source_root_control.crossing_mount_point(
        Path("/tmp/namespace/build-root/probe"),
        Path("/tmp/namespace"),
        mount_points,
    ) == Path("/tmp/namespace/build-root")
    assert (
        source_root_control.crossing_mount_point(
            Path("/tmp/namespace/other/probe"),
            Path("/tmp/namespace"),
            mount_points,
        )
        is None
    )


def test_crossing_mount_point_allows_trusted_boundary_and_ancestors() -> None:
    mount_points = (
        Path("/"),
        Path("/tmp"),
        Path("/tmp/namespace"),
        Path("/tmp/namespace/sibling"),
        Path("/tmp/namespace-sibling"),
    )
    assert (
        source_root_control.crossing_mount_point(
            Path("/tmp/namespace/build-root/probe"),
            Path("/tmp/namespace"),
            mount_points,
        )
        is None
    )


def test_crossing_mount_point_requires_strict_containment() -> None:
    with pytest.raises(source_root_control.ControlError):
        source_root_control.crossing_mount_point(
            Path("/tmp/other/probe"),
            Path("/tmp/namespace"),
            (Path("/"),),
        )


def test_current_mount_points_fails_closed_when_unreadable(
    tmp_path: Path,
) -> None:
    with pytest.raises(source_root_control.ControlError):
        source_root_control.current_mount_points(tmp_path / "missing-mountinfo")


def test_owned_build_namespaces_ignore_home_environment(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    user_id = 1234
    account_home = tmp_path / "account-home"
    account_home.mkdir()
    monkeypatch.setenv("HOME", "/")
    monkeypatch.setattr(source_root_control.os, "getuid", lambda: user_id)
    monkeypatch.setattr(
        source_root_control.pwd,
        "getpwuid",
        lambda _user_id: SimpleNamespace(pw_dir=str(account_home)),
    )
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


def test_validate_build_namespace_chain_creates_private_intermediates(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    user_id = os.getuid()
    account_home = tmp_path / "account-home"
    account_home.mkdir(mode=0o700)
    namespace = account_home / ".cache" / "mesa-26-gororoba" / "external-builds"
    monkeypatch.setattr(
        source_root_control,
        "trusted_account_home",
        lambda _user_id: account_home,
    )
    source_root_control.validate_build_namespace_chain(
        namespace,
        tmp_path / "control",
        create=True,
    )
    for candidate in (
        account_home / ".cache",
        account_home / ".cache" / "mesa-26-gororoba",
        namespace,
    ):
        assert candidate.stat().st_uid == user_id
        assert candidate.stat().st_mode & 0o777 == 0o700


def test_validate_build_namespace_chain_rejects_writable_intermediate(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    account_home = tmp_path / "account-home"
    account_home.mkdir(mode=0o700)
    cache = account_home / ".cache"
    cache.mkdir(mode=0o770)
    cache.chmod(0o770)
    namespace = cache / "mesa-26-gororoba" / "external-builds"
    monkeypatch.setattr(
        source_root_control,
        "trusted_account_home",
        lambda _user_id: account_home,
    )
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_build_namespace_chain(
            namespace,
            tmp_path / "control",
            create=True,
        )


def test_validate_build_namespace_chain_rejects_writable_boundary(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    account_home = tmp_path / "account-home"
    account_home.mkdir(mode=0o777)
    account_home.chmod(0o777)
    namespace = account_home / ".cache" / "mesa-26-gororoba" / "external-builds"
    monkeypatch.setattr(
        source_root_control,
        "trusted_account_home",
        lambda _user_id: account_home,
    )
    with pytest.raises(
        source_root_control.ControlError,
        match="group/world writable",
    ):
        source_root_control.validate_build_namespace_chain(
            namespace,
            tmp_path / "control",
            create=True,
        )


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


@pytest.mark.parametrize("mode", (0o770, 0o777))
def test_validate_owned_namespace_rejects_writable_mode(
    tmp_path: Path,
    mode: int,
) -> None:
    namespace = tmp_path / "namespace"
    namespace.mkdir(mode=mode)
    namespace.chmod(mode)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_owned_namespace(
            namespace,
            os.getuid(),
        )


def test_validate_owned_namespace_rejects_foreign_owner(
    tmp_path: Path,
) -> None:
    namespace = tmp_path / "namespace"
    namespace.mkdir(mode=0o700)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_owned_namespace(
            namespace,
            os.getuid() + 1,
        )


def test_create_test_directory_accepts_new_and_existing_private_namespace(
    tmp_path: Path,
) -> None:
    first = source_root_control.create_test_directory(
        "source-root",
        parent=tmp_path,
    )
    second = source_root_control.create_test_directory(
        "build-lease",
        parent=tmp_path,
    )
    assert first.is_dir()
    assert second.is_dir()
    assert first.parent == second.parent
    assert first.parent.stat().st_mode & 0o777 == 0o700


@pytest.mark.parametrize("mode", (0o770, 0o777))
def test_create_test_directory_rejects_writable_namespace(
    tmp_path: Path,
    mode: int,
) -> None:
    namespace = tmp_path / f"mesa-26-gororoba-{os.getuid()}"
    namespace.mkdir(mode=mode)
    namespace.chmod(mode)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.create_test_directory(
            "source-root",
            parent=tmp_path,
        )


def test_create_test_directory_rejects_symlink_namespace(
    tmp_path: Path,
) -> None:
    target = tmp_path / "target"
    target.mkdir(mode=0o700)
    namespace = tmp_path / f"mesa-26-gororoba-{os.getuid()}"
    namespace.symlink_to(target, target_is_directory=True)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.create_test_directory(
            "source-root",
            parent=tmp_path,
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
        "sysconfdir": "GOROROBA_SYSCONFDIR_INPUT",
    }
    anchor_names = {
        "source_root": "GOROROBA_SOURCE_ROOT_ANCHOR",
        "control_root": "GOROROBA_CONTROL_ROOT_ANCHOR",
        "build_root": "GOROROBA_BUILD_ROOT_ANCHOR",
        "builddir": "GOROROBA_BUILDDIR_ANCHOR",
        "prefix": "GOROROBA_PREFIX_ANCHOR",
        "sysconfdir": "GOROROBA_SYSCONFDIR_ANCHOR",
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
        ("control_tree", "GOROROBA_CONTROL_TREE_CAPTURED"),
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
    monkeypatch.setenv(variable_name, "f" * 40)
    with pytest.raises(
        source_root_control.ControlError,
        match=f"build lease: {field}",
    ):
        source_root_control.require_captured_inputs(values, ())


def test_require_clean_worktree_detects_assume_unchanged(
    tmp_path: Path,
) -> None:
    repository = tmp_path / "repository"
    source_file = committed_repository(repository)
    source_root_control.require_clean_worktree(
        repository,
        "test worktree",
    )
    source_root_control.run_git(
        repository,
        "update-index",
        "--assume-unchanged",
        "meson.build",
    )
    source_file.write_text("project('dirty')\n", encoding="utf-8")
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_clean_worktree(
            repository,
            "test worktree",
        )


def test_require_clean_worktree_detects_staged_only_change(
    tmp_path: Path,
) -> None:
    repository = tmp_path / "repository"
    source_file = committed_repository(repository)
    source_file.write_text("project('staged')\n", encoding="utf-8")
    source_root_control.run_git(repository, "add", "meson.build")
    source_file.write_text("project('clean')\n", encoding="utf-8")
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_clean_worktree(
            repository,
            "test worktree",
        )


def test_require_clean_worktree_bypasses_repository_clean_filter(
    tmp_path: Path,
) -> None:
    repository = tmp_path / "repository"
    source_file = committed_repository(repository)
    attributes_file = repository / ".gitattributes"
    attributes_file.write_text("meson.build filter=ambient\n", encoding="utf-8")
    commit_repository(repository, "test: select ambient clean filter")

    filter_marker = tmp_path / "clean-filter-executed"
    filter_command = f"sed '/^ambient-only$/d'; : > {shlex.quote(str(filter_marker))}"
    source_root_control.run_git(
        repository,
        "config",
        "filter.ambient.clean",
        filter_command,
    )
    source_file.write_text(
        "project('clean')\nambient-only\n",
        encoding="utf-8",
    )

    with pytest.raises(source_root_control.ControlError, match="is dirty"):
        source_root_control.require_clean_worktree(repository, "test worktree")
    assert not filter_marker.exists()


def test_require_clean_worktree_accepts_checkout_line_endings_and_detects_mutation(
    tmp_path: Path,
) -> None:
    repository = tmp_path / "repository"
    source_file = committed_repository(repository)
    attributes_file = repository / ".gitattributes"
    attributes_file.write_text("meson.build text eol=crlf\n", encoding="utf-8")
    commit_repository(repository, "test: select checkout line endings")
    source_file.write_bytes(b"project('clean')\r\n")

    source_root_control.require_clean_worktree(repository, "test worktree")

    source_file.write_bytes(b"project('clean')\r\nmutation\r\n")

    with pytest.raises(source_root_control.ControlError, match="is dirty"):
        source_root_control.require_clean_worktree(repository, "test worktree")


def test_require_clean_worktree_rejects_ignored_subproject_sources(
    tmp_path: Path,
) -> None:
    repository = tmp_path / "repository"
    committed_repository(repository)
    subprojects = repository / "subprojects"
    subprojects.mkdir()
    (subprojects / ".gitignore").write_text("/*/\n", encoding="utf-8")
    commit_repository(repository, "test: ignore populated subprojects")
    source_root_control.require_clean_worktree(repository, "test worktree")
    ignored_source = subprojects / "Vulkan-Profiles" / "meson.build"
    ignored_source.parent.mkdir()
    ignored_source.write_text("project('ignored-input')\n", encoding="utf-8")
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_clean_worktree(repository, "test worktree")


def test_require_clean_worktree_accepts_ignored_build_outputs(
    tmp_path: Path,
) -> None:
    repository = tmp_path / "repository"
    committed_repository(repository)
    (repository / ".gitignore").write_text("/build/\n", encoding="utf-8")
    commit_repository(repository, "test: ignore build outputs")
    ignored_output = repository / "build" / "artifact"
    ignored_output.parent.mkdir()
    ignored_output.write_text("regenerable\n", encoding="utf-8")
    source_root_control.require_clean_worktree(repository, "test worktree")


def test_require_hashed_wrap_sources_accepts_vulkan_profiles(
    tmp_path: Path,
) -> None:
    source_view = tmp_path / "source-view"
    subprojects = source_view / "subprojects"
    subprojects.mkdir(parents=True)
    (subprojects / "Vulkan-Profiles.wrap").write_text(
        "[wrap-file]\n"
        "directory = Vulkan-Profiles-source\n"
        "source_url = file:///fixtures/Vulkan-Profiles.tar\n"
        "source_filename = Vulkan-Profiles.tar\n"
        f"source_hash = {'1' * 64}\n",
        encoding="utf-8",
    )
    source_root_control.require_hashed_wrap_sources(source_view)


def test_require_hashed_wrap_sources_rejects_unhashed_download(
    tmp_path: Path,
) -> None:
    source_view = tmp_path / "source-view"
    subprojects = source_view / "subprojects"
    subprojects.mkdir(parents=True)
    wrap_path = subprojects / "Vulkan-Profiles.wrap"
    wrap_path.write_text(
        "[wrap-file]\n"
        "directory = Vulkan-Profiles-source\n"
        "source_url = file:///fixtures/Vulkan-Profiles.tar\n"
        "source_filename = Vulkan-Profiles.tar\n",
        encoding="utf-8",
    )
    with pytest.raises(
        source_root_control.ControlError,
        match="wrap-file source_hash must be SHA-256",
    ):
        source_root_control.require_hashed_wrap_sources(source_view)


def test_require_hashed_wrap_sources_rejects_unhashed_patch(
    tmp_path: Path,
) -> None:
    source_view = tmp_path / "source-view"
    subprojects = source_view / "subprojects"
    subprojects.mkdir(parents=True)
    wrap_path = subprojects / "Vulkan-Profiles.wrap"
    wrap_path.write_text(
        "[wrap-file]\n"
        "directory = Vulkan-Profiles-source\n"
        "source_url = file:///fixtures/Vulkan-Profiles.tar\n"
        "source_filename = Vulkan-Profiles.tar\n"
        f"source_hash = {'1' * 64}\n"
        "patch_url = file:///fixtures/Vulkan-Profiles-patch.tar\n"
        "patch_filename = Vulkan-Profiles-patch.tar\n",
        encoding="utf-8",
    )
    with pytest.raises(
        source_root_control.ControlError,
        match="wrap-file patch_hash must be SHA-256",
    ):
        source_root_control.require_hashed_wrap_sources(source_view)


def test_source_view_content_digest_is_deterministic_and_complete(
    tmp_path: Path,
) -> None:
    source_view = tmp_path / "source-view"
    source_view.mkdir()
    source_file = source_view / "source"
    source_file.write_text("stable\n", encoding="utf-8")
    source_file.chmod(0o755)
    (source_view / "source-link").symlink_to("source")
    initial_digest = source_root_control.source_view_content_digest(source_view)
    assert initial_digest == source_root_control.source_view_content_digest(source_view)
    source_file.write_text("mutated\n", encoding="utf-8")
    assert initial_digest != source_root_control.source_view_content_digest(source_view)


@pytest.mark.parametrize("object_format", ("sha1", "sha256"))
def test_run_git_archive_preserves_tracked_archive_attributes(
    tmp_path: Path,
    object_format: str,
) -> None:
    repository = tmp_path / "repository"
    source_file = committed_repository(repository, object_format)
    source_file.write_text(
        "project('clean')\n# $Format:%H$\n",
        encoding="utf-8",
    )
    (repository / "tracked-source").write_text(
        "excluded by tracked attributes\n",
        encoding="utf-8",
    )
    (repository / ".gitattributes").write_text(
        "meson.build export-subst\ntracked-source export-ignore\n",
        encoding="utf-8",
    )
    commit_repository(repository, "test: select tracked archive attributes")
    source_commit = source_root_control.run_git(repository, "rev-parse", "HEAD")
    archive_path = tmp_path / "tracked-attributes.tar"

    source_root_control.run_git_archive(repository, source_commit, archive_path)

    with tarfile.open(archive_path, mode="r:") as archive:
        assert "tracked-source" not in archive.getnames()
        meson_build = archive.extractfile("meson.build")
        assert meson_build is not None
        meson_build_bytes = meson_build.read()
    assert b"$Format:%H$" not in meson_build_bytes
    assert source_commit.encode("ascii") in meson_build_bytes


@pytest.mark.parametrize("object_format", ("sha1", "sha256"))
def test_run_git_archive_ignores_repository_info_attributes(
    tmp_path: Path,
    object_format: str,
) -> None:
    repository = tmp_path / "repository"
    source_file = committed_repository(repository, object_format)
    source_file.write_text(
        "project('clean')\n# $Format:%H$\n",
        encoding="utf-8",
    )
    (repository / "tracked-source").write_text(
        "retained without tracked attributes\n",
        encoding="utf-8",
    )
    commit_repository(repository, "test: add archive inputs")
    source_commit = source_root_control.run_git(repository, "rev-parse", "HEAD")
    baseline_archive = source_root_control.run_git_bytes(
        repository,
        "archive",
        "--format=tar",
        source_commit,
    )
    git_directory = Path(
        source_root_control.run_git(repository, "rev-parse", "--absolute-git-dir")
    )
    info_attributes = git_directory / "info" / "attributes"
    info_attributes.write_text(
        "meson.build export-subst\ntracked-source export-ignore\n",
        encoding="utf-8",
    )
    ambient_archive = source_root_control.run_git_bytes(
        repository,
        "archive",
        "--format=tar",
        source_commit,
    )
    isolated_archive_path = tmp_path / "isolated-attributes.tar"

    source_root_control.run_git_archive(
        repository,
        source_commit,
        isolated_archive_path,
    )

    assert ambient_archive != baseline_archive
    assert isolated_archive_path.read_bytes() == baseline_archive


def test_prepare_source_view_archives_exact_source_and_replaces_owned_view(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path)
    source_root = values["source_root"]
    assert isinstance(source_root, Path)
    subprojects = source_root / "subprojects"
    subprojects.mkdir()
    (subprojects / "Vulkan-Profiles.wrap").write_text(
        "[wrap-file]\n"
        "directory = Vulkan-Profiles-source\n"
        "source_url = file:///fixtures/Vulkan-Profiles.tar\n"
        "source_filename = Vulkan-Profiles.tar\n"
        f"source_hash = {'2' * 64}\n",
        encoding="utf-8",
    )
    commit_repository(source_root, "test: add hashed wrap")
    values["source_commit"] = source_root_control.run_git(
        source_root,
        "rev-parse",
        "HEAD",
    )
    values["source_tree"] = source_root_control.run_git(
        source_root,
        "rev-parse",
        "HEAD^{tree}",
    )
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_provisional_identity(values)
    source_view = source_root_control.source_view_path(values)
    nested_git_marker = source_view / "subprojects" / "old-wrap" / ".git"
    nested_git_marker.mkdir(parents=True)
    (nested_git_marker / "HEAD").write_text("ref: refs/heads/main\n", encoding="utf-8")

    source_root_control.prepare_source_view(values)

    assert (source_view / "tracked-source").read_text(encoding="utf-8") == (
        "archive input\n"
    )
    assert not (source_view / ".git").exists()
    assert not nested_git_marker.exists()
    assert (
        source_root_control.run_git(
            source_root,
            "status",
            "--porcelain=v1",
            "--untracked-files=all",
        )
        == ""
    )
    root_record = source_root_control.read_identity(
        source_root_control.root_identity_path(values)
    )
    assert root_record["source_view"] == str(source_view)
    assert root_record["source_view_digest"] == (
        source_root_control.source_view_content_digest(source_view)
    )


def test_prepare_source_view_rejects_unhashed_wrap_before_publication(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path)
    source_root = values["source_root"]
    assert isinstance(source_root, Path)
    subprojects = source_root / "subprojects"
    subprojects.mkdir()
    (subprojects / "Vulkan-Profiles.wrap").write_text(
        "[wrap-file]\n"
        "directory = Vulkan-Profiles-source\n"
        "source_url = file:///fixtures/Vulkan-Profiles.tar\n"
        "source_filename = Vulkan-Profiles.tar\n",
        encoding="utf-8",
    )
    commit_repository(source_root, "test: add unhashed wrap")
    values["source_commit"] = source_root_control.run_git(
        source_root,
        "rev-parse",
        "HEAD",
    )
    values["source_tree"] = source_root_control.run_git(
        source_root,
        "rev-parse",
        "HEAD^{tree}",
    )
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_provisional_identity(values)

    with pytest.raises(
        source_root_control.ControlError,
        match="wrap-file source_hash must be SHA-256",
    ):
        source_root_control.prepare_source_view(values)
    assert not source_root_control.source_view_path(values).exists()


def test_final_identity_consumers_reject_source_view_drift(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_provisional_identity(values)
    source_root_control.prepare_source_view(values)
    source_root_control.write_identity(values)
    source_view = source_root_control.source_view_path(values)
    (source_view / "tracked-source").write_text("drift\n", encoding="utf-8")

    with pytest.raises(source_root_control.ControlError, match="content drift"):
        source_root_control.verify_identity(values)
    for allow_provisional in (False, True):
        with pytest.raises(source_root_control.ControlError, match="content drift"):
            source_root_control.verify_delete_identity(
                values,
                allow_provisional=allow_provisional,
            )


def test_provisional_cleanup_accepts_mesons_source_view_population(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_provisional_identity(values)
    source_root_control.prepare_source_view(values)
    source_view = source_root_control.source_view_path(values)
    populated_source = source_view / "subprojects" / "fixture" / "meson.build"
    populated_source.parent.mkdir(parents=True)
    populated_source.write_text("project('fixture')\n", encoding="utf-8")
    source_root_control.verify_delete_identity(values, allow_provisional=True)


def test_control_source_preparation_retains_the_selected_source(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = layout_values(tmp_path)
    values["source_root"] = values["control_root"]
    monkeypatch.setattr(
        source_root_control,
        "run_git_archive",
        lambda *_arguments: pytest.fail("control source was archived"),
    )
    source_root_control.prepare_source_view(values)


def test_run_git_ignores_ambient_repository_and_config(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    repository = tmp_path / "repository"
    committed_repository(repository)
    hostile_repository = tmp_path / "hostile"
    committed_repository(hostile_repository)
    hostile_index = tmp_path / "hostile-index"
    hostile_config = tmp_path / "hostile-gitconfig"
    hostile_config.write_text(
        "[commit]\n\tgpgsign = true\n[core]\n\thooksPath = /missing\n",
        encoding="utf-8",
    )
    monkeypatch.setenv("GIT_DIR", str(hostile_repository / ".git"))
    monkeypatch.setenv("GIT_WORK_TREE", str(hostile_repository))
    monkeypatch.setenv("GIT_INDEX_FILE", str(hostile_index))
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", str(hostile_config))
    assert source_root_control.run_git(
        repository, "rev-parse", "--show-toplevel"
    ) == str(repository)
    source_root_control.require_clean_worktree(repository, "test worktree")


def test_run_git_disables_repository_fsmonitor(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    repository = tmp_path / "repository"
    committed_repository(repository)
    marker = tmp_path / "fsmonitor-executed"
    hook = tmp_path / "fsmonitor-hook"
    hook.write_text(
        "#!/bin/sh\n: > \"$GOROROBA_FSMONITOR_MARKER\"\nprintf '\\n'\n",
        encoding="utf-8",
    )
    hook.chmod(0o755)
    source_root_control.run_git(
        repository,
        "config",
        "--local",
        "core.fsmonitor",
        str(hook),
    )
    monkeypatch.setenv("GOROROBA_FSMONITOR_MARKER", str(marker))
    source_root_control.require_clean_worktree(repository, "test worktree")
    assert not marker.exists()


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


def test_external_source_rejects_dirty_control_worktree(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    source_root = tmp_path / "source"
    control_root = tmp_path / "control"
    for repository in (source_root, control_root):
        committed_repository(repository)
    monkeypatch.setattr(
        source_root_control,
        "control_root",
        lambda: control_root,
    )
    (control_root / "untracked-control-input").touch()
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_clean_external_source(source_root)


def test_require_identity_fields_reports_value_drift(
    tmp_path: Path,
) -> None:
    expected = {
        "schema_version": source_root_control.SCHEMA_VERSION,
        "source_commit": "1" * 40,
    }
    recorded = {
        "schema_version": source_root_control.SCHEMA_VERSION,
        "source_commit": "2" * 40,
    }
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_identity_fields(
            recorded,
            expected,
            tmp_path / "identity.json",
        )


def test_base_identity_payload_records_build_controls(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = layout_values(tmp_path)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )

    payload = source_root_control.base_identity_payload(values)

    assert {
        field: payload[field]
        for field in (
            "profile",
            "hostenv",
            "mode",
            "compiler_chain",
            "compiler_family",
        )
    } == {
        "profile": "test-profile",
        "hostenv": "test-hostenv",
        "mode": "default",
        "compiler_chain": "direct",
        "compiler_family": "llvm",
    }


def test_require_identity_record_accepts_exact_final_record(
    tmp_path: Path,
) -> None:
    expected = {
        "schema_version": source_root_control.SCHEMA_VERSION,
        "source_commit": "1" * 40,
    }
    recorded = {
        **expected,
        "source_view_digest": f"sha256:{'b' * 64}",
        "state": source_root_control.FINAL_STATE,
        "transaction_id": "a" * 32,
    }
    state, transaction_id, source_view_digest = (
        source_root_control.require_identity_record(
            recorded,
            expected,
            tmp_path / "identity.json",
            frozenset((source_root_control.FINAL_STATE,)),
        )
    )
    assert state == source_root_control.FINAL_STATE
    assert transaction_id == "a" * 32
    assert source_view_digest == f"sha256:{'b' * 64}"


def test_require_identity_record_rejects_provisional_use(
    tmp_path: Path,
) -> None:
    expected = {
        "schema_version": source_root_control.SCHEMA_VERSION,
        "source_commit": "1" * 40,
    }
    recorded = {
        **expected,
        "source_view_digest": source_root_control.PENDING_SOURCE_VIEW_DIGEST,
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
    expected = {
        "schema_version": source_root_control.SCHEMA_VERSION,
        "source_commit": "1" * 40,
    }
    recorded = {
        **expected,
        "source_view_digest": f"sha256:{'b' * 64}",
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
