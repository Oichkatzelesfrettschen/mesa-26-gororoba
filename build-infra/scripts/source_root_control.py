#!/usr/bin/env python3
"""Validate external Mesa source selection and bind it to a build directory."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NoReturn

SCHEMA_VERSION = 1
IDENTITY_FILENAME = ".gororoba-source-identity.json"
ROOT_IDENTITY_FILENAME = ".gororoba-external-source-identity.json"
SAFE_PATH_INPUT = re.compile(r"^[A-Za-z0-9_./:+@=~-]+$")


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
    command = ["git", "-C", str(root), *arguments]
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
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
    assert isinstance(prefix, Path)
    assert isinstance(source_root, Path)
    assert isinstance(repository_root, Path)

    profile = os.environ.get("GOROROBA_PROFILE_INPUT", "")
    allowed_opt_prefixes = {
        Path(f"/opt/local/mesa-{profile}"),
        Path("/opt/local/mesa-26-gororoba"),
        Path("/opt/local/mesa-gororoba-debug-optimized"),
        Path("/opt/mesa-gororoba-debug-optimized"),
        Path("/opt/mesa-gororoba-debug-asan"),
        Path("/opt/mesa-gororoba-debug-o0"),
    }
    if is_within_or_equal(prefix, Path("/opt")) and prefix not in allowed_opt_prefixes:
        fail(f"refusing unsafe PREFIX: {prefix}")
    reject_protected_path(prefix, "PREFIX")
    home = Path.home().resolve(strict=False)
    if prefix == home:
        fail(f"refusing unsafe PREFIX: {prefix}")
    if is_within_or_equal(prefix, source_root):
        fail(f"refusing PREFIX inside TOPSRC: {prefix}")
    if is_within_or_equal(prefix, repository_root):
        fail(f"refusing PREFIX inside the control worktree: {prefix}")


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
    if not is_strict_descendant(builddir, build_root):
        fail(f"refusing BUILDDIR outside BUILD_ROOT: {builddir}")

    source_is_control = source_root == repository_root
    repository_build_root = repository_root / "build"
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


def identity_payload(values: dict[str, Path | str]) -> dict[str, str | int]:
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


def root_identity_payload(values: dict[str, Path | str]) -> dict[str, str | int]:
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


def prepare_identity(values: dict[str, Path | str]) -> None:
    source_root = values["source_root"]
    build_root = values["build_root"]
    builddir = values["builddir"]
    assert isinstance(source_root, Path)
    assert isinstance(build_root, Path)
    assert isinstance(builddir, Path)
    if source_root == values["control_root"]:
        return

    expected = identity_payload(values)
    root_expected = root_identity_payload(values)
    root_path = root_identity_path(values)
    if root_path.exists():
        require_identity_fields(read_identity(root_path), root_expected, root_path)
    elif build_root.exists() and any(build_root.iterdir()):
        fail(
            "external build root lacks a source identity; "
            f"select an empty build root: {build_root}"
        )

    path = identity_path(values)
    if path.exists():
        require_identity_fields(read_identity(path), expected, path)
        return

    if builddir.exists() and any(builddir.iterdir()):
        fail(
            "external build directory lacks a source identity; "
            f"clean it before configure: {builddir}"
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

    payload = identity_payload(values)
    root_payload = root_identity_payload(values)
    root_path = root_identity_path(values)
    if root_path.exists():
        require_identity_fields(read_identity(root_path), root_payload, root_path)
    write_json_atomic(root_path, root_payload)
    write_json_atomic(identity_path(values), payload)


def verify_identity(values: dict[str, Path | str]) -> None:
    source_root = values["source_root"]
    assert isinstance(source_root, Path)
    if source_root == values["control_root"]:
        return

    expected = identity_payload(values)
    root_expected = root_identity_payload(values)
    root_path = root_identity_path(values)
    if not root_path.is_file():
        fail(f"external build root lacks source identity: {root_path}")
    require_identity_fields(read_identity(root_path), root_expected, root_path)

    path = identity_path(values)
    if not path.is_file():
        fail(f"external build directory lacks source identity: {path}")
    require_identity_fields(read_identity(path), expected, path)


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
    else:
        fail(f"unsupported command: {arguments.command}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ControlError as error:
        print(f"source-root-control: {error}", file=sys.stderr)
        raise SystemExit(2)
