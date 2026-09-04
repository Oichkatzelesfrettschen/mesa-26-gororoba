#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prove a repository change lies outside a built ICD's input closure.

A qualification build whose only failing test is a repository-policy
check on a documentation ledger has two separable verdicts: the executable
one, which asks whether any input the compiled ICD read changed under the
repair, and the repository-policy one, which asks whether the repaired
check passes.  This tool derives the executable verdict from the build
graph rather than from a claim: the closure of a target is the union of
ninja's explicit and implicit inputs for the target (``ninja -t inputs``)
and the header dependencies the compiler recorded for every object the
target links (``ninja -t deps``), mapped back to source-root-relative
paths.  The repair's changed paths come from ``git diff --name-only`` over
the declared range, and the verdict is the intersection: empty means the
ICD bytes are reproducible from the pre-repair inputs, nonempty names the
inputs that moved and requires a rebuild.

Usage:
  check_icd_input_closure.py --builddir DIR --target PATH --source-root DIR
                             --repair-range A..B [--git-root DIR]
                             [--closure-out FILE] [--ratchet CMD...]

A reproducible qualification compiles an archive-derived source view that
is not a repository, so ``--git-root`` names the checkout the range
resolves in while ``--source-root`` stays the view the build graph read.

Exit 0 when the intersection is empty (and the ratchet, when given, passes),
1 when an input moved or the ratchet fails, 2 on usage or tool failure.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def run(args: list[str], cwd: Path | None = None) -> str:
    completed = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if completed.returncode != 0:
        print(f"check_icd_input_closure: {' '.join(args)} failed:\n"
              f"{completed.stderr}", file=sys.stderr)
        sys.exit(2)
    return completed.stdout


def normalize(builddir: Path, source_root: Path, path: str) -> str | None:
    """A ninja path is builddir-relative; return it source-root-relative,
    or None for a generated file inside the builddir."""
    absolute = (builddir / path).resolve()
    try:
        return absolute.relative_to(source_root.resolve()).as_posix()
    except ValueError:
        return None


def target_closure(builddir: Path, source_root: Path, target: str):
    inputs = run(["ninja", "-C", str(builddir), "-t", "inputs", target])
    input_paths = [line for line in inputs.splitlines() if line]
    objects = [p for p in input_paths if p.endswith(".o")]
    deps_out = run(["ninja", "-C", str(builddir), "-t", "deps", *objects]) \
        if objects else ""
    header_paths = []
    for line in deps_out.splitlines():
        if line.startswith("    "):
            header_paths.append(line.strip())
    closure_source: set[str] = set()
    generated: set[str] = set()
    for p in input_paths + header_paths:
        rel = normalize(builddir, source_root, p)
        if rel is None:
            generated.add(p)
        else:
            closure_source.add(rel)
    return closure_source, generated, len(objects)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--builddir", type=Path, required=True)
    parser.add_argument("--target", required=True,
                        help="builddir-relative target, e.g. "
                             "src/amd/r300/vulkan/libvulkan_r3v.so")
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--repair-range", required=True,
                        help="git range A..B of the repository-policy repair")
    parser.add_argument("--git-root", type=Path,
                        help="repository the repair range resolves in when "
                             "the source root is an archive-derived view "
                             "(default: the source root)")
    parser.add_argument("--closure-out", type=Path)
    parser.add_argument("--ratchet", nargs=argparse.REMAINDER,
                        help="command running the repaired repository-policy "
                             "check; its exit status is the second verdict")
    args = parser.parse_args()

    closure, generated, object_count = target_closure(
        args.builddir, args.source_root, args.target)
    if not closure:
        print("check_icd_input_closure: the target has an empty source "
              "closure; the build graph did not resolve", file=sys.stderr)
        return 2
    changed = [line for line in run(
        ["git", "diff", "--name-only", args.repair_range],
        cwd=args.git_root or args.source_root).splitlines() if line]
    if not changed:
        print("check_icd_input_closure: the repair range changes no path; "
              "the denominator is empty", file=sys.stderr)
        return 2
    moved = sorted(set(changed) & closure)
    print(f"target {args.target}: {object_count} objects, "
          f"{len(closure)} source inputs, {len(generated)} generated inputs")
    print(f"repair {args.repair_range}: {len(changed)} changed paths")
    for path in changed:
        mark = "INSIDE closure" if path in closure else "outside closure"
        print(f"  {mark}: {path}")
    if args.closure_out:
        args.closure_out.write_text(
            "".join(f"{p}\n" for p in sorted(closure)), encoding="utf-8")
    status = 0
    if moved:
        print(f"executable qualification: FAIL ({len(moved)} inputs of the "
              f"ICD moved under the repair; rebuild the ICD)")
        status = 1
    else:
        print("executable qualification: frozen ICD input closure PASS")
    if args.ratchet:
        completed = subprocess.run(args.ratchet, capture_output=True,
                                   text=True)
        tail = (completed.stdout + completed.stderr).strip().splitlines()
        tail = tail[-1] if tail else ""
        if completed.returncode == 0:
            print(f"repository-policy qualification: repaired ratchet PASS "
                  f"({tail})")
        else:
            print(f"repository-policy qualification: FAIL "
                  f"(exit {completed.returncode}: {tail})")
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
