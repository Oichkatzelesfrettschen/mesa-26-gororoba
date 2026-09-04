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

``--lint-with`` runs the linter over the judged set directly, passing each
path as one argument.  A path carrying whitespace survives that call, where
a shell word-split would turn it into arguments naming no file and let the
linter report clean over a candidate it never opened.

Usage:
  comment_hygiene_candidates.py --repo-root DIR [--base REF]
                                [--mode working-tree|committed]
                                [--lint-with LINTER]
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


class GitQueryFailed(Exception):
    """A git query the denominator depends on did not answer."""


def _git(root, *args):
    """Run one git query; a failure raises so the gate never judges a
    set it could not build.  An absent base ref, a corrupt index, or a
    missing repository all reach the caller as a refusal."""
    done = subprocess.run(("git", "-C", str(root)) + args,
                          capture_output=True, text=True)
    if done.returncode != 0:
        raise GitQueryFailed("git %s: %s" % (" ".join(args),
                                             done.stderr.strip()
                                             or "exit %d" % done.returncode))
    return [line for line in done.stdout.split("\n") if line]


def collect(root, base):
    """Return the four candidate sources, each a sorted path list."""
    merge_base = _git(root, "merge-base", base, "HEAD")
    if not merge_base:
        raise GitQueryFailed("merge-base %s HEAD named no commit" % base)
    committed = _git(root, "diff", "--name-only", FILTER, merge_base[0],
                     "HEAD")
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
        # index bytes are absent from HEAD, so a populated index unbinds the
        # verdict from the declared SHA exactly as an unstaged edit does
        outside = (sources["staged"] + sources["unstaged"]
                   + sources["untracked"])
        if outside:
            return [], ("%d file(s) sit outside HEAD: %s"
                        % (len(outside), ", ".join(sorted(set(outside))[:5])))
        judged = sources["committed"]
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


def collect_exits_zero_outside_a_repository(root):
    """True when collect() answers outside a repository instead of refusing."""
    try:
        collect(root, "main")
        return True
    except GitQueryFailed:
        return False


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

    # a git query that cannot answer refuses; it never reports an empty set
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _fixture(root)
        try:
            collect(root, "refs/heads/absent-base")
            failures.append("an absent base ref reported a set")
        except GitQueryFailed:
            pass
    with tempfile.TemporaryDirectory() as tmp:
        if collect_exits_zero_outside_a_repository(Path(tmp)):
            failures.append("a non-repository reported a set")

    # committed mode refuses a populated working tree and judges a clean one
    for name in ("staged", "unstaged", "untracked"):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _fixture(root)
            if name == "staged":
                (root / "tracked.c").write_text("int staged;\n")
                _run(root, "add", "tracked.c")
            elif name == "unstaged":
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

    # a path carrying whitespace stays one candidate through the lint call
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _fixture(root)
        spaced = "a file with spaces.c"
        (root / spaced).write_text("int spaced;\n")
        files, _ = select(collect(root, "main"), "working-tree")
        if spaced not in files:
            failures.append("a path with spaces never entered the judged set")
        probe = root / "argv_probe.py"
        probe.write_text("import sys\n"
                         "sys.exit(0 if len(sys.argv) == 3 else 1)\n")
        done = subprocess.run([sys.executable, str(probe), "--strict",
                               str(root / spaced)])
        if done.returncode != 0:
            failures.append("a path with spaces split into several arguments")

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
    parser.add_argument("--lint-with")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    root = Path(args.repo_root).resolve()
    try:
        sources = collect(root, args.base)
    except GitQueryFailed as failure:
        print("FAIL  comment-hygiene candidates: %s" % failure,
              file=sys.stderr)
        return 1
    files, refusal = select(sources, args.mode)
    print(contract(root, args.base, args.mode, sources, files))
    sys.stdout.flush()
    if refusal:
        print("FAIL  comment-hygiene candidates: %s" % refusal,
              file=sys.stderr)
        return 1
    if args.lint_with:
        if not files:
            print("OK    comment hygiene: the judged set is empty")
            return 0
        # relative names run from the repository root, so a report locates
        # each file the way every other repository gate spells it
        return subprocess.run([sys.executable, args.lint_with, "--strict"]
                              + files, cwd=str(root)).returncode
    for name in files:
        print(name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
