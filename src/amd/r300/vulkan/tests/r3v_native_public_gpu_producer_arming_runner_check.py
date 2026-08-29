# SPDX-License-Identifier: MIT
#
# Drives the non-submitting public GPU-producer arming runner over a
# fresh evidence directory: requires the refusal a run with no
# declarations must produce, the route-identity lines an operator reads
# to build an authorization, the delivery-gate report that separates the
# composed stream from the consumer alone, and the wrong-cell refusal
# when the producer runner's digest is declared against the route.

import os
import re
import subprocess
import sys
import tempfile


def run(runner, evidence_dir, environment, *extra):
    return subprocess.run(
        [runner, evidence_dir, *extra], env=environment, capture_output=True, text=True
    )


def fail(message, result=None):
    print(f"FAIL: {message}", file=sys.stderr)
    if result is not None:
        print(result.stdout, file=sys.stderr)
    return 1


def main():
    if len(sys.argv) != 3:
        print(
            "usage: r3v_native_public_gpu_producer_arming_runner_check.py "
            "<route-runner> <producer-runner>",
            file=sys.stderr,
        )
        return 2
    runner, producer_runner = sys.argv[1], sys.argv[2]

    environment = dict(os.environ)
    for declaration in (
        "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED",
        "R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
        "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE",
        "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
        "R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL",
        "R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL",
    ):
        environment.pop(declaration, None)

    with tempfile.TemporaryDirectory() as evidence_dir:
        undeclared = run(runner, evidence_dir, environment)
        if undeclared.returncode == 0:
            return fail("undeclared run reported an armed verdict", undeclared)
        if "hazard gate closed" not in undeclared.stdout:
            return fail("undeclared run did not name the closed gate", undeclared)
        if "cell_kind=r2vb-gpu-producer-public" not in undeclared.stdout:
            return fail("report does not name the public route cell", undeclared)

        dwords = re.search(
            r"^ib_dwords=([1-9][0-9]*)$", undeclared.stdout, re.MULTILINE
        )
        split = re.search(
            r"^consumer_start_dwords=([1-9][0-9]*)$", undeclared.stdout, re.MULTILINE
        )
        if dwords is None or split is None:
            return fail("report carries no stream geometry", undeclared)
        if int(split.group(1)) >= int(dwords.group(1)):
            return fail("the reported split lands outside the stream", undeclared)
        digest = re.search(
            r"^ib_blake3=([0-9a-f]{64})$", undeclared.stdout, re.MULTILINE
        )
        if digest is None:
            return fail("report carries no route digest", undeclared)

        # Both delivery gates unset: the report names the CPU route, so
        # an operator reading it cannot mistake a consumer-only
        # submission for the composed stream this digest names.
        if "route: cpu" not in undeclared.stdout:
            return fail(
                "report does not name the route under closed " "delivery gates",
                undeclared,
            )

        # A fresh fixture directory calibrates the complete armed branch
        # without reading host kernel state.  The public-route runner carries
        # no submission path, so this asserts only the authorization gate.
        armed_environment = dict(environment)
        armed_environment["R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL"] = "1"
        armed_environment["R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL"] = "1"
        armed = run(runner, evidence_dir, armed_environment, "--fixture")
        if armed.returncode != 0 or "verdict: armed" not in armed.stdout:
            return fail("fixture ARMED calibration refused", armed)
        for required in (
            "route: gpu-producer",
            "provider=fixture",
            "no submission attempted",
        ):
            if required not in armed.stdout:
                return fail("fixture ARMED report omits " + required, armed)

        # The producer runner's digest names the producer half alone.
        # The composed stream is longer, so the digests differ, and the
        # producer digest declared against the route refuses.
        producer = run(producer_runner, evidence_dir, environment)
        producer_digest = re.search(
            r"^ib_blake3=([0-9a-f]{64})$", producer.stdout, re.MULTILINE
        )
        producer_dwords = re.search(
            r"^ib_dwords=([1-9][0-9]*)$", producer.stdout, re.MULTILINE
        )
        if producer_digest is None or producer_dwords is None:
            return fail("producer runner report is incomplete", producer)
        if producer_digest.group(1) == digest.group(1):
            return fail(
                "the route and its producer half report one "
                "digest; each stream declares its own bytes"
            )
        if int(producer_dwords.group(1)) != int(split.group(1)):
            return fail("the reported split is not the producer length")

        environment["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
        environment["R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL"] = "1"
        environment["R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL"] = "1"
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = producer_digest.group(1)
        wrong_cell = run(runner, evidence_dir, environment)
        if wrong_cell.returncode == 0 or "MISMATCH" not in wrong_cell.stdout:
            return fail(
                "producer-digest authorization did not refuse on " "the digest factor",
                wrong_cell,
            )
        if "route: gpu-producer" not in wrong_cell.stdout:
            return fail(
                "report does not name the route under open " "delivery gates",
                wrong_cell,
            )

        # A stale digest -- one hex character off the live value --
        # refuses the same way.
        live = digest.group(1)
        stale = ("1" if live[0] != "1" else "0") + live[1:]
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = stale
        stale_run = run(runner, evidence_dir, environment)
        if stale_run.returncode == 0 or "MISMATCH" not in stale_run.stdout:
            return fail("stale digest did not refuse", stale_run)

        # A wrong chip refuses even with the stream declared correctly.
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = live
        environment["R3V_NATIVE_RUNNER_PCI_DEVICE"] = "0x5975"
        wrong_chip = run(runner, evidence_dir, environment)
        if (
            wrong_chip.returncode == 0
            or "not the authorized RS482 identity" not in wrong_chip.stdout
        ):
            return fail("wrong chip did not refuse", wrong_chip)
        environment.pop("R3V_NATIVE_RUNNER_PCI_DEVICE", None)

        # One delivery gate open is the CPU route, so the run refuses
        # however complete the rest of the authorization is.
        environment.pop("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL", None)
        half_gated = run(runner, evidence_dir, environment)
        if half_gated.returncode == 0:
            return fail("a single delivery gate reported an armed route", half_gated)
        if "route: cpu" not in half_gated.stdout:
            return fail(
                "a single delivery gate did not name the CPU " "route", half_gated
            )

        for result in (undeclared, wrong_cell, stale_run, wrong_chip, half_gated):
            if "no submission attempted" not in result.stdout:
                return fail("report omits the no-submission statement")

    print("r3v_native_public_gpu_producer_arming_runner_check: " "refusals hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
