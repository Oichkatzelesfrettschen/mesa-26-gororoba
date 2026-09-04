# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib.util
import os
import shlex
import stat
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
    monkeypatch.setenv("MESA_TEST_IDENTIFIER", identifier)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.input_identifier("MESA_TEST_IDENTIFIER")


@pytest.mark.parametrize(
    ("legacy_name", "replacement_name"),
    (
        ("GOROROBA_TOPSRC_INPUT", "MESA_TOPSRC_INPUT"),
        ("GOROROBA_MESA_PREFIX", "MESA_INSTALL_PREFIX"),
        ("GOROROBA_MESA_ENV", "MESA_ENV_FILE"),
        ("MESA_GOROROBA_DEPLOY_ACCEPTED", "MESA_DEPLOY_ACCEPTED"),
    ),
)
def test_reject_legacy_environment_names_replacement(
    legacy_name: str,
    replacement_name: str,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv(legacy_name, "fixture")
    with pytest.raises(source_root_control.ControlError, match=replacement_name):
        source_root_control.reject_legacy_environment()


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
    monkeypatch.setenv("MESA_TEST_IDENTIFIER", identifier)
    assert source_root_control.input_identifier("MESA_TEST_IDENTIFIER") == identifier


def test_selected_builddir_state_distinguishes_absence_from_directory_entries(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    selected_builddir = tmp_path / "selected-builddir"
    monkeypatch.setenv("MESA_BUILDDIR_LEXICAL_INPUT", str(selected_builddir))
    assert source_root_control.selected_builddir_state() == "absent"

    selected_builddir.symlink_to(tmp_path / "missing-build-target")
    assert source_root_control.selected_builddir_state() == "present"
    selected_builddir.unlink()

    selected_builddir.mkdir()
    assert source_root_control.selected_builddir_state() == "present"


@pytest.mark.parametrize("value", ("", "stable"))
def test_input_enum_accepts_declared_values(
    value: str,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("MESA_TEST_ENUM", value)
    assert (
        source_root_control.input_enum(
            "MESA_TEST_ENUM",
            frozenset(("", "stable")),
        )
        == value
    )


@pytest.mark.parametrize("value", ("debug", "stable;id", "$(shell id)"))
def test_input_enum_rejects_undeclared_values(
    value: str,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("MESA_TEST_ENUM", value)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.input_enum(
            "MESA_TEST_ENUM",
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
    monkeypatch.setenv("MESA_TEST_DECIMAL", value)
    assert (
        source_root_control.input_decimal(
            "MESA_TEST_DECIMAL",
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
    monkeypatch.setenv("MESA_TEST_DECIMAL", value)
    with pytest.raises(source_root_control.ControlError):
        source_root_control.input_decimal(
            "MESA_TEST_DECIMAL",
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
        "reproducible_run": "0",
    }


def isolate_fixture_build_namespace(
    values: dict[str, Path | str],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    build_root = values["build_root"]
    control_root = values["control_root"]
    assert isinstance(build_root, Path)
    assert isinstance(control_root, Path)
    build_namespace = build_root.parent
    build_namespace.mkdir(mode=0o700, parents=True, exist_ok=True)
    source_root_control.validate_owned_namespace(build_namespace, os.getuid())

    def fixture_owned_build_namespaces(repository_root: Path) -> tuple[Path, ...]:
        assert repository_root == control_root
        return (build_namespace,)

    monkeypatch.setattr(
        source_root_control,
        "owned_build_namespaces",
        fixture_owned_build_namespaces,
    )


def set_captured_revisions(
    values: dict[str, Path | str],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    revision_names = {
        "source_commit": "MESA_SOURCE_COMMIT_CAPTURED",
        "source_tree": "MESA_SOURCE_TREE_CAPTURED",
        "control_commit": "MESA_CONTROL_COMMIT_CAPTURED",
        "control_tree": "MESA_CONTROL_TREE_CAPTURED",
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


def add_detached_worktree(repository: Path, worktree: Path) -> None:
    source_root_control.run_git(
        repository,
        "worktree",
        "add",
        "--detach",
        str(worktree),
        "HEAD",
    )


def source_view_values(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> dict[str, Path | str]:
    values = layout_values(tmp_path)
    isolate_fixture_build_namespace(values, monkeypatch)
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


def initialize_control_identity(values: dict[str, Path | str]) -> None:
    control_root = values["control_root"]
    assert isinstance(control_root, Path)
    committed_repository(control_root)
    values["control_commit"] = source_root_control.run_git(
        control_root,
        "rev-parse",
        "HEAD",
    )
    values["control_tree"] = source_root_control.run_git(
        control_root,
        "rev-parse",
        "HEAD^{tree}",
    )


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


def write_final_identity(
    values: dict[str, Path | str],
    *,
    schema_version: int,
) -> dict[str, str | int]:
    build_root = values["build_root"]
    builddir = values["builddir"]
    assert isinstance(build_root, Path)
    assert isinstance(builddir, Path)
    if schema_version == source_root_control.LEGACY_SCHEMA_VERSION:
        source_view = build_root / source_root_control.LEGACY_SOURCE_VIEW_DIRECTORY
        root_identity = build_root / source_root_control.LEGACY_ROOT_IDENTITY_FILENAME
        build_identity = builddir / source_root_control.LEGACY_IDENTITY_FILENAME
    else:
        assert schema_version == source_root_control.SCHEMA_VERSION
        source_view = source_root_control.source_view_path(values)
        root_identity = source_root_control.root_identity_path(values)
        build_identity = source_root_control.identity_path(values)
    source_view.mkdir(parents=True, exist_ok=True)
    (source_view / "meson.build").write_text(
        "project('identity-fixture')\n",
        encoding="utf-8",
    )
    source_view_digest = source_root_control.source_view_content_digest(source_view)
    current_payload = source_root_control.base_identity_payload(values)
    if schema_version == source_root_control.LEGACY_SCHEMA_VERSION:
        current_payload["source_view"] = str(source_view)
        payload = source_root_control.legacy_identity_payload(current_payload)
    else:
        payload = current_payload
    record = source_root_control.identity_record(
        payload,
        source_root_control.FINAL_STATE,
        "a" * 32,
        source_view_digest,
    )
    builddir.mkdir(parents=True, exist_ok=True)
    source_root_control.write_json_atomic(
        root_identity,
        record,
    )
    source_root_control.write_json_atomic(
        build_identity,
        record,
    )
    return record


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
    values = layout_values(tmp_path)
    isolate_fixture_build_namespace(values, monkeypatch)
    source_root_control.validate_layout("build", values)


@pytest.mark.parametrize("operation", ("build", "clean", "configure", "test"))
def test_validate_layout_rejects_builddir_inside_source_view(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    operation: str,
) -> None:
    values = layout_values(tmp_path)
    isolate_fixture_build_namespace(values, monkeypatch)
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
    values = layout_values(tmp_path)
    isolate_fixture_build_namespace(values, monkeypatch)
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


def write_git_repository_marker(worktree_root: Path) -> Path:
    """Write the .git directory shape git gives a primary checkout.

    A marker is a repository only if it carries HEAD beside objects/ and
    refs/, so a fixture that creates an empty .git directory stands in
    for nothing git writes.
    """
    marker = worktree_root / ".git"
    (marker / "objects").mkdir(parents=True)
    (marker / "refs").mkdir(parents=True)
    (marker / "HEAD").write_text("ref: refs/heads/main\n")
    return marker


def write_git_linked_worktree_marker(worktree_root: Path,
                                     metadata: Path) -> Path:
    """Write the .git file shape git gives a linked worktree.

    The metadata directory is written too, because the marker names it
    and a marker naming an absent path is a different fixture.
    """
    metadata.mkdir(parents=True, exist_ok=True)
    (metadata / "HEAD").write_text("ref: refs/heads/main\n")
    (metadata / "commondir").write_text("../..\n")
    (metadata / "gitdir").write_text(f"{worktree_root / '.git'}\n")
    worktree_root.mkdir(parents=True, exist_ok=True)
    marker = worktree_root / ".git"
    marker.write_text(f"gitdir: {metadata}\n")
    return marker


def test_validate_layout_rejects_peer_git_worktree(
    tmp_path: Path,
) -> None:
    values = layout_values(tmp_path)
    peer_root = tmp_path / ".mesa-26-gororoba-builds" / "peer"
    peer_root.mkdir(parents=True)
    write_git_repository_marker(peer_root)
    values["build_root"] = peer_root / "build-output"
    values["builddir"] = peer_root / "build-output" / "build"
    values["prefix"] = peer_root / "build-output" / "prefix"
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_layout("build", values)


def test_validate_layout_rejects_peer_linked_git_worktree(
    tmp_path: Path,
) -> None:
    """A linked worktree bounds a build root exactly as a checkout does."""
    values = layout_values(tmp_path)
    peer_root = tmp_path / ".mesa-26-gororoba-builds" / "linked-peer"
    write_git_linked_worktree_marker(
        peer_root, tmp_path / "elsewhere" / ".git" / "worktrees" / "peer")
    values["build_root"] = peer_root / "build-output"
    values["builddir"] = peer_root / "build-output" / "build"
    values["prefix"] = peer_root / "build-output" / "prefix"
    with pytest.raises(source_root_control.ControlError):
        source_root_control.validate_layout("build", values)


def test_validate_layout_accepts_build_root_under_a_dangling_gitdir_marker(
    tmp_path: Path,
) -> None:
    """A gitdir line naming an absent path names no repository."""
    values = layout_values(tmp_path)
    peer_root = tmp_path / ".mesa-26-gororoba-builds" / "dangling"
    peer_root.mkdir(parents=True)
    (peer_root / ".git").write_text(
        f"gitdir: {tmp_path / 'absent' / 'worktrees' / 'gone'}\n")
    values["build_root"] = peer_root / "build-output"
    values["builddir"] = peer_root / "build-output" / "build"
    values["prefix"] = peer_root / "build-output" / "prefix"
    source_root_control.validate_layout("build", values)


def test_validate_layout_accepts_build_root_under_an_empty_git_marker(
    tmp_path: Path,
) -> None:
    """An empty .git directory names no repository, so it bounds nothing.

    Any process may create one under a shared temporary root, and when a
    bare existence check treated it as a worktree every build root
    beneath that root refused.
    """
    values = layout_values(tmp_path)
    peer_root = tmp_path / ".mesa-26-gororoba-builds" / "not-a-worktree"
    (peer_root / ".git").mkdir(parents=True)
    values["build_root"] = peer_root / "build-output"
    values["builddir"] = peer_root / "build-output" / "build"
    values["prefix"] = peer_root / "build-output" / "prefix"
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
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = layout_values(tmp_path)
    isolate_fixture_build_namespace(values, monkeypatch)
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
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = layout_values(tmp_path)
    isolate_fixture_build_namespace(values, monkeypatch)
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
    with pytest.raises(
        source_root_control.ControlError,
        match="root cannot own Mesa build namespaces",
    ):
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
        "source_root": "MESA_TOPSRC_INPUT",
        "control_root": "MESA_CONTROL_ROOT_INPUT",
        "build_root": "MESA_BUILD_ROOT_INPUT",
        "builddir": "MESA_BUILDDIR_INPUT",
        "prefix": "MESA_PREFIX_INPUT",
        "sysconfdir": "MESA_SYSCONFDIR_INPUT",
    }
    anchor_names = {
        "source_root": "MESA_SOURCE_ROOT_ANCHOR",
        "control_root": "MESA_CONTROL_ROOT_ANCHOR",
        "build_root": "MESA_BUILD_ROOT_ANCHOR",
        "builddir": "MESA_BUILDDIR_ANCHOR",
        "prefix": "MESA_PREFIX_ANCHOR",
        "sysconfdir": "MESA_SYSCONFDIR_ANCHOR",
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
    monkeypatch.setenv("MESA_BUILDDIR_INPUT", str(captured_path))
    monkeypatch.setenv(
        "MESA_BUILDDIR_ANCHOR",
        source_root_control.path_anchor(selected_path),
    )
    with pytest.raises(source_root_control.ControlError):
        source_root_control.require_captured_inputs(values, ("builddir",))


@pytest.mark.parametrize(
    ("field", "variable_name"),
    (
        ("source_commit", "MESA_SOURCE_COMMIT_CAPTURED"),
        ("source_tree", "MESA_SOURCE_TREE_CAPTURED"),
        ("control_commit", "MESA_CONTROL_COMMIT_CAPTURED"),
        ("control_tree", "MESA_CONTROL_TREE_CAPTURED"),
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


def test_require_clean_worktree_rejects_removed_owner_execute_bit(
    tmp_path: Path,
) -> None:
    repository = tmp_path / "repository"
    source_file = committed_repository(repository)
    source_file.chmod(0o755)
    commit_repository(repository, "test: make source executable")
    source_root_control.require_clean_worktree(repository, "test worktree")

    source_file.chmod(0o455)
    with pytest.raises(source_root_control.ControlError, match="is dirty"):
        source_root_control.require_clean_worktree(repository, "test worktree")


def test_require_clean_worktree_ignores_non_owner_execute_bits(
    tmp_path: Path,
) -> None:
    repository = tmp_path / "repository"
    source_file = committed_repository(repository)
    source_file.chmod(0o655)

    source_root_control.require_clean_worktree(repository, "test worktree")


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


def test_prepare_source_view_excludes_peer_write_after_clean_preflight(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    source_root = values["source_root"]
    source_commit = values["source_commit"]
    assert isinstance(source_root, Path)
    assert isinstance(source_commit, str)
    source_root_control.require_clean_worktree(source_root, "test worktree")
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_provisional_identity(values)

    tracked_source = source_root / "tracked-source"
    original_run_git_archive = source_root_control.run_git_archive

    def mutate_source_then_archive(
        repository_root: Path,
        selected_commit: str,
        archive_path: Path,
    ) -> None:
        tracked_source.write_text("peer mutation\n", encoding="utf-8")
        original_run_git_archive(
            repository_root,
            selected_commit,
            archive_path,
        )

    monkeypatch.setattr(
        source_root_control,
        "run_git_archive",
        mutate_source_then_archive,
    )

    source_root_control.prepare_source_view(values)

    source_view = source_root_control.source_view_path(values)
    assert tracked_source.read_text(encoding="utf-8") == "peer mutation\n"
    assert (source_view / "tracked-source").read_text(encoding="utf-8") == (
        "archive input\n"
    )


def test_prepare_source_view_archives_exact_source_and_replaces_owned_view(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
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
    values = source_view_values(tmp_path, monkeypatch)
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
    values = source_view_values(tmp_path, monkeypatch)
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


@pytest.mark.parametrize("allow_provisional", (False, True))
def test_cleanup_rejects_absent_build_without_root_identity(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    allow_provisional: bool,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    builddir = values["builddir"]
    assert isinstance(builddir, Path)
    assert not builddir.exists()
    assert not source_root_control.root_identity_path(values).exists()

    with pytest.raises(
        source_root_control.ControlError,
        match="external build root lacks source identity",
    ):
        source_root_control.verify_delete_identity(
            values,
            allow_provisional=allow_provisional,
        )


def test_provisional_cleanup_accepts_mesons_source_view_population(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
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
        "#!/bin/sh\n: > \"$MESA_FSMONITOR_MARKER\"\nprintf '\\n'\n",
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
    monkeypatch.setenv("MESA_FSMONITOR_MARKER", str(marker))
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


def test_reproducible_source_worktrees_reject_control_as_topsrc(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    control_root = tmp_path / "control"
    monkeypatch.setattr(
        source_root_control,
        "control_root",
        lambda: control_root,
    )
    with pytest.raises(
        source_root_control.ControlError,
        match="TOPSRC must be an external detached worktree",
    ):
        source_root_control.require_reproducible_source_worktrees(control_root)


def test_reproducible_source_worktrees_reject_filesystem_alias(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    repository = tmp_path / "repository"
    committed_repository(repository)
    detached_worktree = tmp_path / "detached-worktree"
    add_detached_worktree(repository, detached_worktree)
    worktree_alias = tmp_path / "worktree-alias"
    worktree_alias.symlink_to(detached_worktree, target_is_directory=True)
    monkeypatch.setattr(
        source_root_control,
        "control_root",
        lambda: worktree_alias,
    )

    with pytest.raises(
        source_root_control.ControlError,
        match="same filesystem directory",
    ):
        source_root_control.require_reproducible_source_worktrees(detached_worktree)


def test_reproducible_source_worktrees_reject_shared_git_administration(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    control_root = tmp_path / "control"
    source_root = tmp_path / "source"
    control_root.mkdir()
    source_root.mkdir()
    monkeypatch.setattr(source_root_control, "control_root", lambda: control_root)
    monkeypatch.setattr(
        source_root_control,
        "git_worktree_administration_identity",
        lambda _root, _label: (1, 2, stat.S_IFDIR),
    )

    with pytest.raises(
        source_root_control.ControlError,
        match="share one Git administrative directory",
    ):
        source_root_control.require_reproducible_source_worktrees(source_root)


@pytest.mark.parametrize("attached_role", ("control", "source"))
def test_reproducible_source_worktrees_reject_attached_worktree(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    attached_role: str,
) -> None:
    repository = tmp_path / "repository"
    committed_repository(repository)
    detached_control = tmp_path / "detached-control"
    detached_source = tmp_path / "detached-source"
    add_detached_worktree(repository, detached_control)
    add_detached_worktree(repository, detached_source)
    control_root = repository if attached_role == "control" else detached_control
    source_root = repository if attached_role == "source" else detached_source
    monkeypatch.setattr(
        source_root_control,
        "control_root",
        lambda: control_root,
    )
    with pytest.raises(
        source_root_control.ControlError,
        match="attached to a branch",
    ):
        source_root_control.require_reproducible_source_worktrees(source_root)


def test_reproducible_source_worktrees_accept_clean_detached_trees_and_reject_ignored_source(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    repository = tmp_path / "repository"
    committed_repository(repository)
    subprojects = repository / "subprojects"
    subprojects.mkdir()
    (subprojects / ".gitignore").write_text("/*/\n", encoding="utf-8")
    (repository / ".gitignore").write_text("/build/\n", encoding="utf-8")
    commit_repository(repository, "test: classify ignored source inputs")
    control_root = tmp_path / "detached-control"
    source_root = tmp_path / "detached-source"
    add_detached_worktree(repository, control_root)
    add_detached_worktree(repository, source_root)
    monkeypatch.setattr(
        source_root_control,
        "control_root",
        lambda: control_root,
    )
    ignored_build_output = source_root / "build" / "artifact"
    ignored_build_output.parent.mkdir()
    ignored_build_output.write_text("regenerable\n", encoding="utf-8")

    source_root_control.require_reproducible_source_worktrees(source_root)

    ignored_source = source_root / "subprojects" / "Vulkan-Profiles" / "meson.build"
    ignored_source.parent.mkdir()
    ignored_source.write_text("project('ignored-input')\n", encoding="utf-8")
    with pytest.raises(source_root_control.ControlError, match="is dirty"):
        source_root_control.require_reproducible_source_worktrees(source_root)


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


def test_require_identity_fields_rejects_reproducible_run_relabel(
    tmp_path: Path,
) -> None:
    expected = {
        "schema_version": source_root_control.SCHEMA_VERSION,
        "reproducible_run": "1",
    }
    recorded = {
        "schema_version": source_root_control.SCHEMA_VERSION,
        "reproducible_run": "0",
    }
    with pytest.raises(
        source_root_control.ControlError,
        match="identity drift: reproducible_run",
    ):
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
            "reproducible_run",
        )
    } == {
        "profile": "test-profile",
        "hostenv": "test-hostenv",
        "mode": "default",
        "compiler_chain": "direct",
        "compiler_family": "llvm",
        "reproducible_run": "0",
    }


def test_base_identity_payload_rechecks_reproducible_source_worktrees(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = layout_values(tmp_path)
    values["reproducible_run"] = "1"
    source_root = values["source_root"]
    assert isinstance(source_root, Path)
    checked: list[Path] = []
    monkeypatch.setattr(
        source_root_control,
        "require_reproducible_source_worktrees",
        checked.append,
    )
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: pytest.fail("ordinary source gate selected"),
    )

    payload = source_root_control.base_identity_payload(values)

    assert checked == [source_root]
    assert payload["reproducible_run"] == "1"


def test_remove_identity_v7_build_root_removes_exact_verified_root(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    initialize_control_identity(values)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_final_identity(
        values,
        schema_version=source_root_control.LEGACY_SCHEMA_VERSION,
    )
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    assert build_root.is_dir()

    source_root_control.remove_identity_v7_build_root(values)

    assert not build_root.exists()


def test_remove_identity_v7_build_root_preserves_swapped_directory(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    initialize_control_identity(values)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_final_identity(
        values,
        schema_version=source_root_control.LEGACY_SCHEMA_VERSION,
    )
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    preserved_validated_root = tmp_path / "preserved-validated-root"
    replacement_root = tmp_path / "replacement-root"
    replacement_root.mkdir()
    (replacement_root / "user-data").write_text("preserve\n", encoding="utf-8")
    real_rename = os.rename
    swapped = False

    def swap_before_quarantine(
        source: str | Path,
        target: str | Path,
        *,
        src_dir_fd: int | None = None,
        dst_dir_fd: int | None = None,
    ) -> None:
        nonlocal swapped
        if (
            source == build_root.name
            and src_dir_fd is not None
            and dst_dir_fd is not None
            and not swapped
        ):
            swapped = True
            real_rename(build_root, preserved_validated_root)
            real_rename(replacement_root, build_root)
        real_rename(
            source,
            target,
            src_dir_fd=src_dir_fd,
            dst_dir_fd=dst_dir_fd,
        )

    monkeypatch.setattr(source_root_control.os, "rename", swap_before_quarantine)

    with pytest.raises(
        source_root_control.ControlError,
        match="changed between validation and quarantine",
    ):
        source_root_control.remove_identity_v7_build_root(values)

    assert (build_root / "user-data").read_text(encoding="utf-8") == "preserve\n"
    assert (
        preserved_validated_root / source_root_control.LEGACY_ROOT_IDENTITY_FILENAME
    ).is_file()


def test_remove_identity_v7_build_root_revalidates_quarantine(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    initialize_control_identity(values)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_final_identity(
        values,
        schema_version=source_root_control.LEGACY_SCHEMA_VERSION,
    )
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    real_validate = source_root_control.validate_identity_v7_build_root_candidate
    validation_count = 0

    def mutate_before_revalidation(
        candidate_values: dict[str, Path | str],
        candidate_root: Path,
    ) -> None:
        nonlocal validation_count
        validation_count += 1
        if validation_count == 2:
            (candidate_root / "late-output").write_text(
                "preserve\n",
                encoding="utf-8",
            )
        real_validate(candidate_values, candidate_root)

    monkeypatch.setattr(
        source_root_control,
        "validate_identity_v7_build_root_candidate",
        mutate_before_revalidation,
    )

    with pytest.raises(
        source_root_control.ControlError,
        match="not an exact cleanup target: unexpected",
    ):
        source_root_control.remove_identity_v7_build_root(values)

    assert validation_count == 2
    assert (build_root / "late-output").read_text(encoding="utf-8") == "preserve\n"


def test_remove_identity_v7_build_root_preserves_late_swapped_directory(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    initialize_control_identity(values)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_final_identity(
        values,
        schema_version=source_root_control.LEGACY_SCHEMA_VERSION,
    )
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    replacement_root = tmp_path / "late-replacement-root"
    replacement_root.mkdir()
    (replacement_root / "user-data").write_text("preserve\n", encoding="utf-8")
    preserved_validated_root = tmp_path / "late-preserved-validated-root"
    real_scandir = os.scandir
    real_rename = os.rename
    swapped = False

    def swap_immediately_before_descriptor_deletion(
        path: str | bytes | int | os.PathLike[str] | os.PathLike[bytes],
    ):
        nonlocal swapped
        if isinstance(path, int) and not swapped:
            quarantine_directories = tuple(
                build_root.parent.glob(f".{build_root.name}.schema-7-cleanup.*")
            )
            assert len(quarantine_directories) == 1
            quarantined_root = quarantine_directories[0] / "validated-build-root"
            swapped = True
            real_rename(quarantined_root, preserved_validated_root)
            real_rename(replacement_root, quarantined_root)
        return real_scandir(path)

    monkeypatch.setattr(
        source_root_control.os,
        "scandir",
        swap_immediately_before_descriptor_deletion,
    )

    with pytest.raises(
        source_root_control.ControlError,
        match="changed during quarantine validation",
    ):
        source_root_control.remove_identity_v7_build_root(values)

    assert swapped
    quarantine_directories = tuple(
        build_root.parent.glob(f".{build_root.name}.schema-7-cleanup.*")
    )
    assert len(quarantine_directories) == 1
    replacement_data = quarantine_directories[0] / "validated-build-root" / "user-data"
    assert replacement_data.read_text(encoding="utf-8") == "preserve\n"


def test_remove_identity_v7_build_root_preserves_late_file_insertion(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    initialize_control_identity(values)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_final_identity(
        values,
        schema_version=source_root_control.LEGACY_SCHEMA_VERSION,
    )
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    real_rename = os.rename
    inserted = False

    def insert_before_first_private_move(
        source: str | Path,
        target: str | Path,
        *,
        src_dir_fd: int | None = None,
        dst_dir_fd: int | None = None,
    ) -> None:
        nonlocal inserted
        if (
            isinstance(target, str)
            and target.startswith("validated-object-")
            and not inserted
        ):
            quarantine_directories = tuple(
                build_root.parent.glob(f".{build_root.name}.schema-7-cleanup.*")
            )
            assert len(quarantine_directories) == 1
            quarantined_root = quarantine_directories[0] / "validated-build-root"
            (quarantined_root / "late-user-data").write_text(
                "preserve\n",
                encoding="utf-8",
            )
            inserted = True
        real_rename(
            source,
            target,
            src_dir_fd=src_dir_fd,
            dst_dir_fd=dst_dir_fd,
        )

    monkeypatch.setattr(
        source_root_control.os, "rename", insert_before_first_private_move
    )

    with pytest.raises(
        source_root_control.ControlError,
        match="gained entries during cleanup",
    ):
        source_root_control.remove_identity_v7_build_root(values)

    assert inserted
    quarantine_directories = tuple(
        build_root.parent.glob(f".{build_root.name}.schema-7-cleanup.*")
    )
    assert len(quarantine_directories) == 1
    late_data = quarantine_directories[0] / "validated-build-root" / "late-user-data"
    assert late_data.read_text(encoding="utf-8") == "preserve\n"


def test_remove_identity_v7_build_root_preserves_final_file_replacement(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    initialize_control_identity(values)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_final_identity(
        values,
        schema_version=source_root_control.LEGACY_SCHEMA_VERSION,
    )
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    real_unlink = os.unlink
    replaced = False

    def replace_vacated_file_before_unlink(
        path: str | bytes | Path,
        *,
        dir_fd: int | None = None,
    ) -> None:
        nonlocal replaced
        if (
            isinstance(path, str)
            and path.startswith("validated-object-")
            and dir_fd is not None
            and not replaced
        ):
            quarantine_directories = tuple(
                build_root.parent.glob(f".{build_root.name}.schema-7-cleanup.*")
            )
            assert len(quarantine_directories) == 1
            quarantined_root = quarantine_directories[0] / "validated-build-root"
            replacement = (
                quarantined_root / source_root_control.LEGACY_ROOT_IDENTITY_FILENAME
            )
            replacement.write_text("replacement-user-data\n", encoding="utf-8")
            replaced = True
        real_unlink(path, dir_fd=dir_fd)

    monkeypatch.setattr(
        source_root_control.os,
        "unlink",
        replace_vacated_file_before_unlink,
    )

    with pytest.raises(
        source_root_control.ControlError,
        match="gained entries during cleanup",
    ):
        source_root_control.remove_identity_v7_build_root(values)

    assert replaced
    quarantine_directories = tuple(
        build_root.parent.glob(f".{build_root.name}.schema-7-cleanup.*")
    )
    assert len(quarantine_directories) == 1
    replacement = (
        quarantine_directories[0]
        / "validated-build-root"
        / source_root_control.LEGACY_ROOT_IDENTITY_FILENAME
    )
    assert replacement.read_text(encoding="utf-8") == "replacement-user-data\n"


def test_remove_identity_v7_build_root_preserves_final_directory_replacement(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    initialize_control_identity(values)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_final_identity(
        values,
        schema_version=source_root_control.LEGACY_SCHEMA_VERSION,
    )
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    real_rmdir = os.rmdir
    replaced = False

    def replace_vacated_directory_before_rmdir(
        path: str | bytes | Path,
        *,
        dir_fd: int | None = None,
    ) -> None:
        nonlocal replaced
        if (
            isinstance(path, str)
            and path.startswith("validated-object-")
            and dir_fd is not None
            and not replaced
        ):
            quarantine_directories = tuple(
                build_root.parent.glob(f".{build_root.name}.schema-7-cleanup.*")
            )
            assert len(quarantine_directories) == 1
            quarantined_root = quarantine_directories[0] / "validated-build-root"
            replacement = (
                quarantined_root / source_root_control.LEGACY_SOURCE_VIEW_DIRECTORY
            )
            replacement.mkdir()
            (replacement / "user-data").write_text(
                "replacement-directory\n",
                encoding="utf-8",
            )
            replaced = True
        real_rmdir(path, dir_fd=dir_fd)

    monkeypatch.setattr(
        source_root_control.os,
        "rmdir",
        replace_vacated_directory_before_rmdir,
    )

    with pytest.raises(
        source_root_control.ControlError,
        match="gained entries during cleanup",
    ):
        source_root_control.remove_identity_v7_build_root(values)

    assert replaced
    quarantine_directories = tuple(
        build_root.parent.glob(f".{build_root.name}.schema-7-cleanup.*")
    )
    assert len(quarantine_directories) == 1
    replacement_data = (
        quarantine_directories[0]
        / "validated-build-root"
        / source_root_control.LEGACY_SOURCE_VIEW_DIRECTORY
        / "user-data"
    )
    assert replacement_data.read_text(encoding="utf-8") == "replacement-directory\n"


def test_remove_identity_v7_build_root_preserves_destination_recreated_before_restore(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    initialize_control_identity(values)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_final_identity(
        values,
        schema_version=source_root_control.LEGACY_SCHEMA_VERSION,
    )
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    preserved_original = tmp_path / "preserved-original-identity"
    replacement_entry = tmp_path / "replacement-identity"
    replacement_entry.write_text("replacement-before-move\n", encoding="utf-8")
    real_rename = os.rename
    real_rename_without_replacement = source_root_control.rename_without_replacement
    swapped = False
    recreated = False

    def swap_before_private_move(
        source: str | Path,
        target: str | Path,
        *,
        src_dir_fd: int | None = None,
        dst_dir_fd: int | None = None,
    ) -> None:
        nonlocal swapped
        if (
            source == source_root_control.LEGACY_ROOT_IDENTITY_FILENAME
            and isinstance(target, str)
            and target.startswith("validated-object-")
            and not swapped
        ):
            quarantine_directories = tuple(
                build_root.parent.glob(f".{build_root.name}.schema-7-cleanup.*")
            )
            assert len(quarantine_directories) == 1
            quarantined_root = quarantine_directories[0] / "validated-build-root"
            original_entry = (
                quarantined_root / source_root_control.LEGACY_ROOT_IDENTITY_FILENAME
            )
            real_rename(original_entry, preserved_original)
            real_rename(replacement_entry, original_entry)
            swapped = True
        real_rename(
            source,
            target,
            src_dir_fd=src_dir_fd,
            dst_dir_fd=dst_dir_fd,
        )

    def recreate_destination_before_restore(
        source_name: str,
        destination_name: str,
        *,
        source_directory_descriptor: int,
        destination_directory_descriptor: int,
    ) -> None:
        nonlocal recreated
        if (
            destination_name == source_root_control.LEGACY_ROOT_IDENTITY_FILENAME
            and not recreated
        ):
            destination_descriptor = os.open(
                destination_name,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                0o600,
                dir_fd=destination_directory_descriptor,
            )
            try:
                os.write(destination_descriptor, b"late-user-data\n")
            finally:
                os.close(destination_descriptor)
            recreated = True
        real_rename_without_replacement(
            source_name,
            destination_name,
            source_directory_descriptor=source_directory_descriptor,
            destination_directory_descriptor=destination_directory_descriptor,
        )

    monkeypatch.setattr(source_root_control.os, "rename", swap_before_private_move)
    monkeypatch.setattr(
        source_root_control,
        "rename_without_replacement",
        recreate_destination_before_restore,
    )

    with pytest.raises(
        source_root_control.ControlError,
        match="cannot restore changed",
    ):
        source_root_control.remove_identity_v7_build_root(values)

    assert swapped
    assert recreated
    quarantine_directories = tuple(
        build_root.parent.glob(f".{build_root.name}.schema-7-cleanup.*")
    )
    assert len(quarantine_directories) == 1
    quarantined_root = quarantine_directories[0] / "validated-build-root"
    recreated_destination = (
        quarantined_root / source_root_control.LEGACY_ROOT_IDENTITY_FILENAME
    )
    assert recreated_destination.read_text(encoding="utf-8") == "late-user-data\n"
    staged_replacements = tuple(
        quarantine_directories[0].glob(
            ".schema-7-validated-objects-*/validated-object-*"
        )
    )
    assert len(staged_replacements) == 1
    assert staged_replacements[0].read_text(encoding="utf-8") == (
        "replacement-before-move\n"
    )
    assert preserved_original.is_file()


def test_remove_identity_v7_build_root_rejects_reproducible_mode(
    tmp_path: Path,
) -> None:
    values = layout_values(tmp_path)
    values["reproducible_run"] = "1"
    with pytest.raises(
        source_root_control.ControlError,
        match="requires REPRODUCIBLE_RUN=0",
    ):
        source_root_control.remove_identity_v7_build_root(values)


def test_remove_identity_v7_build_root_rejects_source_view_drift(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    initialize_control_identity(values)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_final_identity(
        values,
        schema_version=source_root_control.LEGACY_SCHEMA_VERSION,
    )
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    source_view = build_root / source_root_control.LEGACY_SOURCE_VIEW_DIRECTORY
    (source_view / "meson.build").write_text(
        "project('drifted-identity-fixture')\n",
        encoding="utf-8",
    )

    with pytest.raises(source_root_control.ControlError, match="content drift"):
        source_root_control.remove_identity_v7_build_root(values)


def test_remove_identity_v7_build_root_rejects_unexpected_entry(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    initialize_control_identity(values)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_final_identity(
        values,
        schema_version=source_root_control.LEGACY_SCHEMA_VERSION,
    )
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    (build_root / "retained-output").write_text("preserve\n", encoding="utf-8")

    with pytest.raises(
        source_root_control.ControlError,
        match="not an exact cleanup target: unexpected",
    ):
        source_root_control.remove_identity_v7_build_root(values)

    assert build_root.is_dir()


def test_remove_identity_v7_build_root_rejects_schema_8(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = source_view_values(tmp_path, monkeypatch)
    initialize_control_identity(values)
    monkeypatch.setattr(
        source_root_control,
        "require_clean_external_source",
        lambda _source_root: None,
    )
    write_final_identity(
        values,
        schema_version=source_root_control.LEGACY_SCHEMA_VERSION,
    )
    build_root = values["build_root"]
    builddir = values["builddir"]
    assert isinstance(build_root, Path)
    assert isinstance(builddir, Path)
    root_identity = build_root / source_root_control.LEGACY_ROOT_IDENTITY_FILENAME
    build_identity = builddir / source_root_control.LEGACY_IDENTITY_FILENAME
    for identity_file in (root_identity, build_identity):
        forged_record = source_root_control.read_identity(identity_file)
        forged_record["schema_version"] = source_root_control.SCHEMA_VERSION
        source_root_control.write_json_atomic(identity_file, forged_record)

    with pytest.raises(
        source_root_control.ControlError,
        match="identity drift: schema_version",
    ):
        source_root_control.remove_identity_v7_build_root(values)

    assert build_root.is_dir()


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
