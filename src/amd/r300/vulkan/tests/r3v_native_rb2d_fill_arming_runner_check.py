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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r3v_native_rb2d_fill_mutation_table as table  # noqa: E402

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
RUNNER_KEY = table.RUNNER_HANDLE


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


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


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
            ("route_state", "executing"),
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
        base[RUNNER_KEY] = DESTINATION_HANDLE
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

        # Every mutation the canonical table names, the in-process leg:
        # the runner refuses each by the marker the table declares, so the
        # loader leg's VK_ERROR_DEVICE_LOST for the same descriptor has a
        # named cause.
        wrong_handle = str(int(DESTINATION_HANDLE) + 1)
        wrong = run(runner, evidence, {**base, RUNNER_KEY: wrong_handle})
        wrong_identity = field(wrong.stdout, "fill_identity_blake3")
        if wrong_identity is None or wrong_identity == identity:
            print("FAIL: the destination handle does not enter the identity",
                  file=sys.stderr)
            return 1
        symbols = {
            "@stale_digest": table.stale(digest),
            "@stale_identity": table.stale(identity),
            "@wrong_identity": wrong_identity,
            "@handle_plus_one": wrong_handle,
        }
        sysfs_variants = {
            "wrong_subsystem": ({**SPECIMEN, "subsystem_device": "0x0000\n"},
                                SPECIMEN_DMI),
            "wrong_dmi": (SPECIMEN, "Latitude D520\n"),
        }
        for mutation_id, _field, runner_change, *_rest, marker in \
                table.MUTATIONS:
            env = dict(armed_env)
            for key, value in runner_change.items():
                if key == "@sysfs":
                    pci, dmi = sysfs_variants[value]
                    other_root = os.path.join(work, mutation_id)
                    write_sysfs(other_root, pci=pci, dmi=dmi)
                    env["R3V_NATIVE_RUNNER_SYSFS_ROOT"] = other_root
                    continue
                resolved = table.resolve(value, symbols)
                if resolved is None:
                    env.pop(key, None)
                else:
                    env[key] = resolved
            result = run(runner, evidence, env)
            expect_refusal(result, mutation_id, marker)
            if "@sysfs" in runner_change and \
                    field(result.stdout, "platform") != "NONE":
                print(f"FAIL: {mutation_id}: platform did not resolve to "
                      f"NONE", file=sys.stderr)
                return 1
        arm_count = len(table.MUTATIONS)
        # The wrong-cell arm stays beside the table: the direct-write
        # runner's digest names a different stream rather than a mutated
        # fact of this one.
        wrong_cell = run(runner, evidence,
                         {**armed_env, table.IB_BLAKE3: other_digest})
        expect_refusal(wrong_cell, "wrong cell", "MISMATCH")
        undeclared_handle = dict(armed_env)
        undeclared_handle.pop(RUNNER_KEY, None)
        expect_refusal(run(runner, evidence, undeclared_handle),
                       "undeclared destination handle", "UNDECLARED")
        for mutation_id, _field, marker in table.DIRECTORY_MUTATIONS:
            if mutation_id == "absent_directory":
                target = os.path.join(work, "no-such-evidence")
            else:
                target = os.path.join(work, "spent")
                os.mkdir(target)
                open(os.path.join(target, "attempt.token"), "w").close()
            expect_refusal(run(runner, target, armed_env), mutation_id,
                           marker)
        spent = os.path.join(work, "spent")
        os.remove(os.path.join(spent, "attempt.token"))
        open(os.path.join(spent, "ib.bin"), "w").close()
        expect_refusal(run(runner, spent, armed_env), "retained ib.bin",
                       "SPENT")
        # A different die id resolves to no qualified platform, beside the
        # table's subsystem and DMI rows.
        other_root = os.path.join(work, "wrong-die")
        write_sysfs(other_root, pci={**SPECIMEN, "device": "0x5975\n"},
                    dmi=SPECIMEN_DMI)
        result = run(runner, evidence,
                     {**armed_env, "R3V_NATIVE_RUNNER_SYSFS_ROOT": other_root})
        expect_refusal(result, "wrong die", "no qualified platform")

        # An unreadable PCI slot reports the tuple as unreadable.
        env = dict(armed_env)
        env["R3V_NATIVE_RUNNER_PCI_SLOT"] = "0000:ff:1f.7"
        expect_refusal(run(runner, evidence, env), "unreadable slot",
                       "PCI tuple unreadable")

        # The named cells.  --cell selects which lowering the runner
        # builds, so each row below is the legalizer's own verdict read
        # back through the report: the carrier, the window and
        # relocation-site counts, the rectangles, the stream length, and
        # the cell kind the arming digest binds.  The default argument
        # and an explicit v1_public produce one digest, which is what
        # keeps the sealed cell's stream unmoved by the cell table.
        cells = {
            "v1_public": {
                "cell_kind": "rb2d_fill_public",
                "evidence_scope": "route_receipt",
                "route": "rb2d_const_fill",
                "allocation_bytes": "65536",
                "fill_bytes": "4992",
                "pitch_bytes": "256",
                "window_count": "1",
                "rect_count": "3",
                "ib_dwords": "38",
                "relocation_site_count": "1",
            },
            "v2_multiwindow_256": {
                "cell_kind": "rb2d_fill_v2_route",
                "evidence_scope": "route_receipt",
                "route": "rb2d_const_fill_v2",
                "allocation_bytes": "2097152",
                "fill_bytes": "2097012",
                "pitch_bytes": "256",
                "window_count": "2",
                "rect_count": "3",
                "ib_dwords": "58",
                "relocation_site_count": "2",
            },
            "dense_16320_carrier": {
                "cell_kind": "rb2d_carrier_qualification",
                "evidence_scope": "carrier_qualification",
                "route": "rb2d_const_fill_v2",
                "allocation_bytes": "65536",
                "fill_bytes": "65428",
                "pitch_bytes": "16320",
                "window_count": "1",
                "rect_count": "3",
                "ib_dwords": "38",
                "relocation_site_count": "1",
            },
        }
        expected_rects = {
            "v1_public": ["3,0,61,1", "0,1,64,18", "0,19,35,1"],
            "v2_multiwindow_256": ["3,0,61,1", "0,1,64,8190",
                                   "0,3,32,1"],
            "dense_16320_carrier": ["3,0,4077,1", "0,1,4080,3",
                                    "0,4,40,1"],
        }
        default = run(runner, evidence, armed_env)
        for name, fields in cells.items():
            report = run(runner, evidence, armed_env,
                         extra=("--cell", name))
            if field(report.stdout, "cell") != name:
                fail(f"--cell {name} reported cell "
                     f"{field(report.stdout, 'cell')}")
            for key, want in fields.items():
                got = field(report.stdout, key)
                if got != want:
                    fail(f"--cell {name}: {key}={got}, expected {want}")
            rects = re.findall(r"^rect=(\S+)", report.stdout, re.MULTILINE)
            if rects != expected_rects[name]:
                fail(f"--cell {name}: rectangles {rects}, expected "
                     f"{expected_rects[name]}")
            sites = re.findall(r"^relocation_site=(\S+)", report.stdout,
                               re.MULTILINE)
            if len(sites) != int(fields["relocation_site_count"]):
                fail(f"--cell {name}: {len(sites)} site lines for "
                     f"{fields['relocation_site_count']} sites")
            if name == "v1_public" and \
                    field(report.stdout, "ib_blake3") != \
                    field(default.stdout, "ib_blake3"):
                fail("--cell v1_public builds a different stream from the "
                     "default cell")
        # The stream-shape lane: each mutation of the windowed cell's
        # legalized window list is refused by the check the table names,
        # and a mutation the checks admit exits 3 rather than 0.
        for mutation_id, mutated, refused_by in table.WINDOW_MUTATIONS:
            result = subprocess.run(
                [runner, "--cell", table.WINDOW_MUTATION_CELL,
                 "--mutate-window", mutation_id, evidence],
                env=armed_env, capture_output=True, text=True)
            if result.returncode != 0:
                fail(f"{mutation_id}: status {result.returncode}\n"
                     f"{result.stdout}\n{result.stderr}")
            # The report prints the whole check name, which carries
            # spaces, so the comparison reads the line rather than the
            # first whitespace-delimited field.
            line = re.search(r"^window_mutation_refused_by=(.*)$",
                             result.stdout, re.MULTILINE)
            if line is None or line.group(1) != refused_by:
                fail(f"{mutation_id} ({mutated}) is refused by "
                     f"{line.group(1) if line else '(nothing)'}; the table "
                     f"names {refused_by}")
            detail = re.search(r"^window_mutation_detail=(.*)$",
                               result.stdout, re.MULTILINE)
            print(f"  {mutation_id}: refused by {refused_by} -- "
                  f"{detail.group(1) if detail else ''}")
        result = subprocess.run(
            [runner, "--cell", table.WINDOW_MUTATION_CELL,
             "--mutate-window", "no_such_mutation", evidence],
            env=armed_env, capture_output=True, text=True)
        if result.returncode != 2:
            fail("an unnamed window mutation was accepted")

        # The kind spellings the report prints are the plan registry's,
        # so a report and a captured plan name one kind.
        registry = set(re.findall(r'"(rb2d_[a-z0-9_]+)"',
                                  open(os.path.join(
                                      os.path.dirname(
                                          os.path.abspath(__file__)),
                                      "..", "r3v_native_plan.c"),
                                      encoding="utf-8").read()))
        for name, fields in cells.items():
            if fields["cell_kind"] not in registry:
                fail(f"--cell {name} prints kind {fields['cell_kind']}, "
                     f"which r3v_native_plan.c does not name")
        # A name outside the table builds nothing, so this leg bypasses
        # run(): there is no report to hold to the no-submission line.
        result = subprocess.run(
            [runner, "--cell", "no_such_cell", evidence], env=armed_env,
            capture_output=True, text=True)
        if result.returncode != 2 or "no cell is named" not in result.stderr:
            fail("an unnamed cell was accepted")

    print(f"r3v_native_rb2d_fill_arming_runner_check: ARMED under the full "
          f"declaration; {arm_count} table mutations and "
          f"{len(table.DIRECTORY_MUTATIONS)} directory mutations refused "
          f"by name; {len(cells)} named cells build their declared "
          f"lowering; {len(table.WINDOW_MUTATIONS)} stream-shape mutations "
          f"refused by the check each names")
    return 0


if __name__ == "__main__":
    sys.exit(main())
