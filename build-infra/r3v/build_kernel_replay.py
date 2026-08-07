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
mismatch does.
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


def require_clean_pinned_tree(kernel_root: Path, kernel_commit: str) -> str:
    """Refuse a kernel root that is not a clean checkout of kernel_commit.

    Returns the full HEAD SHA once every check passes.
    """
    toplevel = run_git(kernel_root, "rev-parse", "--is-inside-work-tree")
    if toplevel.returncode != 0 or toplevel.stdout.strip() != "true":
        fail(f"{kernel_root} is not a git working tree "
             f"(git rev-parse --is-inside-work-tree: {toplevel.stderr.strip()})")

    status = run_git(kernel_root, "status", "--porcelain")
    if status.returncode != 0:
        fail(f"git status failed in {kernel_root}: {status.stderr.strip()}")
    if status.stdout.strip():
        fail(f"{kernel_root} has a dirty working tree; a decision-grade build "
             f"binds to a clean checkout at one commit:\n{status.stdout}")

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
        "kernel_commit": resolved_commit,
        "kernel_commit_requested": kernel_commit_arg,
        # The tool source and the driver logic compile from one pinned tree,
        # so the two authority fields carry the same object; a future build
        # whose replay tooling rides a different commit than the driver
        # under test splits here rather than silently.
        "kernel_tool_source_sha": resolved_commit,
        "kernel_driver_logic_sha": resolved_commit,
        "kernel_root": str(kernel_root),
        "sources": sources,
        "compiler": compiler_identity(cc),
        "compile_argv": compile_argv,
        "output": str(output),
        "output_sha256": output_sha256,
        "correspondence_gate": correspondence_gate,
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
                        help="build from a fresh detached git worktree at "
                             "the pinned commit instead of the given "
                             "checkout, so concurrent edits or generated "
                             "files in a shared tree cannot enter the "
                             "provenance")
    parser.add_argument("--deployed-driver-tree", default=None,
                        help="driver-tree object of the deployed module "
                             "source, recorded verbatim so the provenance "
                             "separates the tool-source authority from the "
                             "deployed driver logic")
    parser.add_argument("--running-module-srcversion", default=None,
                        help="srcversion of the module on the target, "
                             "recorded verbatim as the running-kernel "
                             "authority the replay verdicts are compared "
                             "against")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    kernel_root = args.kernel_root.resolve()
    output = args.output.resolve()
    provenance_path = args.provenance.resolve()

    # Worktree isolation: the build root becomes a fresh detached checkout
    # at the pinned commit, clean by construction, so a shared checkout's
    # concurrent edits or generated files stay out of the provenance.  The
    # worktree lives outside the given root and is removed after the build.
    isolation_tmp = None
    if args.isolate_worktree:
        isolation_tmp = tempfile.TemporaryDirectory(
            prefix="r300-cs-replay-worktree-")
        worktree = Path(isolation_tmp.name) / "tree"
        add = subprocess.run(
            ["git", "-C", str(kernel_root), "worktree", "add", "--detach",
             str(worktree), args.kernel_commit],
            capture_output=True, text=True)
        if add.returncode != 0:
            fail(f"git worktree add failed: {add.stderr.strip()}")
        kernel_root = worktree

    # The ELF and the provenance are two artifacts: one destination would
    # overwrite the digested ELF with JSON, and a destination inside the
    # kernel root would dirty the pinned tree the build just proved clean.
    if output == provenance_path:
        fail("--output and --provenance resolve to the same path")
    for destination in (output, provenance_path):
        if destination.is_relative_to(kernel_root):
            fail(f"artifact destination {destination} lies inside the "
                 "pinned kernel root")

    resolved_commit = require_clean_pinned_tree(kernel_root, args.kernel_commit)

    output.parent.mkdir(parents=True, exist_ok=True)
    provenance_path.parent.mkdir(parents=True, exist_ok=True)

    correspondence_gate = "skipped"
    if not args.skip_correspondence:
        with tempfile.TemporaryDirectory(prefix="r300-cs-grammar-gate-") as gate_tmp:
            run_correspondence_gate(kernel_root, Path(gate_tmp))
        correspondence_gate = "pass"

    with tempfile.TemporaryDirectory(prefix="r300-cs-replay-build-") as build_tmp:
        build_workdir = Path(build_tmp)
        generate_reg_safe_header(kernel_root, build_workdir, args.cc)
        compile_argv = compile_replay(kernel_root, build_workdir, args.cc, output)
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
        )

    # The clean-tree proof holds only while the tree stays unchanged, so the
    # check runs again after the gate, header generation, compile, and
    # hashing: a tree that moved during the build invalidates the provenance
    # before it is published.
    require_clean_pinned_tree(kernel_root, args.kernel_commit)

    provenance["isolated_worktree"] = bool(args.isolate_worktree)
    provenance["deployed_driver_tree"] = args.deployed_driver_tree
    provenance["running_module_srcversion"] = args.running_module_srcversion

    if isolation_tmp is not None:
        subprocess.run(
            ["git", "-C", str(args.kernel_root.resolve()), "worktree",
             "remove", "--force", str(kernel_root)],
            capture_output=True, text=True)
        isolation_tmp.cleanup()

    provenance_path.write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n")

    print(f"{output} sha256={output_sha256[:12]} correspondence_gate={correspondence_gate}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
