#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Build the r300 CS-track replay tool from a pinned kernel tree.

The qualification runner needs replay_r300_cs_track as a build output, not an
operator-supplied binary.  This module pins the kernel source to one commit,
requires a clean working tree at that commit, regenerates the safe-register
header the replay source depends on from that tree's mkregtable and
reg_srcs/r300, runs the tree's own r300_cs_grammar_correspondence fidelity
gate unless the caller opts out, compiles the replay source, and writes a
provenance record binding the output ELF to the exact kernel commit, the
per-file hashes that fed the compile, the compiler identity, and the compile
argv.  The correspondence gate
(scripts/run_r300_cs_grammar_correspondence.sh) is the replay's fidelity
check against the kernel's own r300_packet0_check grammar; a nonzero exit
from that gate refuses the build, the same way a dirty tree or a commit
mismatch does.  The builder also requires the deployed Radeon subtree and
module srcversion, and checks them against the pinned source tree, the loaded
module, and the installed module metadata before it publishes provenance.
"""

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NoReturn

REPLAY_SOURCE_RELPATH = "scripts/replay_r300_cs_track.c"
CORRESPONDENCE_SCRIPT_RELPATH = "scripts/run_r300_cs_grammar_correspondence.sh"
RADEON_DIR_RELPATH = "drivers/gpu/drm/radeon"
MKREGTABLE_RELPATH = "drivers/gpu/drm/radeon/mkregtable.c"
REG_SRCS_R300_RELPATH = "drivers/gpu/drm/radeon/reg_srcs/r300"
REG_SAFE_HEADER_NAME = "r300_reg_safe.h"
PROVENANCE_SCHEMA = "r3v-cs-track-replay-provenance/v1"
PROVENANCE_BUILDER = "build-infra/r3v/build_kernel_replay.py"
REPLAY_ARTIFACT = "replay_r300_cs_track"
MODULE_NAME = "radeon"
MODULE_SYSFS_SRCVERSION = Path("/sys/module/radeon/srcversion")
MODULE_DRIVER_TREE_FIELD = "gororoba_driver_tree"
OBJECT_ID_RE = re.compile(r"^[0-9a-f]{40}$")
IDENTITY_RE = re.compile(r"^[A-Za-z0-9._+-]+$")
MODULE_SRCVERSION_RE = re.compile(r"^[A-Za-z0-9]+$")

# The replay source and run_r300_cs_grammar_correspondence.sh both build it
# with -O2 -Wall -Wextra -Werror; matching that flag set keeps this build the
# same artifact the in-tree gate exercises, not a differently-flagged twin.
COMPILE_FLAGS = ["-O2", "-Wall", "-Wextra", "-Werror"]

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')


def fail(message: str) -> NoReturn:
    print(f"build_kernel_replay: {message}", file=sys.stderr)
    sys.exit(1)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run_git(kernel_root: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(kernel_root), *args],
        capture_output=True,
        text=True,
    )


def require_identity(value: str, label: str, pattern=IDENTITY_RE) -> str:
    if not isinstance(value, str) or not pattern.fullmatch(value):
        fail(f"{label} is missing or malformed: {value!r}")
    return value


def read_identity_file(path: Path, label: str, pattern=IDENTITY_RE) -> str:
    try:
        value = path.read_text().strip()
    except OSError as error:
        fail(f"cannot read {label} from {path}: {error}")
    return require_identity(value, label, pattern)


def read_modinfo_field(field: str, pattern=IDENTITY_RE) -> str:
    try:
        result = subprocess.run(
            ["modinfo", "-F", field, MODULE_NAME],
            capture_output=True,
            text=True,
        )
    except OSError as error:
        fail(f"cannot execute modinfo for {MODULE_NAME}: {error}")
    if result.returncode != 0:
        fail(f"modinfo cannot read {field} for {MODULE_NAME}: "
             f"{result.stderr.strip()}")
    return require_identity(result.stdout.strip(), f"modinfo {field}", pattern)


def resolve_driver_tree(kernel_root: Path, resolved_commit: str) -> str:
    result = run_git(
        kernel_root, "rev-parse", f"{resolved_commit}:{RADEON_DIR_RELPATH}")
    if result.returncode != 0:
        fail(f"git cannot resolve the Radeon driver tree at {resolved_commit}: "
             f"{result.stderr.strip()}")
    return require_identity(
        result.stdout.strip(),
        f"{RADEON_DIR_RELPATH} tree",
        OBJECT_ID_RE,
    )


def validate_authority_values(
    declared_driver_tree: str,
    source_driver_tree: str,
    declared_srcversion: str,
    running_srcversion: str,
    installed_driver_tree: str,
    installed_srcversion: str,
) -> None:
    """Require one source and runtime identity chain for qualification."""
    require_identity(declared_driver_tree, "deployed driver tree", OBJECT_ID_RE)
    require_identity(source_driver_tree, "source driver tree", OBJECT_ID_RE)
    require_identity(declared_srcversion, "running module srcversion",
                     MODULE_SRCVERSION_RE)
    require_identity(running_srcversion, "observed module srcversion",
                     MODULE_SRCVERSION_RE)
    require_identity(installed_driver_tree, "installed driver tree", OBJECT_ID_RE)
    require_identity(installed_srcversion, "installed module srcversion",
                     MODULE_SRCVERSION_RE)

    if declared_driver_tree != source_driver_tree:
        fail("deployed driver tree does not match the pinned source tree: "
             f"{declared_driver_tree} != {source_driver_tree}")
    if installed_driver_tree != declared_driver_tree:
        fail("installed driver tree does not match the declared deployment: "
             f"{installed_driver_tree} != {declared_driver_tree}")
    if declared_srcversion != running_srcversion:
        fail("running module srcversion does not match the declared "
             f"deployment: {running_srcversion} != {declared_srcversion}")
    if installed_srcversion != declared_srcversion:
        fail("installed module srcversion does not match the declared "
             f"deployment: {installed_srcversion} != {declared_srcversion}")


def verify_deployed_driver_authority(
    source_root: Path,
    resolved_commit: str,
    declared_driver_tree: str,
    declared_srcversion: str,
) -> str:
    require_identity(declared_driver_tree, "deployed driver tree", OBJECT_ID_RE)
    require_identity(declared_srcversion, "running module srcversion",
                     MODULE_SRCVERSION_RE)
    source_driver_tree = resolve_driver_tree(source_root, resolved_commit)
    running_srcversion = read_identity_file(
        MODULE_SYSFS_SRCVERSION, "running module srcversion",
        MODULE_SRCVERSION_RE)
    installed_driver_tree = read_modinfo_field(MODULE_DRIVER_TREE_FIELD)
    installed_srcversion = read_modinfo_field("srcversion",
                                              MODULE_SRCVERSION_RE)
    validate_authority_values(
        declared_driver_tree=declared_driver_tree,
        source_driver_tree=source_driver_tree,
        declared_srcversion=declared_srcversion,
        running_srcversion=running_srcversion,
        installed_driver_tree=installed_driver_tree,
        installed_srcversion=installed_srcversion,
    )
    return source_driver_tree


def require_artifact_destinations_outside(
    source_root: Path, output: Path, provenance_path: Path
) -> None:
    if output == provenance_path:
        fail("--output and --provenance resolve to the same path")
    for destination in (output, provenance_path):
        if destination.is_relative_to(source_root):
            fail(f"artifact destination {destination} lies inside the "
                 "source checkout")


def worktree_is_registered(source_root: Path, worktree: Path) -> bool:
    listed = run_git(source_root, "worktree", "list", "--porcelain")
    if listed.returncode != 0:
        return True
    return f"worktree {worktree}" in listed.stdout.splitlines()


def cleanup_isolated_worktree(
    source_root: Path,
    worktree: Path,
    isolation_tmp: tempfile.TemporaryDirectory,
    worktree_registered: bool,
) -> str | None:
    """Unregister the detached checkout before its temporary directory goes."""
    errors: list[str] = []
    if worktree_registered:
        remove = subprocess.run(
            ["git", "-C", str(source_root), "worktree", "remove", "--force",
             "--force", str(worktree)],
            capture_output=True,
            text=True,
        )
        if remove.returncode != 0:
            errors.append(f"git worktree remove failed: {remove.stderr.strip()}")

    prune = subprocess.run(
        ["git", "-C", str(source_root), "worktree", "prune"],
        capture_output=True,
        text=True,
    )
    if prune.returncode != 0:
        errors.append(f"git worktree prune failed: {prune.stderr.strip()}")

    isolation_tmp.cleanup()
    return "; ".join(errors) if errors else None


def resolve_pinned_commit(kernel_root: Path, kernel_commit: str) -> str:
    """Resolve kernel_commit and require it to name the checkout HEAD."""
    toplevel = run_git(kernel_root, "rev-parse", "--is-inside-work-tree")
    if toplevel.returncode != 0 or toplevel.stdout.strip() != "true":
        fail(f"{kernel_root} is not a git working tree "
             f"(git rev-parse --is-inside-work-tree: {toplevel.stderr.strip()})")

    verify = run_git(kernel_root, "rev-parse", "--verify", f"{kernel_commit}^{{commit}}")
    if verify.returncode != 0:
        fail(f"--kernel-commit {kernel_commit} does not resolve to a commit in "
             f"{kernel_root}: {verify.stderr.strip()}")
    resolved_commit = verify.stdout.strip()

    head = run_git(kernel_root, "rev-parse", "HEAD")
    if head.returncode != 0:
        fail(f"git rev-parse HEAD failed in {kernel_root}: {head.stderr.strip()}")
    head_sha = head.stdout.strip()

    if resolved_commit != head_sha:
        fail(f"--kernel-commit {kernel_commit} resolves to {resolved_commit}, "
             f"which does not match HEAD {head_sha}; the kernel root must be "
             f"checked out at the pinned commit")

    return head_sha


def require_clean_pinned_tree(kernel_root: Path, kernel_commit: str) -> str:
    """Refuse a kernel root that is not clean at kernel_commit.

    Returns the full HEAD SHA once every check passes.  The caller resolves
    the source checkout before creating the detached snapshot; only that
    snapshot needs a clean-tree proof for the build inputs.
    """
    resolved_commit = resolve_pinned_commit(kernel_root, kernel_commit)
    status = run_git(kernel_root, "status", "--porcelain")
    if status.returncode != 0:
        fail(f"git status failed in {kernel_root}: {status.stderr.strip()}")
    if status.stdout.strip():
        fail(f"{kernel_root} has a dirty working tree; a decision-grade build "
             f"binds to a clean checkout at one commit:\n{status.stdout}")
    return resolved_commit


def run_correspondence_gate(kernel_root: Path, workdir: Path) -> None:
    script = kernel_root / CORRESPONDENCE_SCRIPT_RELPATH
    if not script.is_file():
        fail(f"correspondence gate script missing: {script}")
    gate_workdir = workdir / "correspondence"
    gate_workdir.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        ["sh", str(script), str(gate_workdir)],
        cwd=str(kernel_root),
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        fail(f"kernel-grammar correspondence fidelity gate failed "
             f"(exit {result.returncode}); the replay source's fidelity to "
             f"the kernel's r300_packet0_check grammar is unproven, so the "
             f"build refuses rather than ship an unverified replay")


def generate_reg_safe_header(kernel_root: Path, workdir: Path, cc: str) -> Path:
    mkregtable_src = kernel_root / MKREGTABLE_RELPATH
    reg_srcs_r300 = kernel_root / REG_SRCS_R300_RELPATH
    if not mkregtable_src.is_file():
        fail(f"mkregtable source missing: {mkregtable_src}")
    if not reg_srcs_r300.is_file():
        fail(f"reg_srcs/r300 missing: {reg_srcs_r300}")

    mkregtable_bin = workdir / "mkregtable"
    build = subprocess.run(
        [cc, "-O2", "-o", str(mkregtable_bin), str(mkregtable_src)],
        capture_output=True,
        text=True,
    )
    if build.returncode != 0:
        sys.stderr.write(build.stderr)
        fail(f"mkregtable build failed (exit {build.returncode})")

    gen = subprocess.run(
        [str(mkregtable_bin), str(reg_srcs_r300)],
        capture_output=True,
        text=True,
    )
    if gen.returncode != 0:
        sys.stderr.write(gen.stderr)
        fail(f"mkregtable failed to generate {REG_SAFE_HEADER_NAME} "
             f"(exit {gen.returncode})")

    reg_safe_header = workdir / REG_SAFE_HEADER_NAME
    reg_safe_header.write_text(gen.stdout)
    return reg_safe_header


def compiler_identity(cc: str) -> str:
    result = subprocess.run([cc, "--version"], capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"{cc} --version failed (exit {result.returncode}): {result.stderr.strip()}")
    first_line = result.stdout.splitlines()[0] if result.stdout else ""
    if not first_line:
        fail(f"{cc} --version produced no output")
    return first_line


def compile_replay(kernel_root: Path, workdir: Path, cc: str, output: Path) -> list:
    replay_src = kernel_root / REPLAY_SOURCE_RELPATH
    radeon_dir = kernel_root / RADEON_DIR_RELPATH
    if not replay_src.is_file():
        fail(f"replay source missing: {replay_src}")

    argv = [
        cc, *COMPILE_FLAGS,
        "-I", str(workdir),
        "-I", str(radeon_dir),
        "-o", str(output),
        str(replay_src),
    ]
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        fail(f"replay_r300_cs_track.c compile failed (exit {result.returncode})")
    return argv


def resolve_quoted_includes(replay_src: Path, search_dirs: list) -> dict:
    """Resolve replay_r300_cs_track.c's quoted #include lines against
    search_dirs, in order, mirroring the compiler's own -I resolution.

    Returns {included-name: resolved Path}, restricted to includes that
    resolve to a path under one of search_dirs (kernel-tree or generated
    build-staging headers, not system headers pulled in with <>).
    """
    resolved = {}
    for line in replay_src.read_text().splitlines():
        m = INCLUDE_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        for d in search_dirs:
            candidate = d / name
            if candidate.is_file():
                resolved[name] = candidate
                break
    return resolved


def build_provenance(
    kernel_root: Path,
    kernel_commit_arg: str,
    resolved_commit: str,
    workdir: Path,
    cc: str,
    compile_argv: list,
    output: Path,
    output_sha256: str,
    correspondence_gate: str,
    isolated_worktree: bool,
    kernel_driver_tree: str,
    deployed_driver_tree: str,
    running_module_srcversion: str,
) -> dict:
    replay_src = kernel_root / REPLAY_SOURCE_RELPATH
    radeon_dir = kernel_root / RADEON_DIR_RELPATH

    sources = {
        REPLAY_SOURCE_RELPATH: {
            "path": str(replay_src),
            "sha256": sha256_file(replay_src),
        }
    }

    includes = resolve_quoted_includes(replay_src, [workdir, radeon_dir])
    for name, resolved_path in includes.items():
        entry = {
            "path": str(resolved_path),
            "sha256": sha256_file(resolved_path),
        }
        if name == REG_SAFE_HEADER_NAME:
            # r300_reg_safe.h has no static copy in the kernel tree; mkregtable
            # generates it from reg_srcs/r300 at build time, so its provenance
            # is the two kernel-tree inputs that generated it, hashed in turn.
            mkregtable_src = kernel_root / MKREGTABLE_RELPATH
            reg_srcs_r300 = kernel_root / REG_SRCS_R300_RELPATH
            entry["generated_from"] = {
                MKREGTABLE_RELPATH: sha256_file(mkregtable_src),
                REG_SRCS_R300_RELPATH: sha256_file(reg_srcs_r300),
            }
        sources[name] = entry

    return {
        "schema": PROVENANCE_SCHEMA,
        "builder": PROVENANCE_BUILDER,
        "artifact": REPLAY_ARTIFACT,
        "kernel_commit": resolved_commit,
        "kernel_commit_requested": kernel_commit_arg,
        # The tool source and the driver logic compile from one pinned tree,
        # so the two authority fields carry the same object; a future build
        # whose replay tooling rides a different commit than the driver
        # under test splits here rather than silently.
        "kernel_tool_source_sha": resolved_commit,
        "kernel_driver_logic_sha": resolved_commit,
        "kernel_driver_logic_tree": kernel_driver_tree,
        "deployed_driver_tree": deployed_driver_tree,
        "running_module_srcversion": running_module_srcversion,
        "deployed_driver_authority_verified": True,
        "kernel_root": str(kernel_root),
        "sources": sources,
        "compiler": compiler_identity(cc),
        "compile_argv": compile_argv,
        "output": str(output),
        "output_sha256": output_sha256,
        "correspondence_gate": correspondence_gate,
        "isolated_worktree": isolated_worktree,
    }


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build replay_r300_cs_track from a pinned kernel tree "
                    "and record its build provenance."
    )
    parser.add_argument("--kernel-root", required=True, type=Path,
                        help="path to the linux-radeon-gororoba working tree")
    parser.add_argument("--kernel-commit", required=True,
                        help="full or abbreviated SHA the kernel root must "
                             "be checked out at")
    parser.add_argument("--cc", default="cc", help="C compiler (default: cc)")
    parser.add_argument("--output", required=True, type=Path,
                        help="path for the built replay_r300_cs_track ELF")
    parser.add_argument("--provenance", required=True, type=Path,
                        help="path for the JSON provenance record")
    parser.add_argument("--skip-correspondence", action="store_true",
                        help="skip the kernel-grammar correspondence "
                             "fidelity gate (recorded in the provenance; "
                             "the gate runs by default)")
    parser.add_argument("--isolate-worktree", action="store_true",
                        help="accepted for compatibility; every build uses "
                             "a fresh detached git worktree at the pinned "
                             "commit")
    parser.add_argument("--deployed-driver-tree", required=True,
                        help="driver-tree object of the deployed module "
                             "source; it must match the pinned Radeon "
                             "subtree and installed module metadata")
    parser.add_argument("--running-module-srcversion", required=True,
                        help="srcversion of the module on the target, "
                             "which must match both the loaded sysfs "
                             "identity and installed module metadata")
    return parser.parse_args(argv)


def selftest_check(label: str, callback, expected: str | None = None) -> bool:
    try:
        callback()
    except SystemExit as error:
        if error.code == 1:
            return True
        print(f"selftest failure: {label}: unexpected exit {error.code}",
              file=sys.stderr)
        return False
    if expected is None:
        print(f"selftest failure: {label}: expected refusal", file=sys.stderr)
    else:
        print(f"selftest failure: {label}: expected {expected!r}",
              file=sys.stderr)
    return False


def selftest() -> int:
    driver_tree = "a" * 40
    srcversion = "A" * 24
    checks = [
        ("authority good", lambda: validate_authority_values(
            driver_tree, driver_tree, srcversion, srcversion,
            driver_tree, srcversion), False),
        ("authority source tree mismatch", lambda: validate_authority_values(
            driver_tree, "b" * 40, srcversion, srcversion,
            driver_tree, srcversion), True),
        ("authority installed tree mismatch", lambda: validate_authority_values(
            driver_tree, driver_tree, srcversion, srcversion,
            "b" * 40, srcversion), True),
        ("authority running srcversion mismatch", lambda: validate_authority_values(
            driver_tree, driver_tree, srcversion, "B" * 24,
            driver_tree, srcversion), True),
        ("authority installed srcversion mismatch", lambda: validate_authority_values(
            driver_tree, driver_tree, srcversion, srcversion,
            driver_tree, "B" * 24), True),
    ]
    passed = 0
    for label, callback, refusal in checks:
        if refusal:
            passed += selftest_check(label, callback)
        else:
            try:
                callback()
            except SystemExit as error:
                print(f"selftest failure: {label}: unexpected exit {error.code}",
                      file=sys.stderr)
                return 1
            passed += 1

    with tempfile.TemporaryDirectory(prefix="r3v-replay-selftest-") as tmp:
        root = Path(tmp) / "kernel"
        outside = Path(tmp) / "artifacts"
        outside.mkdir()
        require_artifact_destinations_outside(
            root, outside / REPLAY_ARTIFACT, outside / "provenance.json")
        if not selftest_check(
                "artifact inside source checkout",
                lambda: require_artifact_destinations_outside(
                    root, root / REPLAY_ARTIFACT, outside / "provenance.json")):
            return 1
        passed += 1

    with tempfile.TemporaryDirectory(prefix="r3v-replay-cleanup-") as tmp:
        root = Path(tmp) / "source"
        root.mkdir()
        subprocess.run(["git", "-C", str(root), "init", "--quiet"],
                       check=True)
        (root / "README").write_text("replay cleanup fixture\n")
        subprocess.run(["git", "-C", str(root), "add", "README"],
                       check=True)
        subprocess.run([
            "git", "-C", str(root), "-c", "user.name=Replay Selftest",
            "-c", "user.email=replay-selftest@example.invalid", "commit",
            "--quiet", "--no-gpg-sign", "-m", "fixture",
        ], check=True)
        isolation_tmp = tempfile.TemporaryDirectory(
            prefix="r3v-replay-cleanup-worktree-")
        worktree = Path(isolation_tmp.name) / "tree"
        subprocess.run([
            "git", "-C", str(root), "worktree", "add", "--quiet",
            "--detach", str(worktree), "HEAD",
        ], check=True)
        cleanup_error = cleanup_isolated_worktree(
            root, worktree, isolation_tmp, True)
        if cleanup_error or worktree_is_registered(root, worktree):
            print("selftest failure: isolated worktree cleanup left a "
                  "registered worktree", file=sys.stderr)
            return 1
        passed += 1

    print(f"build_kernel_replay selftest: {passed} calibrated legs OK")
    return 0


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if argv == ["--selftest"]:
        return selftest()
    args = parse_args(argv)
    source_root = args.kernel_root.resolve()
    output = args.output.resolve()
    provenance_path = args.provenance.resolve()

    # Resolve and verify the caller's pinned identity before taking the
    # snapshot.  The source checkout supplies the commit object only; all
    # build inputs below come from the detached worktree.
    resolved_commit = resolve_pinned_commit(source_root, args.kernel_commit)

    # The ELF and the provenance are two artifacts: one destination would
    # overwrite the digested ELF with JSON, and a destination inside the
    # kernel root would dirty the pinned tree the build just proved clean.
    require_artifact_destinations_outside(
        source_root, output, provenance_path)

    kernel_driver_tree = verify_deployed_driver_authority(
        source_root,
        resolved_commit,
        args.deployed_driver_tree,
        args.running_module_srcversion,
    )

    output.parent.mkdir(parents=True, exist_ok=True)
    provenance_path.parent.mkdir(parents=True, exist_ok=True)

    isolation_tmp = tempfile.TemporaryDirectory(
        prefix="r300-cs-replay-worktree-")
    kernel_root = Path(isolation_tmp.name) / "tree"
    worktree_registered = False
    try:
        add = subprocess.run(
            ["git", "-C", str(source_root), "worktree", "add", "--detach",
             str(kernel_root), resolved_commit],
            capture_output=True, text=True)
        worktree_registered = (
            add.returncode == 0 or
            worktree_is_registered(source_root, kernel_root))
        if add.returncode != 0:
            fail(f"git worktree add failed: {add.stderr.strip()}")

        for destination in (output, provenance_path):
            if destination.is_relative_to(kernel_root):
                fail(f"artifact destination {destination} lies inside the "
                     "detached source snapshot")
        require_clean_pinned_tree(kernel_root, resolved_commit)

        correspondence_gate = "skipped"
        if not args.skip_correspondence:
            with tempfile.TemporaryDirectory(
                    prefix="r300-cs-grammar-gate-") as gate_tmp:
                run_correspondence_gate(kernel_root, Path(gate_tmp))
            correspondence_gate = "pass"

        with tempfile.TemporaryDirectory(
                prefix="r300-cs-replay-build-") as build_tmp:
            build_workdir = Path(build_tmp)
            generate_reg_safe_header(kernel_root, build_workdir, args.cc)
            compile_argv = compile_replay(kernel_root, build_workdir, args.cc,
                                           output)
            output_sha256 = sha256_file(output)
            provenance = build_provenance(
                kernel_root=kernel_root,
                kernel_commit_arg=args.kernel_commit,
                resolved_commit=resolved_commit,
                workdir=build_workdir,
                cc=args.cc,
                compile_argv=compile_argv,
                output=output,
                output_sha256=output_sha256,
                correspondence_gate=correspondence_gate,
                isolated_worktree=True,
                kernel_driver_tree=kernel_driver_tree,
                deployed_driver_tree=args.deployed_driver_tree,
                running_module_srcversion=args.running_module_srcversion,
            )

        # The clean-tree proof covers every source and generated input used by
        # the gate, header generation, compile, and hash.  Publish nothing
        # until the detached snapshot remains exactly at the pinned commit.
        require_clean_pinned_tree(kernel_root, resolved_commit)

    finally:
        cleanup_error = cleanup_isolated_worktree(
            source_root,
            kernel_root,
            isolation_tmp,
            worktree_registered,
        )
        if cleanup_error:
            print(f"build_kernel_replay: {cleanup_error}", file=sys.stderr)
            if sys.exc_info()[0] is None:
                raise SystemExit(1)

    provenance_path.write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n")

    print(f"{output} sha256={output_sha256[:12]} correspondence_gate={correspondence_gate}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
