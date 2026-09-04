# SPDX-License-Identifier: MIT
#
# Drives the non-submitting re-ingest arming runner over a fresh
# evidence directory: requires the refusal a run with no declarations
# must produce, a deterministic ARMED collection/reporting path, the
# cell-identity lines an operator reads to build an authorization, and the
# wrong-cell refusal when the producer runner's digest is declared against
# the concatenated stream.

import os
import re
import subprocess
import sys
import tempfile


def run(runner, evidence_dir, environment, *extra):
    return subprocess.run(
        [runner, evidence_dir, *extra], env=environment, capture_output=True, text=True
    )


def main():
    if len(sys.argv) != 3:
        print(
            "usage: r3v_native_reingest_arming_runner_check.py "
            "<reingest-runner> <producer-runner>",
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
    ):
        environment.pop(declaration, None)

    with tempfile.TemporaryDirectory() as evidence_dir:
        undeclared = run(runner, evidence_dir, environment)
        if undeclared.returncode == 0:
            print("FAIL: undeclared run reported an armed verdict", file=sys.stderr)
            print(undeclared.stdout, file=sys.stderr)
            return 1
        if "hazard gate closed" not in undeclared.stdout:
            print("FAIL: undeclared run did not name the closed gate", file=sys.stderr)
            print(undeclared.stdout, file=sys.stderr)
            return 1

        # The fixture provider exercises the complete positive collection
        # path with matching kernel, module, chip, digest, and fresh evidence
        # facts.  The arming runner still performs no submission.
        armed = run(runner, evidence_dir, environment, "--fixture")
        if armed.returncode != 0 or "verdict: armed" not in armed.stdout:
            print("FAIL: deterministic ARMED calibration refused", file=sys.stderr)
            print(armed.stdout, file=sys.stderr)
            return 1
        if "provider=fixture" not in armed.stdout:
            print(
                "FAIL: ARMED calibration did not use its fixture provider",
                file=sys.stderr,
            )
            print(armed.stdout, file=sys.stderr)
            return 1
        if "no submission attempted" not in armed.stdout:
            print(
                "FAIL: ARMED calibration omits the no-submission statement",
                file=sys.stderr,
            )
            return 1

        # The report names the cell and carries the digest an
        # authorization declares.
        if "cell_kind=r2vb-reingest" not in undeclared.stdout:
            print("FAIL: report does not name the re-ingest cell", file=sys.stderr)
            print(undeclared.stdout, file=sys.stderr)
            return 1
        digest = re.search(
            r"^ib_blake3=([0-9a-f]{64})$", undeclared.stdout, re.MULTILINE
        )
        if digest is None:
            print("FAIL: report carries no cell digest", file=sys.stderr)
            return 1

        # The producer runner's digest names the producer-only stream;
        # declared against the concatenation it refuses as a digest
        # mismatch.
        producer = run(producer_runner, evidence_dir, environment)
        producer_digest = re.search(
            r"^ib_blake3=([0-9a-f]{64})$", producer.stdout, re.MULTILINE
        )
        if producer_digest is None:
            print("FAIL: producer runner report carries no digest", file=sys.stderr)
            print(producer.stdout, file=sys.stderr)
            return 1
        if producer_digest.group(1) == digest.group(1):
            print(
                "FAIL: the two cells report one digest; each cell "
                "declares its own stream",
                file=sys.stderr,
            )
            return 1
        environment["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = producer_digest.group(1)
        wrong_cell = run(runner, evidence_dir, environment)
        if wrong_cell.returncode == 0 or "MISMATCH" not in wrong_cell.stdout:
            print(
                "FAIL: producer-digest authorization did not refuse "
                "on the digest factor",
                file=sys.stderr,
            )
            print(wrong_cell.stdout, file=sys.stderr)
            return 1

        # A wrong chip refuses even with the bundle declared correctly.
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = digest.group(1)
        environment["R3V_NATIVE_RUNNER_PCI_DEVICE"] = "0x5975"
        wrong_chip = run(runner, evidence_dir, environment, "--fixture")
        if (
            wrong_chip.returncode == 0
            or "not the authorized Dell Vostro 1000 RS485M platform" not in wrong_chip.stdout
        ):
            print("FAIL: a board that is not the authorized platform did not refuse", file=sys.stderr)
            print(wrong_chip.stdout, file=sys.stderr)
            return 1

        # No run may claim a submission happened.
        for result in (undeclared, wrong_cell, wrong_chip):
            if "no submission attempted" not in result.stdout:
                print("FAIL: report omits the no-submission statement", file=sys.stderr)
                return 1

    print("r3v_native_reingest_arming_runner_check: refusals hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
