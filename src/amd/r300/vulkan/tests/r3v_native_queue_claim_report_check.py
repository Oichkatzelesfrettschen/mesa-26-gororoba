#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the queue-claim report and hold it to the expected mode: the
compute bit follows the mode, the report is self-consistent, and the
verb table digest is a BLAKE3 hex string; the two Meson invocations run
under the closed and the exact-value gate."""

import subprocess
import sys

report, expected_mode = sys.argv[1], sys.argv[2]
p = subprocess.run([report], capture_output=True, text=True)
if p.returncode != 0:
    raise SystemExit(f"report failed ({p.returncode}): {p.stderr}")
fields = dict(line.split("\t", 1) for line in p.stdout.splitlines())
if fields["queue_claim_mode"] != expected_mode:
    raise SystemExit(f"mode {fields['queue_claim_mode']} != {expected_mode}")
if fields["compute_bit"] != ("0" if expected_mode == "default_graphics_only"
                             else "1"):
    raise SystemExit("compute bit does not follow the mode")
if fields["claim_consistent"] != "1" or fields["queue_family_count"] != "1":
    raise SystemExit("report inconsistent")
if fields["queue_claim_gate"] != ("1" if expected_mode ==
                                  "experimental_compute_subset" else "0"):
    raise SystemExit("gate state does not follow the mode")
digest = fields["verb_table_blake3"]
if len(digest) != 64 or set(digest) - set("0123456789abcdef"):
    raise SystemExit("verb table digest is not BLAKE3 hex")
print(f"queue-claim-report: {expected_mode}, verb table {digest[:12]}")
