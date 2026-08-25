#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compose one sealed plan per case from a shard's captured transcripts.

A capture shard run one process apiece leaves one transcript per case
that submitted, named by that case's directory.  This composes each
transcript into a plan through r3v_native_plan_tool, filling the shard's
run identities and giving every plan its own nonce and its own evidence
directory: plan replay binds a session to one nonce and creates
`session.state` under its evidence directory with O_EXCL, so two cases
sharing either one would refuse the second.  Every composed plan goes
back through `check`, and `plans.json` records case, transcript digest,
plan digest, entry count, and nonce.

A transcript the tool cannot parse refuses the shard, since a plan
composed from a damaged capture would replay a sequence the silicon
never produced.  A transcript carrying no entries is skipped and listed:
the driver writes no transcript for a device that never submitted, so a
present-but-empty one names a case whose capture produced nothing.

Usage:
  r3v_native_plan_compose_shard.py compose --transcript-dir DIR \
      --tool BIN --out-dir DIR --nonce-file TSV [--generate-nonces] \
      --source-sha HEX40 --dso-blake3 HEX64 --deqp-sha256 HEX64 \
      --deqp-release NAME --partition-sha256 HEX64 \
      --caselist-sha256 HEX64 --queue-claim MODE --kernel-release NAME \
      --module-srcversion NAME --evidence-dir DIR --source-clean 1 \
      --max-runtime-seconds N
  r3v_native_plan_compose_shard.py selftest --harness BIN --tool BIN
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TRANSCRIPT_SUFFIX = ".transcript"
NONCE_PATTERN = re.compile(r"[0-9a-f]{32}")
SUBMISSION_COUNT = re.compile(r"^plan: (\d+) submissions", re.M)
# The identities the plan tool takes verbatim; the nonce and the
# evidence directory are per case, so they stay off this list.
IDENTITY_OPTIONS = ("source-sha", "dso-blake3", "deqp-sha256",
                    "deqp-release", "partition-sha256", "caselist-sha256",
                    "queue-claim", "kernel-release", "module-srcversion",
                    "source-clean", "max-runtime-seconds")


class ComposeRefusal(Exception):
    pass


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def transcript_cases(transcript_dir):
    paths = sorted(Path(transcript_dir).glob("*" + TRANSCRIPT_SUFFIX))
    if not paths:
        raise ComposeRefusal(f"{transcript_dir} holds no *{TRANSCRIPT_SUFFIX}")
    return [(p.name[:-len(TRANSCRIPT_SUFFIX)], p) for p in paths]


def generate_nonces(cases, path):
    """One 32-hex nonce per case from the kernel's entropy pool: the
    nonce is what binds a plan to the single session allowed to replay
    it, so it comes from os.urandom rather than from the case name."""
    Path(path).write_text("".join(f"{c}\t{os.urandom(16).hex()}\n"
                                  for c in cases))


def load_nonces(path, cases):
    nonces = {}
    for n, line in enumerate(Path(path).read_text().splitlines(), start=1):
        if not line.strip():
            continue
        case, sep, nonce = line.partition("\t")
        nonce = nonce.strip()
        if not sep or not NONCE_PATTERN.fullmatch(nonce):
            raise ComposeRefusal(f"{path}:{n} is not a case name and 32 "
                                 "lowercase hex digits")
        if case in nonces:
            raise ComposeRefusal(f"{path}:{n} names {case} twice")
        nonces[case] = nonce
    if len(set(nonces.values())) != len(nonces):
        raise ComposeRefusal(f"{path} reuses a nonce; one nonce binds one "
                             "replay session")
    missing = [c for c in cases if c not in nonces]
    if missing:
        raise ComposeRefusal(f"{path} declares no nonce for {len(missing)} "
                             f"cases, {missing[0]} first")
    return nonces


def entry_count(tool, path):
    """The submissions a transcript carries, through the tool's own
    parser: a transcript the parser refuses refuses the shard."""
    r = subprocess.run([tool, "check", str(path)], capture_output=True,
                       text=True)
    if r.returncode != 0:
        raise ComposeRefusal(f"{path} does not parse as a plan: "
                             f"{r.stderr.strip() or r.stdout.strip()}")
    m = SUBMISSION_COUNT.search(r.stdout)
    if m is None:
        raise ComposeRefusal(f"{path}: the tool reported no submission "
                             f"count: {r.stdout.strip()}")
    return int(m.group(1))


def compose(args):
    cases = transcript_cases(args.transcript_dir)
    if args.generate_nonces:
        generate_nonces([c for c, _ in cases], args.nonce_file)
    nonces = load_nonces(args.nonce_file, [c for c, _ in cases])
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    identity = []
    for option in IDENTITY_OPTIONS:
        identity += [f"--{option}", getattr(args, option.replace("-", "_"))]
    plans = []
    skipped = []
    for case, transcript in cases:
        entries = entry_count(args.tool, transcript)
        if entries == 0:
            skipped.append(case)
            continue
        plan = out_dir / (case + ".plan")
        # Each plan names its own evidence directory: a replay session
        # creates session.state there with O_EXCL and binds to an empty
        # directory, so one directory admits one case.
        argv = [args.tool, "compose", "--transcript", str(transcript),
                "--out", str(plan), "--nonce", nonces[case],
                "--evidence-dir", f"{args.evidence_dir.rstrip('/')}/{case}"]
        r = subprocess.run(argv + identity, capture_output=True, text=True)
        if r.returncode != 0:
            raise ComposeRefusal(f"{case}: compose refused: "
                                 f"{r.stderr.strip() or r.stdout.strip()}")
        checked = entry_count(args.tool, plan)
        if checked != entries:
            raise ComposeRefusal(f"{case}: the plan carries {checked} "
                                 f"submissions, the transcript {entries}")
        plans.append({"case": case, "transcript": str(transcript),
                      "transcript_sha256": sha256_file(transcript),
                      "plan": str(plan), "plan_sha256": sha256_file(plan),
                      "entry_count": entries, "nonce": nonces[case]})
    manifest = {"plan_count": len(plans), "skipped_cases": skipped,
                "entry_total": sum(p["entry_count"] for p in plans),
                "plans": plans}
    (out_dir / "plans.json").write_text(json.dumps(manifest, indent=1,
                                                   sort_keys=True) + "\n")
    print(f"composed {len(plans)} plans over {manifest['entry_total']} "
          f"submissions; {len(skipped)} transcripts carried no entries")
    return manifest


SELFTEST_IDENTITY = {
    "source_sha": "0123456789abcdef0123456789abcdef01234567",
    "dso_blake3": "0123456789abcdef" * 4,
    "deqp_sha256": "0123456789abcdef" * 4,
    "deqp_release": "opengl-cts-4.6.8.0-414-g43c65c132",
    "partition_sha256": "0123456789abcdef" * 4,
    "caselist_sha256": "0123456789abcdef" * 4,
    "queue_claim": "default_graphics_only",
    "kernel_release": "7.1.8-1-cachyos",
    "module_srcversion": "088E045518D972727C1DD1C",
    "source_clean": "1",
    "max_runtime_seconds": "600",
}


def selftest(harness, tool):
    """Calibration against the capture harness's own transcript: three
    cases compose, check, and land in the manifest with distinct
    nonces, while a damaged transcript and a short nonce each refuse."""
    out = subprocess.run([harness, "capture", "--keep"], capture_output=True,
                         text=True, check=True).stdout
    transcript = out.strip().split("capture: ", 1)[1]
    try:
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            tdir = d / "transcripts"
            tdir.mkdir()
            names = ["dEQP-VK.api.smoke.triangle",
                     "dEQP-VK.api.command_buffers.record_simple",
                     "dEQP-VK.memory.pipeline_barrier.host_write_index"]
            for name in names:
                shutil.copyfile(transcript, tdir / (name + ".transcript"))
            nonce_file = d / "nonces.tsv"
            args = argparse.Namespace(
                transcript_dir=str(tdir), tool=tool, out_dir=str(d / "plans"),
                nonce_file=str(nonce_file), generate_nonces=True,
                evidence_dir="/var/tmp/plan-evidence", **SELFTEST_IDENTITY)
            manifest = compose(args)
            if manifest["plan_count"] != 3 or manifest["skipped_cases"]:
                raise SystemExit(f"selftest: {manifest['plan_count']} plans")
            if {p["entry_count"] for p in manifest["plans"]} != {2}:
                raise SystemExit("selftest: an entry count other than two")
            if len({p["nonce"] for p in manifest["plans"]}) != 3:
                raise SystemExit("selftest: the nonces are not distinct")
            for p in manifest["plans"]:
                text = Path(p["plan"]).read_text()
                if f"nonce\t{p['nonce']}\n" not in text or \
                        f"/plan-evidence/{p['case']}\n" not in text:
                    raise SystemExit(f"selftest: {p['case']} carries neither "
                                     "its nonce nor its evidence directory")
            # A damaged transcript refuses the shard.
            damaged = tdir / (names[0] + ".transcript")
            damaged.write_bytes(b"schema_version\t1\nsubmission_count\t9\n")
            args.out_dir = str(d / "plans-damaged")
            args.generate_nonces = False
            try:
                compose(args)
            except ComposeRefusal as e:
                if "does not parse as a plan" not in str(e):
                    raise SystemExit(f"selftest: wrong refusal: {e}")
            else:
                raise SystemExit("selftest: a damaged transcript composed")
            # A nonce of the wrong length refuses before any plan is
            # written.
            shutil.copyfile(transcript, damaged)
            nonce_file.write_text("".join(f"{n}\t{'ab' * 8}\n"
                                          for n in names))
            args.out_dir = str(d / "plans-short-nonce")
            try:
                compose(args)
            except ComposeRefusal as e:
                if "32 lowercase hex" not in str(e):
                    raise SystemExit(f"selftest: wrong refusal: {e}")
            else:
                raise SystemExit("selftest: a short nonce composed")
    finally:
        os.remove(transcript)
        os.rmdir(os.path.dirname(transcript))
    print("compose-shard: three per-case plans sealed with distinct nonces "
          "and their own evidence directories; a damaged transcript and a "
          "short nonce refused")


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compose")
    c.add_argument("--transcript-dir", required=True)
    c.add_argument("--tool", required=True)
    c.add_argument("--out-dir", required=True)
    c.add_argument("--nonce-file", required=True)
    c.add_argument("--generate-nonces", action="store_true")
    c.add_argument("--evidence-dir", required=True)
    for option in IDENTITY_OPTIONS:
        c.add_argument(f"--{option}", required=True)
    s = sub.add_parser("selftest")
    s.add_argument("--harness", required=True)
    s.add_argument("--tool", required=True)
    args = p.parse_args()
    try:
        if args.cmd == "selftest":
            selftest(args.harness, args.tool)
        else:
            compose(args)
    except ComposeRefusal as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
