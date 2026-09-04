#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Name the files a comment-hygiene run judges, and state the denominator.

A hygiene verdict means nothing until the set it judged is known.  Four
git queries hold the files a working checkout can carry: the branch diff
against the merge base, the index, the working tree, and the untracked
set.  This tool reports all four counts, so a run that judges nothing
says so with every source visible rather than through one query's
silence.

Mode ``working-tree`` judges the union of the four and is the developer
and pre-commit denominator.  Mode ``committed`` judges the branch diff
and the index, and refuses when the working tree or the untracked set is
populated: a qualification verdict binds to the declared SHA, so a file
edited after the commit cannot enter the judged set.

The contract line precedes the file list on stdout:

  candidates base=<ref> mode=<mode> committed=<n> staged=<n>
  unstaged=<n> untracked=<n> total=<n> digest=<sha256[:12]>

Usage:
  comment_hygiene_candidates.py --repo-root DIR [--base REF]
                                [--mode working-tree|committed]
  comment_hygiene_candidates.py --self-test
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

FILTER = "--diff-filter=ACMRT"


def _git(root, *args):
    done = subprocess.run(("git", "-C", str(root)) + args,
                          capture_output=True, text=True)
    if done.returncode != 0:
        return []
    return [line for line in done.stdout.split("\n") if line]


def collect(root, base):
    """Return the four candidate sources, each a sorted path list."""
    merge_base = _git(root, "merge-base", base, "HEAD")
    anchor = merge_base[0] if merge_base else None
    committed = (_git(root, "diff", "--name-only", FILTER, anchor, "HEAD")
                 if anchor else [])
    return {
        "committed": sorted(committed),
        "staged": sorted(_git(root, "diff", "--cached", "--name-only", FILTER)),
        "unstaged": sorted(_git(root, "diff", "--name-only", FILTER)),
        "untracked": sorted(_git(root, "ls-files", "--others",
                                 "--exclude-standard")),
    }


def select(sources, mode):
    """Return (files, refusal) for the mode's judged set."""
    if mode == "committed":
        dirty = sources["unstaged"] + sources["untracked"]
        if dirty:
            return [], ("the working tree carries %d file(s) outside the "
                        "commit: %s" % (len(dirty), ", ".join(dirty[:5])))
        judged = sources["committed"] + sources["staged"]
    else:
        judged = (sources["committed"] + sources["staged"]
                  + sources["unstaged"] + sources["untracked"])
    return sorted(set(judged)), None


def contract(root, base, mode, sources, files):
    digest = hashlib.sha256("\n".join(files).encode()).hexdigest()[:12]
    counts = " ".join("%s=%d" % (name, len(sources[name]))
                      for name in ("committed", "staged", "unstaged",
                                   "untracked"))
    return ("candidates base=%s mode=%s %s total=%d digest=%s"
            % (base, mode, counts, len(files), digest))


def counted(line, name):
    """Read one count field; a whole token, so staged never reads unstaged."""
    for field in line.split():
        key, _, value = field.partition("=")
        if key == name:
            return int(value)
    raise KeyError("the contract line omits %s: %r" % (name, line))


def _run(root, *args):
    subprocess.run(("git", "-C", str(root)) + args, check=True,
                   capture_output=True)


def _fixture(root):
    """A repository whose main holds one tracked file, on a branch."""
    _run(root, "init", "-q", "-b", "main")
    _run(root, "config", "user.email", "t@t")
    _run(root, "config", "user.name", "t")
    (root / "tracked.c").write_text("int tracked;\n")
    _run(root, "add", "tracked.c")
    _run(root, "commit", "-qm", "base")
    _run(root, "checkout", "-qb", "work")


def self_test():
    """Each source is judged in working-tree mode and named in the contract."""
    failures = []
    arms = (
        ("committed", lambda root: (
            (root / "tracked.c").write_text("int committed;\n"),
            _run(root, "commit", "-qam", "change"))),
        ("staged", lambda root: (
            (root / "tracked.c").write_text("int staged;\n"),
            _run(root, "add", "tracked.c"))),
        ("unstaged", lambda root: (root / "tracked.c").write_text("int un;\n")),
        ("untracked", lambda root: (root / "new.c").write_text("int new;\n")),
    )
    for name, mutate in arms:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _fixture(root)
            mutate(root)
            sources = collect(root, "main")
            if not sources[name]:
                failures.append("%s: the source reports no file" % name)
                continue
            files, refusal = select(sources, "working-tree")
            if refusal or not files:
                failures.append("%s: working-tree mode judges nothing" % name)
            line = contract(root, "main", "working-tree", sources, files)
            if counted(line, name) == 0:
                failures.append("%s: the contract line reports zero" % name)

    # committed mode refuses a populated working tree and judges a clean one
    for name in ("unstaged", "untracked"):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _fixture(root)
            if name == "unstaged":
                (root / "tracked.c").write_text("int dirty;\n")
            else:
                (root / "extra.c").write_text("int extra;\n")
            _, refusal = select(collect(root, "main"), "committed")
            if refusal is None:
                failures.append("committed mode accepted a %s tree" % name)
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _fixture(root)
        (root / "tracked.c").write_text("int committed;\n")
        _run(root, "commit", "-qam", "change")
        files, refusal = select(collect(root, "main"), "committed")
        if refusal or files != ["tracked.c"]:
            failures.append("committed mode failed on a clean tree: "
                            "%r %r" % (files, refusal))

    # an empty checkout reports zero through every source, not through one
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _fixture(root)
        sources = collect(root, "main")
        files, _ = select(sources, "working-tree")
        line = contract(root, "main", "working-tree", sources, files)
        for name in ("committed", "staged", "unstaged", "untracked",
                     "total"):
            if counted(line, name) != 0:
                failures.append("an unchanged checkout misreports %s" % name)

    for failure in failures:
        print("FAIL  comment-hygiene candidates: %s" % failure,
              file=sys.stderr)
    if failures:
        return 1
    print("comment-hygiene candidates self-test: every source is judged and "
          "counted")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--mode", default="working-tree",
                        choices=("working-tree", "committed"))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    root = Path(args.repo_root).resolve()
    sources = collect(root, args.base)
    files, refusal = select(sources, args.mode)
    print(contract(root, args.base, args.mode, sources, files))
    if refusal:
        print("FAIL  comment-hygiene candidates: %s" % refusal,
              file=sys.stderr)
        return 1
    for name in files:
        print(name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
