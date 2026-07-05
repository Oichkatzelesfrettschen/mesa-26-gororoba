#!/usr/bin/env python3
"""Emit the source allowlist a physical prune must respect, from the build graph.

The physical source-prune branch may delete only paths the active
profiles' meson build graphs never reach.  Hand-curated lists rot; this
generator introspects one or more configured build directories
(``meson introspect --targets``), collects every source and extra file
each target compiles or generates from, adds the meson build files that
define reachable targets, and emits a sorted repo-relative allowlist
plus a summary of top-level directories with zero reachable files --
the candidate deletion surface.  Headers pulled via include paths do
not appear in target_sources, so the emitted list is a LOWER BOUND:
the prune branch pairs it with a clean-checkout build as the real gate,
and the keep-set stop conditions in the migration plan override it.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys


def collect(builddir: str, repo_root: str) -> set[str]:
    build_real = os.path.realpath(builddir) + os.sep
    raw = subprocess.run(
        ["meson", "introspect", builddir, "--targets"],
        capture_output=True, text=True, check=True).stdout
    targets = json.loads(raw)
    root = os.path.realpath(repo_root) + os.sep
    keep: set[str] = set()

    def add(path: str) -> None:
        real = os.path.realpath(path)
        if real.startswith(build_real):
            return  # generated output; the generator, not the file, is the source
        if real.startswith(root):
            keep.add(os.path.relpath(real, root))

    for target in targets:
        add(target.get("defined_in", ""))
        for group in target.get("target_sources", []):
            for key in ("sources", "generated_sources"):
                for path in group.get(key, []) or []:
                    add(path)
        for path in target.get("extra_files", []) or []:
            add(path)
    return keep


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("builddirs", nargs="+",
                        help="configured meson build directories to union")
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--out", default="-",
                        help="allowlist output path ('-' for stdout)")
    args = parser.parse_args()

    repo_root = os.path.realpath(args.repo_root)
    keep: set[str] = set()
    for builddir in args.builddirs:
        keep |= collect(builddir, repo_root)

    listing = sorted(keep)
    out = sys.stdout if args.out == "-" else open(args.out, "w")
    for path in listing:
        print(path, file=out)
    if out is not sys.stdout:
        out.close()

    reachable_top = {p.split(os.sep, 1)[0] for p in listing}
    tracked = subprocess.run(
        ["git", "-C", repo_root, "ls-tree", "--name-only", "HEAD"],
        capture_output=True, text=True, check=True).stdout.split()
    all_top = {
        entry for entry in tracked
        if os.path.isdir(os.path.join(repo_root, entry))}
    print(f"allowlist: {len(listing)} files across "
          f"{len(reachable_top)} top-level dirs", file=sys.stderr)
    unreachable = sorted(all_top - reachable_top)
    print("top-level dirs with zero reachable files (candidate prune "
          f"surface, keep-set rules override): {' '.join(unreachable)}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
