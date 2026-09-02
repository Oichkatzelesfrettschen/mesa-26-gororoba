#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage a planning pass's captures under the composer's spelling.

A planning pass names its captures through the runner's
`R3V_NATIVE_PLAN_CAPTURE_FILE` template, so a shard directory holds
`<case>.plan` for a case whose driver built an IB, `<case>.plan.N` for
the Nth extra device that case's process created, and
`<case>.plan.no_nonempty_ib` for every case whose family carries no
entries.  The shard composer reads `<case>.transcript` and
`<case>.transcript.N`.  Staging copies each capture to that spelling
and records both digests, so the transformation between the pass's
output and the composer's input is itself evidence: a copy that changed
a byte, swept a marker in beside a real capture, or landed two shards'
copies of one case name refuses here instead of composing a plan the
silicon never produced.

The ordinal suffix separates a marker from a device: the driver assigns
a strictly positive decimal ordinal, so `.no_nonempty_ib` names no
device and stages nothing, while a suffix that is numeric yet names no
ordinal the driver could assign -- a leading zero, or the base path's
implicit 0 spelled out -- refuses the run.

`--expect-captures N` states how many captures the caller expects to
stage.  A glob that swept more than the pass produced, or fewer,
refuses before any copy: fail-closed on an empty match alone leaves a
too-wide match reporting success.

Usage:
  r3v_native_plan_stage_captures.py stage --capture-root DIR \
      --out-dir DIR --manifest FILE [--expect-captures N]
  r3v_native_plan_stage_captures.py selftest
"""

import argparse
import hashlib
import os
import re
import sys
import tempfile
import time
from pathlib import Path

CAPTURE_SUFFIX = ".plan"
TRANSCRIPT_SUFFIX = ".transcript"
MARKER_SUFFIX = ".no_nonempty_ib"
ORDINAL_SUFFIX = re.compile(r"^[1-9]\d*$")
CASE_PREFIX = "dEQP-VK."
CASE_NAME_UNSAFE = re.compile(r"[^A-Za-z0-9_.-]")
DSO_BLAKE3 = re.compile(r"^dso_blake3\t([0-9a-f]{64})$", re.M)

MANIFEST_FIELDS = ("case", "device_ordinal", "raw_path", "raw_size",
                   "raw_sha256", "staged_path", "staged_size",
                   "staged_sha256", "dso_blake3", "staged_at",
                   "staging_tool_sha256")


class StageRefusal(Exception):
    pass


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def capture_members(capture_root):
    """Every capture under the root, keyed by (case, ordinal).

    The root holds one directory per shard, and one case belongs to one
    shard, so a case name reaching this twice names two shards that
    covered it and refuses: the partition assigns each case once.
    """
    root = Path(capture_root)
    if not root.is_dir():
        raise StageRefusal(f"{capture_root} is not a directory")
    members = {}
    for path in sorted(root.rglob("*" + CAPTURE_SUFFIX + "*")):
        if not path.is_file():
            continue
        name = path.name
        if name.endswith(MARKER_SUFFIX):
            # A case whose family carries no entries names no
            # transcript and stages nothing.  The ordinal filter below
            # reaches the same verdict for as long as the marker's
            # final component parses as no ordinal, which the selftest
            # pins; this branch names the marker, so a marker spelled
            # with a decimal tail would still stage nothing.
            continue
        if name.endswith(CAPTURE_SUFFIX):
            case, ordinal = name[:-len(CAPTURE_SUFFIX)], 0
        else:
            base, _, suffix = name.rpartition(".")
            if not base.endswith(CAPTURE_SUFFIX) or not suffix.isdigit():
                continue
            if not ORDINAL_SUFFIX.fullmatch(suffix):
                raise StageRefusal(f"{name} names an ordinal suffix that "
                                   "does not parse as a strictly positive "
                                   "decimal integer")
            case, ordinal = base[:-len(CAPTURE_SUFFIX)], int(suffix)
        if not case.startswith(CASE_PREFIX):
            raise StageRefusal(f"{name} names {case!r}, which carries no "
                               f"{CASE_PREFIX} prefix")
        if CASE_NAME_UNSAFE.sub("_", case) != case:
            raise StageRefusal(f"{name} names {case!r}, which sanitizes to a "
                               "different name; a case name reaches the "
                               "evidence directory as a path component")
        if (case, ordinal) in members:
            raise StageRefusal(f"{case} ordinal {ordinal} is captured twice, "
                               f"in {members[(case, ordinal)]} and {path}; "
                               "the partition assigns each case one shard")
        members[(case, ordinal)] = path
    return members


def stage(args):
    tool_sha256 = sha256_file(__file__)
    members = capture_members(args.capture_root)
    if args.expect_captures is not None and len(members) != \
            args.expect_captures:
        raise StageRefusal(f"{args.capture_root} holds {len(members)} "
                           f"captures, not the {args.expect_captures} "
                           "expected")
    if not members:
        raise StageRefusal(f"{args.capture_root} holds no *{CAPTURE_SUFFIX} "
                           "capture")
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    for (case, ordinal), raw in sorted(members.items()):
        name = case + TRANSCRIPT_SUFFIX
        if ordinal:
            name += f".{ordinal}"
        staged = out_dir / name
        payload = raw.read_bytes()
        # O_EXCL decides the collision: two runs sharing an output
        # directory refuse rather than the second overwriting the
        # first's evidence, and the refusal comes from the create
        # itself, so no window separates the test from the write.
        try:
            fd = os.open(staged, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        except FileExistsError:
            raise StageRefusal(f"{staged} already exists; staging never "
                               "replaces a transcript in place")
        with os.fdopen(fd, "wb") as f:
            f.write(payload)
        raw_sha, staged_sha = sha256_file(raw), sha256_file(staged)
        if raw_sha != staged_sha or raw.stat().st_size != staged.stat().st_size:
            raise StageRefusal(f"{staged} does not reproduce {raw} byte for "
                               "byte")
        m = DSO_BLAKE3.search(payload.decode("utf-8", "replace"))
        if m is None:
            raise StageRefusal(f"{raw} declares no dso_blake3; a capture "
                               "names the driver that wrote it")
        rows.append({
            "case": case,
            "device_ordinal": ordinal,
            "raw_path": os.path.relpath(raw, args.capture_root),
            "raw_size": raw.stat().st_size,
            "raw_sha256": raw_sha,
            "staged_path": name,
            "staged_size": staged.stat().st_size,
            "staged_sha256": staged_sha,
            "dso_blake3": m.group(1),
            "staged_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "staging_tool_sha256": tool_sha256,
        })
    with open(args.manifest, "w") as f:
        f.write("\t".join(MANIFEST_FIELDS) + "\n")
        for row in rows:
            f.write("\t".join(str(row[k]) for k in MANIFEST_FIELDS) + "\n")
    print(f"staged {len(rows)} captures from {args.capture_root} to "
          f"{args.out_dir}; manifest {args.manifest}")


CAPTURE_TEXT = ("r3v-native-conformance-plan\t1\n"
                "dso_blake3\t" + "d7" * 32 + "\n"
                "submission_count\t0\n")


def write_capture(directory, name, text=CAPTURE_TEXT):
    p = Path(directory) / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)
    return p


def run_stage(root, out, manifest, expect=None):
    args = argparse.Namespace(capture_root=str(root), out_dir=str(out),
                              manifest=str(manifest), expect_captures=expect)
    stage(args)


def refuses(fragment, fn):
    try:
        fn()
    except StageRefusal as e:
        assert fragment in str(e), \
            f"refusal {str(e)!r} does not name {fragment!r}"
        return
    raise AssertionError(f"expected a refusal naming {fragment!r}")


def selftest():
    # The marker names no device: its final component parses as no
    # ordinal, so the marker branch and the ordinal filter agree on
    # every marker the runner writes.
    assert not ORDINAL_SUFFIX.fullmatch(MARKER_SUFFIX.rpartition(".")[2])
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        # Accept: one real capture, one extra-device member, and a
        # marker beside them; the marker stages nothing.
        root = tmp / "captures"
        write_capture(root / "s0", "dEQP-VK.a.b" + CAPTURE_SUFFIX)
        write_capture(root / "s0", "dEQP-VK.a.b" + CAPTURE_SUFFIX + ".2")
        write_capture(root / "s1",
                      "dEQP-VK.c.d" + CAPTURE_SUFFIX + MARKER_SUFFIX)
        run_stage(root, tmp / "staged", tmp / "m.tsv", expect=2)
        staged = sorted(p.name for p in (tmp / "staged").iterdir())
        assert staged == ["dEQP-VK.a.b.transcript",
                          "dEQP-VK.a.b.transcript.2"], staged
        rows = (tmp / "m.tsv").read_text().splitlines()
        assert len(rows) == 3, rows
        assert rows[0].split("\t") == list(MANIFEST_FIELDS)
        assert all("d7" * 32 in r for r in rows[1:]), rows
        # A marker alone stages nothing, so an all-refused shard
        # refuses rather than composing an empty directory.
        empty = tmp / "empty"
        write_capture(empty / "s0",
                      "dEQP-VK.e.f" + CAPTURE_SUFFIX + MARKER_SUFFIX)
        refuses("holds no *.plan capture",
                lambda: run_stage(empty, tmp / "o1", tmp / "m1.tsv"))
        # The count the caller expects is the count staged.
        refuses("holds 2 captures, not the 1 expected",
                lambda: run_stage(root, tmp / "o2", tmp / "m2.tsv", expect=1))
        # A staged path that already exists is never replaced.
        refuses("already exists",
                lambda: run_stage(root, tmp / "staged", tmp / "m3.tsv"))
        # One case belongs to one shard.
        twice = tmp / "twice"
        write_capture(twice / "s0", "dEQP-VK.a.b" + CAPTURE_SUFFIX)
        write_capture(twice / "s1", "dEQP-VK.a.b" + CAPTURE_SUFFIX)
        refuses("is captured twice",
                lambda: run_stage(twice, tmp / "o4", tmp / "m4.tsv"))
        # A numeric suffix that names no ordinal the driver assigns.
        zero = tmp / "zero"
        write_capture(zero / "s0", "dEQP-VK.a.b" + CAPTURE_SUFFIX + ".0")
        refuses("strictly positive decimal integer",
                lambda: run_stage(zero, tmp / "o5", tmp / "m5.tsv"))
        # A case name that reaches the evidence directory as a path
        # component survives sanitizing unchanged.
        unsafe = tmp / "unsafe"
        write_capture(unsafe / "s0", "dEQP-VK.a b" + CAPTURE_SUFFIX)
        refuses("sanitizes to a different name",
                lambda: run_stage(unsafe, tmp / "o6", tmp / "m6.tsv"))
        # A name outside the dEQP-VK namespace names no case.
        foreign = tmp / "foreign"
        write_capture(foreign / "s0", "notacase" + CAPTURE_SUFFIX)
        refuses("carries no dEQP-VK. prefix",
                lambda: run_stage(foreign, tmp / "o7", tmp / "m7.tsv"))
        # A capture that names no driver.
        nodso = tmp / "nodso"
        write_capture(nodso / "s0", "dEQP-VK.a.b" + CAPTURE_SUFFIX,
                      "r3v-native-conformance-plan\t1\n")
        refuses("declares no dso_blake3",
                lambda: run_stage(nodso, tmp / "o8", tmp / "m8.tsv"))
        refuses("is not a directory",
                lambda: run_stage(tmp / "absent", tmp / "o9", tmp / "m9.tsv"))
    print("stage-captures selftest: pass")


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stage")
    s.add_argument("--capture-root", required=True)
    s.add_argument("--out-dir", required=True)
    s.add_argument("--manifest", required=True)
    s.add_argument("--expect-captures", type=int)
    sub.add_parser("selftest")
    args = p.parse_args()
    try:
        if args.cmd == "selftest":
            selftest()
        else:
            stage(args)
    except StageRefusal as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
