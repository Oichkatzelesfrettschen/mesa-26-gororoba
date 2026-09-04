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


def runner_report(runner, sysfs, evidence, handle, emit=None):
    env = dict(os.environ)
    env["R3V_NATIVE_RUNNER_SYSFS_ROOT"] = sysfs
    env["R3V_NATIVE_RUNNER_DESTINATION_HANDLE"] = str(handle)
    extra = ["--emit-ib", emit] if emit else []
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
            extra=None, plan=None):
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
        if declare:
            env["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
            env.update(declare)
        if plan:
            env["R3V_NATIVE_PLAN_CAPTURE_FILE"] = plan
        env["R3V_LOADER_FILL_EXPECT"] = expect
        env["R3V_LOADER_FILL_PROTECT"] = "1" if protect else "0"
        if extra:
            env.update(extra)
        result = subprocess.run([self.application], env=env,
                                capture_output=True, text=True)
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

    print("r3v_native_loader_fill_application_check: the loader-path RB2D "
          "fill reaches the shim once under the full declaration; every "
          "refusal and both host controls hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
