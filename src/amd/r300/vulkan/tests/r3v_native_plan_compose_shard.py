#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compose one sealed plan per case from a shard's captured transcripts.

A capture shard run one process apiece leaves one transcript family per
case, named by that case's directory: the driver assigns each device in
a process its own ordinal, since a test creates devices freely (dEQP's
own robustness cases create a device ahead of the one they drive), so a
case's capture spans `<case>.transcript` (device ordinal 0) and, when
the case created more devices, `<case>.transcript.N` for the Nth extra
device.  This composes the one family member that carries entries into
a plan through r3v_native_plan_tool, filling the shard's run identities
and giving every plan its own nonce and its own evidence directory:
plan replay binds a session to one nonce and creates `session.state`
under its evidence directory with O_EXCL, so two cases sharing either
one would refuse the second.  Every composed plan goes back through
`check`, and `plans.json` records case, transcript digest, plan digest,
entry count, nonce, and the submitting device's ordinal
(`device_ordinal`, 0 for the base path).

A transcript the tool cannot parse, or a transcript filename whose
ordinal suffix does not parse as a strictly positive decimal integer,
refuses the shard, since a plan composed from a damaged capture would
replay a sequence the silicon never produced.  A case whose family
carries no entries anywhere is skipped and listed: the driver writes no
transcript for a device that never submitted, so an all-empty family
names a case whose capture produced nothing.  A case whose family
carries entries in more than one member is refused as
`multiple_submitting_devices` and listed in `plans.json`, without
refusing the rest of the shard: composing either member would silently
drop the other device's submissions from the case's plan.

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
# The case name a transcript carries becomes a path component of that
# plan's evidence directory, so it passes the runner's own allowlist:
# a dEQP-VK case name whose every character is already filesystem-safe,
# which sanitizing to itself decides.
CASE_PREFIX = "dEQP-VK."
CASE_NAME_UNSAFE = re.compile(r"[^A-Za-z0-9_.-]")
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


ORDINAL_SUFFIX = re.compile(r"^[1-9]\d*$")


def transcript_members(transcript_dir):
    """Every case's transcript family, grouped by case and sorted by
    device ordinal.  The base `<case>.transcript` path names ordinal 0;
    `<case>.transcript.N` names the driver's own ordinal for the Nth
    extra device in the process.  A case name reaches the plan as a
    component of that case's evidence directory, so it carries the
    dEQP-VK prefix and survives sanitizing unchanged; `..`, a separator,
    and any other traversal character refuse here rather than escaping
    the evidence root.  A name whose final component is not all decimal
    digits names no device at all -- `r3v_native_plan_capture_write`'s
    own `<path>.tmp` staging name is exactly this shape -- and is
    skipped rather than treated as a transcript.  A decimal suffix that
    is not a strictly positive integer -- a leading zero, or the base
    path's own implicit 0 spelled out -- refuses, since it is numeric
    but names no ordinal the driver could have assigned."""
    paths = sorted(Path(transcript_dir).glob("*" + TRANSCRIPT_SUFFIX)) + \
        sorted(Path(transcript_dir).glob("*" + TRANSCRIPT_SUFFIX + ".*"))
    if not paths:
        raise ComposeRefusal(f"{transcript_dir} holds no *{TRANSCRIPT_SUFFIX}")
    members = {}
    for p in paths:
        name = p.name
        if name.endswith(TRANSCRIPT_SUFFIX):
            case, ordinal = name[:-len(TRANSCRIPT_SUFFIX)], 0
        else:
            base, _, suffix = name.rpartition(".")
            if not base.endswith(TRANSCRIPT_SUFFIX) or not suffix.isdigit():
                # Not an ordinal-suffixed transcript at all: a staging
                # name like the capture write's own <path>.tmp, or an
                # unrelated file the directory happens to hold.
                continue
            if not ORDINAL_SUFFIX.fullmatch(suffix):
                raise ComposeRefusal(f"{name} names an ordinal suffix that "
                                     "does not parse as a strictly positive "
                                     "decimal integer")
            case, ordinal = base[:-len(TRANSCRIPT_SUFFIX)], int(suffix)
        if not case.startswith(CASE_PREFIX):
            raise ComposeRefusal(f"{name} names {case!r}, which carries no "
                                 f"{CASE_PREFIX} prefix")
        if CASE_NAME_UNSAFE.sub("_", case) != case:
            raise ComposeRefusal(f"{name} names {case!r}, which sanitizes "
                                 "to a different name; a case name reaches "
                                 "the evidence directory as a path component")
        members.setdefault(case, []).append((ordinal, p))
    for case in members:
        members[case].sort()
    return members


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
    members = transcript_members(args.transcript_dir)
    cases = sorted(members)
    if args.generate_nonces:
        generate_nonces(cases, args.nonce_file)
    nonces = load_nonces(args.nonce_file, cases)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    identity = []
    for option in IDENTITY_OPTIONS:
        identity += [f"--{option}", getattr(args, option.replace("-", "_"))]
    plans = []
    skipped = []
    refused = []
    for case in cases:
        # Every family member that carries entries: the driver writes
        # no transcript for a device that never submitted, so a member
        # with entries names the device that drove this case.
        submitting = [(ordinal, path, entry_count(args.tool, path))
                      for ordinal, path in members[case]]
        submitting = [(o, p, n) for o, p, n in submitting if n > 0]
        if not submitting:
            skipped.append(case)
            continue
        if len(submitting) > 1:
            # Composing either member would silently drop the other
            # device's submissions from the case's plan, so the case
            # refuses rather than picking one arbitrarily.
            refused.append({
                "case": case, "reason": "multiple_submitting_devices",
                "members": [str(p) for _, p, _ in submitting],
            })
            continue
        ordinal, transcript, entries = submitting[0]
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
                      "entry_count": entries, "nonce": nonces[case],
                      "device_ordinal": ordinal})
    manifest = {"plan_count": len(plans), "skipped_cases": skipped,
                "refused_cases": refused,
                "entry_total": sum(p["entry_count"] for p in plans),
                "plans": plans}
    (out_dir / "plans.json").write_text(json.dumps(manifest, indent=1,
                                                   sort_keys=True) + "\n")
    print(f"composed {len(plans)} plans over {manifest['entry_total']} "
          f"submissions; {len(skipped)} transcripts carried no entries; "
          f"{len(refused)} cases refused (multiple submitting devices)")
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
            # A case whose only transcript-family member is a device's
            # extra ordinal composes with that ordinal recorded; the
            # base path never existed, matching a device that claimed
            # ordinal 0 but never submitted.
            odir = d / "transcripts-ordinal-one"
            odir.mkdir()
            ordinal_case = "dEQP-VK.robustness.buffer_access.two_devices"
            shutil.copyfile(transcript,
                            odir / (ordinal_case + ".transcript.1"))
            oargs = argparse.Namespace(
                transcript_dir=str(odir), tool=tool,
                out_dir=str(d / "plans-ordinal-one"),
                nonce_file=str(d / "nonces-ordinal-one.tsv"),
                generate_nonces=True, evidence_dir="/var/tmp/plan-evidence",
                **SELFTEST_IDENTITY)
            omanifest = compose(oargs)
            if omanifest["plan_count"] != 1 or omanifest["skipped_cases"] or \
                    omanifest["refused_cases"]:
                raise SystemExit(f"selftest: ordinal-one case: {omanifest}")
            if omanifest["plans"][0]["device_ordinal"] != 1:
                raise SystemExit("selftest: ordinal-one case did not carry "
                                 "device_ordinal 1")

            # A case whose family carries entries in two members refuses
            # as multiple_submitting_devices rather than composing one
            # member and silently dropping the other device's entries.
            tdir_two = d / "transcripts-two-devices"
            tdir_two.mkdir()
            two_case = "dEQP-VK.robustness.buffer_access.both_submit"
            shutil.copyfile(transcript, tdir_two / (two_case + ".transcript"))
            shutil.copyfile(transcript,
                            tdir_two / (two_case + ".transcript.1"))
            two_args = argparse.Namespace(
                transcript_dir=str(tdir_two), tool=tool,
                out_dir=str(d / "plans-two-devices"),
                nonce_file=str(d / "nonces-two-devices.tsv"),
                generate_nonces=True, evidence_dir="/var/tmp/plan-evidence",
                **SELFTEST_IDENTITY)
            two_manifest = compose(two_args)
            if two_manifest["plan_count"] != 0 or \
                    len(two_manifest["refused_cases"]) != 1 or \
                    two_manifest["refused_cases"][0]["reason"] != \
                        "multiple_submitting_devices" or \
                    two_manifest["refused_cases"][0]["case"] != two_case:
                raise SystemExit(f"selftest: two-devices case: "
                                 f"{two_manifest}")

            # A malformed ordinal suffix refuses the shard rather than
            # composing a device ordinal the driver never assigned.
            tdir_bad = d / "transcripts-bad-ordinal"
            tdir_bad.mkdir()
            bad_case = "dEQP-VK.robustness.buffer_access.bad_ordinal"
            shutil.copyfile(transcript,
                            tdir_bad / (bad_case + ".transcript.01"))
            bad_args = argparse.Namespace(
                transcript_dir=str(tdir_bad), tool=tool,
                out_dir=str(d / "plans-bad-ordinal"),
                nonce_file=str(d / "nonces-bad-ordinal.tsv"),
                generate_nonces=True, evidence_dir="/var/tmp/plan-evidence",
                **SELFTEST_IDENTITY)
            try:
                compose(bad_args)
            except ComposeRefusal as e:
                if "strictly positive decimal integer" not in str(e):
                    raise SystemExit(f"selftest: bad ordinal: wrong "
                                     f"refusal: {e}")
            else:
                raise SystemExit("selftest: a malformed ordinal composed")

            # A leftover <path>.tmp staging file beside a real transcript
            # names no device and is ignored, not treated as a malformed
            # ordinal: r3v_native_plan_capture_write stages under this
            # exact name before its rename, so a case interrupted between
            # the two leaves one behind.
            tdir_tmp = d / "transcripts-tmp-residue"
            tdir_tmp.mkdir()
            tmp_case = "dEQP-VK.robustness.buffer_access.tmp_residue"
            shutil.copyfile(transcript, tdir_tmp / (tmp_case + ".transcript"))
            (tdir_tmp / (tmp_case + ".transcript.tmp")).write_bytes(b"")
            tmp_args = argparse.Namespace(
                transcript_dir=str(tdir_tmp), tool=tool,
                out_dir=str(d / "plans-tmp-residue"),
                nonce_file=str(d / "nonces-tmp-residue.tsv"),
                generate_nonces=True, evidence_dir="/var/tmp/plan-evidence",
                **SELFTEST_IDENTITY)
            tmp_manifest = compose(tmp_args)
            if tmp_manifest["plan_count"] != 1 or \
                    tmp_manifest["skipped_cases"] or \
                    tmp_manifest["refused_cases"] or \
                    tmp_manifest["plans"][0]["device_ordinal"] != 0:
                raise SystemExit(f"selftest: tmp-residue case: "
                                 f"{tmp_manifest}")

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
            # A transcript whose derived case name would escape the
            # evidence root refuses before any plan is composed: the
            # name reaches the plan as a path component.
            for name, message in (
                    (".._.._escape", "carries no dEQP-VK. prefix"),
                    ("notadeqpcase.x", "carries no dEQP-VK. prefix"),
                    ("dEQP-VK.a b", "sanitizes to a different name"),
                    ("dEQP-VK.a:b", "sanitizes to a different name")):
                evil = tdir / (name + ".transcript")
                shutil.copyfile(transcript, evil)
                args.out_dir = str(d / ("plans-" + name.replace(" ", "_")
                                        .replace(":", "_")))
                try:
                    compose(args)
                except ComposeRefusal as e:
                    if message not in str(e):
                        raise SystemExit(f"selftest: {name}: wrong "
                                         f"refusal: {e}")
                else:
                    raise SystemExit(f"selftest: {name} composed")
                evil.unlink()
            args.generate_nonces = False
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
          "and their own evidence directories; an ordinal-1-only family "
          "composed with its device_ordinal recorded, a two-submitting-"
          "member family refused as multiple_submitting_devices, a stale "
          "<path>.tmp staging file ignored beside a real transcript, a "
          "malformed ordinal suffix, a damaged transcript, four case "
          "names that would escape the evidence root, and a short nonce "
          "refused")


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
