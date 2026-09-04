# SPDX-License-Identifier: MIT
#
# Drives the non-submitting producer-cell arming runner over a fresh
# evidence directory: requires the refusal a run with no declarations
# must produce, a deterministic ARMED collection/reporting path, the
# cell-identity lines an operator reads to build an authorization, and the
# wrong-cell refusal when the triangle runner's digest is declared against
# the producer pass.

import os
import re
import subprocess
import sys
import tempfile


def run(runner, evidence_dir, environment, *extra):
    return subprocess.run([runner, evidence_dir, *extra], env=environment,
                          capture_output=True, text=True)


def main():
    if len(sys.argv) != 3:
        print("usage: r3v_native_producer_arming_runner_check.py "
              "<producer-runner> <triangle-runner>", file=sys.stderr)
        return 2
    runner, triangle_runner = sys.argv[1], sys.argv[2]

    environment = dict(os.environ)
    for declaration in (
        "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED",
        "R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
        "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE",
        "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
        "R3V_NATIVE_RUNNER_PCI_VENDOR",
        "R3V_NATIVE_RUNNER_PCI_DEVICE",
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
        if "cell_kind=r2vb-producer" not in undeclared.stdout:
            print("FAIL: report does not name the producer cell",
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

        # The fixture provider exercises the runner's complete collection and
        # reporting path with exact kernel/module declarations, while the
        # production runner remains host-backed by default.  The fixture mode
        # cannot submit because this executable has no submission path.
        armed = run(runner, evidence_dir, environment, "reference", "--fixture")
        if armed.returncode != 0 or "verdict: armed" not in armed.stdout:
            print("FAIL: deterministic ARMED calibration refused",
                  file=sys.stderr)
            print(armed.stdout, file=sys.stderr)
            return 1
        if "provider=fixture" not in armed.stdout:
            print("FAIL: ARMED calibration did not use its fixture provider",
                  file=sys.stderr)
            return 1
        if "no submission attempted" not in armed.stdout:
            print("FAIL: ARMED calibration omits the no-submission statement",
                  file=sys.stderr)
            return 1

        # A malformed PCI declaration must fail before the report can claim
        # a matching identity.  Exercise both trailing data and uint32_t
        # overflow so prefix parsing cannot authorize a wrapped device.
        for variable, value in (
            ("R3V_NATIVE_RUNNER_PCI_DEVICE", "0x5974junk"),
            ("R3V_NATIVE_RUNNER_PCI_DEVICE", "0x100000000"),
            ("R3V_NATIVE_RUNNER_PCI_VENDOR", "0x1002junk"),
        ):
            environment[variable] = value
            malformed = run(runner, evidence_dir, environment)
            if malformed.returncode != 2 or \
                    "invalid {}".format(variable) not in malformed.stderr:
                print("FAIL: malformed {} was accepted".format(variable),
                      file=sys.stderr)
                print(malformed.stdout, file=sys.stderr)
                print(malformed.stderr, file=sys.stderr)
                return 1
            environment.pop(variable, None)

        # The triangle runner's digest names a different stream; declared
        # against the producer pass, the wrong-cell authorization refuses
        # as a digest mismatch.
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
        wrong_chip = run(runner, evidence_dir, environment, "reference",
                         "--fixture")
        if wrong_chip.returncode == 0 or \
                "not the authorized Dell Vostro 1000 RS485M platform" not in wrong_chip.stdout:
            print("FAIL: a board that is not the authorized platform did not refuse", file=sys.stderr)
            print(wrong_chip.stdout, file=sys.stderr)
            return 1

        # The FP24 boundary-sweep stream: its report names the stream,
        # its digest differs from the reference stream's, and the
        # reference digest declared against the sweep refuses as a
        # digest mismatch -- one authorization never covers both.
        environment.pop("R3V_NATIVE_RUNNER_PCI_DEVICE", None)
        environment.pop("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", None)
        environment.pop("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", None)
        sweep = run(runner, evidence_dir, environment, "fp24-sweep")
        if sweep.returncode == 0:
            print("FAIL: undeclared sweep run reported an armed verdict",
                  file=sys.stderr)
            return 1
        if "stream=fp24-sweep" not in sweep.stdout:
            print("FAIL: sweep report does not name its stream",
                  file=sys.stderr)
            print(sweep.stdout, file=sys.stderr)
            return 1
        sweep_digest = re.search(r"^ib_blake3=([0-9a-f]{64})$", sweep.stdout,
                                 re.MULTILINE)
        if sweep_digest is None or sweep_digest.group(1) == digest.group(1):
            print("FAIL: sweep stream does not carry its own digest",
                  file=sys.stderr)
            return 1

        # A matching sweep digest must arm the sweep stream under the same
        # deterministic fixture contract as the reference calibration.
        environment["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = \
            sweep_digest.group(1)
        sweep_authorized = run(runner, evidence_dir, environment,
                               "fp24-sweep", "--fixture")
        if sweep_authorized.returncode != 0 or \
                "verdict: armed" not in sweep_authorized.stdout:
            print("FAIL: sweep digest did not arm the sweep stream",
                  file=sys.stderr)
            print(sweep_authorized.stdout, file=sys.stderr)
            return 1
        if "provider=fixture" not in sweep_authorized.stdout or \
                "stream=fp24-sweep" not in sweep_authorized.stdout:
            print("FAIL: sweep ARMED calibration did not identify its "
                  "fixture stream", file=sys.stderr)
            print(sweep_authorized.stdout, file=sys.stderr)
            return 1

        environment["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = digest.group(1)
        cross = run(runner, evidence_dir, environment, "fp24-sweep")
        if cross.returncode == 0 or "MISMATCH" not in cross.stdout:
            print("FAIL: reference digest declared against the sweep "
                  "stream did not refuse", file=sys.stderr)
            print(cross.stdout, file=sys.stderr)
            return 1
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = \
            sweep_digest.group(1)
        reverse_cross = run(runner, evidence_dir, environment)
        if reverse_cross.returncode == 0 or \
                "MISMATCH" not in reverse_cross.stdout:
            print("FAIL: sweep digest declared against the reference "
                  "stream did not refuse", file=sys.stderr)
            print(reverse_cross.stdout, file=sys.stderr)
            return 1
        bad_selector = run(runner, evidence_dir, environment, "fp25-sweep")
        if bad_selector.returncode != 2:
            print("FAIL: unknown stream selector did not refuse usage",
                  file=sys.stderr)
            return 1

        # The FP24 upper-ceiling bisection stream: the same contract as
        # the sweep -- named stream, distinct digest, and refusal when a
        # sibling stream's digest is declared against it.
        environment.pop("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", None)
        environment.pop("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", None)
        bisect = run(runner, evidence_dir, environment, "fp24-bisect")
        if bisect.returncode == 0:
            print("FAIL: undeclared bisect run reported an armed verdict",
                  file=sys.stderr)
            return 1
        if "stream=fp24-bisect" not in bisect.stdout:
            print("FAIL: bisect report does not name its stream",
                  file=sys.stderr)
            print(bisect.stdout, file=sys.stderr)
            return 1
        bisect_digest = re.search(r"^ib_blake3=([0-9a-f]{64})$",
                                  bisect.stdout, re.MULTILINE)
        if bisect_digest is None or bisect_digest.group(1) in (
                digest.group(1), sweep_digest.group(1)):
            print("FAIL: bisect stream does not carry its own digest",
                  file=sys.stderr)
            return 1
        environment["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = \
            sweep_digest.group(1)
        bisect_cross = run(runner, evidence_dir, environment, "fp24-bisect")
        if bisect_cross.returncode == 0 or \
                "MISMATCH" not in bisect_cross.stdout:
            print("FAIL: sweep digest declared against the bisect stream "
                  "did not refuse", file=sys.stderr)
            print(bisect_cross.stdout, file=sys.stderr)
            return 1

        # No run may claim a submission happened.
        for result in (undeclared, armed, wrong_cell, stale_run, wrong_chip,
                       sweep, sweep_authorized, cross, reverse_cross, bisect,
                       bisect_cross):
            if "no submission attempted" not in result.stdout:
                print("FAIL: report omits the no-submission statement",
                      file=sys.stderr)
                return 1

    print("r3v_native_producer_arming_runner_check: refusals hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
