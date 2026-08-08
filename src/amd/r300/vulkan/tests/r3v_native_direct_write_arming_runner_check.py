# SPDX-License-Identifier: MIT
#
# Drives the non-submitting direct-write arming runner over a fresh
# evidence directory: requires the refusal a run with no declarations
# must produce, the cell-identity lines an operator reads to build an
# authorization, and the wrong-cell refusal when the triangle runner's
# digest is declared against the control.

import os
import re
import subprocess
import sys
import tempfile


def run(runner, evidence_dir, environment):
    return subprocess.run([runner, evidence_dir], env=environment,
                          capture_output=True, text=True)


def main():
    if len(sys.argv) != 3:
        print("usage: r3v_native_direct_write_arming_runner_check.py "
              "<direct-write-runner> <triangle-runner>", file=sys.stderr)
        return 2
    runner, triangle_runner = sys.argv[1], sys.argv[2]

    environment = dict(os.environ)
    for declaration in (
        "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED",
        "R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
        "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE",
        "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
    ):
        environment.pop(declaration, None)

    with tempfile.TemporaryDirectory() as evidence_dir:
        undeclared = run(runner, evidence_dir, environment)
        if undeclared.returncode == 0:
            print("FAIL: undeclared run reported an armed verdict",
                  file=sys.stderr)
            print(undeclared.stdout, file=sys.stderr)
            return 1
        if "hazard gate closed" not in undeclared.stdout:
            print("FAIL: undeclared run did not name the closed gate",
                  file=sys.stderr)
            print(undeclared.stdout, file=sys.stderr)
            return 1

        # The report names the cell and carries the digest an
        # authorization declares.
        if "cell_kind=direct-write" not in undeclared.stdout:
            print("FAIL: report does not name the direct-write cell",
                  file=sys.stderr)
            print(undeclared.stdout, file=sys.stderr)
            return 1
        if re.search(r"^ib_dwords=[1-9][0-9]*$", undeclared.stdout,
                     re.MULTILINE) is None:
            print("FAIL: report carries no dword count", file=sys.stderr)
            return 1
        digest = re.search(r"^ib_blake3=([0-9a-f]{64})$", undeclared.stdout,
                           re.MULTILINE)
        if digest is None:
            print("FAIL: report carries no cell digest", file=sys.stderr)
            return 1

        # The triangle runner's digest names a different stream; declared
        # against the control, the wrong-cell authorization refuses as a
        # digest mismatch.
        triangle = run(triangle_runner, evidence_dir, environment)
        triangle_digest = re.search(r"blake3 ([0-9a-f]{64})",
                                    triangle.stdout)
        if triangle_digest is None:
            print("FAIL: triangle runner report carries no digest",
                  file=sys.stderr)
            print(triangle.stdout, file=sys.stderr)
            return 1
        if triangle_digest.group(1) == digest.group(1):
            print("FAIL: the two cells report one digest; each cell "
                  "declares its own stream", file=sys.stderr)
            return 1
        environment["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = \
            triangle_digest.group(1)
        wrong_cell = run(runner, evidence_dir, environment)
        if wrong_cell.returncode == 0 or "MISMATCH" not in wrong_cell.stdout:
            print("FAIL: triangle-digest authorization did not refuse "
                  "on the digest factor", file=sys.stderr)
            print(wrong_cell.stdout, file=sys.stderr)
            return 1

        # A stale digest -- one hex character off the live value --
        # refuses the same way.
        stale = "1" + digest.group(1)[1:] if digest.group(1)[0] != "1" \
            else "0" + digest.group(1)[1:]
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = stale
        stale_run = run(runner, evidence_dir, environment)
        if stale_run.returncode == 0 or "MISMATCH" not in stale_run.stdout:
            print("FAIL: stale digest did not refuse", file=sys.stderr)
            print(stale_run.stdout, file=sys.stderr)
            return 1

        # A wrong chip refuses even with the bundle declared correctly.
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = digest.group(1)
        environment["R3V_NATIVE_RUNNER_PCI_DEVICE"] = "0x5975"
        wrong_chip = run(runner, evidence_dir, environment)
        if wrong_chip.returncode == 0 or \
                "not the authorized RS482 identity" not in wrong_chip.stdout:
            print("FAIL: wrong chip did not refuse", file=sys.stderr)
            print(wrong_chip.stdout, file=sys.stderr)
            return 1

        # No run may claim a submission happened.
        for result in (undeclared, wrong_cell, stale_run, wrong_chip):
            if "no submission attempted" not in result.stdout:
                print("FAIL: report omits the no-submission statement",
                      file=sys.stderr)
                return 1

    print("r3v_native_direct_write_arming_runner_check: refusals hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
