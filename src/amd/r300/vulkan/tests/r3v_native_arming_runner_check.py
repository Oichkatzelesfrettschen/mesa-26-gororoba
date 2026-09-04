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
        nonmax_ib = os.path.join(evidence_dir, "nonmax-ib.bin")
        nonmax_emit = subprocess.run(
            [runner, "--extent", "48", "20", "--emit-ib", nonmax_ib],
            env=environment, capture_output=True, text=True)
        if nonmax_emit.returncode != 0 or not os.path.isfile(nonmax_ib):
            print("FAIL: non-maximum --emit-ib calibration failed",
                  file=sys.stderr)
            print(nonmax_emit.stdout, nonmax_emit.stderr, file=sys.stderr)
            return 1

        nonmax_arm = subprocess.run(
            [runner, "--extent", "48", "20", evidence_dir],
            env=environment, capture_output=True, text=True)
        if nonmax_arm.returncode != 2 or \
                "non-maximum extent" not in nonmax_arm.stderr:
            print("FAIL: non-maximum arming report was not refused",
                  file=sys.stderr)
            print(nonmax_arm.stdout, nonmax_arm.stderr, file=sys.stderr)
            return 1

        # The render-shape cell: the composed dEQP smoke shape emits, its
        # digest differs from the reference cell's, and an authorization
        # declaring the reference digest refuses the shape as a mismatch.
        smoke_shape = ["--shape", "256", "256", "256", "rgba",
                       "0x3f800000", "0x0", "0x3f800000", "0x3f800000"]
        shape_ib = os.path.join(evidence_dir, "shape-ib.bin")
        shape_emit = subprocess.run(
            [runner] + smoke_shape + ["--emit-ib", shape_ib],
            env=environment, capture_output=True, text=True)
        if shape_emit.returncode != 0 or not os.path.isfile(shape_ib):
            print("FAIL: render-shape --emit-ib failed", file=sys.stderr)
            print(shape_emit.stdout, shape_emit.stderr, file=sys.stderr)
            return 1
        shape_report = subprocess.run(
            [runner] + smoke_shape + [evidence_dir],
            env=environment, capture_output=True, text=True)
        shape_digest = re.search(r"blake3 ([0-9a-f]{64})",
                                 shape_report.stdout)
        if shape_report.returncode == 0 or shape_digest is None or \
                "draw dword 0xffff00ff" not in shape_report.stdout:
            print("FAIL: render-shape report lacks its digest or the "
                  "predicted dword", file=sys.stderr)
            print(shape_report.stdout, shape_report.stderr,
                  file=sys.stderr)
            return 1
        bad_shape = subprocess.run(
            [runner, "--shape", "256", "256", "255", "rgba",
             "0x3f800000", "0x0", "0x3f800000", "0x3f800000", evidence_dir],
            env=environment, capture_output=True, text=True)
        if bad_shape.returncode != 2 or \
                "refused by the render-shape family" not in bad_shape.stderr:
            print("FAIL: odd-pitch shape was not refused", file=sys.stderr)
            print(bad_shape.stdout, bad_shape.stderr, file=sys.stderr)
            return 1

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

        # The contract-free 76-dword IB is the stale-authorization negative
        # control: the runner's own digest names the contract-prefixed
        # successor, and an authorization still declaring the bare cell
        # refuses as a digest mismatch instead of arming.
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

        # The reference digest authorizes the reference cell alone: the
        # render-shape cell under it refuses on the digest factor.
        if shape_digest.group(1) == digest.group(1):
            print("FAIL: render-shape digest equals the reference digest",
                  file=sys.stderr)
            return 1
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = digest.group(1)
        shape_under_reference = subprocess.run(
            [runner] + smoke_shape + [evidence_dir],
            env=environment, capture_output=True, text=True)
        if shape_under_reference.returncode == 0 or \
                "MISMATCH" not in shape_under_reference.stdout:
            print("FAIL: render-shape cell armed under the reference "
                  "digest", file=sys.stderr)
            print(shape_under_reference.stdout, file=sys.stderr)
            return 1

        # A wrong chip refuses even with the bundle declared correctly.
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = digest.group(1)
        environment["R3V_NATIVE_RUNNER_PCI_DEVICE"] = "0x5975"
        wrong_chip = subprocess.run([runner, evidence_dir], env=environment,
                                    capture_output=True, text=True)
        if wrong_chip.returncode == 0 or \
                "not the authorized Dell Vostro 1000 RS485M platform" not in wrong_chip.stdout:
            print("FAIL: a board that is not the authorized platform did not refuse", file=sys.stderr)
            print(wrong_chip.stdout, file=sys.stderr)
            return 1

        # No run may claim a submission happened.
        for result in (undeclared, gated, stale, wrong_chip, shape_report,
                       shape_under_reference):
            if "no submission attempted" not in result.stdout:
                print("FAIL: report omits the no-submission statement",
                      file=sys.stderr)
                return 1

    print("r3v_native_arming_runner_check: refusals hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
