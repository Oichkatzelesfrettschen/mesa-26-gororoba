# SPDX-License-Identifier: MIT
#
# Drives the non-submitting arming runner over a fresh evidence directory
# and requires the refusal a run with no declarations must produce, plus
# the report lines an operator reads to build an authorization.

import os
import re
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 2:
        print("usage: r3v_native_arming_runner_check.py <runner>",
              file=sys.stderr)
        return 2
    runner = sys.argv[1]

    environment = dict(os.environ)
    for declaration in (
        "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED",
        "R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
        "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE",
        "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
    ):
        environment.pop(declaration, None)

    with tempfile.TemporaryDirectory() as evidence_dir:
        undeclared = subprocess.run([runner, evidence_dir], env=environment,
                                    capture_output=True, text=True)
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

        # The report carries the cell digest an authorization declares.
        digest = re.search(r"blake3 ([0-9a-f]{64})", undeclared.stdout)
        if digest is None:
            print("FAIL: report carries no cell digest", file=sys.stderr)
            return 1

        # With the gate open but the bundle undeclared, the verdict moves
        # to the next factor rather than arming.
        environment["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
        gated = subprocess.run([runner, evidence_dir], env=environment,
                               capture_output=True, text=True)
        if gated.returncode == 0 or "bundle digest undeclared" not in \
                gated.stdout:
            print("FAIL: open gate without a declared bundle did not "
                  "refuse on the bundle factor", file=sys.stderr)
            print(gated.stdout, file=sys.stderr)
            return 1

        # The retained bare-cell digest -- the 76-dword IB the first
        # attended run submitted, before the first-draw contract prefix
        # -- is the stale-authorization negative control: the runner's
        # own digest names the contract-prefixed successor, and an
        # authorization still declaring the bare cell refuses as a
        # digest mismatch instead of arming.
        bare_cell_digest = ("855a8c2f5926dbb685ff0710caa2101e"
                            "5be39269f6e1d075379fa0a02bf80ebf")
        if digest.group(1) == bare_cell_digest:
            print("FAIL: runner digest names the bare cell, not the "
                  "contract-prefixed successor", file=sys.stderr)
            return 1
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = bare_cell_digest
        stale = subprocess.run([runner, evidence_dir], env=environment,
                               capture_output=True, text=True)
        if stale.returncode == 0 or "MISMATCH" not in stale.stdout:
            print("FAIL: stale bare-cell authorization did not refuse "
                  "on the digest factor", file=sys.stderr)
            print(stale.stdout, file=sys.stderr)
            return 1

        # A wrong chip refuses even with the bundle declared correctly.
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = digest.group(1)
        environment["R3V_NATIVE_RUNNER_PCI_DEVICE"] = "0x5975"
        wrong_chip = subprocess.run([runner, evidence_dir], env=environment,
                                    capture_output=True, text=True)
        if wrong_chip.returncode == 0 or \
                "not the authorized RS482 identity" not in wrong_chip.stdout:
            print("FAIL: wrong chip did not refuse", file=sys.stderr)
            print(wrong_chip.stdout, file=sys.stderr)
            return 1

        # No run may claim a submission happened.
        for result in (undeclared, gated, stale, wrong_chip):
            if "no submission attempted" not in result.stdout:
                print("FAIL: report omits the no-submission statement",
                      file=sys.stderr)
                return 1

    print("r3v_native_arming_runner_check: refusals hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
