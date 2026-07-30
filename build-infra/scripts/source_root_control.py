#!/usr/bin/env python3
# Copyright 2026 Oichkatzelesfrettschen
# SPDX-License-Identifier: MIT
"""Validate external Mesa source selection and bind it to a build directory."""

from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NoReturn

SCHEMA_VERSION = 3
IDENTITY_FILENAME = ".gororoba-source-identity.json"
ROOT_IDENTITY_FILENAME = ".gororoba-external-source-identity.json"
SAFE_PATH_INPUT = re.compile(r"^[A-Za-z0-9_./:+@=~-]+$")
TRANSACTION_ID = re.compile(r"^[0-9a-f]{32}$")
PROVISIONAL_STATE = "provisional"
FINAL_STATE = "final"


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


def run_git(root: Path, *arguments: str) -> str:
    command = ["git", "--no-optional-locks", "-C", str(root), *arguments]
    environment = os.environ.copy()
    environment["GIT_OPTIONAL_LOCKS"] = "0"
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        env=environment,
        shell=False,
        text=True,
    )
    if result.returncode != 0:
        diagnostic = result.stderr.strip() or result.stdout.strip()
        fail(f"{' '.join(command)} failed: {diagnostic}")
    return result.stdout.strip()


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
        fail(
            "TOPSRC is not the Git worktree root: "
            f"{source_root} (root {git_root})"
        )

    commit = run_git(source_root, "rev-parse", "--verify", "HEAD^{commit}")
    tree = run_git(source_root, "rev-parse", "--verify", "HEAD^{tree}")
    return commit, tree


def require_clean_external_source(source_root: Path) -> None:
    if source_root == control_root():
        return
    status = run_git(
        source_root,
        "status",
        "--porcelain=v1",
        "--untracked-files=all",
    )
    if status:
        fail(f"external TOPSRC is dirty: {source_root}")


def resolved_inputs() -> dict[str, Path | str]:
    source_root = input_path("GOROROBA_TOPSRC_INPUT")
    source_commit, source_tree = source_identity(source_root)
    build_root = input_path("GOROROBA_BUILD_ROOT_INPUT")
    builddir = input_path("GOROROBA_BUILDDIR_INPUT")
    prefix = input_path("GOROROBA_PREFIX_INPUT")
    repository_root = control_root()
    control_commit, _control_tree = source_identity(repository_root)
    return {
        "source_root": source_root,
        "source_commit": source_commit,
        "source_tree": source_tree,
        "build_root": build_root,
        "builddir": builddir,
        "prefix": prefix,
        "control_root": repository_root,
        "control_commit": control_commit,
    }


def is_within_or_equal(path: Path, parent: Path) -> bool:
    return path == parent or path.is_relative_to(parent)


def is_strict_descendant(path: Path, parent: Path) -> bool:
    return path != parent and path.is_relative_to(parent)


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
    owned_build_boundaries = (
        home,
        repository_root.parent,
        Path("/tmp"),
        Path("/var/tmp"),
    )
    if build_root != repository_build_root and not any(
        is_strict_descendant(build_root, boundary)
        for boundary in owned_build_boundaries
    ):
        fail(
            "BUILD_ROOT must be the repository build root or a descendant "
            f"of an owned workspace or temporary root: {build_root}"
        )
    if not is_strict_descendant(builddir, build_root):
        fail(f"refusing BUILDDIR outside BUILD_ROOT: {builddir}")

    source_is_control = source_root == repository_root
    if source_is_control:
        if (
            is_within_or_equal(build_root, repository_root)
            and build_root != repository_build_root
        ):
            fail(f"refusing BUILD_ROOT inside TOPSRC: {build_root}")
        if (
            is_within_or_equal(builddir, repository_root)
            and not is_strict_descendant(builddir, repository_build_root)
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

    if operation in {"configure", "install", "distclean", "artifact"}:
        validate_prefix(values)
        if not source_is_control:
            prefix = values["prefix"]
            assert isinstance(prefix, Path)
            if prefix.parent != build_root:
                fail(
                    "external PREFIX must be a direct child of BUILD_ROOT: "
                    f"{prefix}"
                )
            if prefix == builddir:
                fail(f"external PREFIX aliases BUILDDIR: {prefix}")

    if operation == "clean-all":
        if not source_is_control:
            fail("clean-all is unavailable with an external TOPSRC")
        if build_root != repository_build_root:
            fail(
                "clean-all only removes the repository build root: "
                f"{repository_build_root}"
            )


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
        "control_root": str(values["control_root"]),
        "control_commit": str(values["control_commit"]),
        "build_root": str(values["build_root"]),
        "builddir": str(values["builddir"]),
        "prefix": str(values["prefix"]),
    }


def identity_record(
    base_payload: dict[str, str | int],
    state: str,
    transaction_id: str,
) -> dict[str, str | int]:
    return {
        **base_payload,
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
        value = json.loads(path.read_text(encoding="ascii"))
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
) -> tuple[str, str]:
    require_identity_fields(recorded, expected_base, path)
    expected_fields = set(expected_base) | {"state", "transaction_id"}
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
    return state, transaction_id


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
    if source_root == values["control_root"]:
        return

    expected_base = base_identity_payload(values)
    root_path = root_identity_path(values)
    root_state: str | None = None
    root_transaction_id: str | None = None
    if root_path.exists():
        root_state, root_transaction_id = require_identity_record(
            read_identity(root_path),
            expected_base,
            root_path,
            frozenset((PROVISIONAL_STATE, FINAL_STATE)),
        )
    elif build_root.exists() and any(build_root.iterdir()):
        fail(
            "external build root lacks a source identity; "
            f"select an empty build root: {build_root}"
        )

    path = identity_path(values)
    if path.exists():
        _build_state, build_transaction_id = require_identity_record(
            read_identity(path),
            expected_base,
            path,
            frozenset((FINAL_STATE,)),
        )
        if root_state == FINAL_STATE:
            assert root_transaction_id is not None
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
        with os.fdopen(descriptor, "w", encoding="ascii") as temporary:
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
        fail(
            "external build root lacks prepared source identity: "
            f"{root_path}"
        )
    _root_state, transaction_id = require_identity_record(
        read_identity(root_path),
        expected_base,
        root_path,
        frozenset((PROVISIONAL_STATE,)),
    )
    final_record = identity_record(
        expected_base,
        FINAL_STATE,
        transaction_id,
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
    _root_state, root_transaction_id = require_identity_record(
        read_identity(root_path),
        expected_base,
        root_path,
        frozenset((FINAL_STATE,)),
    )

    path = identity_path(values)
    if not path.is_file():
        fail(f"external build directory lacks source identity: {path}")
    _build_state, build_transaction_id = require_identity_record(
        read_identity(path),
        expected_base,
        path,
        frozenset((FINAL_STATE,)),
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
    root_state, root_transaction_id = require_identity_record(
        read_identity(root_path),
        expected_base,
        root_path,
        allowed_root_states,
    )
    if not builddir.exists():
        return

    path = identity_path(values)
    if path.exists():
        _build_state, build_transaction_id = require_identity_record(
            read_identity(path),
            expected_base,
            path,
            frozenset((FINAL_STATE,)),
        )
        if root_state == FINAL_STATE:
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
    subparsers.add_parser("write-identity")
    subparsers.add_parser("verify-identity")
    subparsers.add_parser("verify-delete-identity")
    subparsers.add_parser("verify-distclean-identity")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    values = resolved_inputs()

    if arguments.command == "resolve-make":
        fields = (
            values["source_root"],
            values["build_root"],
            values["builddir"],
            values["prefix"],
            values["source_commit"],
            values["source_tree"],
            values["control_commit"],
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
    elif arguments.command == "write-identity":
        validate_layout("configure", values)
        write_identity(values)
    elif arguments.command == "verify-identity":
        verify_identity(values)
    elif arguments.command == "verify-delete-identity":
        verify_delete_identity(values, allow_provisional=True)
    elif arguments.command == "verify-distclean-identity":
        verify_delete_identity(values, allow_provisional=False)
    else:
        fail(f"unsupported command: {arguments.command}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ControlError as error:
        print(f"source-root-control: {error}", file=sys.stderr)
        raise SystemExit(2)
