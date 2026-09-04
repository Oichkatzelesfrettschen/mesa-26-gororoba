# SPDX-License-Identifier: MIT
#
# Drives the non-submitting RB2D fill arming runner over a fixture sysfs
# tree and a fresh evidence directory: the positive ARMED leg under a full
# declaration, then every arming factor refused one at a time, each by its
# own named factor, and the direct-write runner's digest refused as the
# wrong cell.  Every leg requires the runner to leave the evidence
# directory exactly as it found it, because a runner that wrote a token
# would spend the attempt it exists to preview.

import os
import platform
import re
import subprocess
import sys
import tempfile

SPECIMEN = {
    "vendor": "0x1002\n",
    "device": "0x5974\n",
    "subsystem_vendor": "0x1028\n",
    "subsystem_device": "0x022a\n",
}
SPECIMEN_DMI = "Vostro   1000 \n"
SPECIMEN_SLOT = "0000:01:05.0"
FIXTURE_SRCVERSION = "FIXTURESRCVERSION0000000"
DESTINATION_HANDLE = "3"


def write_sysfs(root, pci=SPECIMEN, dmi=SPECIMEN_DMI,
                srcversion=FIXTURE_SRCVERSION):
    slot = os.path.join(root, "bus", "pci", "devices", SPECIMEN_SLOT)
    os.makedirs(slot, exist_ok=True)
    for leaf, contents in pci.items():
        with open(os.path.join(slot, leaf), "w", encoding="ascii") as f:
            f.write(contents)
    dmi_dir = os.path.join(root, "class", "dmi", "id")
    os.makedirs(dmi_dir, exist_ok=True)
    with open(os.path.join(dmi_dir, "product_name"), "w",
              encoding="ascii") as f:
        f.write(dmi)
    module_dir = os.path.join(root, "module", "radeon")
    os.makedirs(module_dir, exist_ok=True)
    with open(os.path.join(module_dir, "srcversion"), "w",
              encoding="ascii") as f:
        f.write(srcversion + "\n")


def run(runner, evidence_dir, environment, extra=()):
    before = sorted(os.listdir(evidence_dir)) if os.path.isdir(
        evidence_dir) else None
    result = subprocess.run([runner, *extra, evidence_dir], env=environment,
                            capture_output=True, text=True)
    after = sorted(os.listdir(evidence_dir)) if os.path.isdir(
        evidence_dir) else None
    if before != after:
        print(f"FAIL: the runner changed the evidence directory: "
              f"{before} -> {after}", file=sys.stderr)
        sys.exit(1)
    if "no submission attempted" not in result.stdout:
        print("FAIL: report omits the no-submission statement",
              file=sys.stderr)
        print(result.stdout, file=sys.stderr)
        sys.exit(1)
    return result


def field(stdout, key):
    match = re.search(rf"^{re.escape(key)}=(.*)$", stdout, re.MULTILINE)
    return match.group(1) if match else None


def expect_refusal(result, label, marker):
    if result.returncode == 0:
        print(f"FAIL: {label}: the runner reported ARMED", file=sys.stderr)
        print(result.stdout, file=sys.stderr)
        sys.exit(1)
    if marker not in result.stdout:
        print(f"FAIL: {label}: the report does not carry '{marker}'",
              file=sys.stderr)
        print(result.stdout, file=sys.stderr)
        sys.exit(1)


def main():
    if len(sys.argv) != 3:
        print("usage: r3v_native_rb2d_fill_arming_runner_check.py "
              "<rb2d-fill-runner> <direct-write-runner>", file=sys.stderr)
        return 2
    runner, direct_write_runner = sys.argv[1], sys.argv[2]

    base = dict(os.environ)
    for declaration in (
        "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED",
        "R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
        "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE",
        "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
        "R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3",
        "R3V_NATIVE_RUNNER_DESTINATION_HANDLE",
        "R3V_NATIVE_RUNNER_SYSFS_ROOT",
        "R3V_NATIVE_RUNNER_PCI_SLOT",
    ):
        base.pop(declaration, None)

    with tempfile.TemporaryDirectory() as work:
        sysfs = os.path.join(work, "sys")
        write_sysfs(sysfs)
        evidence = os.path.join(work, "evidence")
        os.mkdir(evidence)
        emitted = os.path.join(work, "reference-ib.bin")
        base["R3V_NATIVE_RUNNER_SYSFS_ROOT"] = sysfs

        # Undeclared: the report names the closed gate and carries every
        # identity line an operator builds an authorization from.
        undeclared = run(runner, evidence, base, ("--emit-ib", emitted))
        expect_refusal(undeclared, "undeclared", "hazard gate")
        if "CLOSED" not in undeclared.stdout:
            print("FAIL: undeclared run did not name the closed gate",
                  file=sys.stderr)
            return 1
        out = undeclared.stdout
        digest = field(out, "ib_blake3")
        dwords = field(out, "ib_dwords")
        if digest is None or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            print("FAIL: report carries no cell digest", file=sys.stderr)
            return 1
        if dwords is None or not dwords.isdigit() or int(dwords) == 0:
            print("FAIL: report carries no dword count", file=sys.stderr)
            return 1
        if os.path.getsize(emitted) != int(dwords) * 4:
            print("FAIL: emitted IB length differs from ib_dwords",
                  file=sys.stderr)
            return 1
        for key, value in (
            ("cell_kind", "rb2d_fill_public"),
            ("route", "rb2d_const_fill"),
            ("route_state", "precommitted"),
            ("fill_offset", "12"),
            ("fill_bytes", "4992"),
            ("fill_value", "0x11223344"),
            ("pitch_bytes", "256"),
            ("rect_count", "3"),
            ("platform", "DELL_VOSTRO1000_RS485M"),
            ("module_srcversion", FIXTURE_SRCVERSION),
            ("kernel_release", platform.release()),
            ("relocation_site_count", "1"),
            ("fill_identity_blake3", "(uncomputed)"),
        ):
            if field(out, key) != value:
                print(f"FAIL: {key}={field(out, key)!r}, expected {value!r}",
                      file=sys.stderr)
                print(out, file=sys.stderr)
                return 1
        rects = re.findall(r"^rect=(.*)$", out, re.MULTILINE)
        if rects != ["3,0,61,1", "0,1,64,18", "0,19,35,1"]:
            print(f"FAIL: rectangles {rects} differ from the attended cell",
                  file=sys.stderr)
            return 1
        source = field(out, "mesa_source")
        if source is None or "git-" not in source:
            print("FAIL: report carries no Mesa source identity",
                  file=sys.stderr)
            return 1

        # A declared handle computes the identity; a full declaration arms.
        base["R3V_NATIVE_RUNNER_DESTINATION_HANDLE"] = DESTINATION_HANDLE
        probe = run(runner, evidence, base)
        identity = field(probe.stdout, "fill_identity_blake3")
        if identity is None or re.fullmatch(r"[0-9a-f]{64}", identity) is None:
            print("FAIL: a declared handle computed no identity",
                  file=sys.stderr)
            print(probe.stdout, file=sys.stderr)
            return 1
        armed_env = dict(base)
        armed_env.update({
            "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED": "1",
            "R3V_NATIVE_AUTHORIZED_IB_BLAKE3": digest,
            "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE": platform.release(),
            "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION": FIXTURE_SRCVERSION,
            "R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3": identity,
        })
        armed = run(runner, evidence, armed_env)
        if armed.returncode != 0 or "verdict: ARMED" not in armed.stdout:
            print("FAIL: the full declaration did not arm", file=sys.stderr)
            print(armed.stdout, file=sys.stderr)
            return 1

        # The direct-write runner's digest names a different stream.
        other = subprocess.run([direct_write_runner, evidence], env=base,
                               capture_output=True, text=True)
        other_digest = field(other.stdout, "ib_blake3")
        if other_digest is None or other_digest == digest:
            print("FAIL: the direct-write cell does not report its own "
                  "distinct digest", file=sys.stderr)
            return 1

        def arm(label, marker, **changes):
            env = dict(armed_env)
            for key, value in changes.items():
                if value is None:
                    env.pop(key, None)
                else:
                    env[key] = value
            expect_refusal(run(runner, evidence, env), label, marker)

        stale = ("1" if digest[0] != "1" else "0") + digest[1:]
        arm("wrong cell", "MISMATCH",
            R3V_NATIVE_AUTHORIZED_IB_BLAKE3=other_digest)
        arm("stale digest", "MISMATCH", R3V_NATIVE_AUTHORIZED_IB_BLAKE3=stale)
        arm("wrong kernel", "MISMATCH",
            R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE="0.0.0-fixture")
        arm("wrong srcversion", "MISMATCH",
            R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION="WRONGSRCVERSION000000")
        arm("wrong identity", "different submission",
            R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3=stale)
        arm("undeclared identity", "no submission identity",
            R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3=None)
        arm("wrong destination handle", "different submission",
            R3V_NATIVE_RUNNER_DESTINATION_HANDLE=str(
                int(DESTINATION_HANDLE) + 1))
        arm("undeclared destination handle", "UNDECLARED",
            R3V_NATIVE_RUNNER_DESTINATION_HANDLE=None)
        missing = os.path.join(work, "no-such-evidence")
        absent = run(runner, missing, armed_env)
        expect_refusal(absent, "absent evidence directory", "ABSENT")

        # A spent directory: a token, then each retained name alone.
        spent = os.path.join(work, "spent")
        os.mkdir(spent)
        open(os.path.join(spent, "attempt.token"), "w").close()
        expect_refusal(run(runner, spent, armed_env), "spent token",
                       "already attempted")
        os.remove(os.path.join(spent, "attempt.token"))
        open(os.path.join(spent, "ib.bin"), "w").close()
        expect_refusal(run(runner, spent, armed_env), "retained ib.bin",
                       "SPENT")

        # The board: a wrong subsystem pair, a wrong DMI product, and a
        # different die id each resolve to no qualified platform.
        for label, pci, dmi in (
            ("wrong subsystem", {**SPECIMEN, "subsystem_device": "0x0000\n"},
             SPECIMEN_DMI),
            ("wrong DMI product", SPECIMEN, "Latitude D520\n"),
            ("wrong die", {**SPECIMEN, "device": "0x5975\n"}, SPECIMEN_DMI),
        ):
            other_root = os.path.join(work, label.replace(" ", "-"))
            write_sysfs(other_root, pci=pci, dmi=dmi)
            env = dict(armed_env)
            env["R3V_NATIVE_RUNNER_SYSFS_ROOT"] = other_root
            result = run(runner, evidence, env)
            expect_refusal(result, label, "no qualified platform")
            if field(result.stdout, "platform") != "NONE":
                print(f"FAIL: {label}: platform did not resolve to NONE",
                      file=sys.stderr)
                return 1

        # An unreadable PCI slot reports the tuple as unreadable.
        env = dict(armed_env)
        env["R3V_NATIVE_RUNNER_PCI_SLOT"] = "0000:ff:1f.7"
        expect_refusal(run(runner, evidence, env), "unreadable slot",
                       "PCI tuple unreadable")

    print("r3v_native_rb2d_fill_arming_runner_check: ARMED under the full "
          "declaration; every single-fact refusal holds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
