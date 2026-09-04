#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Carry the review-history checker's unreachable commits as a Git bundle.

review_thread_group_history.py replays each reviewed path's history from
the review commit to the disposition revision, and the review commits of
squash-merged pull requests are reachable from no branch: they survive on
the development checkout as loose history and are absent from a fresh
clone, so the check passes on one host and fails on another at the same
source revision.  This tool makes that denominator portable.  ``export``
enumerates every commit the corpus names, classifies each as reachable
from the main ref, unreachable-but-present, or absent, pins the
unreachable ones under ``refs/qualification/review-history/<sha>`` in a
scratch object store that borrows the checkout's objects, and writes a
bundle of exactly their closure beyond the main ref, packed with one
thread and no delta window so the bytes are deterministic; ``verify``
checks the bundle against its declared digest and its prerequisites;
``check`` imports the bundle into a temporary clone that has had every
remote removed and network transport disabled, then runs the history
checker there against the declared revision.  The working qualification
checkout is never written and never gains the historical refs.

Exit 0 when the requested operation holds, 1 when it fails, 2 on usage.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REF_PREFIX = "refs/qualification/review-history/"
OID = re.compile(r"\b[0-9a-f]{40}\b")
CHECKER = Path(__file__).resolve().with_name("review_thread_group_history.py")
NO_NETWORK = {
    "GIT_PROXY_COMMAND": "/bin/false",
    "GIT_SSH_COMMAND": "/bin/false",
    "GIT_CONFIG_COUNT": "2",
    "GIT_CONFIG_KEY_0": "url.nonexistent://.insteadOf",
    "GIT_CONFIG_VALUE_0": "https://",
    "GIT_CONFIG_KEY_1": "url.nonexistent://.insteadOf",
    "GIT_CONFIG_VALUE_1": "ssh://",
}


def git(repo: Path, *args: str, env: dict | None = None,
        check: bool = True) -> subprocess.CompletedProcess:
    completed = subprocess.run(["git", "-C", str(repo), *args],
                               capture_output=True, text=True, env=env)
    if check and completed.returncode != 0:
        print(f"git {' '.join(args)} failed in {repo}:\n{completed.stderr}",
              file=sys.stderr)
        sys.exit(2)
    return completed


def sha256_path(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def corpus_commits(corpus_dir: Path) -> set[str]:
    found: set[str] = set()
    for p in sorted(corpus_dir.rglob("*")):
        if p.is_file():
            found.update(OID.findall(p.read_text(encoding="utf-8",
                                                 errors="replace")))
    return found


def classify(repo: Path, oids: set[str], main_ref: str):
    reachable, unreachable, absent = [], [], []
    for oid in sorted(oids):
        if git(repo, "cat-file", "-e", f"{oid}^{{commit}}",
               check=False).returncode != 0:
            absent.append(oid)
        elif git(repo, "merge-base", "--is-ancestor", oid, main_ref,
                 check=False).returncode == 0:
            reachable.append(oid)
        else:
            unreachable.append(oid)
    return reachable, unreachable, absent


def export(args) -> int:
    repo = args.repo_root.resolve()
    oids = corpus_commits(args.corpus_dir)
    reachable, unreachable, absent = classify(repo, oids, args.main_ref)
    print(f"corpus names {len(oids)} object ids: {len(reachable)} reachable "
          f"from {args.main_ref}, {len(unreachable)} present but unreachable, "
          f"{len(absent)} absent")
    if not unreachable:
        print("no unreachable commit to carry; the denominator is empty",
              file=sys.stderr)
        return 1
    objects = git(repo, "rev-parse", "--git-path", "objects").stdout.strip()
    objects = (repo / objects).resolve()
    with tempfile.TemporaryDirectory() as scratch:
        store = Path(scratch) / "store.git"
        git(Path(scratch), "init", "-q", "--bare", str(store))
        (store / "objects" / "info").mkdir(parents=True, exist_ok=True)
        (store / "objects" / "info" / "alternates").write_text(
            f"{objects}\n", encoding="utf-8")
        for oid in unreachable:
            git(store, "update-ref", REF_PREFIX + oid, oid)
        main_oid = git(repo, "rev-parse", f"{args.main_ref}^{{commit}}") \
            .stdout.strip()
        git(store, "update-ref", "refs/qualification/main-prerequisite",
            main_oid)
        git(store, "-c", "pack.threads=1", "-c", "pack.window=0",
            "-c", "pack.depth=0", "bundle", "create", str(args.output.resolve()),
            f"^{main_oid}", *[REF_PREFIX + oid for oid in unreachable])
    digest = sha256_path(args.output)
    refs_text = "".join(f"{REF_PREFIX}{oid}\t{oid}\n" for oid in unreachable)
    manifest = (f"schema=review-history-portable-bundle-v1\n"
                f"main_ref={args.main_ref}\nmain_oid={main_oid}\n"
                f"unreachable_count={len(unreachable)}\n"
                f"absent_count={len(absent)}\n"
                f"bundle_sha256={digest}\n")
    args.output.with_suffix(".manifest.txt").write_text(manifest + refs_text,
                                                        encoding="utf-8")
    print(f"bundle {args.output}: sha256 {digest}, "
          f"{len(unreachable)} refs beyond {args.main_ref} ({main_oid[:12]})")
    for oid in absent:
        print(f"  absent (not a commit here): {oid}")
    return 0


def verify(args) -> int:
    digest = sha256_path(args.bundle)
    if digest != args.digest:
        print(f"bundle digest {digest} differs from declared {args.digest}")
        return 1
    result = git(args.repo_root, "bundle", "verify", str(args.bundle.resolve()),
                 check=False)
    if result.returncode != 0:
        print(f"bundle verification FAIL:\n{result.stderr}")
        return 1
    heads = [line for line in git(args.repo_root, "bundle", "list-heads",
                                  str(args.bundle.resolve())).stdout.splitlines()
             if REF_PREFIX in line]
    print(f"bundle verification PASS: digest declared and matched, "
          f"{len(heads)} review-history refs, prerequisites present")
    return 0


def check(args) -> int:
    if verify(args) != 0:
        return 1
    repo = args.repo_root.resolve()
    with tempfile.TemporaryDirectory() as scratch:
        clone = Path(scratch) / "qualification-clone"
        if args.fresh_clone:
            # A transport clone of one branch carries only the objects
            # reachable from it, which is the shape of a target checkout
            # that never saw the review commits; the checker must fail
            # there before the bundle and pass after it.
            git(Path(scratch), "clone", "-q", "--no-local", "--no-checkout",
                "--single-branch", "--branch", args.fresh_clone,
                str(repo), str(clone))
        else:
            git(Path(scratch), "clone", "-q", "--shared", "--no-checkout",
                str(repo), str(clone))
        for remote in git(clone, "remote").stdout.split():
            git(clone, "remote", "remove", remote)
        env = dict(os.environ)
        env.update(NO_NETWORK)
        head = git(clone, "rev-parse", f"{args.revision}^{{commit}}",
                   check=False)
        if head.returncode != 0:
            print(f"revision {args.revision} is absent from the clone")
            return 1
        git(clone, "update-ref", "HEAD", head.stdout.strip())
        cmd = [sys.executable, str(CHECKER), "check",
               "--corpus-dir", str(args.corpus_dir),
               "--repo-root", str(clone),
               "--revision", args.revision,
               "--output-dir", str(args.output_dir)]
        if args.fresh_clone:
            before = subprocess.run(cmd, capture_output=True, text=True,
                                    env=env)
            if before.returncode == 0:
                print("negative control: the checker passed on the fresh "
                      "clone before the bundle, so the bundle carries "
                      "nothing the checker needs")
                return 1
            print("negative control: the checker fails on the fresh clone "
                  "before the bundle, as a target checkout does")
        fetch = git(clone, "fetch", "-q", str(args.bundle.resolve()),
                    f"{REF_PREFIX}*:{REF_PREFIX}*", env=env)
        imported = git(clone, "for-each-ref", "--format=%(objectname)",
                       REF_PREFIX).stdout.split()
        print(f"imported {len(imported)} review-history refs into a "
              f"remote-less clone at {head.stdout.strip()[:12]}; network "
              f"transport disabled ({fetch.returncode == 0})")
        result = subprocess.run(cmd, capture_output=True, text=True, env=env)
        tail = (result.stdout + result.stderr).strip().splitlines()
        tail = tail[-1] if tail else ""
        if result.returncode != 0:
            print(f"history checker on the imported clone: FAIL ({tail})")
            return 1
        print(f"history checker on the imported clone: PASS ({tail})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    e = sub.add_parser("export")
    e.add_argument("--corpus-dir", type=Path, required=True)
    e.add_argument("--repo-root", type=Path, required=True)
    e.add_argument("--main-ref", default="origin/main")
    e.add_argument("--output", type=Path, required=True)
    for name in ("verify", "check"):
        s = sub.add_parser(name)
        s.add_argument("--bundle", type=Path, required=True)
        s.add_argument("--digest", required=True)
        s.add_argument("--repo-root", type=Path, required=True)
        if name == "check":
            s.add_argument("--corpus-dir", type=Path, required=True)
            s.add_argument("--revision", required=True)
            s.add_argument("--output-dir", type=Path, required=True)
            s.add_argument("--fresh-clone", metavar="BRANCH",
                           help="clone BRANCH alone over transport so the "
                                "clone lacks the review commits, run the "
                                "checker as a negative control, then import")
    args = parser.parse_args()
    return {"export": export, "verify": verify, "check": check}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
