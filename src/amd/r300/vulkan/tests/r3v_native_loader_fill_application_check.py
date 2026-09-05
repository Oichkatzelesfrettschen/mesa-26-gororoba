# SPDX-License-Identifier: MIT
#
# Loader-path qualification of the public RB2D fill route under the radeon
# drm-shim.  The application links libvulkan and libc alone; the shim
# presents the declared Dell Vostro 1000 RS485M board and a fixture module
# srcversion, absorbs the one DRM_RADEON_CS, and counts it.
#
# Legs, each in a fresh evidence directory:
#
#   closed      the ordinary state: gate closed, the route declines,
#               GPU_ONLY refuses the submit, the shim sees no CS
#   armed       the full declaration: the route reaches vkQueueSubmit,
#               the shim sees exactly one CS, the fence signals, the
#               destination is untouched, the retained submit object is
#               byte-identical to the runner's independent emission, and
#               the token lands
#   refusals    every declared fact the public API can vary, wrong one at
#               a time, refused before the shim's CS handler with the
#               directory unspent
#   host bad    the same record on the host path over the read-only
#               mapping faults, proving the protection judges stores
#   host good   the host path over a writable mapping fills exactly the
#               interval, proving the sweep judges bytes
#
# The destination buffer object's kernel name enters the submission
# identity.  The application allocates one VkDeviceMemory before it
# submits and the shim names objects from 1 in allocation order, so the
# destination is object 1; the wrong-destination arm declares object 2.
#
# The wrong-write-domain, wrong-relocation-site, and wrong-pitch arms are
# not reachable from the public API -- the route owns those fields -- and
# r3v-native-fill-route mutates them in process.
#
# What the public API shows of a refusal is one bit: the common queue
# layer marks the queue lost and returns VK_ERROR_DEVICE_LOST for any
# failure the driver's submit reports, so every arm below observes that
# result, an unmoved shim counter, and an unspent directory.  Each arm is
# therefore a one-variable experiment against the armed leg -- the same
# process shape, one declared fact changed -- which proves the fact is
# necessary for admission; which gate refused it is named in process by
# r3v-native-fill-route and r3v-native-rb2d-fill-arming-runner.

import json
import os
import platform
import re
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r3v_native_rb2d_fill_mutation_table as table  # noqa: E402

FIXTURE_SRCVERSION = "FIXTURESRCVERSION0000000"
SPECIMEN_SUBSYSTEM = "1028:022a"
SPECIMEN_DMI = "Vostro   1000 "
DESTINATION_HANDLE = 1
CELL_ALLOCATION_BYTES = 65536
COMPLETION_BYTES = 4
GTT = 2
RETAINED = ("ib.bin", "relocs.bin", "manifest.json", "submit_relocs.bin",
            "submit_manifest.json", "attempt.token")


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def field(text, key):
    match = re.search(rf"^{re.escape(key)}=(\S+)", text, re.MULTILINE)
    return match.group(1) if match else None


def runner_report(runner, sysfs, evidence, handle, emit=None, cell=None):
    env = dict(os.environ)
    env["R3V_NATIVE_RUNNER_SYSFS_ROOT"] = sysfs
    env["R3V_NATIVE_RUNNER_DESTINATION_HANDLE"] = str(handle)
    extra = ["--emit-ib", emit] if emit else []
    if cell:
        extra += ["--cell", cell]
    result = subprocess.run([runner, *extra, evidence], env=env,
                            capture_output=True, text=True)
    digest = field(result.stdout, "ib_blake3")
    identity = field(result.stdout, "fill_identity_blake3")
    dwords = field(result.stdout, "ib_dwords")
    sites = field(result.stdout, "relocation_site_count")
    if not digest or not identity or not dwords or not sites:
        fail(f"runner report incomplete:\n{result.stdout}")
    return digest, identity, int(dwords), int(sites)


class Leg:
    def __init__(self, application, work):
        self.application = application
        self.work = work
        self.count = 0

    def run(self, label, expect, *, evidence=None, protect=True,
            declare=None, shim=None, policy="gpu_only", gate="1",
            extra=None, plan=None, cell=None, v2_gate=None, pinned=None,
            expected=None, qualification=None):
        self.count += 1
        if evidence is None:
            evidence = os.path.join(self.work, f"evidence-{self.count}")
            os.mkdir(evidence)
        env = dict(os.environ)
        for key in list(env):
            if key.startswith("R3V_NATIVE_") or key.startswith(
                    "R3V_DRM_SHIM_") or key.startswith("R3V_LOADER_"):
                del env[key]
        env["R3V_DRM_SHIM_SUBSYSTEM_ID"] = SPECIMEN_SUBSYSTEM
        env["R3V_DRM_SHIM_DMI_PRODUCT_NAME"] = SPECIMEN_DMI
        env["R3V_DRM_SHIM_MODULE_SRCVERSION"] = FIXTURE_SRCVERSION
        if shim:
            env.update(shim)
        env["R3V_NATIVE_MANIFEST_DIR"] = evidence
        if policy:
            env["R3V_NATIVE_EXECUTION_POLICY"] = policy
        if gate:
            env["R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL"] = gate
        if v2_gate:
            env["R3V_NATIVE_ROUTE_RB2D_CONST_FILL_V2_EXPERIMENTAL"] = v2_gate
        if pinned:
            env["R3V_NATIVE_RB2D_V2_PINNED_PITCH_BYTES"] = pinned
        if expected:
            env["R3V_NATIVE_RB2D_V2_EXPECTED_PITCH_BYTES"] = expected
        if qualification:
            env["R3V_NATIVE_RB2D_CARRIER_QUALIFICATION_PITCH_BYTES"] = \
                qualification
        if declare:
            env["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
            env.update(declare)
        if plan:
            env["R3V_NATIVE_PLAN_CAPTURE_FILE"] = plan
        env["R3V_LOADER_FILL_EXPECT"] = expect
        env["R3V_LOADER_FILL_PROTECT"] = "1" if protect else "0"
        if extra:
            env.update(extra)
        argv = [self.application]
        if cell:
            argv += ["--cell", cell]
        result = subprocess.run(argv, env=env, capture_output=True,
                                text=True)
        present = sorted(name for name in RETAINED
                         if os.path.exists(os.path.join(evidence, name))) \
            if os.path.isdir(evidence) else []
        print(f"{label}: status {result.returncode}, "
              f"submit {field(result.stdout, 'submit_result')}, "
              f"retained {present}")
        return result, evidence, present


def require_refused(label, result, present):
    if result.returncode != 0:
        fail(f"{label}: application status {result.returncode}\n"
             f"{result.stdout}\n{result.stderr}")
    if field(result.stdout, "submit_result") != "VK_ERROR_DEVICE_LOST":
        fail(f"{label}: the loader saw {field(result.stdout, 'submit_result')}"
             f" rather than the queue-lost refusal")
    if field(result.stdout, "shim_cs_ioctls") != "0":
        fail(f"{label}: the shim observed a CS")
    if present:
        fail(f"{label}: the refused submit retained {present}")


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <loader-fill-application> "
              f"<rb2d-fill-arming-runner>", file=sys.stderr)
        return 2
    application, runner = sys.argv[1], sys.argv[2]
    kernel = platform.release()

    with tempfile.TemporaryDirectory() as work:
        # The runner's sysfs fixture carries only what the identity reads.
        sysfs = os.path.join(work, "sys")
        os.makedirs(os.path.join(sysfs, "module", "radeon"))
        with open(os.path.join(sysfs, "module", "radeon", "srcversion"),
                  "w", encoding="ascii") as f:
            f.write(FIXTURE_SRCVERSION + "\n")
        reference = os.path.join(work, "reference-ib.bin")
        scratch = os.path.join(work, "runner-scratch")
        os.mkdir(scratch)
        digest, identity, dwords, site_count = runner_report(
            runner, sysfs, scratch, DESTINATION_HANDLE, emit=reference)
        _, wrong_identity, _, _ = runner_report(
            runner, sysfs, scratch, DESTINATION_HANDLE + 1)
        if wrong_identity == identity:
            fail("the destination handle does not enter the identity")
        with open(reference, "rb") as f:
            reference_bytes = f.read()
        if len(reference_bytes) != dwords * 4:
            fail("the runner's emitted IB disagrees with its dword count")

        declaration = {
            "R3V_NATIVE_AUTHORIZED_IB_BLAKE3": digest,
            "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE": kernel,
            "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION": FIXTURE_SRCVERSION,
            "R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3": identity,
        }
        leg = Leg(application, work)

        # The ordinary state.
        result, _, present = leg.run("closed gate", "refused")
        require_refused("closed gate", result, present)

        # The armed loader-path submission.  Plan capture stays off: the
        # device refuses a capture session while the hazard gate is open,
        # and this route runs only with it open, so the cell kind of this
        # submission is not observable through a capture file.
        result, evidence, present = leg.run("armed", "submitted",
                                            declare=declaration)
        if result.returncode != 0:
            fail(f"armed: application status {result.returncode}\n"
                 f"{result.stdout}\n{result.stderr}")
        if field(result.stdout, "shim_cs_ioctls") != "1":
            fail("armed: the shim did not observe exactly one CS")
        if present != sorted(RETAINED):
            fail(f"armed: retained {present}, expected every name")
        with open(os.path.join(evidence, "ib.bin"), "rb") as f:
            retained_ib = f.read()
        if retained_ib != reference_bytes:
            fail(f"armed: retained ib.bin ({len(retained_ib)} bytes) differs "
                 f"from the runner's emission ({len(reference_bytes)})")
        with open(os.path.join(evidence, "manifest.json"),
                  encoding="utf-8") as f:
            manifest = json.load(f)
        if manifest.get("ib_blake3") != digest or \
                manifest.get("ib_dwords") != dwords:
            fail("armed: manifest.json digest or dword count differs from "
                 "the runner")
        with open(os.path.join(evidence, "submit_manifest.json"),
                  encoding="utf-8") as f:
            submit = json.load(f)
        if submit.get("ib_blake3") != digest:
            fail("armed: submit_manifest.json binds a different IB")
        rows = submit.get("bo_table")
        entries = os.path.getsize(
            os.path.join(evidence, "submit_relocs.bin")) // 16
        counts = {
            "relocation_site_count": site_count,
            "relocation_entry_count": entries,
            "bo_reference_count": len(rows) if isinstance(rows, list) else 0,
        }
        print(f"armed: {counts}")
        if counts != {"relocation_site_count": 1,
                      "relocation_entry_count": 2,
                      "bo_reference_count": 2}:
            fail(f"armed: relocation counts {counts} differ from one site, "
                 f"two entries, two references")
        destination, completion = rows
        if destination.get("reloc_index") != 0 or \
                destination.get("size") != CELL_ALLOCATION_BYTES or \
                destination.get("read_domains") != 0 or \
                destination.get("write_domain") != GTT or \
                destination.get("handle") != DESTINATION_HANDLE:
            fail(f"armed: destination row {destination} differs from the "
                 f"contract")
        if completion.get("reloc_index") != 1 or \
                completion.get("size") != COMPLETION_BYTES or \
                completion.get("read_domains") != 0 or \
                completion.get("write_domain") != GTT or \
                completion.get("role") != "completion":
            fail(f"armed: completion row {completion} differs from the "
                 f"contract")
        # A second submission into the spent directory refuses.
        result, _, present = leg.run("spent directory", "refused",
                                     evidence=evidence, declare=declaration)
        if result.returncode != 0 or \
                field(result.stdout, "shim_cs_ioctls") != "0":
            fail("spent directory: the second submission was admitted")

        # Every mutation the canonical table names, the public leg: the
        # same descriptor the in-process runner check refuses by name is
        # driven here through the loader, and each observes the public
        # VK_ERROR_DEVICE_LOST, a shim CS count of 0, and an unspent
        # directory.
        symbols = {
            "@stale_digest": table.stale(digest),
            "@stale_identity": table.stale(identity),
            "@wrong_identity": wrong_identity,
        }
        for mutation_id, _field, _runner, declare_change, extra, shim, \
                marker in table.MUTATIONS:
            declare = dict(declaration)
            gate = "1"
            for key, value in declare_change.items():
                if key == table.HAZARD_GATE and value == "@unset":
                    gate = None
                    continue
                resolved = table.resolve(value, symbols)
                if resolved is None:
                    declare.pop(key, None)
                else:
                    declare[key] = resolved
            if gate is None:
                result, _, present = leg.run(mutation_id, "refused",
                                             extra=extra or None,
                                             shim=shim or None)
            else:
                result, _, present = leg.run(mutation_id, "refused",
                                             declare=declare,
                                             extra=extra or None,
                                             shim=shim or None)
            require_refused(mutation_id, result, present)
            print(f"  {mutation_id}: public {table.PUBLIC_RESULT}, shim CS "
                  f"{table.SHIM_CS_COUNT}, directory {table.DIRECTORY_STATE}; "
                  f"internal refusal '{marker}' per the in-process leg")

        # The evidence directory rows: absent, and spent by a token alone.
        for mutation_id, _field, marker in table.DIRECTORY_MUTATIONS:
            if mutation_id == "absent_directory":
                target = os.path.join(work, "no-such-evidence")
                result, _, present = leg.run(mutation_id, "refused",
                                             evidence=target,
                                             declare=declaration)
                require_refused(mutation_id, result, present)
            else:
                target = os.path.join(work, "spent-by-token")
                os.mkdir(target)
                open(os.path.join(target, "attempt.token"), "w").close()
                result, _, present = leg.run(mutation_id, "refused",
                                             evidence=target,
                                             declare=declaration)
                if result.returncode != 0 or \
                        field(result.stdout, "shim_cs_ioctls") != "0" or \
                        present != ["attempt.token"]:
                    fail("spent token: the submission was admitted or the "
                         "directory changed")

        # A wrong ICD refuses ahead of any recording.
        result, _, present = leg.run(
            "wrong ICD DSO", "submitted", declare=declaration,
            extra={"R3V_EXPECTED_ICD_DSO": "/no/such/libvulkan_r3v.so"})
        if result.returncode != 2 or present or \
                "the loader did not map" not in result.stderr:
            fail("wrong ICD DSO: the application did not refuse on the "
                 "mapped DSO before recording")

        # The windowed cell through the loader.  The route's own gate
        # opens with the receipted route's closed beside it, the carrier
        # is pinned at the cell's 256 bytes, and the retained submit
        # object carries the two-window stream: 58 dwords over two
        # relocation sites against the 2 MiB allocation.
        v2_scratch = os.path.join(work, "runner-scratch-v2")
        os.mkdir(v2_scratch)
        v2_digest, v2_identity, v2_dwords, v2_sites = runner_report(
            runner, sysfs, v2_scratch, DESTINATION_HANDLE,
            cell="v2_multiwindow_256")
        if (v2_dwords, v2_sites) != (58, 2):
            fail(f"the windowed cell emits {v2_dwords} dwords over "
                 f"{v2_sites} sites, not 58 over 2")
        v2_declaration = {
            **declaration,
            "R3V_NATIVE_AUTHORIZED_IB_BLAKE3": v2_digest,
            "R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3": v2_identity,
        }
        v2_arms = dict(cell="v2_multiwindow_256", gate=None, v2_gate="1",
                       pinned="256")
        result, evidence, present = leg.run("v2 armed", "submitted",
                                            declare=v2_declaration,
                                            **v2_arms)
        if result.returncode != 0:
            fail(f"v2 armed: application status {result.returncode}\n"
                 f"{result.stdout}\n{result.stderr}")
        if field(result.stdout, "shim_cs_ioctls") != "1":
            fail("v2 armed: the shim did not observe exactly one CS")
        if field(result.stdout, "cell") != "v2_multiwindow_256" or \
                field(result.stdout, "allocation_bytes") != "2097152":
            fail(f"v2 armed: the application ran cell "
                 f"{field(result.stdout, 'cell')} over allocation "
                 f"{field(result.stdout, 'allocation_bytes')}")
        with open(os.path.join(evidence, "submit_manifest.json"),
                  encoding="utf-8") as f:
            v2_submit = json.load(f)
        if v2_submit.get("ib_blake3") != v2_digest or \
                v2_submit.get("ib_dwords") != 58:
            fail(f"v2 armed: submit_manifest.json binds "
                 f"{v2_submit.get('ib_dwords')} dwords under "
                 f"{v2_submit.get('ib_blake3')}")
        v2_rows = v2_submit.get("bo_table")
        v2_entries = os.path.getsize(
            os.path.join(evidence, "submit_relocs.bin")) // 16
        # Two windows bind the destination twice, but both sites name one
        # buffer object and the winsys merges a handle into one relocation
        # entry, so the submission carries two entries over two objects --
        # the destination and the completion -- exactly as the sealed
        # cell's one-window stream does.  The site count is the stream's
        # and the entry count is the submission's, and they part company
        # the moment a stream rebases the same object twice.
        if v2_sites != 2 or v2_entries != 2 or len(v2_rows) != 2:
            fail(f"v2 armed: {v2_sites} sites, {v2_entries} relocation "
                 f"entries, {len(v2_rows)} references")
        if v2_rows[0].get("size") != 2097152 or \
                v2_rows[0].get("write_domain") != GTT or \
                v2_rows[0].get("read_domains") != 0:
            fail(f"v2 armed: destination row {v2_rows[0]} differs from the "
                 f"windowed cell's 2 MiB GTT destination")

        # The windowed cell's refusals, one declared fact at a time.
        result, _, present = leg.run(
            "v2 wrong pinned pitch", "refused", declare=v2_declaration,
            **{**v2_arms, "pinned": "64"})
        require_refused("v2 wrong pinned pitch", result, present)
        result, _, present = leg.run(
            "v2 gate absent", "refused", declare=v2_declaration,
            **{**v2_arms, "v2_gate": None})
        require_refused("v2 gate absent", result, present)

        # Two refusals the device makes at creation rather than at the
        # route: a qualification carrier declared without the windowed
        # route's gate, and both RB2D gates open naming two executors for
        # one destination.  vkCreateDevice fails, so the application stops
        # at its own fixture check ahead of any recording.
        for label, arms in (
                ("v2 qualification pin without the gate",
                 {**v2_arms, "v2_gate": None, "qualification": "256"}),
                ("both RB2D gates open",
                 {**v2_arms, "gate": "1"}),
        ):
            result, _, present = leg.run(label, "refused",
                                         declare=v2_declaration, **arms)
            if result.returncode != 2 or \
                    "vkCreateDevice" not in result.stderr:
                fail(f"{label}: status {result.returncode}, device creation "
                     f"was expected to fail\n{result.stdout}\n"
                     f"{result.stderr}")
            if present:
                fail(f"{label}: the refused run retained {present}")

        # The chooser cell through the loader.  Its interval is the
        # windowed cell's, so the pin is the only field that moves: with
        # it withdrawn the cost model returns the 16320-byte carrier, the
        # stream collapses to one window over one relocation site, and the
        # declared expectation asserts that verdict.
        ch_scratch = os.path.join(work, "runner-scratch-chooser")
        os.mkdir(ch_scratch)
        ch_digest, ch_identity, ch_dwords, ch_sites = runner_report(
            runner, sysfs, ch_scratch, DESTINATION_HANDLE,
            cell="v2_chooser_16320")
        if (ch_dwords, ch_sites) != (38, 1):
            fail(f"the chooser cell emits {ch_dwords} dwords over "
                 f"{ch_sites} sites, not 38 over 1")
        ch_declaration = {
            **declaration,
            "R3V_NATIVE_AUTHORIZED_IB_BLAKE3": ch_digest,
            "R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3": ch_identity,
        }
        ch_arms = dict(cell="v2_chooser_16320", gate=None, v2_gate="1",
                       expected="16320")
        result, evidence, present = leg.run("chooser armed", "submitted",
                                            declare=ch_declaration,
                                            **ch_arms)
        if result.returncode != 0:
            fail(f"chooser armed: application status {result.returncode}\n"
                 f"{result.stdout}\n{result.stderr}")
        if field(result.stdout, "shim_cs_ioctls") != "1":
            fail("chooser armed: the shim did not observe exactly one CS")
        if field(result.stdout, "expected_pitch") != "16320":
            fail(f"chooser armed: the application ran expected pitch "
                 f"{field(result.stdout, 'expected_pitch')}")
        with open(os.path.join(evidence, "submit_manifest.json"),
                  encoding="utf-8") as f:
            ch_submit = json.load(f)
        if ch_submit.get("ib_blake3") != ch_digest or \
                ch_submit.get("ib_dwords") != 38:
            fail(f"chooser armed: submit_manifest.json binds "
                 f"{ch_submit.get('ib_dwords')} dwords under "
                 f"{ch_submit.get('ib_blake3')}")
        with open(os.path.join(evidence, "ib.bin"), "rb") as f:
            chooser_ib = f.read()

        # The chooser cell's refusals.  A declared expectation of 256
        # names the carrier the cost model declined, and the route
        # declines the chosen carrier; a pin at 256 beside it moves the
        # lowering to two windows, which no longer matches the authorized
        # stream.
        result, _, present = leg.run(
            "chooser expects 256", "refused", declare=ch_declaration,
            **{**ch_arms, "expected": "256"})
        require_refused("chooser expects 256", result, present)
        result, _, present = leg.run(
            "chooser pinned 256", "refused", declare=ch_declaration,
            **{**ch_arms, "pinned": "256"})
        require_refused("chooser pinned 256", result, present)

        # The qualification declaration beside the chooser cell.  The
        # route reaches the qualification floor only through a pinned
        # carrier, so with the pin withdrawn the declaration decides
        # nothing: the device admits it because the windowed gate is
        # open, and the submission is the byte-identical armed stream.
        # The attended application is where a route receipt refuses this
        # declaration, by name and ahead of vkCreateInstance.
        result, evidence, present = leg.run(
            "chooser qualification pin inert", "submitted",
            declare=ch_declaration,
            **{**ch_arms, "qualification": "16320"})
        if result.returncode != 0 or \
                field(result.stdout, "shim_cs_ioctls") != "1":
            fail(f"chooser qualification pin: status {result.returncode}\n"
                 f"{result.stdout}\n{result.stderr}")
        with open(os.path.join(evidence, "ib.bin"), "rb") as f:
            if f.read() != chooser_ib:
                fail("chooser qualification pin: the retained stream "
                     "differs from the armed chooser stream")

        # The host path over the protected mapping faults: the protection
        # judges stores.  The same path over a writable mapping fills the
        # interval and nothing else: the sweep judges bytes.
        result, _, present = leg.run("host known-bad", "host-filled",
                                     policy="auto", gate=None)
        if result.returncode != -signal.SIGSEGV:
            fail(f"host known-bad: status {result.returncode}, expected "
                 f"termination by SIGSEGV\n{result.stdout}\n{result.stderr}")
        result, _, present = leg.run("host control", "host-filled",
                                     policy="auto", gate=None, protect=False)
        if result.returncode != 0 or \
                field(result.stdout, "shim_cs_ioctls") != "0":
            fail(f"host control: status {result.returncode}\n"
                 f"{result.stdout}\n{result.stderr}")

    print("r3v_native_loader_fill_application_check: the sealed, the "
          "windowed, and the chooser cell each reach the shim once under "
          "their own full declaration; every refusal, both "
          "device-creation refusals, and both host controls hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
