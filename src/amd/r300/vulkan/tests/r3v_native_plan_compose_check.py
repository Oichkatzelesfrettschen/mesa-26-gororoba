#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compose a plan from the capture harness's transcript and check it.

The harness prints the transcript path; compose fills the declared run
identities and seals; check parses the result.  Calibration: a missing
identity refuses, a malformed digest refuses through the parser, and the
composed plan carries the declared nonce and both entries."""

import os
import subprocess
import sys
import tempfile

harness, tool = sys.argv[1], sys.argv[2]
H64 = "0123456789abcdef" * 4
out = subprocess.run([harness, "capture", "--keep"], capture_output=True,
                     text=True, check=True).stdout
transcript = out.strip().split("capture: ", 1)[1]
ident = ["--source-sha", "0123456789abcdef0123456789abcdef01234567",
         "--dso-blake3", H64, "--deqp-sha256", H64,
         "--deqp-release", "opengl-cts-4.6.8.0-414-g43c65c132",
         "--partition-sha256", H64, "--caselist-sha256", H64,
         "--queue-claim", "default_graphics_only",
         "--kernel-release", "7.1.8-1-cachyos",
         "--module-srcversion", "088E045518D972727C1DD1C",
         "--nonce", "00112233445566778899aabbccddeeff",
         "--evidence-dir", "/var/tmp/plan-evidence",
         "--source-clean", "1", "--max-runtime-seconds", "600"]
try:
    with tempfile.TemporaryDirectory() as d:
        plan = os.path.join(d, "shard.plan")
        r = subprocess.run([tool, "compose", "--transcript", transcript,
                            "--out", plan] + ident, capture_output=True,
                           text=True)
        if r.returncode != 0 or "plan: 2 submissions" not in r.stdout:
            raise SystemExit(f"compose failed: {r.stdout}{r.stderr}")
        text = open(plan).read()
        if "nonce\t00112233445566778899aabbccddeeff\n" not in text or \
                text.count("\nsubmission\t") != 2 or \
                "source_clean\t1\n" not in text:
            raise SystemExit("composed plan lacks the declared identity")
        r = subprocess.run([tool, "check", plan], capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"check failed: {r.stderr}")
        # A missing identity refuses.
        r = subprocess.run([tool, "compose", "--transcript", transcript,
                            "--out", plan] + ident[2:], capture_output=True,
                           text=True)
        if r.returncode == 0 or "requires --source-sha" not in r.stderr:
            raise SystemExit("compose admitted a missing identity")
        # A malformed digest refuses in the writer before anything is sealed.
        bad = list(ident)
        bad[1] = "zz"
        r = subprocess.run([tool, "compose", "--transcript", transcript,
                            "--out", plan] + bad, capture_output=True, text=True)
        if r.returncode == 0 or "--source-sha: expected 40" not in r.stderr:
            raise SystemExit("compose admitted a malformed source SHA")
        # An over-length value refuses by name rather than truncating.
        long_ = list(ident)
        long_[1] = "0123456789abcdef0123456789abcdef012345678"
        r = subprocess.run([tool, "compose", "--transcript", transcript,
                            "--out", plan] + long_, capture_output=True,
                           text=True)
        if r.returncode == 0 or "does not fit" not in r.stderr:
            raise SystemExit("compose truncated an over-length source SHA")
        # A runtime outside 1..86400 refuses by option name.
        slow = list(ident)
        slow[-1] = "0"
        r = subprocess.run([tool, "compose", "--transcript", transcript,
                            "--out", plan] + slow, capture_output=True, text=True)
        if r.returncode == 0 or "--max-runtime-seconds" not in r.stderr:
            raise SystemExit("compose admitted a zero runtime")
        # The transcript itself parses as a plan; its placeholders keep it
        # from binding to any run.
        r = subprocess.run([tool, "check", transcript], capture_output=True,
                           text=True)
        if r.returncode != 0:
            raise SystemExit("transcript check failed")
finally:
    os.remove(transcript)
    os.rmdir(os.path.dirname(transcript))
print("compose: two-entry plan sealed with the declared identity; missing "
      "identity and malformed digest refused")
