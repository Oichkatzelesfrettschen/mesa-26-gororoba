#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Holds the crossover harness to the campaign contract, under the shim.

The harness is the instrument the crossover measurement runs on, so its
mechanism is checked where a shim absorbs the submission and no timing
claim is made: the numbers a shim run produces measure the shim.  What
this check judges is the mechanism around them -- that each arm opens its
own declaration, that the host arm opens none, that every warmup and
every measured repetition is verified, that a case retains its evidence
once and no later repetition writes, that the declared budget is exact,
that a run whose request disagrees with its declaration is refused before
any device exists, and that every requested row survives publication.

Each negative row states the exact verdict it expects, so a crashed
runner or a missing artifact does not satisfy an expected refusal.
"""

import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile

FIXTURE_SRCVERSION = "FIXTURESRCVERSION0000000"
SPECIMEN_SUBSYSTEM = "1028:022a"
SPECIMEN_DMI = "Vostro   1000 "
SPECIMEN_PLATFORM = "vostro1000_rs485m_5974"
# Two intervals the V1 contract covers in one window, so both arms admit
# both cases and the schedule is the three-arm cycle throughout.
SIZES = (256, 4096)
FILL_VALUE = 0x11223344
WAIT_BOUND_NS = 30000000000
WARMUP = 1
# The three-arm balancing cycle is 6, so the measured repetition count
# completes it.
REPS = 6

REFUSED = 2
ORACLE_FAILED = 1


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def declaration_text(nonce, kernel, route, allocation_bytes, *, sizes=SIZES,
                     warmup=WARMUP, reps=REPS, value=FILL_VALUE,
                     completion_ns=WAIT_BOUND_NS):
    lines = [
        "schema = r3v-measurement-session-v1",
        f"session_nonce = {nonce}",
        f"platform = {SPECIMEN_PLATFORM}",
        f"route = {route}",
        "pci_vendor_id = 0x1002",
        "pci_device_id = 0x5974",
        f"kernel_release = {kernel}",
        f"module_srcversion = {FIXTURE_SRCVERSION}",
        f"allocation_bytes = {allocation_bytes}",
        f"buffer_bytes = {allocation_bytes}",
        "binding_offset = 0",
        "memory_property_flags = 0xf",
        "buffer_usage = 0x2",
        "write_domain = 0x2",
        f"max_total_submissions = {len(sizes) * (warmup + reps)}",
        f"completion_timeout_ns = {completion_ns}",
    ]
    for case_id, size in enumerate(sizes):
        lines.append(f"case = {case_id}, 0, {size}, 0x{value:08x}, "
                     f"{warmup}, {reps}")
    return "\n".join(lines) + "\n"


class Harness:
    def __init__(self, crossover, work, allocation_bytes):
        self.crossover = crossover
        self.work = work
        self.allocation_bytes = allocation_bytes
        self.kernel = platform.release()
        self.count = 0

    def environment(self):
        env = dict(os.environ)
        for key in list(env):
            if key.startswith(("R3V_NATIVE_", "R3V_DRM_SHIM_")):
                del env[key]
        env["R3V_DRM_SHIM_SUBSYSTEM_ID"] = SPECIMEN_SUBSYSTEM
        env["R3V_DRM_SHIM_DMI_PRODUCT_NAME"] = SPECIMEN_DMI
        env["R3V_DRM_SHIM_MODULE_SRCVERSION"] = FIXTURE_SRCVERSION
        env["R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE"] = self.kernel
        env["R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION"] = FIXTURE_SRCVERSION
        return env

    def run(self, label, *, sizes=SIZES, warmup=WARMUP, reps=REPS,
            v2_declaration=None, v1_declaration=None, extra_args=(),
            no_v1=False, absorbing=True):
        self.count += 1
        root = os.path.join(self.work, f"run-{self.count}")
        os.makedirs(root)
        evidence = {}
        args = [self.crossover, "--warmup", str(warmup), "--reps", str(reps),
                "--wait-bound-ns", str(WAIT_BOUND_NS),
                "--json", os.path.join(root, "samples.json")]
        for size in sizes:
            args += ["--size", str(size)]
        for arm, route, declaration in (
                ("v2", "rb2d_const_fill_v2", v2_declaration),
                ("v1", "rb2d_const_fill", v1_declaration)):
            if no_v1 and arm == "v1":
                continue
            path = os.path.join(root, f"{arm}.declaration")
            with open(path, "w", encoding="ascii") as handle:
                handle.write(declaration if declaration is not None else
                             declaration_text(f"{label}-{arm}", self.kernel,
                                              route, self.allocation_bytes,
                                              sizes=sizes, warmup=warmup,
                                              reps=reps))
            directory = os.path.join(root, f"evidence-{arm}")
            os.makedirs(directory)
            evidence[arm] = directory
            args += [f"--declaration-{arm}", path,
                     f"--evidence-{arm}", directory]
        if no_v1:
            args.append("--no-v1")
        if absorbing:
            args.append("--absorbing-transport")
        args += list(extra_args)
        result = subprocess.run(args, env=self.environment(),
                                capture_output=True, text=True, timeout=600)
        result.root = root
        result.evidence = evidence
        result.samples = os.path.join(root, "samples.json")
        return result


def rows(result, kind):
    if not os.path.exists(result.samples):
        return []
    out = []
    with open(result.samples, encoding="ascii") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            if record.get("row") == kind:
                out.append(record)
    return out


def expect_refusal(label, result, needle):
    if result.returncode != REFUSED:
        fail(f"{label}: expected the refusal exit {REFUSED}, got "
             f"{result.returncode}\n{result.stdout}\n{result.stderr}")
    if needle not in result.stderr:
        fail(f"{label}: the refusal does not name {needle!r}\n"
             f"{result.stderr}")


def main():
    if len(sys.argv) != 2:
        fail("usage: r3v_crossover_campaign_check.py <crossover>")
    crossover = sys.argv[1]
    work = tempfile.mkdtemp(prefix="r3v-crossover-check-")
    try:
        run_checks(crossover, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("crossover campaign harness: every row holds")


def run_checks(crossover, work):
    allocation_bytes = max(SIZES) + 64
    harness = Harness(crossover, work, allocation_bytes)

    # The oracle runs on the first warmup and its verdict governs.  The
    # shim absorbs the submission without executing it, so the GPU arm's
    # destination still carries the sentinel; without the fixture flag
    # the campaign ends there, at warmup 0, on the verify stage.  A
    # warmup that only submitted would have reached the measured
    # repetitions instead.
    unexecuted = harness.run("warmup-oracle", absorbing=False)
    if unexecuted.returncode != ORACLE_FAILED:
        fail(f"warmup-oracle: expected the oracle exit {ORACLE_FAILED}, got "
             f"{unexecuted.returncode}\n{unexecuted.stderr}")
    if "REPETITION_MISMATCH: warmup 0 at verify" not in unexecuted.stderr:
        fail("warmup-oracle: the run did not stop on the first warmup's "
             f"oracle\n{unexecuted.stderr}")
    warm_samples = rows(unexecuted, "sample")
    if any(s["phase"] == "measured" for s in warm_samples):
        fail("warmup-oracle: a measured sample was published after a "
             "warmup whose oracle failed")

    # The whole campaign: every arm runs, every case enrolls against the
    # transport the device retained, and every requested row is published.
    ok = harness.run("campaign")
    if ok.returncode != 0:
        fail(f"campaign: exit {ok.returncode}\n{ok.stdout}\n{ok.stderr}")

    samples = rows(ok, "sample")
    expected_per_arm = len(SIZES) * (WARMUP + REPS)
    for arm in ("host", "v2", "v1"):
        arm_rows = [s for s in samples if s["arm"] == arm]
        if len(arm_rows) != expected_per_arm:
            fail(f"campaign: arm {arm} published {len(arm_rows)} samples, "
                 f"the run declared {expected_per_arm}")
        # The host arm executes on the host, so its oracle governs.  The
        # GPU arms run under a transport that absorbs the submission, so
        # every one of their rows records that execution was not asserted
        # rather than claiming a fill it never received.
        if arm == "host":
            if any(s["oracle"] != "pass" for s in arm_rows):
                fail(f"campaign: arm {arm} published a sample whose oracle "
                     f"did not pass")
            if any(not s["execution_asserted"] for s in arm_rows):
                fail(f"campaign: arm {arm} executes and must assert it")
        elif any(s["execution_asserted"] for s in arm_rows):
            fail(f"campaign: arm {arm} claims execution under a transport "
                 f"that absorbs its submissions")
        warmups = [s for s in arm_rows if s["phase"] == "warmup"]
        measured = [s for s in arm_rows if s["phase"] == "measured"]
        if len(warmups) != len(SIZES) * WARMUP:
            fail(f"campaign: arm {arm} counted {len(warmups)} warmups")
        if len(measured) != len(SIZES) * REPS:
            fail(f"campaign: arm {arm} counted {len(measured)} measured "
                 f"repetitions")
        # The declared budget is exact: the last allowance a case spends
        # is the sum of every submission the arm performed.
        if max(s["allowance_consumed"] for s in arm_rows) != expected_per_arm:
            fail(f"campaign: arm {arm} spent an allowance that does not "
                 f"match its declared budget")

    # The measured schedule balances position: over a completed cycle
    # every arm holds every position equally often.  A schedule that
    # reverses three arms leaves the middle one fixed, which this counts.
    for case_id in range(len(SIZES)):
        histogram = {}
        for s in samples:
            if s["case_id"] != case_id or s["phase"] != "measured":
                continue
            histogram.setdefault(s["arm"], {}).setdefault(s["position"], 0)
            histogram[s["arm"]][s["position"]] += 1
        for arm, positions in histogram.items():
            if sorted(positions) != list(range(len(histogram))):
                fail(f"campaign: case {case_id} arm {arm} never held every "
                     f"position; it held {sorted(positions)}")
            counts = set(positions.values())
            if len(counts) != 1:
                fail(f"campaign: case {case_id} arm {arm} held its "
                     f"positions unequally: {positions}")

    # Every sample carries both intervals, and the transport interval is
    # nested inside the delivery interval it belongs to.
    for s in samples:
        if s["transport_ns"] > s["delivery_ns"]:
            fail("campaign: a sample reports a transport interval longer "
                 "than the delivery interval that encloses it")

    # The host arm reports its executor as host and carries no executed
    # GPU stream shape.
    cells = rows(ok, "cell")
    host_cells = [c for c in cells if c["arm"] == "host"]
    if not host_cells:
        fail("campaign: the host arm published no cell")
    for cell in host_cells:
        if cell.get("executor") != "host":
            fail("campaign: a host cell does not report the host executor")
        if cell.get("executed_gpu_shape") != "not_applicable":
            fail("campaign: a host cell claims an executed GPU shape")
        if "retained_ib_dwords" in cell:
            fail("campaign: a host cell claims a retained GPU transport")
    for cell in (c for c in cells if c["arm"] in ("v1", "v2")):
        if cell.get("predicted_ib_dwords") != cell.get("retained_ib_dwords"):
            fail("campaign: a GPU cell publishes a prediction the retained "
                 "transport does not match")

    # Each GPU arm retained exactly one transport per case, into its own
    # evidence directory, and nothing into the shared root.
    for arm in ("v2", "v1"):
        directory = ok.evidence[arm]
        entries = sorted(os.listdir(directory))
        expected = [f"case-{i}" for i in range(len(SIZES))]
        if entries != expected:
            fail(f"campaign: arm {arm} retained {entries}, not {expected}")
        digests = set()
        for case in expected:
            manifest = os.path.join(directory, case, "manifest.json")
            if not os.path.exists(manifest):
                fail(f"campaign: arm {arm} {case} retained no manifest")
            with open(manifest, encoding="ascii") as handle:
                digests.add(json.load(handle)["ib_blake3"])
        if len(digests) != len(SIZES):
            fail(f"campaign: arm {arm} retained one stream for two cases")

    # The two GPU arms are separate campaigns: neither retained into the
    # other's destination, and each opened its own declaration.
    if ok.evidence["v2"] == ok.evidence["v1"]:
        fail("campaign: the two GPU arms share one evidence destination")

    # The completeness record accounts for every published row.
    completeness = rows(ok, "completeness")
    if len(completeness) != 1:
        fail("campaign: the run published no single completeness record")
    record = completeness[0]
    if record["observed_samples"] != len(samples):
        fail("campaign: the completeness record counts samples the file "
             "does not carry")
    if record["declared_samples"] != record["observed_samples"]:
        fail("campaign: the campaign published fewer samples than it "
             "declared")
    if not re.fullmatch(r"0x[0-9a-f]{16}", record["stream_fnv1a64"]):
        fail("campaign: the completeness record carries no stream digest")

    # A declaration whose cases disagree with the requested sweep refuses
    # before any device is created, so no allowance is spent.
    mismatched = declaration_text("mismatch-v2", harness.kernel,
                                  "rb2d_const_fill_v2", allocation_bytes,
                                  sizes=(512, 4096))
    result = harness.run("size-mismatch", v2_declaration=mismatched)
    expect_refusal("size-mismatch", result, "the declaration states")

    # A declaration written for the other route refuses this arm.
    wrong_route = declaration_text("route-v2", harness.kernel,
                                   "rb2d_const_fill", allocation_bytes)
    result = harness.run("route-mismatch", v2_declaration=wrong_route)
    expect_refusal("route-mismatch", result, "measures route")

    # A declaration whose repetition count differs from the run refuses.
    wrong_counts = declaration_text("counts-v2", harness.kernel,
                                    "rb2d_const_fill_v2", allocation_bytes,
                                    warmup=WARMUP, reps=REPS + 6)
    result = harness.run("count-mismatch", v2_declaration=wrong_counts)
    expect_refusal("count-mismatch", result, "repetitions")

    # A completion bound the run does not wait to refuses.
    wrong_bound = declaration_text("bound-v2", harness.kernel,
                                   "rb2d_const_fill_v2", allocation_bytes,
                                   completion_ns=WAIT_BOUND_NS + 1)
    result = harness.run("bound-mismatch", v2_declaration=wrong_bound)
    expect_refusal("bound-mismatch", result, "bounds completion")

    # A GPU arm without a declaration refuses rather than inheriting
    # whatever the environment carried.
    result = subprocess.run(
        [crossover, "--warmup", str(WARMUP), "--reps", str(REPS),
         "--size", str(SIZES[0])],
        env=harness.environment(), capture_output=True, text=True,
        timeout=600)
    if result.returncode != REFUSED:
        fail(f"undeclared-arm: expected {REFUSED}, got {result.returncode}")
    if "needs --declaration" not in result.stderr:
        fail(f"undeclared-arm: the refusal does not name the missing "
             f"declaration\n{result.stderr}")

    # A repetition count that does not complete the balancing cycle
    # refuses, so no cell rests on a schedule that favors a position.
    result = harness.run("unbalanced", reps=4)
    expect_refusal("unbalanced", result, "balancing cycle")

    # A zero-warmup configuration refuses: a case that never enrolled has
    # no retained transport to hold its prediction against.
    result = harness.run("zero-warmup", warmup=0)
    expect_refusal("zero-warmup", result, "enrolls each case")

    # Malformed and out-of-range numeric inputs refuse rather than
    # wrapping into a small allocation the oracle would read past.
    for argument, value, needle in (
            ("--size", "-4", "dword multiple"),
            ("--size", "18446744073709551615", "dword multiple"),
            ("--reps", "0", "--reps"),
            ("--reps", "99999999999999999999999", "--reps"),
            ("--wait-bound-ns", "0", "--wait-bound-ns"),
            ("--inject-delay-ns", "2000000000", "--inject-delay-ns")):
        result = subprocess.run(
            [crossover, argument, value, "--size", "256"],
            env=harness.environment(), capture_output=True, text=True,
            timeout=600)
        if result.returncode != REFUSED:
            fail(f"input {argument}={value}: expected {REFUSED}, got "
                 f"{result.returncode}\n{result.stderr}")
        if needle not in result.stderr:
            fail(f"input {argument}={value}: the refusal does not name "
                 f"{needle!r}\n{result.stderr}")

    # One interval named twice refuses, so a case index never names two
    # declarations.
    result = subprocess.run(
        [crossover, "--size", "256", "--size", "256"],
        env=harness.environment(), capture_output=True, text=True,
        timeout=600)
    if result.returncode != REFUSED or "twice" not in result.stderr:
        fail(f"duplicate-size: expected a refusal naming the repeat\n"
             f"{result.stderr}")


if __name__ == "__main__":
    main()
