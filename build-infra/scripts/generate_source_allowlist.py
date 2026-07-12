#!/usr/bin/env python3
"""Emit the source allowlist a physical prune must respect, from the build graph.

The physical source-prune branch may delete only paths the active
profiles' meson build graphs never reach.  Hand-curated lists rot; this
generator introspects one or more configured build directories
(``meson introspect --targets``, ``--buildsystem-files``, and
``--installed``), collects every source, generator dependency, install
input, and Meson build file the graph reads, and emits a sorted
repo-relative allowlist plus a summary of top-level directories with zero
reachable files -- the candidate deletion surface.

Headers pulled via include paths still do not appear in target_sources,
so the emitted list remains a LOWER BOUND: the prune branch pairs it
with a clean-checkout build as the real gate, and the keep-set stop
conditions in the migration plan override it.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys


def _repo_root_default() -> str:
    return os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def collect(builddir: str, repo_root: str) -> set[str]:
    build_real = os.path.realpath(builddir) + os.sep
    root = os.path.realpath(repo_root) + os.sep
    keep: set[str] = set()

    def add(path: str) -> None:
        if not path:
            return
        # Resolve relative paths against the repository root so output does
        # not depend on the caller's cwd.
        if not os.path.isabs(path):
            path = os.path.join(repo_root, path)
        real = os.path.realpath(path)
        if real.startswith(build_real):
            return  # generated output; the generator, not the file, is the source
        if real.startswith(root):
            keep.add(os.path.relpath(real, root))

    def introspect(flag: str) -> object:
        raw = subprocess.run(
            ["meson", "introspect", builddir, flag],
            capture_output=True, text=True, check=True).stdout
        return json.loads(raw)

    targets = introspect("--targets")
    assert isinstance(targets, list)
    for target in targets:
        add(target.get("defined_in", "") or "")
        for group in target.get("target_sources", []) or []:
            for key in ("sources", "generated_sources"):
                for path in group.get(key, []) or []:
                    add(path)
            # custom_target depend_files appear here on Meson versions that
            # export them; ignore missing keys so older introspect still runs.
            for path in group.get("extra_files", []) or []:
                add(path)
            for path in group.get("depend_files", []) or []:
                add(path)
        for path in target.get("extra_files", []) or []:
            add(path)
        for path in target.get("depend_files", []) or []:
            add(path)

    # Every Meson file read during configure (parents and option gates
    # that define no targets themselves, e.g. src/amd/meson.build).
    try:
        build_files = introspect("--buildsystem-files")
        if isinstance(build_files, list):
            for path in build_files:
                add(path)
    except (subprocess.CalledProcessError, json.JSONDecodeError):
        print("allowlist: WARN: --buildsystem-files unavailable; "
              "configure parents may be under-kept", file=sys.stderr)

    # Install-only inputs (public headers and data files with no compile edge).
    # meson introspect --installed is a dict of src->dest on older Meson and a
    # list of objects with source/destination keys on typical current versions.
    try:
        installed = introspect("--installed")
        if isinstance(installed, dict):
            for src in installed.keys():
                add(src)
        elif isinstance(installed, list):
            for entry in installed:
                if not isinstance(entry, dict):
                    continue
                src = entry.get("source") or entry.get("file")
                if src:
                    add(src)
        else:
            print("allowlist: WARN: unexpected --installed format",
                  file=sys.stderr)
    except (subprocess.CalledProcessError, json.JSONDecodeError):
        print("allowlist: WARN: --installed unavailable; "
              "install-only files may be under-kept", file=sys.stderr)

    return keep


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("builddirs", nargs="+",
                        help="configured meson build directories to union")
    parser.add_argument("--repo-root", default=None,
                        help="repository root (default: inferred from script path)")
    parser.add_argument("--out", default="-",
                        help="allowlist output path ('-' for stdout)")
    args = parser.parse_args()

    repo_root = os.path.realpath(args.repo_root or _repo_root_default())
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
