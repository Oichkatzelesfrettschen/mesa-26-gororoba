# SPDX-License-Identifier: MIT
#
# Drives the non-submitting fetched GPU-producer arming runner over a
# fresh evidence directory: requires the refusal a run with no
# declarations must produce, the route-identity lines an operator reads
# to build an authorization, the three-gate report that separates the
# fetched composition from the immediate route and from the consumer
# alone, the per-width identities (F32_4 unnamed, F32_3 and F32_2 by
# name, each its own digest over one stream geometry), and the wrong-cell
# refusal when the immediate public route runner's digest is declared
# against the fetched route.

import os
import re
import subprocess
import sys
import tempfile


def run(runner, evidence_dir, environment, *extra):
    return subprocess.run([runner, evidence_dir, *extra], env=environment,
                          capture_output=True, text=True)


def fail(message, result=None):
    print(f"FAIL: {message}", file=sys.stderr)
    if result is not None:
        print(result.stdout, file=sys.stderr)
    return 1


def main():
    if len(sys.argv) != 3:
        print("usage: r3v_native_fetched_gpu_producer_arming_runner_check.py "
              "<fetched-runner> <immediate-route-runner>", file=sys.stderr)
        return 2
    runner, immediate_runner = sys.argv[1], sys.argv[2]

    environment = dict(os.environ)
    for declaration in (
        "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED",
        "R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
        "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE",
        "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
        "R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL",
        "R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL",
        "R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL",
    ):
        environment.pop(declaration, None)

    with tempfile.TemporaryDirectory() as evidence_dir:
        undeclared = run(runner, evidence_dir, environment)
        if undeclared.returncode == 0:
            return fail("undeclared run reported an armed verdict",
                        undeclared)
        if "hazard gate closed" not in undeclared.stdout:
            return fail("undeclared run did not name the closed gate",
                        undeclared)
        if "cell_kind=r2vb-gpu-producer-fetched" not in undeclared.stdout:
            return fail("report does not name the fetched route cell",
                        undeclared)
        if "source_format=F32_4" not in undeclared.stdout:
            return fail("the unnamed width is F32_4", undeclared)

        dwords = re.search(r"^ib_dwords=([1-9][0-9]*)$", undeclared.stdout,
                           re.MULTILINE)
        split = re.search(r"^consumer_start_dwords=([1-9][0-9]*)$",
                          undeclared.stdout, re.MULTILINE)
        if dwords is None or split is None:
            return fail("report carries no stream geometry", undeclared)
        if int(split.group(1)) >= int(dwords.group(1)):
            return fail("the reported split lands outside the stream",
                        undeclared)
        digest = re.search(r"^ib_blake3=([0-9a-f]{64})$", undeclared.stdout,
                           re.MULTILINE)
        if digest is None:
            return fail("report carries no route digest", undeclared)

        # Each source width is its own cell: the named width reports
        # itself and a distinct digest over the same length and split,
        # and a width outside the admitted three refuses by usage.
        width_digests = {"F32_4": digest.group(1)}
        for width in ("f32_3", "f32_2"):
            named = run(runner, evidence_dir, environment, width)
            label = width.upper()
            if named.returncode == 0:
                return fail(f"undeclared {width} run reported an armed "
                            "verdict", named)
            if f"source_format={label}" not in named.stdout:
                return fail(f"{width} report does not name its width", named)
            named_digest = re.search(r"^ib_blake3=([0-9a-f]{64})$",
                                     named.stdout, re.MULTILINE)
            named_dwords = re.search(r"^ib_dwords=([1-9][0-9]*)$",
                                     named.stdout, re.MULTILINE)
            named_split = re.search(r"^consumer_start_dwords=([1-9][0-9]*)$",
                                    named.stdout, re.MULTILINE)
            if named_digest is None or named_dwords is None or \
                    named_split is None:
                return fail(f"{width} report carries no stream identity",
                            named)
            if named_dwords.group(1) != dwords.group(1) or \
                    named_split.group(1) != split.group(1):
                return fail(f"{width} stream geometry differs from F32_4; "
                            "the widths differ in the fetch swizzle alone",
                            named)
            if named_digest.group(1) in width_digests.values():
                return fail(f"{width} reports another width's digest", named)
            width_digests[label] = named_digest.group(1)
        outside = run(runner, evidence_dir, environment, "f32_1")
        if outside.returncode != 2:
            return fail("a width outside the admitted three did not refuse "
                        "by usage", outside)

        # Every delivery gate unset: the report names the CPU route, so
        # an operator reading it cannot mistake a consumer-only
        # submission for the composed stream this digest names.
        if "route: cpu" not in undeclared.stdout:
            return fail("report does not name the route under closed "
                        "delivery gates", undeclared)

        # The immediate public route runner's digest names the immediate
        # composition.  Both streams share a length and a split, so the
        # digests alone separate the two cells, and the immediate digest
        # declared against the fetched route refuses.
        immediate = run(immediate_runner, evidence_dir, environment)
        immediate_digest = re.search(r"^ib_blake3=([0-9a-f]{64})$",
                                     immediate.stdout, re.MULTILINE)
        immediate_dwords = re.search(r"^ib_dwords=([1-9][0-9]*)$",
                                     immediate.stdout, re.MULTILINE)
        if immediate_digest is None or immediate_dwords is None:
            return fail("immediate route runner report is incomplete",
                        immediate)
        if immediate_digest.group(1) == digest.group(1):
            return fail("the fetched and immediate routes report one "
                        "digest; each stream declares its own bytes")

        environment["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
        environment["R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL"] = "1"
        environment["R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL"] = "1"
        environment["R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL"] = "1"
        environment["R3V_NATIVE_AUTHORIZED_IB_BLAKE3"] = \
            immediate_digest.group(1)
        wrong_cell = run(runner, evidence_dir, environment)
        if wrong_cell.returncode == 0 or "MISMATCH" not in wrong_cell.stdout:
            return fail("immediate-digest authorization did not refuse on "
                        "the digest factor", wrong_cell)
        if "route: gpu-producer-fetched" not in wrong_cell.stdout:
            return fail("report does not name the fetched route under the "
                        "three open gates", wrong_cell)

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
        if wrong_chip.returncode == 0 or \
                "not the authorized RS482 identity" not in wrong_chip.stdout:
            return fail("wrong chip did not refuse", wrong_chip)
        environment.pop("R3V_NATIVE_RUNNER_PCI_DEVICE", None)

        # The fetched gate alone closed is the immediate route, and a
        # producer gate closed is the CPU route; each refuses however
        # complete the rest of the authorization is.
        environment.pop("R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL", None)
        immediate_gated = run(runner, evidence_dir, environment)
        if immediate_gated.returncode == 0:
            return fail("the fetched gate unset reported an armed route",
                        immediate_gated)
        if "route: gpu-producer (the fetched gate is unset" \
                not in immediate_gated.stdout:
            return fail("the fetched gate unset did not name the immediate "
                        "route", immediate_gated)
        environment["R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL"] = "1"
        environment.pop("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL", None)
        half_gated = run(runner, evidence_dir, environment)
        if half_gated.returncode == 0:
            return fail("a single producer gate reported an armed route",
                        half_gated)
        if "route: cpu" not in half_gated.stdout:
            return fail("a single producer gate did not name the CPU "
                        "route", half_gated)

        for result in (undeclared, wrong_cell, stale_run, wrong_chip,
                       immediate_gated, half_gated):
            if "no submission attempted" not in result.stdout:
                return fail("report omits the no-submission statement")

    print("r3v_native_fetched_gpu_producer_arming_runner_check: "
          "refusals hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
