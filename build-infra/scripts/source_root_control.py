#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bind an immutable external Mesa source view to one build identity."""

from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import os
import pwd
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from typing import NoReturn

SCHEMA_VERSION = 7
IDENTITY_FILENAME = ".gororoba-source-identity.json"
ROOT_IDENTITY_FILENAME = ".gororoba-external-source-identity.json"
SOURCE_VIEW_DIRECTORY = ".gororoba-source-view"
SAFE_PATH_INPUT = re.compile(r"^[A-Za-z0-9_./:+@=~-]+$")
SAFE_LEAF_IDENTIFIER = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.+-]*$")
DECIMAL_INTEGER = re.compile(r"^[0-9]+$")
GNU_MAKE_VERSION = re.compile(r"^[0-9]+(?:\.[0-9]+)+$")
TRANSACTION_ID = re.compile(r"^[0-9a-f]{32}$")
SHA256_VALUE = re.compile(r"^[0-9a-fA-F]{64}$")
SOURCE_VIEW_DIGEST = re.compile(r"^sha256:[0-9a-f]{64}$")
PENDING_SOURCE_VIEW_DIGEST = "pending"
PROVISIONAL_STATE = "provisional"
FINAL_STATE = "final"
MOUNTINFO_PATH = Path("/proc/self/mountinfo")
SHARED_TEMPORARY_BOUNDARIES = (Path("/tmp"), Path("/var/tmp"))


class ControlError(RuntimeError):
    """A fail-closed source-root or path-control error."""


def fail(message: str) -> NoReturn:
    raise ControlError(message)


def control_root() -> Path:
    return Path(__file__).resolve().parents[2]


def input_path(name: str) -> Path:
    raw = os.environ.get(name, "")
    if not raw:
        fail(f"{name} is empty")
    if not SAFE_PATH_INPUT.fullmatch(raw):
        fail(f"{name} contains unsupported path characters")
    resolved = Path(raw).expanduser().resolve(strict=False)
    if not SAFE_PATH_INPUT.fullmatch(str(resolved)):
        fail(f"{name} resolves to a path with unsupported characters")
    return resolved


def input_identifier(name: str) -> str:
    raw = os.environ.get(name, "")
    if not raw or raw in {".", ".."} or SAFE_LEAF_IDENTIFIER.fullmatch(raw) is None:
        fail(f"{name} contains an invalid leaf identifier")
    return raw


def input_enum(
    name: str,
    allowed_values: frozenset[str],
) -> str:
    raw = os.environ.get(name, "")
    if raw not in allowed_values:
        allowed_text = ", ".join(
            "default" if value == "" else value for value in sorted(allowed_values)
        )
        fail(f"{name} must be one of: {allowed_text}")
    return raw


def input_decimal(name: str, *, minimum: int) -> str:
    raw = os.environ.get(name, "")
    if DECIMAL_INTEGER.fullmatch(raw) is None:
        fail(f"{name} must be a decimal integer")
    value = int(raw)
    if value < minimum:
        fail(f"{name} must be at least {minimum}")
    return str(value)


def validate_make_version(version: str) -> None:
    if GNU_MAKE_VERSION.fullmatch(version) is None:
        fail(f"GNU Make version is invalid: {version}")
    components = tuple(int(component) for component in version.split("."))
    if components < (4, 2):
        fail(f"GNU Make 4.2 or newer is required; found {version}")


def git_command(root: Path, *arguments: str) -> list[str]:
    return [
        "git",
        "--no-optional-locks",
        "-c",
        "core.fsmonitor=false",
        "-c",
        "core.hooksPath=/dev/null",
        "-c",
        "core.untrackedCache=false",
        "-C",
        str(root),
        *arguments,
    ]


def git_environment(
    extra_environment: dict[str, str] | None = None,
) -> dict[str, str]:
    environment = {
        name: value for name, value in os.environ.items() if not name.startswith("GIT_")
    }
    environment.update(
        {
            "GIT_ATTR_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_SYSTEM": os.devnull,
            "GIT_NO_REPLACE_OBJECTS": "1",
            "GIT_OPTIONAL_LOCKS": "0",
            "GIT_TERMINAL_PROMPT": "0",
        }
    )
    if extra_environment is not None:
        environment.update(extra_environment)
    return environment


def run_git_process(
    root: Path,
    *arguments: str,
    extra_environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    command = git_command(root, *arguments)
    return subprocess.run(
        command,
        check=False,
        capture_output=True,
        env=git_environment(extra_environment),
        shell=False,
        text=True,
    )


def run_git(
    root: Path,
    *arguments: str,
    extra_environment: dict[str, str] | None = None,
) -> str:
    command = git_command(root, *arguments)
    result = run_git_process(
        root,
        *arguments,
        extra_environment=extra_environment,
    )
    if result.returncode != 0:
        diagnostic = result.stderr.strip() or result.stdout.strip()
        fail(f"{' '.join(command)} failed: {diagnostic}")
    return result.stdout.strip()


def run_git_bytes(
    root: Path,
    *arguments: str,
    input_data: bytes | None = None,
    extra_environment: dict[str, str] | None = None,
) -> bytes:
    command = git_command(root, *arguments)
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        env=git_environment(extra_environment),
        input=input_data,
        shell=False,
    )
    if result.returncode != 0:
        diagnostic_bytes = result.stderr.strip() or result.stdout.strip()
        diagnostic = diagnostic_bytes.decode("utf-8", errors="replace")
        fail(f"{' '.join(command)} failed: {diagnostic}")
    return result.stdout


def source_identity(source_root: Path) -> tuple[str, str]:
    if not source_root.is_dir():
        fail(f"TOPSRC is not a directory: {source_root}")
    if not (source_root / "meson.build").is_file():
        fail(f"TOPSRC lacks meson.build: {source_root}")
    if not (
        (source_root / "meson.options").is_file()
        or (source_root / "meson_options.txt").is_file()
    ):
        fail(f"TOPSRC lacks meson.options or meson_options.txt: {source_root}")

    git_root_text = run_git(source_root, "rev-parse", "--show-toplevel")
    git_root = Path(git_root_text).resolve(strict=True)
    if git_root != source_root:
        fail(f"TOPSRC is not the Git worktree root: {source_root} (root {git_root})")

    commit = run_git(source_root, "rev-parse", "--verify", "HEAD^{commit}")
    tree = run_git(source_root, "rev-parse", "--verify", "HEAD^{tree}")
    return commit, tree


def file_identity(path_status: os.stat_result) -> tuple[int, int, int, int, int]:
    return (
        path_status.st_dev,
        path_status.st_ino,
        path_status.st_mode,
        path_status.st_size,
        path_status.st_mtime_ns,
    )


def isolated_object_database_environment(
    root: Path,
    temporary_root: Path,
) -> dict[str, str]:
    object_format = run_git(root, "rev-parse", "--show-object-format")
    if object_format not in {"sha1", "sha256"}:
        fail(f"unsupported Git object format: {object_format}")
    object_directory = Path(
        run_git(
            root,
            "rev-parse",
            "--path-format=absolute",
            "--git-path",
            "objects",
        )
    ).resolve(strict=True)
    isolated_git_directory = temporary_root / "isolated.git"
    command = [
        "git",
        "init",
        "--bare",
        "--quiet",
        f"--object-format={object_format}",
        str(isolated_git_directory),
    ]
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        cwd=temporary_root,
        env=git_environment(),
        shell=False,
        text=True,
    )
    if result.returncode != 0:
        diagnostic = result.stderr.strip() or result.stdout.strip()
        fail(f"{' '.join(command)} failed: {diagnostic}")
    return {
        "GIT_DIR": str(isolated_git_directory),
        "GIT_OBJECT_DIRECTORY": str(object_directory),
    }


def isolated_checkout_environment(
    root: Path,
    index_environment: dict[str, str],
    temporary_root: Path,
) -> dict[str, str]:
    return {
        **index_environment,
        **isolated_object_database_environment(root, temporary_root),
        "GIT_WORK_TREE": str(root),
    }


def run_git_archive(root: Path, commit: str, output: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="gororoba-git-archive.") as directory:
        archive_environment = isolated_object_database_environment(
            root,
            Path(directory),
        )
        run_git(
            root,
            "archive",
            "--format=tar",
            f"--output={output}",
            commit,
            extra_environment=archive_environment,
        )


def expected_checkout_bytes(
    root: Path,
    relative_path_bytes: bytes,
    expected_object_id: bytes,
    checkout_environment: dict[str, str],
) -> bytes:
    relative_path = os.fsdecode(relative_path_bytes)
    return run_git_bytes(
        root,
        "cat-file",
        "--filters",
        f"--path={relative_path}",
        expected_object_id.decode("ascii"),
        extra_environment=checkout_environment,
    )


def tracked_worktree_matches_head(
    root: Path,
    index_environment: dict[str, str],
    label: str,
    temporary_root: Path,
) -> bool:
    raw_entries = run_git_bytes(
        root,
        "ls-files",
        "--stage",
        "-z",
        extra_environment=index_environment,
    )
    regular_entries: list[tuple[bytes, bytes, os.stat_result]] = []

    for raw_entry in raw_entries.split(b"\0"):
        if not raw_entry:
            continue
        try:
            metadata, relative_path_bytes = raw_entry.split(b"\t", 1)
            mode, expected_object_id, stage = metadata.split()
        except ValueError:
            fail(f"{label} contains a malformed tracked-file entry")
        if stage != b"0":
            fail(f"{label} contains an unmerged tracked-file entry")

        relative_path = Path(os.fsdecode(relative_path_bytes))
        if relative_path.is_absolute() or ".." in relative_path.parts:
            fail(f"{label} contains an unsafe tracked path: {relative_path}")
        worktree_path = root / relative_path
        try:
            path_status = worktree_path.lstat()
        except FileNotFoundError:
            return False
        except OSError as error:
            fail(f"cannot inspect {label} tracked path {worktree_path}: {error}")

        if mode in (b"100644", b"100755"):
            if not stat.S_ISREG(path_status.st_mode):
                return False
            expected_executable = mode == b"100755"
            if bool(path_status.st_mode & 0o111) != expected_executable:
                return False
            regular_entries.append(
                (relative_path_bytes, expected_object_id, path_status)
            )
            continue

        if mode == b"120000":
            if not stat.S_ISLNK(path_status.st_mode):
                return False
            try:
                link_target = os.fsencode(os.readlink(worktree_path))
                post_read_status = worktree_path.lstat()
            except OSError as error:
                fail(f"cannot read {label} tracked link {worktree_path}: {error}")
            if file_identity(path_status) != file_identity(post_read_status):
                return False
            actual_object_id = run_git_bytes(
                root,
                "hash-object",
                "--stdin",
                input_data=link_target,
            ).strip()
            if actual_object_id != expected_object_id:
                return False
            continue

        if mode == b"160000":
            if not stat.S_ISDIR(path_status.st_mode):
                return False
            submodule_commit = run_git(
                worktree_path,
                "rev-parse",
                "--verify",
                "HEAD^{commit}",
            )
            if submodule_commit.encode("ascii") != expected_object_id:
                return False
            require_clean_worktree(
                worktree_path,
                f"{label} submodule {relative_path}",
            )
            continue

        fail(f"{label} contains unsupported tracked mode {mode.decode('ascii')}")

    batch_entries = [entry for entry in regular_entries if b"\n" not in entry[0]]
    newline_entries = [entry for entry in regular_entries if b"\n" in entry[0]]
    actual_object_ids: dict[bytes, bytes] = {}
    if batch_entries:
        path_input = b"".join(
            relative_path_bytes + b"\n"
            for relative_path_bytes, _expected_object_id, _path_status in batch_entries
        )
        batch_output = run_git_bytes(
            root,
            "hash-object",
            "--no-filters",
            "--stdin-paths",
            input_data=path_input,
        ).splitlines()
        if len(batch_output) != len(batch_entries):
            fail(f"{label} raw tracked-file hash count differs")
        actual_object_ids.update(
            (entry[0], object_id)
            for entry, object_id in zip(batch_entries, batch_output, strict=True)
        )
    for relative_path_bytes, _expected_object_id, _path_status in newline_entries:
        newline_relative_path = os.fsdecode(relative_path_bytes)
        actual_object_ids[relative_path_bytes] = run_git_bytes(
            root,
            "hash-object",
            "--no-filters",
            "--",
            newline_relative_path,
        ).strip()

    checkout_environment: dict[str, str] | None = None
    for relative_path_bytes, expected_object_id, initial_status in regular_entries:
        worktree_path = root / Path(os.fsdecode(relative_path_bytes))
        if actual_object_ids[relative_path_bytes] != expected_object_id:
            if checkout_environment is None:
                checkout_environment = isolated_checkout_environment(
                    root,
                    index_environment,
                    temporary_root,
                )
            expected_bytes = expected_checkout_bytes(
                root,
                relative_path_bytes,
                expected_object_id,
                checkout_environment,
            )
            try:
                actual_bytes = worktree_path.read_bytes()
            except OSError:
                return False
            if actual_bytes != expected_bytes:
                return False
        try:
            post_hash_status = worktree_path.lstat()
        except OSError:
            return False
        if file_identity(initial_status) != file_identity(post_hash_status):
            return False
    return True


def require_clean_worktree(root: Path, label: str) -> None:
    staged_result = run_git_process(
        root,
        "diff-index",
        "--cached",
        "--quiet",
        "--ignore-submodules=none",
        "HEAD",
        "--",
    )
    if staged_result.returncode not in (0, 1):
        diagnostic = staged_result.stderr.strip() or staged_result.stdout.strip()
        fail(f"failed to verify {label} index state: {diagnostic}")
    if staged_result.returncode == 1:
        fail(f"{label} is dirty: {root}")

    with tempfile.TemporaryDirectory(prefix="gororoba-git-index.") as directory:
        temporary_index = Path(directory) / "index"
        index_environment = {"GIT_INDEX_FILE": str(temporary_index)}
        run_git(
            root,
            "read-tree",
            "HEAD",
            extra_environment=index_environment,
        )
        tracked_matches = tracked_worktree_matches_head(
            root,
            index_environment,
            label,
            Path(directory),
        )
        untracked = run_git(
            root,
            "ls-files",
            "--others",
            "--exclude-standard",
            extra_environment=index_environment,
        )
        # External Meson setup uses an archive-derived source view. Ignored
        # subproject bytes in TOPSRC therefore sit outside the selected commit
        # and never enter the derived view.
        ignored_subprojects = run_git(
            root,
            "ls-files",
            "--others",
            "--ignored",
            "--exclude-standard",
            "--directory",
            "--",
            "subprojects",
            extra_environment=index_environment,
        )
        if not tracked_matches or untracked or ignored_subprojects:
            fail(f"{label} is dirty: {root}")


def require_clean_external_source(source_root: Path) -> None:
    repository_root = control_root()
    if source_root == repository_root:
        return
    require_clean_worktree(source_root, "external TOPSRC")
    require_clean_worktree(repository_root, "control worktree")


def resolved_inputs() -> dict[str, Path | str]:
    profile = input_identifier("GOROROBA_PROFILE_INPUT")
    hostenv = input_identifier("GOROROBA_HOSTENV_INPUT")
    mode = input_enum(
        "GOROROBA_MODE_INPUT",
        frozenset(("", "stable")),
    )
    compiler_chain = input_enum(
        "GOROROBA_COMPILER_CHAIN_INPUT",
        frozenset(("ccache", "direct", "distcc")),
    )
    compiler_family = input_enum(
        "GOROROBA_COMPILER_FAMILY_INPUT",
        frozenset(("gnu", "llvm")),
    )
    source_root = input_path("GOROROBA_TOPSRC_INPUT")
    source_commit, source_tree = source_identity(source_root)
    build_root = input_path("GOROROBA_BUILD_ROOT_INPUT")
    builddir = input_path("GOROROBA_BUILDDIR_INPUT")
    prefix = input_path("GOROROBA_PREFIX_INPUT")
    sysconfdir = input_path("GOROROBA_SYSCONFDIR_INPUT")
    repository_root = control_root()
    control_commit, control_tree = source_identity(repository_root)
    return {
        "source_root": source_root,
        "source_commit": source_commit,
        "source_tree": source_tree,
        "build_root": build_root,
        "builddir": builddir,
        "prefix": prefix,
        "sysconfdir": sysconfdir,
        "profile": profile,
        "hostenv": hostenv,
        "mode": mode or "default",
        "compiler_chain": compiler_chain,
        "compiler_family": compiler_family,
        "control_root": repository_root,
        "control_commit": control_commit,
        "control_tree": control_tree,
    }


def is_within_or_equal(path: Path, parent: Path) -> bool:
    return path == parent or path.is_relative_to(parent)


def is_strict_descendant(path: Path, parent: Path) -> bool:
    return path != parent and path.is_relative_to(parent)


def trusted_account_home(user_id: int) -> Path:
    if user_id == 0:
        fail("root cannot own Mesa build namespaces")
    try:
        account_home_text = pwd.getpwuid(user_id).pw_dir
    except KeyError:
        fail(f"account home is unavailable for uid {user_id}")
    try:
        account_home = Path(account_home_text).resolve(strict=True)
    except OSError as error:
        fail(f"cannot resolve account home {account_home_text}: {error}")
    if account_home in {
        Path("/"),
        Path("/boot"),
        Path("/dev"),
        Path("/etc"),
        Path("/home"),
        Path("/proc"),
        Path("/root"),
        Path("/run"),
        Path("/sys"),
        Path("/tmp"),
        Path("/usr"),
        Path("/var"),
        Path("/var/lib"),
        Path("/var/tmp"),
    }:
        fail(f"refusing unsafe account home: {account_home}")
    return account_home


def validate_owned_namespace(namespace: Path, user_id: int) -> Path:
    lexical_path = Path(os.path.abspath(namespace))
    resolved_path = lexical_path.resolve(strict=False)
    if resolved_path != lexical_path:
        fail(f"build namespace contains a symlink: {lexical_path}")
    try:
        namespace_status = lexical_path.lstat()
    except FileNotFoundError:
        return lexical_path
    except OSError as error:
        fail(f"cannot inspect build namespace {lexical_path}: {error}")
    if not stat.S_ISDIR(namespace_status.st_mode):
        fail(f"build namespace is not a directory: {lexical_path}")
    if namespace_status.st_uid != user_id:
        fail(f"build namespace has a foreign owner: {lexical_path}")
    if namespace_status.st_mode & 0o022:
        fail(f"build namespace is group/world writable: {lexical_path}")
    return lexical_path


def create_test_directory(
    label: str,
    *,
    parent: Path = Path("/var/tmp"),
    user_id: int | None = None,
) -> Path:
    if SAFE_LEAF_IDENTIFIER.fullmatch(label) is None:
        fail(f"invalid test directory label: {label}")
    selected_user_id = os.getuid() if user_id is None else user_id
    namespace = parent / f"mesa-26-gororoba-{selected_user_id}"
    try:
        namespace.mkdir(mode=0o700, exist_ok=True)
    except OSError as error:
        fail(f"cannot create test namespace {namespace}: {error}")
    validate_owned_namespace(namespace, selected_user_id)
    try:
        directory = Path(tempfile.mkdtemp(prefix=f"{label}.", dir=namespace))
    except OSError as error:
        fail(f"cannot create test directory in {namespace}: {error}")
    validate_owned_namespace(directory, selected_user_id)
    return directory


def owned_build_namespaces(repository_root: Path) -> tuple[Path, ...]:
    user_id = os.getuid()
    account_home = trusted_account_home(user_id)
    return (
        (account_home / ".cache" / "mesa-26-gororoba" / "external-builds"),
        Path("/tmp") / f"mesa-26-gororoba-{user_id}",
        Path("/var/tmp") / f"mesa-26-gororoba-{user_id}",
        repository_root.parent / ".mesa-26-gororoba-builds",
    )


def build_namespace_parent_boundary(
    namespace: Path,
    repository_root: Path,
) -> Path:
    user_id = os.getuid()
    temporary_boundaries = (*SHARED_TEMPORARY_BOUNDARIES, repository_root.parent)
    for boundary in temporary_boundaries:
        if namespace.parent == boundary:
            return boundary
    account_home = trusted_account_home(user_id)
    if is_strict_descendant(namespace, account_home):
        return account_home
    return namespace.parent


def validate_build_namespace_boundary(boundary: Path, user_id: int) -> Path:
    if boundary not in SHARED_TEMPORARY_BOUNDARIES:
        return validate_owned_namespace(boundary, user_id)
    lexical_path = Path(os.path.abspath(boundary))
    try:
        boundary_status = lexical_path.lstat()
    except OSError as error:
        fail(f"cannot inspect shared build boundary {lexical_path}: {error}")
    if (
        lexical_path.resolve(strict=False) != lexical_path
        or not stat.S_ISDIR(boundary_status.st_mode)
        or boundary_status.st_uid != 0
        or not boundary_status.st_mode & stat.S_ISVTX
    ):
        fail(f"shared build boundary is unsafe: {lexical_path}")
    return lexical_path


def validate_build_namespace_chain(
    namespace: Path,
    repository_root: Path,
    *,
    create: bool,
) -> Path:
    user_id = os.getuid()
    boundary = build_namespace_parent_boundary(namespace, repository_root)
    validate_build_namespace_boundary(boundary, user_id)
    if not is_strict_descendant(namespace, boundary):
        fail(f"build namespace is outside its parent boundary: {namespace}")
    relative_parts = namespace.relative_to(boundary).parts
    candidate = boundary
    for part in relative_parts:
        candidate /= part
        if create and not candidate.exists() and not candidate.is_symlink():
            try:
                candidate.mkdir(mode=0o700)
                candidate.chmod(0o700)
            except OSError as error:
                fail(f"cannot create build namespace {candidate}: {error}")
        validate_owned_namespace(candidate, user_id)
    return namespace


def ensure_selected_build_namespace(
    values: dict[str, Path | str],
) -> None:
    repository_root = values["control_root"]
    assert isinstance(repository_root, Path)
    namespace = selected_build_boundary(values)
    if namespace == repository_root:
        return
    validate_build_namespace_chain(
        namespace,
        repository_root,
        create=True,
    )


def selected_build_boundary(
    values: dict[str, Path | str],
) -> Path:
    repository_root = values["control_root"]
    build_root = values["build_root"]
    assert isinstance(repository_root, Path)
    assert isinstance(build_root, Path)
    if build_root == repository_root / "build":
        return repository_root
    matching_namespaces = tuple(
        validate_build_namespace_chain(
            namespace,
            repository_root,
            create=False,
        )
        for namespace in owned_build_namespaces(repository_root)
        if is_strict_descendant(build_root, namespace)
    )
    if len(matching_namespaces) != 1:
        fail(f"external BUILD_ROOT has no unique namespace: {build_root}")
    return matching_namespaces[0]


def containing_git_worktree(path: Path) -> Path | None:
    for candidate in (path, *path.parents):
        git_marker = candidate / ".git"
        if git_marker.exists() or git_marker.is_symlink():
            return candidate
    return None


def is_git_directory(path: Path) -> bool:
    if not (path / "HEAD").is_file():
        return False
    bare_layout = (path / "objects").is_dir() and (path / "refs").is_dir()
    linked_layout = (path / "commondir").is_file() and (path / "gitdir").is_file()
    return bare_layout or linked_layout


def containing_git_directory(path: Path) -> Path | None:
    for candidate in (path, *path.parents):
        if is_git_directory(candidate):
            return candidate
    return None


def decode_mountinfo_path(encoded_path: bytes) -> Path:
    decoded_path = bytearray()
    index = 0
    while index < len(encoded_path):
        value = encoded_path[index]
        if value != ord("\\"):
            decoded_path.append(value)
            index += 1
            continue
        octal_escape = encoded_path[index + 1 : index + 4]
        if octal_escape not in {b"011", b"012", b"040", b"134"}:
            fail("invalid mountinfo path escape")
        decoded_path.append(int(octal_escape, 8))
        index += 4

    try:
        path_text = os.fsdecode(bytes(decoded_path))
        mount_point = Path(path_text)
    except (TypeError, ValueError) as error:
        fail(f"invalid mountinfo path: {error}")
    if not mount_point.is_absolute():
        fail(f"mountinfo path is not absolute: {path_text}")
    normalized_mount_point = Path(os.path.normpath(path_text))
    if normalized_mount_point != mount_point:
        fail(f"mountinfo path is not normalized: {path_text}")
    return mount_point


def parse_mountinfo(mountinfo: bytes) -> tuple[Path, ...]:
    mount_points = []
    for line_number, record in enumerate(mountinfo.splitlines(), start=1):
        pre_separator, separator, post_separator = record.partition(b" - ")
        pre_fields = pre_separator.split()
        post_fields = post_separator.split()
        if (
            not separator
            or len(pre_fields) < 6
            or len(post_fields) < 3
            or not pre_fields[0].isdigit()
            or not pre_fields[1].isdigit()
        ):
            fail(f"invalid mountinfo record at line {line_number}")
        mount_points.append(decode_mountinfo_path(pre_fields[4]))
    if not mount_points:
        fail("mountinfo contains no mount records")
    return tuple(mount_points)


def current_mount_points(
    mountinfo_path: Path = MOUNTINFO_PATH,
) -> tuple[Path, ...]:
    try:
        mountinfo = mountinfo_path.read_bytes()
    except OSError as error:
        fail(f"cannot read mount topology {mountinfo_path}: {error}")
    return parse_mountinfo(mountinfo)


def crossing_mount_point(
    target: Path,
    trusted_boundary: Path,
    mount_points: tuple[Path, ...] | None = None,
) -> Path | None:
    if not is_strict_descendant(target, trusted_boundary):
        fail(
            "mutation target is outside its trusted boundary: "
            f"{target} ({trusted_boundary})"
        )
    selected_mount_points = (
        current_mount_points() if mount_points is None else mount_points
    )
    for mount_point in selected_mount_points:
        if is_strict_descendant(mount_point, trusted_boundary) and (
            is_within_or_equal(mount_point, target)
            or is_within_or_equal(target, mount_point)
        ):
            return mount_point
    return None


def contained_git_worktree(path: Path) -> Path | None:
    if not path.is_dir():
        return None

    def raise_walk_error(error: OSError) -> None:
        raise error

    try:
        for directory, directory_names, file_names in os.walk(
            path,
            topdown=True,
            onerror=raise_walk_error,
            followlinks=False,
        ):
            if ".git" in directory_names or ".git" in file_names:
                return Path(directory)
            if is_git_directory(Path(directory)):
                return Path(directory)
    except OSError as error:
        fail(f"cannot inspect destructive target {path}: {error}")
    return None


def path_anchor(path: Path) -> str:
    try:
        path_status = path.stat()
    except FileNotFoundError:
        return "absent"
    except OSError as error:
        fail(f"cannot stat selected path {path}: {error}")
    file_type = stat.S_IFMT(path_status.st_mode)
    return f"{path_status.st_dev:x}:{path_status.st_ino:x}:{file_type:x}"


def require_captured_inputs(
    values: dict[str, Path | str],
    fields: tuple[str, ...],
) -> None:
    revision_variables = {
        "source_commit": "GOROROBA_SOURCE_COMMIT_CAPTURED",
        "source_tree": "GOROROBA_SOURCE_TREE_CAPTURED",
        "control_commit": "GOROROBA_CONTROL_COMMIT_CAPTURED",
        "control_tree": "GOROROBA_CONTROL_TREE_CAPTURED",
    }
    for field, variable_name in revision_variables.items():
        selected_revision = values[field]
        assert isinstance(selected_revision, str)
        captured_revision = os.environ.get(variable_name, "")
        if not captured_revision:
            fail(f"captured revision is missing for {field}")
        if selected_revision != captured_revision:
            fail(
                "selected revision changed while waiting for the "
                f"build lease: {field} ({captured_revision})"
            )

    anchor_variables = {
        "source_root": "GOROROBA_SOURCE_ROOT_ANCHOR",
        "control_root": "GOROROBA_CONTROL_ROOT_ANCHOR",
        "build_root": "GOROROBA_BUILD_ROOT_ANCHOR",
        "builddir": "GOROROBA_BUILDDIR_ANCHOR",
        "prefix": "GOROROBA_PREFIX_ANCHOR",
        "sysconfdir": "GOROROBA_SYSCONFDIR_ANCHOR",
    }
    input_variables = {
        "source_root": "GOROROBA_TOPSRC_INPUT",
        "control_root": "GOROROBA_CONTROL_ROOT_INPUT",
        "build_root": "GOROROBA_BUILD_ROOT_INPUT",
        "builddir": "GOROROBA_BUILDDIR_INPUT",
        "prefix": "GOROROBA_PREFIX_INPUT",
        "sysconfdir": "GOROROBA_SYSCONFDIR_INPUT",
    }
    for field in fields:
        selected_path = values[field]
        assert isinstance(selected_path, Path)
        captured_text = os.environ.get(input_variables[field], "")
        if not captured_text:
            fail(f"captured canonical path is missing for {field}")
        captured_path = Path(captured_text)
        if not captured_path.is_absolute():
            fail(f"captured canonical path is not absolute for {field}")
        if selected_path != captured_path:
            fail(
                "selected path target changed while waiting for the "
                f"build lease: {field} ({captured_path})"
            )
        expected_anchor = os.environ.get(anchor_variables[field], "")
        if not expected_anchor:
            fail(f"captured path anchor is missing for {field}")
        current_anchor = path_anchor(selected_path)
        if current_anchor != expected_anchor:
            fail(
                "selected path identity changed while waiting for the "
                f"build lease: {field} ({selected_path})"
            )


def reject_git_worktree_target(
    path: Path,
    label: str,
    repository_root: Path,
    repository_build_root: Path,
    *,
    scan_descendants: bool,
) -> None:
    git_directory = containing_git_directory(path)
    if git_directory is not None:
        fail(f"refusing {label} inside a Git directory: {git_directory}")
    repository_boundary = containing_git_worktree(path)
    canonical_build_exception = (
        repository_boundary == repository_root
        and is_within_or_equal(path, repository_build_root)
    )
    if repository_boundary is not None and not canonical_build_exception:
        fail(f"refusing {label} inside a Git worktree: {repository_boundary}")
    if scan_descendants:
        contained_repository = contained_git_worktree(path)
        if contained_repository is not None:
            fail(
                f"refusing {label} containing a Git repository marker: "
                f"{contained_repository}"
            )


def reject_mount_target(
    path: Path,
    label: str,
    trusted_boundary: Path,
) -> None:
    mount_point = crossing_mount_point(path, trusted_boundary)
    if mount_point is not None:
        fail(
            f"refusing {label} crossing a mount point below "
            f"{trusted_boundary}: {mount_point}"
        )


def reject_protected_path(path: Path, label: str) -> None:
    protected = (
        Path("/"),
        Path("/usr"),
        Path("/bin"),
        Path("/sbin"),
        Path("/lib"),
        Path("/lib64"),
    )
    for boundary in protected:
        if (boundary == Path("/") and path == boundary) or (
            boundary != Path("/") and is_within_or_equal(path, boundary)
        ):
            fail(f"refusing unsafe {label}: {path}")


def validate_prefix(values: dict[str, Path | str]) -> None:
    prefix = values["prefix"]
    source_root = values["source_root"]
    repository_root = values["control_root"]
    build_root = values["build_root"]
    assert isinstance(prefix, Path)
    assert isinstance(source_root, Path)
    assert isinstance(repository_root, Path)
    assert isinstance(build_root, Path)

    profile = os.environ.get("GOROROBA_PROFILE_INPUT", "")
    allowed_opt_prefixes = {
        Path(f"/opt/local/mesa-{profile}"),
        Path("/opt/local/mesa-26-gororoba"),
        Path("/opt/local/mesa-gororoba-debug-optimized"),
        Path("/opt/mesa-gororoba-debug-optimized"),
        Path("/opt/mesa-gororoba-debug-asan"),
        Path("/opt/mesa-gororoba-debug-o0"),
    }
    reject_protected_path(prefix, "PREFIX")
    home = Path.home().resolve(strict=False)
    if prefix == home:
        fail(f"refusing unsafe PREFIX: {prefix}")
    if is_within_or_equal(prefix, source_root):
        fail(f"refusing PREFIX inside TOPSRC: {prefix}")
    if is_within_or_equal(prefix, repository_root):
        fail(f"refusing PREFIX inside the control worktree: {prefix}")
    if (
        source_root == repository_root
        and prefix not in allowed_opt_prefixes
        and prefix.parent != build_root
    ):
        fail(
            "control-source PREFIX must be an owned profile prefix or "
            f"a direct child of BUILD_ROOT: {prefix}"
        )


def selected_prefix_boundary(
    values: dict[str, Path | str],
    build_boundary: Path,
) -> Path:
    prefix = values["prefix"]
    build_root = values["build_root"]
    assert isinstance(prefix, Path)
    assert isinstance(build_root, Path)
    if prefix.parent == build_root:
        return build_boundary
    if is_strict_descendant(prefix, Path("/opt")):
        return Path("/opt")
    fail(f"PREFIX has no trusted mutation boundary: {prefix}")


def validate_layout(operation: str, values: dict[str, Path | str]) -> None:
    source_root = values["source_root"]
    build_root = values["build_root"]
    builddir = values["builddir"]
    repository_root = values["control_root"]
    assert isinstance(source_root, Path)
    assert isinstance(build_root, Path)
    assert isinstance(builddir, Path)
    assert isinstance(repository_root, Path)

    reject_protected_path(build_root, "BUILD_ROOT")
    home = Path.home().resolve(strict=False)
    if build_root in {home, source_root, repository_root}:
        fail(f"refusing unsafe BUILD_ROOT: {build_root}")
    repository_build_root = repository_root / "build"
    build_boundary = selected_build_boundary(values)
    repository_boundary = containing_git_worktree(build_root)
    if repository_boundary is not None and build_root != repository_build_root:
        fail(f"refusing BUILD_ROOT inside a Git worktree: {repository_boundary}")
    if not is_strict_descendant(builddir, build_root):
        fail(f"refusing BUILDDIR outside BUILD_ROOT: {builddir}")
    builddir_mutation = operation in {
        "build",
        "clean",
        "configure",
        "distclean",
        "install",
        "test",
    }
    destructive_builddir = operation in {"clean", "distclean"}
    if builddir_mutation:
        reject_mount_target(builddir, "BUILDDIR", build_boundary)
    reject_git_worktree_target(
        builddir,
        "BUILDDIR",
        repository_root,
        repository_build_root,
        scan_descendants=destructive_builddir,
    )

    source_is_control = source_root == repository_root
    if source_is_control:
        if (
            is_within_or_equal(build_root, repository_root)
            and build_root != repository_build_root
        ):
            fail(f"refusing BUILD_ROOT inside TOPSRC: {build_root}")
        if is_within_or_equal(builddir, repository_root) and not is_strict_descendant(
            builddir, repository_build_root
        ):
            fail(f"refusing BUILDDIR inside TOPSRC: {builddir}")
    else:
        for boundary, name in (
            (source_root, "external TOPSRC"),
            (repository_root, "the control worktree"),
        ):
            if is_within_or_equal(build_root, boundary):
                fail(f"refusing BUILD_ROOT inside {name}: {build_root}")
            if is_within_or_equal(builddir, boundary):
                fail(f"refusing BUILDDIR inside {name}: {builddir}")
        source_view = source_view_path(values)
        if is_within_or_equal(builddir, source_view) or is_within_or_equal(
            source_view,
            builddir,
        ):
            fail(f"external BUILDDIR overlaps the source view: {builddir}")
        reject_mount_target(
            source_view,
            "source view",
            build_boundary,
        )

    if operation in {"configure", "install", "distclean", "artifact"}:
        validate_prefix(values)
        prefix = values["prefix"]
        assert isinstance(prefix, Path)
        if operation in {"artifact", "distclean", "install"}:
            prefix_boundary = selected_prefix_boundary(values, build_boundary)
            reject_mount_target(prefix, "PREFIX", prefix_boundary)
        reject_git_worktree_target(
            prefix,
            "PREFIX",
            repository_root,
            repository_build_root,
            scan_descendants=operation == "distclean",
        )
        if not source_is_control:
            if prefix.parent != build_root:
                fail(f"external PREFIX must be a direct child of BUILD_ROOT: {prefix}")
            if prefix == builddir:
                fail(f"external PREFIX aliases BUILDDIR: {prefix}")
            if prefix == source_view_path(values):
                fail(f"external PREFIX aliases the source view: {prefix}")

    if operation == "clean-all":
        if not source_is_control:
            fail("clean-all is unavailable with an external TOPSRC")
        if build_root != repository_build_root:
            fail(
                "clean-all only removes the repository build root: "
                f"{repository_build_root}"
            )
        reject_mount_target(build_root, "BUILD_ROOT", repository_root)
        reject_git_worktree_target(
            build_root,
            "BUILD_ROOT",
            repository_root,
            repository_build_root,
            scan_descendants=True,
        )


def source_view_path(values: dict[str, Path | str]) -> Path:
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    return build_root / SOURCE_VIEW_DIRECTORY


def require_source_view_target(values: dict[str, Path | str]) -> Path:
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    source_view = source_view_path(values)
    if source_view.parent != build_root:
        fail(f"source view is outside BUILD_ROOT: {source_view}")
    if source_view.is_symlink():
        fail(f"source view is a symlink: {source_view}")
    if source_view.exists() and not source_view.is_dir():
        fail(f"source view is not a directory: {source_view}")
    reject_mount_target(
        source_view,
        "source view",
        selected_build_boundary(values),
    )
    return source_view


def require_hashed_wrap_sources(source_view: Path) -> None:
    subprojects = source_view / "subprojects"
    if not subprojects.is_dir():
        return
    for wrap_path in sorted(subprojects.rglob("*.wrap")):
        if wrap_path.is_symlink() or not wrap_path.is_file():
            fail(f"Meson wrap path is not a regular file: {wrap_path}")
        parser = configparser.ConfigParser(
            interpolation=None,
            strict=True,
        )
        try:
            wrap_text = wrap_path.read_text(encoding="utf-8")
            parser.read_string(wrap_text, source=str(wrap_path))
        except (OSError, UnicodeError, configparser.Error) as error:
            fail(f"invalid Meson wrap file {wrap_path}: {error}")
        if not parser.has_section("wrap-file"):
            continue
        wrap_file = parser["wrap-file"]
        if not wrap_file.get("source_url") or not wrap_file.get("source_filename"):
            fail(f"wrap-file source archive is incomplete: {wrap_path}")
        source_hash = wrap_file.get("source_hash", "")
        if SHA256_VALUE.fullmatch(source_hash) is None:
            fail(f"wrap-file source_hash must be SHA-256: {wrap_path}")
        if wrap_file.get("patch_url") or wrap_file.get("patch_filename"):
            patch_hash = wrap_file.get("patch_hash", "")
            if SHA256_VALUE.fullmatch(patch_hash) is None:
                fail(f"wrap-file patch_hash must be SHA-256: {wrap_path}")


def digest_frame(
    digest: hashlib._Hash,
    label: bytes,
    value: bytes,
) -> None:
    digest.update(label)
    digest.update(len(value).to_bytes(8, byteorder="big"))
    digest.update(value)


def source_view_content_digest(source_view: Path) -> str:
    if source_view.is_symlink() or not source_view.is_dir():
        fail(f"source view is not a directory: {source_view}")
    digest = hashlib.sha256()

    def visit(directory: Path, relative_directory: Path) -> None:
        try:
            with os.scandir(directory) as directory_entries:
                entries = sorted(
                    directory_entries,
                    key=lambda entry: os.fsencode(entry.name),
                )
        except OSError as error:
            fail(f"cannot read source view {directory}: {error}")
        for entry in entries:
            relative_path = relative_directory / entry.name
            path_bytes = os.fsencode(relative_path)
            try:
                entry_status = entry.stat(follow_symlinks=False)
            except OSError as error:
                fail(f"cannot stat source view entry {entry.path}: {error}")
            mode_bytes = f"{stat.S_IMODE(entry_status.st_mode):04o}".encode()
            digest_frame(digest, b"path", path_bytes)
            digest_frame(digest, b"mode", mode_bytes)
            if stat.S_ISDIR(entry_status.st_mode):
                digest_frame(digest, b"type", b"directory")
                visit(Path(entry.path), relative_path)
            elif stat.S_ISREG(entry_status.st_mode):
                digest_frame(digest, b"type", b"regular")
                digest_frame(
                    digest,
                    b"size",
                    str(entry_status.st_size).encode("utf-8"),
                )
                try:
                    with open(entry.path, "rb") as source_file:
                        while data := source_file.read(1024 * 1024):
                            digest.update(data)
                except OSError as error:
                    fail(f"cannot read source view entry {entry.path}: {error}")
            elif stat.S_ISLNK(entry_status.st_mode):
                digest_frame(digest, b"type", b"symlink")
                try:
                    link_target = os.fsencode(os.readlink(entry.path))
                except OSError as error:
                    fail(f"cannot read source view link {entry.path}: {error}")
                digest_frame(digest, b"target", link_target)
            else:
                fail(f"source view contains an unsupported file type: {entry.path}")

    visit(source_view, Path())
    return f"sha256:{digest.hexdigest()}"


def remove_directory_tree(path: Path, label: str) -> None:
    if path.is_symlink():
        fail(f"{label} is a symlink: {path}")
    if not path.exists():
        return
    if not path.is_dir():
        fail(f"{label} is not a directory: {path}")
    try:
        shutil.rmtree(path)
    except OSError as error:
        fail(f"cannot remove {label} {path}: {error}")
    if path.exists() or path.is_symlink():
        fail(f"{label} remains after removal: {path}")


def remove_source_view(source_view: Path) -> None:
    remove_directory_tree(source_view, "source view")


def base_identity_payload(
    values: dict[str, Path | str],
) -> dict[str, str | int]:
    source_root = values["source_root"]
    assert isinstance(source_root, Path)
    require_clean_external_source(source_root)
    return {
        "schema_version": SCHEMA_VERSION,
        "source_root": str(source_root),
        "source_commit": str(values["source_commit"]),
        "source_tree": str(values["source_tree"]),
        "source_view": str(source_view_path(values)),
        "control_root": str(values["control_root"]),
        "control_commit": str(values["control_commit"]),
        "control_tree": str(values["control_tree"]),
        "build_root": str(values["build_root"]),
        "builddir": str(values["builddir"]),
        "prefix": str(values["prefix"]),
        "sysconfdir": str(values["sysconfdir"]),
        "profile": str(values["profile"]),
        "hostenv": str(values["hostenv"]),
        "mode": str(values["mode"]),
        "compiler_chain": str(values["compiler_chain"]),
        "compiler_family": str(values["compiler_family"]),
    }


def identity_record(
    base_payload: dict[str, str | int],
    state: str,
    transaction_id: str,
    source_view_digest: str,
) -> dict[str, str | int]:
    return {
        **base_payload,
        "source_view_digest": source_view_digest,
        "state": state,
        "transaction_id": transaction_id,
    }


def identity_path(values: dict[str, Path | str]) -> Path:
    builddir = values["builddir"]
    assert isinstance(builddir, Path)
    return builddir / IDENTITY_FILENAME


def root_identity_path(values: dict[str, Path | str]) -> Path:
    build_root = values["build_root"]
    assert isinstance(build_root, Path)
    return build_root / ROOT_IDENTITY_FILENAME


def read_identity(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"invalid source identity {path}: {error}")
    if not isinstance(value, dict):
        fail(f"invalid source identity object: {path}")
    return value


def require_identity_fields(
    recorded: dict[str, object],
    expected: dict[str, str | int],
    path: Path,
) -> None:
    mismatches = [
        field
        for field, expected_value in expected.items()
        if recorded.get(field) != expected_value
    ]
    if mismatches:
        fail(
            "external source identity drift: "
            + ", ".join(sorted(mismatches))
            + f" ({path})"
        )


def require_identity_record(
    recorded: dict[str, object],
    expected_base: dict[str, str | int],
    path: Path,
    allowed_states: frozenset[str],
    *,
    expected_source_view_digest: str | None = None,
) -> tuple[str, str, str]:
    require_identity_fields(recorded, expected_base, path)
    expected_fields = set(expected_base) | {
        "source_view_digest",
        "state",
        "transaction_id",
    }
    actual_fields = set(recorded)
    if actual_fields != expected_fields:
        missing = sorted(expected_fields - actual_fields)
        unexpected = sorted(actual_fields - expected_fields)
        details = []
        if missing:
            details.append("missing " + ", ".join(missing))
        if unexpected:
            details.append("unexpected " + ", ".join(unexpected))
        fail(f"invalid source identity fields: {'; '.join(details)} ({path})")

    state = recorded["state"]
    if not isinstance(state, str) or state not in allowed_states:
        expected_states = ", ".join(sorted(allowed_states))
        fail(
            "external source identity state is "
            f"{state!r}, expected {expected_states} ({path})"
        )

    transaction_id = recorded["transaction_id"]
    if not isinstance(transaction_id, str) or not TRANSACTION_ID.fullmatch(
        transaction_id
    ):
        fail(f"invalid source identity transaction_id ({path})")

    source_view_digest = recorded["source_view_digest"]
    valid_digest = isinstance(source_view_digest, str) and (
        SOURCE_VIEW_DIGEST.fullmatch(source_view_digest) is not None
        or (
            state == PROVISIONAL_STATE
            and source_view_digest == PENDING_SOURCE_VIEW_DIGEST
        )
    )
    if not valid_digest:
        fail(f"invalid source identity source_view_digest ({path})")
    assert isinstance(source_view_digest, str)
    if (
        expected_source_view_digest is not None
        and source_view_digest != expected_source_view_digest
    ):
        fail(f"external source identity drift: source_view_digest ({path})")
    return state, transaction_id, source_view_digest


def verify_recorded_source_view(
    values: dict[str, Path | str],
    state: str,
    recorded_digest: str,
    *,
    allow_provisional_content: bool,
) -> None:
    source_view = require_source_view_target(values)
    if state == PROVISIONAL_STATE and allow_provisional_content:
        return
    if recorded_digest == PENDING_SOURCE_VIEW_DIGEST:
        fail(f"source view content identity is pending: {source_view}")
    current_digest = source_view_content_digest(source_view)
    if current_digest != recorded_digest:
        fail(f"source view content drift: {source_view}")


def require_matching_transaction(
    root_transaction_id: str,
    build_transaction_id: str,
    path: Path,
) -> None:
    if root_transaction_id != build_transaction_id:
        fail(f"external source identity transaction drift ({path})")


def prepare_identity(values: dict[str, Path | str]) -> None:
    source_root = values["source_root"]
    build_root = values["build_root"]
    builddir = values["builddir"]
    assert isinstance(source_root, Path)
    assert isinstance(build_root, Path)
    assert isinstance(builddir, Path)
    ensure_selected_build_namespace(values)
    if source_root == values["control_root"]:
        return

    expected_base = base_identity_payload(values)
    root_path = root_identity_path(values)
    root_state: str | None = None
    root_transaction_id: str | None = None
    root_source_view_digest = PENDING_SOURCE_VIEW_DIGEST
    if root_path.exists():
        (
            root_state,
            root_transaction_id,
            root_source_view_digest,
        ) = require_identity_record(
            read_identity(root_path),
            expected_base,
            root_path,
            frozenset((PROVISIONAL_STATE, FINAL_STATE)),
        )
        verify_recorded_source_view(
            values,
            root_state,
            root_source_view_digest,
            allow_provisional_content=True,
        )
    elif build_root.exists() and any(build_root.iterdir()):
        fail(
            "external build root lacks a source identity; "
            f"select an empty build root: {build_root}"
        )

    path = identity_path(values)
    if path.exists():
        (
            _build_state,
            build_transaction_id,
            build_source_view_digest,
        ) = require_identity_record(
            read_identity(path),
            expected_base,
            path,
            frozenset((FINAL_STATE,)),
        )
        if root_state == FINAL_STATE:
            assert root_transaction_id is not None
            if build_source_view_digest != root_source_view_digest:
                fail(f"external source identity digest drift ({path})")
            require_matching_transaction(
                root_transaction_id,
                build_transaction_id,
                path,
            )
    elif builddir.exists() and any(builddir.iterdir()):
        if root_state != PROVISIONAL_STATE:
            fail(
                "external build directory lacks a source identity; "
                f"clean it before configure: {builddir}"
            )

    transaction_id = secrets.token_hex(16)
    write_json_atomic(
        root_path,
        identity_record(
            expected_base,
            PROVISIONAL_STATE,
            transaction_id,
            root_source_view_digest,
        ),
    )


def prepare_source_view(values: dict[str, Path | str]) -> None:
    source_root = values["source_root"]
    source_commit = values["source_commit"]
    build_root = values["build_root"]
    assert isinstance(source_root, Path)
    assert isinstance(source_commit, str)
    assert isinstance(build_root, Path)
    if source_root == values["control_root"]:
        return

    expected_base = base_identity_payload(values)
    root_path = root_identity_path(values)
    if not root_path.is_file():
        fail(f"external build root lacks prepared source identity: {root_path}")
    _root_state, transaction_id, _recorded_digest = require_identity_record(
        read_identity(root_path),
        expected_base,
        root_path,
        frozenset((PROVISIONAL_STATE,)),
    )
    source_view = require_source_view_target(values)

    try:
        staging_path = Path(
            tempfile.mkdtemp(
                prefix=f"{SOURCE_VIEW_DIRECTORY}.staging.",
                dir=build_root,
            )
        )
    except OSError as error:
        fail(f"cannot create source view staging directory in {build_root}: {error}")
    try:
        archive_descriptor, archive_name = tempfile.mkstemp(
            prefix=".gororoba-source-archive.",
            suffix=".tar",
            dir=build_root,
        )
    except OSError as error:
        remove_directory_tree(staging_path, "source view staging directory")
        fail(f"cannot create source archive in {build_root}: {error}")
    os.close(archive_descriptor)
    archive_path = Path(archive_name)
    staging_is_live = True
    try:
        run_git_archive(source_root, source_commit, archive_path)
        try:
            with tarfile.open(archive_path, mode="r:") as archive:
                archive.extractall(staging_path, filter="data")
        except (OSError, tarfile.TarError) as error:
            fail(f"cannot extract source archive {archive_path}: {error}")
        if not (staging_path / "meson.build").is_file():
            fail(f"source archive lacks meson.build: {source_commit}")
        if not (
            (staging_path / "meson.options").is_file()
            or (staging_path / "meson_options.txt").is_file()
        ):
            fail(f"source archive lacks Mesa option definitions: {source_commit}")
        require_hashed_wrap_sources(staging_path)
        remove_source_view(source_view)
        try:
            os.replace(staging_path, source_view)
        except OSError as error:
            fail(f"cannot publish source view {source_view}: {error}")
        staging_is_live = False
    finally:
        try:
            archive_path.unlink()
        except FileNotFoundError:
            pass
        except OSError as error:
            fail(f"cannot remove source archive {archive_path}: {error}")
        if staging_is_live:
            remove_directory_tree(staging_path, "source view staging directory")

    source_view_digest = source_view_content_digest(source_view)
    write_json_atomic(
        root_path,
        identity_record(
            expected_base,
            PROVISIONAL_STATE,
            transaction_id,
            source_view_digest,
        ),
    )


def write_json_atomic(output: Path, payload: dict[str, str | int]) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=output.parent,
        prefix=f"{output.name}.",
        text=True,
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as temporary:
            json.dump(payload, temporary, indent=2, sort_keys=True)
            temporary.write("\n")
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_name, output)
        directory_descriptor = os.open(
            output.parent,
            os.O_RDONLY | getattr(os, "O_DIRECTORY", 0),
        )
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def write_identity(values: dict[str, Path | str]) -> None:
    source_root = values["source_root"]
    assert isinstance(source_root, Path)
    if source_root == values["control_root"]:
        return

    expected_base = base_identity_payload(values)
    root_path = root_identity_path(values)
    if not root_path.is_file():
        fail(f"external build root lacks prepared source identity: {root_path}")
    _root_state, transaction_id, _recorded_digest = require_identity_record(
        read_identity(root_path),
        expected_base,
        root_path,
        frozenset((PROVISIONAL_STATE,)),
    )
    source_view = require_source_view_target(values)
    require_hashed_wrap_sources(source_view)
    source_view_digest = source_view_content_digest(source_view)
    final_record = identity_record(
        expected_base,
        FINAL_STATE,
        transaction_id,
        source_view_digest,
    )
    write_json_atomic(identity_path(values), final_record)
    write_json_atomic(root_path, final_record)


def verify_identity(values: dict[str, Path | str]) -> None:
    source_root = values["source_root"]
    assert isinstance(source_root, Path)
    if source_root == values["control_root"]:
        return

    expected_base = base_identity_payload(values)
    root_path = root_identity_path(values)
    if not root_path.is_file():
        fail(f"external build root lacks source identity: {root_path}")
    (
        root_state,
        root_transaction_id,
        root_source_view_digest,
    ) = require_identity_record(
        read_identity(root_path),
        expected_base,
        root_path,
        frozenset((FINAL_STATE,)),
    )
    verify_recorded_source_view(
        values,
        root_state,
        root_source_view_digest,
        allow_provisional_content=False,
    )

    path = identity_path(values)
    if not path.is_file():
        fail(f"external build directory lacks source identity: {path}")
    _build_state, build_transaction_id, _build_source_view_digest = (
        require_identity_record(
            read_identity(path),
            expected_base,
            path,
            frozenset((FINAL_STATE,)),
            expected_source_view_digest=root_source_view_digest,
        )
    )
    require_matching_transaction(
        root_transaction_id,
        build_transaction_id,
        path,
    )


def verify_delete_identity(
    values: dict[str, Path | str],
    *,
    allow_provisional: bool,
) -> None:
    source_root = values["source_root"]
    builddir = values["builddir"]
    assert isinstance(source_root, Path)
    assert isinstance(builddir, Path)
    if source_root == values["control_root"]:
        return
    expected_base = base_identity_payload(values)
    root_path = root_identity_path(values)
    if not root_path.is_file():
        fail(f"external build root lacks source identity: {root_path}")
    allowed_root_states = (
        frozenset((PROVISIONAL_STATE, FINAL_STATE))
        if allow_provisional
        else frozenset((FINAL_STATE,))
    )
    (
        root_state,
        root_transaction_id,
        root_source_view_digest,
    ) = require_identity_record(
        read_identity(root_path),
        expected_base,
        root_path,
        allowed_root_states,
    )
    verify_recorded_source_view(
        values,
        root_state,
        root_source_view_digest,
        allow_provisional_content=allow_provisional,
    )
    if not builddir.exists():
        return

    path = identity_path(values)
    if path.exists():
        _build_state, build_transaction_id, build_source_view_digest = (
            require_identity_record(
                read_identity(path),
                expected_base,
                path,
                frozenset((FINAL_STATE,)),
            )
        )
        if root_state == FINAL_STATE:
            if build_source_view_digest != root_source_view_digest:
                fail(f"external source identity digest drift ({path})")
            require_matching_transaction(
                root_transaction_id,
                build_transaction_id,
                path,
            )
    elif root_state != PROVISIONAL_STATE:
        fail(f"external build directory lacks source identity: {path}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("validate-selectors")
    subparsers.add_parser("validate-build-execution")
    subparsers.add_parser("validate-make-version")
    test_directory_parser = subparsers.add_parser("create-test-directory")
    test_directory_parser.add_argument(
        "--label",
        choices=("build-jobs", "build-lease", "source-root"),
        required=True,
    )
    subparsers.add_parser("resolve-make")
    subparsers.add_parser("check-source")

    layout_parser = subparsers.add_parser("check-layout")
    layout_parser.add_argument(
        "--operation",
        required=True,
        choices=(
            "artifact",
            "build",
            "clean",
            "clean-all",
            "configure",
            "distclean",
            "install",
            "test",
        ),
    )
    subparsers.add_parser("prepare-identity")
    subparsers.add_parser("prepare-source-view")
    subparsers.add_parser("write-identity")
    subparsers.add_parser("verify-identity")
    subparsers.add_parser("verify-artifact-identity")
    subparsers.add_parser("verify-delete-identity")
    subparsers.add_parser("verify-distclean-identity")
    captured_parser = subparsers.add_parser("verify-captured-inputs")
    captured_parser.add_argument(
        "--fields",
        nargs="+",
        required=True,
        choices=(
            "source_root",
            "control_root",
            "build_root",
            "builddir",
            "prefix",
            "sysconfdir",
        ),
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if arguments.command == "validate-make-version":
        validate_make_version(os.environ.get("GOROROBA_MAKE_VERSION_INPUT", ""))
        print("yes")
        return 0
    if arguments.command == "validate-selectors":
        mode = input_enum(
            "GOROROBA_MODE_INPUT",
            frozenset(("", "stable")),
        )
        print(
            input_identifier("GOROROBA_PROFILE_INPUT"),
            input_identifier("GOROROBA_HOSTENV_INPUT"),
            input_identifier("GOROROBA_INSTALL_NAMESPACE_INPUT"),
            mode or "default",
            input_enum(
                "GOROROBA_COMPILER_CHAIN_INPUT",
                frozenset(("ccache", "direct", "distcc")),
            ),
            input_enum(
                "GOROROBA_COMPILER_FAMILY_INPUT",
                frozenset(("gnu", "llvm")),
            ),
        )
        return 0
    if arguments.command == "validate-build-execution":
        print(
            input_decimal("GOROROBA_JOBS_INPUT", minimum=1),
            input_decimal("GOROROBA_DISTCC_JOBS_INPUT", minimum=1),
            input_decimal("GOROROBA_LOCK_WAIT_INPUT", minimum=0),
            input_path("GOROROBA_BUILD_LOCK_INPUT"),
        )
        return 0
    if arguments.command == "create-test-directory":
        print(create_test_directory(arguments.label))
        return 0
    values = resolved_inputs()

    if arguments.command == "resolve-make":
        source_root = values["source_root"]
        control_root_path = values["control_root"]
        build_root = values["build_root"]
        builddir = values["builddir"]
        prefix = values["prefix"]
        sysconfdir = values["sysconfdir"]
        assert isinstance(source_root, Path)
        assert isinstance(control_root_path, Path)
        assert isinstance(build_root, Path)
        assert isinstance(builddir, Path)
        assert isinstance(prefix, Path)
        assert isinstance(sysconfdir, Path)
        fields = (
            source_root,
            build_root,
            builddir,
            prefix,
            sysconfdir,
            values["source_commit"],
            values["source_tree"],
            values["control_commit"],
            values["control_tree"],
            control_root_path,
            path_anchor(source_root),
            path_anchor(control_root_path),
            path_anchor(build_root),
            path_anchor(builddir),
            path_anchor(prefix),
            path_anchor(sysconfdir),
        )
        print(" ".join(str(field) for field in fields))
    elif arguments.command == "check-source":
        source_root = values["source_root"]
        assert isinstance(source_root, Path)
        require_clean_external_source(source_root)
    elif arguments.command == "check-layout":
        validate_layout(arguments.operation, values)
    elif arguments.command == "prepare-identity":
        validate_layout("configure", values)
        prepare_identity(values)
    elif arguments.command == "prepare-source-view":
        validate_layout("configure", values)
        prepare_source_view(values)
    elif arguments.command == "write-identity":
        validate_layout("configure", values)
        write_identity(values)
    elif arguments.command == "verify-identity":
        verify_identity(values)
    elif arguments.command == "verify-artifact-identity":
        verify_delete_identity(values, allow_provisional=False)
    elif arguments.command == "verify-delete-identity":
        verify_delete_identity(values, allow_provisional=True)
    elif arguments.command == "verify-distclean-identity":
        verify_delete_identity(values, allow_provisional=False)
    elif arguments.command == "verify-captured-inputs":
        require_captured_inputs(values, tuple(arguments.fields))
    else:
        fail(f"unsupported command: {arguments.command}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ControlError as error:
        print(f"source-root-control: {error}", file=sys.stderr)
        raise SystemExit(2)
