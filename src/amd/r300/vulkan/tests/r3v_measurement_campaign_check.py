# SPDX-License-Identifier: MIT
#
# The declared measurement campaign through the real loader and queue path
# under the radeon drm-shim.  The application links libvulkan and libc
# alone; the shim presents the declared Dell Vostro 1000 RS485M board and a
# fixture module srcversion, absorbs each DRM_RADEON_CS, and counts it.
#
# The rows below are the integration matrix.  Each is a one-variable
# experiment against the campaign row: the same declaration, the same
# process shape, one fact changed.
#
#   campaign            every declared case, its declared count of times,
#                       one CS apiece; the claim lands once; each case
#                       retains one complete transport of its own
#   excess              the declared budget, then one more request, which
#                       reaches no CS
#   undeclared          a fill outside every declared case
#   mixed               two fills in one command buffer
#   rebound             the same declared fill over a replacement
#                       allocation
#   second-device       a second process against the claimed arm
#   other-output        the claimed arm with another evidence directory
#   claim-write-fails   a campaign root that admits no entry
#   ioctl-fails         the shim refusing DRM_RADEON_CS
#   completion-fails    the shim failing the completion wait
#   oracle-stop         a mismatch the application introduces after the
#                       first completion, and no further submission
#
# A refusal is judged by its exact expected verdict.  A crashed runner or a
# missing artifact satisfies none of them: every row requires the
# application's own status and its printed counters.

import os
import platform
import re
import subprocess
import sys
import tempfile

FIXTURE_SRCVERSION = "FIXTURESRCVERSION0000000"
SPECIMEN_SUBSYSTEM = "1028:022a"
SPECIMEN_DMI = "Vostro   1000 "
SPECIMEN_PLATFORM = "vostro1000_rs485m_5974"
ROUTE = "rb2d_const_fill"
ALLOCATION_BYTES = 65536
# Two declared cases over one allocation: a 256-byte interval the V1
# carrier covers in one window, and a second interval of another size at
# another offset, so "another declared case" is a real second stream.
CASES = (
    # case_id, offset, bytes, value, warmups, repetitions.  Both intervals
    # carry the qualified V1 shape at two offsets and two values, so the
    # second case is a second stream with a second identity rather than a
    # relabeling of the first.
    (0, 12, 4992, 0x11223344, 1, 2),
    (1, 8192, 4992, 0x55667788, 1, 1),
)
TOTAL = sum(c[4] + c[5] for c in CASES)


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def field(text, key):
    match = re.search(rf"^{re.escape(key)}=(\S+)", text, re.MULTILINE)
    return match.group(1) if match else None


def declaration_text(nonce, kernel, *, route=ROUTE, platform_name=None,
                     cases=None):
    lines = [
        "schema = r3v-measurement-session-v1",
        f"session_nonce = {nonce}",
        f"platform = {platform_name or SPECIMEN_PLATFORM}",
        f"route = {route}",
        "pci_vendor_id = 0x1002",
        "pci_device_id = 0x5974",
        f"kernel_release = {kernel}",
        f"module_srcversion = {FIXTURE_SRCVERSION}",
        f"allocation_bytes = {ALLOCATION_BYTES}",
        f"buffer_bytes = {ALLOCATION_BYTES}",
        "binding_offset = 0",
        # Memory type 0: host-visible, coherent, cached, device-local.
        "memory_property_flags = 0xf",
        # VK_BUFFER_USAGE_TRANSFER_DST_BIT, and RADEON_GEM_DOMAIN_GTT.
        "buffer_usage = 0x2",
        "write_domain = 0x2",
        f"max_total_submissions = "
        f"{sum(c[4] + c[5] for c in (cases or CASES))}",
        "completion_timeout_ns = 30000000000",
    ]
    for case_id, offset, size, value, warmups, repetitions in (cases or
                                                              CASES):
        lines.append(f"case = {case_id}, {offset}, {size}, 0x{value:08x}, "
                     f"{warmups}, {repetitions}")
    return "\n".join(lines) + "\n"


class Campaign:
    def __init__(self, application, work):
        self.application = application
        self.work = work
        self.count = 0
        self.kernel = platform.release()

    def environment(self, declaration, evidence, policy, gate, *,
                    cases=None, excess=False, shim=None, extra=None):
        env = dict(os.environ)
        for key in list(env):
            if key.startswith(("R3V_NATIVE_", "R3V_DRM_SHIM_",
                               "R3V_LOADER_", "R3V_CAMPAIGN_")):
                del env[key]
        env["R3V_DRM_SHIM_SUBSYSTEM_ID"] = SPECIMEN_SUBSYSTEM
        env["R3V_DRM_SHIM_DMI_PRODUCT_NAME"] = SPECIMEN_DMI
        env["R3V_DRM_SHIM_MODULE_SRCVERSION"] = FIXTURE_SRCVERSION
        if shim:
            env.update(shim)
        env["R3V_NATIVE_MANIFEST_DIR"] = evidence
        env["R3V_NATIVE_EXECUTION_POLICY"] = policy
        if gate:
            env["R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL"] = gate
        env["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
        env["R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE"] = self.kernel
        env["R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION"] = FIXTURE_SRCVERSION
        if declaration:
            env["R3V_NATIVE_MEASUREMENT_DECLARATION"] = declaration
        env["R3V_CAMPAIGN_ALLOCATION_BYTES"] = str(ALLOCATION_BYTES)
        if cases is None:
            cases = ",".join(
                f"{offset}:{size}:0x{value:08x}:{warmups + repetitions}"
                for _, offset, size, value, warmups, repetitions in CASES)
        env["R3V_CAMPAIGN_CASES"] = cases
        if excess:
            env["R3V_CAMPAIGN_EXCESS"] = "1"
        if extra:
            env.update(extra)
        return env

    def run(self, label, *, arm="repetitions", declaration=None,
            evidence=None, cases=None, excess=False, shim=None,
            gate="1", policy="gpu_only", extra=None):
        self.count += 1
        if evidence is None:
            evidence = os.path.join(self.work, f"evidence-{self.count}")
            os.makedirs(evidence, exist_ok=True)
        env = self.environment(declaration, evidence, policy, gate,
                               cases=cases, excess=excess, shim=shim,
                               extra=extra)
        result = subprocess.run([self.application, "--arm", arm], env=env,
                                capture_output=True, text=True)
        print(f"{label}: status {result.returncode}, "
              f"samples {field(result.stdout, 'samples')}, "
              f"shim_cs_total {field(result.stdout, 'shim_cs_total')}")
        return result, evidence


    def race(self, label, declaration, processes):
        """Starts `processes` campaigns against one arm at once and returns
        how many completed successfully.  The exclusive creation is the
        winner-selection operation, so exactly one is expected."""
        started = []
        for _ in range(processes):
            self.count += 1
            evidence = os.path.join(self.work, f"evidence-{self.count}")
            os.makedirs(evidence, exist_ok=True)
            env = self.environment(declaration, evidence, "gpu_only", "1")
            started.append(subprocess.Popen(
                [self.application, "--arm", "repetitions"], env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True))
        codes = [p.wait() for p in started]
        print(f"{label}: exit codes {codes}")
        return sum(1 for code in codes if code == 0)


def write_declaration(work, name, nonce, kernel, **kwargs):
    path = os.path.join(work, name)
    with open(path, "w", encoding="ascii") as f:
        f.write(declaration_text(nonce, kernel, **kwargs))
    return path


def require_pass(label, result):
    if result.returncode != 0:
        fail(f"{label}: status {result.returncode}\n{result.stdout}\n"
             f"{result.stderr}")


def require_no_cs(label, result):
    total = field(result.stdout, "shim_cs_total")
    if total is None:
        fail(f"{label}: the application printed no shim counter\n"
             f"{result.stdout}\n{result.stderr}")
    if total != "0":
        fail(f"{label}: the refused run reached the shim {total} times")


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <campaign-application>",
              file=sys.stderr)
        return 2
    application = sys.argv[1]

    with tempfile.TemporaryDirectory() as work:
        campaign = Campaign(application, work)
        kernel = campaign.kernel

        # Row 1 and row 2: the whole declared campaign.  Every case
        # executes its declared count, each repetition reaches exactly one
        # CS, and the two cases bind independently.
        root = os.path.join(work, "campaign-a")
        os.makedirs(root)
        declaration = write_declaration(root, "declaration.txt", "arm_a",
                                        kernel)
        result, evidence = campaign.run("campaign", declaration=declaration)
        require_pass("campaign", result)
        if field(result.stdout, "samples") != str(TOTAL):
            fail(f"campaign: {field(result.stdout, 'samples')} samples, "
                 f"expected {TOTAL}")
        if field(result.stdout, "shim_cs_total") != str(TOTAL):
            fail("campaign: the shim did not observe one CS per repetition")
        if field(result.stdout, "oracle_passes") != str(TOTAL):
            fail("campaign: an oracle check failed on a routed repetition")
        claim = os.path.join(root, f"measurement-claim-arm_a-{ROUTE}.token")
        if not os.path.exists(claim):
            fail("campaign: the declared arm left no durable claim")
        with open(claim, encoding="ascii") as f:
            claim_text = f.read()
        for key in ("session_nonce: arm_a", f"arm: {ROUTE}",
                    f"platform: {SPECIMEN_PLATFORM}", "claim_scope:"):
            if key not in claim_text:
                fail(f"campaign: the claim record omits {key!r}")
        # Each declared case retained one complete transport of its own.
        for case_id, *_ in CASES:
            case_dir = os.path.join(evidence, f"case-{case_id}")
            for name in ("ib.bin", "manifest.json", "submit_manifest.json"):
                if not os.path.exists(os.path.join(case_dir, name)):
                    fail(f"campaign: case {case_id} retained no {name}")
        if os.path.exists(os.path.join(evidence, "ib.bin")):
            fail("campaign: a case retained into the shared directory")
        streams = []
        for case_id, *_ in CASES:
            with open(os.path.join(evidence, f"case-{case_id}", "ib.bin"),
                      "rb") as f:
                streams.append(f.read())
        if streams[0] == streams[1]:
            fail("campaign: the two declared cases built one stream, so the "
                 "second case relabels the first rather than measuring "
                 "another")

        # Row 3: the final allowance, then one excess request.
        root = os.path.join(work, "campaign-b")
        os.makedirs(root)
        declaration = write_declaration(root, "declaration.txt", "arm_b",
                                        kernel)
        result, _ = campaign.run("excess", declaration=declaration,
                                 excess=True)
        require_pass("excess", result)
        if field(result.stdout, "excess_result") == "VK_SUCCESS":
            fail("excess: the request past the budget was admitted")
        if field(result.stdout, "excess_shim_cs") != "0":
            fail("excess: the request past the budget reached the shim")
        if field(result.stdout, "shim_cs_total") != str(TOTAL):
            fail("excess: the shim count moved past the declared budget")

        # Row 4: undeclared work and mixed command-buffer work, under
        # both execution policies.  Under gpu_only a refusal is the
        # policy's own answer to work no GPU route performs; under auto
        # the host would perform it, so the declaration's refusal is the
        # only thing between the campaign and a host store, and the
        # application's oracle would see the bytes move.
        for arm in ("undeclared", "mixed"):
            for policy in ("gpu_only", "auto"):
                root = os.path.join(work, f"campaign-{arm}-{policy}")
                os.makedirs(root)
                declaration = write_declaration(
                    root, "declaration.txt", f"arm_{arm}_{policy}", kernel)
                result, _ = campaign.run(f"{arm}/{policy}", arm=arm,
                                         declaration=declaration,
                                         policy=policy)
                require_pass(f"{arm}/{policy}", result)
                require_no_cs(f"{arm}/{policy}", result)
                if field(result.stdout, "oracle_passes") != "1":
                    fail(f"{arm}/{policy}: the refused work moved the "
                         f"destination")

        # A declaration naming a route whose opt-in stands closed refuses
        # at device creation, where an operator can act on it, rather than
        # once per command.
        root = os.path.join(work, "campaign-gate-closed")
        os.makedirs(root)
        declaration = write_declaration(root, "declaration.txt",
                                        "arm_gate", kernel)
        result, _ = campaign.run("gate-closed-at-load",
                                 declaration=declaration, gate=None)
        if result.returncode == 0:
            fail("gate-closed-at-load: the declaration opened over a closed "
                 "opt-in")
        if field(result.stdout, "create_device") != \
                "VK_ERROR_INITIALIZATION_FAILED":
            fail("gate-closed-at-load: the device did not refuse the "
                 "declaration")

        # A declaration this deployment does not arm.  Every ordinary
        # refusal inside the route is a decline, and a decline returns
        # VK_SUCCESS under any policy but gpu_only: the host would perform
        # the fill, the submit would report success, no allowance would be
        # spent, no claim would be taken, and the campaign would publish
        # host stores as routed samples.  The declaration here names the
        # running deployment, so it loads; the arming gate's own authorized
        # kernel release names another, so the route declines.  A declared
        # campaign refuses instead, under both policies, and the
        # destination stays as the application wrote it.
        for policy in ("auto", "gpu_only"):
            root = os.path.join(work, f"campaign-arming-{policy}")
            os.makedirs(root)
            declaration = write_declaration(
                root, "declaration.txt", f"arm_arming_{policy}", kernel)
            result, _ = campaign.run(
                f"arming-mismatch/{policy}", declaration=declaration,
                policy=policy,
                extra={"R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE":
                       "0.0.0-fixture"})
            if result.returncode == 0:
                fail(f"arming-mismatch/{policy}: the declared campaign "
                     f"admitted an unarmed deployment")
            require_no_cs(f"arming-mismatch/{policy}", result)
            if field(result.stdout, "oracle_passes") != "1":
                fail(f"arming-mismatch/{policy}: the host performed the "
                     f"declared fill")
            # The control: the same declaration, the same policy, the same
            # process shape, the authorized kernel release correct.  The
            # public API collapses every driver refusal to
            # VK_ERROR_DEVICE_LOST and carries no reason, so the pairing is
            # what names the fact that refused.
            control_root = os.path.join(work, f"campaign-arming-ok-{policy}")
            os.makedirs(control_root)
            control = write_declaration(control_root, "declaration.txt",
                                        f"arm_arming_ok_{policy}", kernel)
            control_result, _ = campaign.run(
                f"arming-control/{policy}", declaration=control,
                policy=policy)
            require_pass(f"arming-control/{policy}", control_result)
            if field(control_result.stdout, "shim_cs_total") != str(TOTAL):
                fail(f"arming-control/{policy}: the control campaign did "
                     f"not execute, so the mismatch row varies more than "
                     f"the authorized kernel release")

        # Row 5: the destination replaced under a bound case.
        root = os.path.join(work, "campaign-rebound")
        os.makedirs(root)
        declaration = write_declaration(root, "declaration.txt",
                                        "arm_rebound", kernel)
        result, _ = campaign.run("rebound", arm="rebound",
                                 declaration=declaration)
        require_pass("rebound", result)
        if field(result.stdout, "shim_cs_total") != "1":
            fail("rebound: the replacement destination reached the shim")

        # Rows 7 and 8: a second process against the claimed arm, and the
        # same arm with another evidence directory.  Both find the claim
        # standing and neither replenishes the allowance.
        root = os.path.join(work, "campaign-a")
        declaration = os.path.join(root, "declaration.txt")
        result, _ = campaign.run("second-process", declaration=declaration)
        if result.returncode == 0:
            fail("second-process: the claimed arm ran a second campaign")
        require_no_cs("second-process", result)
        # The same rule under interleaving: four processes start together
        # against one unclaimed arm and exactly one wins the exclusive
        # creation.
        root = os.path.join(work, "campaign-race")
        os.makedirs(root)
        racing = write_declaration(root, "declaration.txt", "arm_race",
                                   kernel)
        winners = campaign.race("race", racing, 4)
        if winners != 1:
            fail(f"race: {winners} of four racing processes claimed the arm")
        other = os.path.join(work, "other-output")
        os.makedirs(other)
        result, _ = campaign.run("other-output", declaration=declaration,
                                 evidence=other)
        if result.returncode == 0:
            fail("other-output: another output directory replenished the arm")
        require_no_cs("other-output", result)

        # A submit-object retention that cannot write.  The manifest
        # retains first, so this is the window where a case's directory
        # already holds artifacts a retry could not write past: planting
        # one of the submit object's own names makes its freshness rule
        # refuse, and the campaign ends there rather than wedging the case.
        root = os.path.join(work, "campaign-submit-object")
        os.makedirs(root)
        declaration = write_declaration(root, "declaration.txt",
                                        "arm_submit_object", kernel)
        evidence = os.path.join(work, "evidence-submit-object")
        os.makedirs(os.path.join(evidence, "case-0"))
        with open(os.path.join(evidence, "case-0", "submit_manifest.json"),
                  "w", encoding="ascii") as f:
            f.write("{}\n")
        # The application keeps submitting, so the row reads what the
        # repetitions after the failure do.  The manifest retained before
        # the failure, so the case is marked retained and a later
        # repetition would skip both writers and reach the ioctl -- unless
        # the failure closed the campaign, which is the rule this row
        # names.
        result, _ = campaign.run(
            "submit-object-retention-fails", declaration=declaration,
            evidence=evidence,
            extra={"R3V_CAMPAIGN_CONTINUE_AFTER_FAILURE": "1"})
        if result.returncode == 0:
            fail("submit-object-retention-fails: the campaign ran with its "
                 "submit object unretained")
        if field(result.stdout, "samples") != str(TOTAL):
            fail("submit-object-retention-fails: the application stopped "
                 "early, so the row reads only the first refusal")
        require_no_cs("submit-object-retention-fails", result)
        if not os.path.exists(os.path.join(evidence, "case-0", "ib.bin")):
            fail("submit-object-retention-fails: the manifest did not "
                 "retain first, so the row tests another window")

        # Row 10: the claim cannot be written.  The campaign root admits no
        # entry, so nothing is submitted and the session terminates.
        root = os.path.join(work, "campaign-readonly")
        os.makedirs(root)
        declaration = write_declaration(root, "declaration.txt",
                                        "arm_readonly", kernel)
        os.chmod(root, 0o500)
        try:
            result, _ = campaign.run("claim-write-fails",
                                     declaration=declaration)
        finally:
            os.chmod(root, 0o700)
        if result.returncode == 0:
            fail("claim-write-fails: the campaign ran without a claim")
        require_no_cs("claim-write-fails", result)

        # Row 11: the transport and the completion each failing.  One
        # execution is consumed, the campaign terminates, and nothing
        # retries.
        for label, knob in (("ioctl-fails", "R3V_NATIVE_SHIM_CS_REFUSE"),
                            ("completion-fails",
                             "R3V_NATIVE_SHIM_COMPLETION_FAIL")):
            root = os.path.join(work, f"campaign-{label}")
            os.makedirs(root)
            declaration = write_declaration(root, "declaration.txt",
                                            f"arm_{label.replace('-', '_')}",
                                            kernel)
            result, _ = campaign.run(label, declaration=declaration,
                                     shim={knob: "1"})
            if result.returncode == 0:
                fail(f"{label}: the campaign reported success")
            claim = os.path.join(
                root,
                f"measurement-claim-arm_{label.replace('-', '_')}-"
                f"{ROUTE}.token")
            if not os.path.exists(claim):
                fail(f"{label}: the failed transport left no claim standing")

        # The application-owned oracle: a mismatch after the first
        # completion stops the campaign with no further submission.
        root = os.path.join(work, "campaign-oracle")
        os.makedirs(root)
        declaration = write_declaration(root, "declaration.txt",
                                        "arm_oracle", kernel)
        result, _ = campaign.run("oracle-stop", arm="oracle-stop",
                                 declaration=declaration)
        require_pass("oracle-stop", result)
        if field(result.stdout, "shim_cs_total") != "1":
            fail("oracle-stop: the campaign submitted past the mismatch")
        if field(result.stdout, "oracle_passes") != "0":
            fail("oracle-stop: the introduced mismatch passed the oracle")

        # Row 12: no declaration at all preserves the ordinary path.  The
        # one-shot authorization is undeclared here, so the route refuses
        # and reaches no CS -- the behavior an undeclared device had before
        # any of this existed.
        result, _ = campaign.run("no-declaration")
        if result.returncode == 0:
            fail("no-declaration: an undeclared fill was admitted")
        require_no_cs("no-declaration", result)

    print("r3v-measurement-campaign: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
