# SPDX-License-Identifier: MIT
#
# Drives the non-submitting depth address-discovery arming runner over a
# fresh evidence directory: requires the refusal a run with no
# declarations must produce, the cell-identity lines an operator reads to
# build an authorization, the wrong-cell refusal when the depth-control
# runner's digest is declared against the discovery cell, and the two
# digest relations the campaign rests on -- one stream digest across the
# three linear scenarios, and one initial-image digest per scenario.

import os
import re
import subprocess
import sys
import tempfile

SCENARIOS = (
    "z24_linear",
    "z24_linear_seed_5a",
    "z24_linear_seed_a5",
    "z24_microtiled",
    "z24_macrotiled",
)

ARMS = ("measure", "writes_disabled", "never")


def run(runner, evidence_dir, environment, scenario="z24_linear",
        arm="measure"):
    return subprocess.run([runner, evidence_dir, scenario, arm],
                          env=environment, capture_output=True, text=True)


def field(text, name):
    match = re.search(r"^%s=([0-9a-f]{64})$" % name, text, re.MULTILINE)
    return match.group(1) if match else None


def main():
    if len(sys.argv) != 3:
        print("usage: r3v_native_zb_depth_discovery_arming_runner_check.py "
              "<discovery-runner> <depth-control-runner>", file=sys.stderr)
        return 2
    runner, control_runner = sys.argv[1], sys.argv[2]

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
        if "cell_kind=zb-depth-discovery" not in undeclared.stdout:
            print("FAIL: report does not name the discovery cell",
                  file=sys.stderr)
            return 1
        if re.search(r"^ib_dwords=[1-9][0-9]*$", undeclared.stdout,
                     re.MULTILINE) is None:
            print("FAIL: report carries no dword count", file=sys.stderr)
            return 1
        digest = field(undeclared.stdout, "ib_blake3")
        if digest is None:
            print("FAIL: report carries no cell digest", file=sys.stderr)
            return 1
        if field(undeclared.stdout, "initial_image_blake3") is None:
            print("FAIL: report carries no initial-image digest",
                  file=sys.stderr)
            return 1

        # Every scenario and arm builds, so a rung is reachable before an
        # operator reaches for it.
        streams = {}
        images = {}
        for scenario in SCENARIOS:
            for arm in ARMS:
                result = run(runner, evidence_dir, environment, scenario, arm)
                stream = field(result.stdout, "ib_blake3")
                image = field(result.stdout, "initial_image_blake3")
                if stream is None or image is None:
                    print("FAIL: %s/%s reports no digest pair"
                          % (scenario, arm), file=sys.stderr)
                    print(result.stdout, file=sys.stderr)
                    return 1
                streams[(scenario, arm)] = stream
                images[(scenario, arm)] = image

        # The three linear scenarios differ in their host fill alone, so
        # one stream digest covers all three at a given arm.  This is the
        # relation that makes the initial-image digest necessary rather
        # than decorative.
        for arm in ARMS:
            linear = {streams[(s, arm)] for s in
                      ("z24_linear", "z24_linear_seed_5a",
                       "z24_linear_seed_a5")}
            if len(linear) != 1:
                print("FAIL: the three linear scenarios report %d stream "
                      "digests at arm %s; their streams are identical"
                      % (len(linear), arm), file=sys.stderr)
                return 1

        # Each stencil seed names its own experiment, which is exactly
        # what the stream digest cannot express.
        seeds = {images[(s, "measure")] for s in
                 ("z24_linear", "z24_linear_seed_5a", "z24_linear_seed_a5")}
        if len(seeds) != 3:
            print("FAIL: the three stencil seeds report %d initial-image "
                  "digests; each seed is its own experiment" % len(seeds),
                  file=sys.stderr)
            return 1

        # A tiled rung carries its own stream, because DEPTHPITCH's tile
        # bits move with the surface.
        for tiled in ("z24_microtiled", "z24_macrotiled"):
            if streams[(tiled, "measure")] == streams[("z24_linear",
                                                       "measure")]:
                print("FAIL: %s reports the linear stream digest; the tile "
                      "bits move with the surface" % tiled, file=sys.stderr)
                return 1

        # The three arms differ in their comparison and write enable, so
        # each carries its own stream and one authorization admits one
        # arm.
        arm_digests = {streams[("z24_linear", arm)] for arm in ARMS}
        if len(arm_digests) != len(ARMS):
            print("FAIL: the arms report %d stream digests; each arm "
                  "declares its own stream" % len(arm_digests),
                  file=sys.stderr)
            return 1

        # The depth-control cell's digest names a different stream;
        # declared against the discovery cell it refuses.
        control = subprocess.run([control_runner, evidence_dir],
                                 env=environment, capture_output=True,
                                 text=True)
        control_digest = field(control.stdout, "ib_blake3")
        if control_digest is None:
            print("FAIL: depth-control runner report carries no digest",
                  file=sys.stderr)
            print(control.stdout, file=sys.stderr)
            return 1
        if control_digest == digest:
            print("FAIL: the two cells report one digest; each cell "
                  "declares its own stream", file=sys.stderr)
            return 1
        environment["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = control_digest
        wrong_cell = run(runner, evidence_dir, environment)
        if wrong_cell.returncode == 0 or "MISMATCH" not in wrong_cell.stdout:
            print("FAIL: depth-control authorization did not refuse on the "
                  "digest factor", file=sys.stderr)
            print(wrong_cell.stdout, file=sys.stderr)
            return 1

        # A stale digest, one hex character off the live value, refuses
        # the same way.
        stale = ("1" + digest[1:]) if digest[0] != "1" else ("0" + digest[1:])
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = stale
        stale_run = run(runner, evidence_dir, environment)
        if stale_run.returncode == 0 or "MISMATCH" not in stale_run.stdout:
            print("FAIL: stale digest did not refuse", file=sys.stderr)
            print(stale_run.stdout, file=sys.stderr)
            return 1

        # An authorization built for one arm does not admit another.
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = digest
        other_arm = run(runner, evidence_dir, environment, "z24_linear",
                        "never")
        if other_arm.returncode == 0 or "MISMATCH" not in other_arm.stdout:
            print("FAIL: a measurement-arm authorization admitted the NEVER "
                  "arm", file=sys.stderr)
            print(other_arm.stdout, file=sys.stderr)
            return 1

        # A wrong chip refuses even with the bundle declared correctly.
        environment["R3V_NATIVE_RUNNER_PCI_DEVICE"] = "0x5975"
        wrong_chip = run(runner, evidence_dir, environment)
        if wrong_chip.returncode == 0 or \
                "not the authorized Dell Vostro 1000 RS485M platform" \
                not in wrong_chip.stdout:
            print("FAIL: a board that is not the authorized platform did "
                  "not refuse", file=sys.stderr)
            print(wrong_chip.stdout, file=sys.stderr)
            return 1
        environment.pop("R3V_NATIVE_RUNNER_PCI_DEVICE", None)

        # An unknown scenario or arm is a usage error, not a verdict.
        for bad in (("z24_octiled", "measure"), ("z24_linear", "sometimes")):
            usage = run(runner, evidence_dir, environment, bad[0], bad[1])
            if usage.returncode != 2:
                print("FAIL: unknown %s did not report a usage error"
                      % str(bad), file=sys.stderr)
                return 1

        # No run may claim a submission happened.
        for result in (undeclared, wrong_cell, stale_run, other_arm,
                       wrong_chip):
            if "no submission attempted" not in result.stdout:
                print("FAIL: report omits the no-submission statement",
                      file=sys.stderr)
                return 1

    print("r3v_native_zb_depth_discovery_arming_runner_check: refusals hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
