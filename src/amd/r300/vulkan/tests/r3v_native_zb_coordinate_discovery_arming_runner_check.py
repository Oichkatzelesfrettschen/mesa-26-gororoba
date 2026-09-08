# SPDX-License-Identifier: MIT

import os
import re
import subprocess
import sys
import tempfile


def run(runner, evidence_dir, environment, layout="microtiled", x="37",
        y="21", pitch="64", base="2048", arm="measure"):
    return subprocess.run(
        [runner, evidence_dir, layout, x, y, pitch, base, arm],
        env=environment, capture_output=True, text=True)


def field(text, name):
    match = re.search(r"^%s=(.+)$" % re.escape(name), text, re.MULTILINE)
    return match.group(1) if match else None


def require_report(result, expected):
    for name, value in expected.items():
        if field(result.stdout, name) != value:
            raise AssertionError("%s: expected %r, got %r\n%s" %
                                 (name, value, field(result.stdout, name),
                                  result.stdout))
    for digest in ("scenario_blake3", "ib_blake3",
                   "initial_image_blake3"):
        value = field(result.stdout, digest)
        if value is None or re.fullmatch(r"[0-9a-f]{64}", value) is None:
            raise AssertionError("invalid %s\n%s" % (digest, result.stdout))


def require_frozen_stream_identity(result, expected_digest):
    if field(result.stdout, "ib_dwords") != "245":
        raise AssertionError("frozen scenario stream length changed\n%s" %
                             result.stdout)
    if field(result.stdout, "ib_blake3") != expected_digest:
        raise AssertionError("frozen scenario stream identity changed\n%s" %
                             result.stdout)


def require_stream_identity_changed(baseline, changed, factor):
    if field(changed.stdout, "ib_blake3") == field(baseline.stdout,
                                                     "ib_blake3"):
        raise AssertionError("%s did not move stream identity" % factor)


def main():
    if len(sys.argv) != 3:
        print("usage: r3v_native_zb_coordinate_discovery_arming_runner_check.py "
              "<runner> <attended>", file=sys.stderr)
        return 2
    runner, attended = sys.argv[1:3]
    environment = dict(os.environ)
    for name in (
        "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED",
        "R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
        "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE",
        "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
    ):
        environment.pop(name, None)

    with tempfile.TemporaryDirectory() as evidence_dir:
        reports = {}
        for layout in ("microtiled", "macrotiled"):
            for pitch in ("64", "96"):
                for base in ("2048", "4096"):
                    result = run(runner, evidence_dir, environment,
                                 layout=layout, pitch=pitch, base=base)
                    if result.returncode == 0:
                        raise AssertionError("undeclared runner armed")
                    expected_storage = {
                        ("microtiled", "64"): "16896",
                        ("microtiled", "96"): "25344",
                        ("macrotiled", "64"): "20480",
                        ("macrotiled", "96"): "30720",
                    }[(layout, pitch)]
                    expected_allocation = str(2 * int(base) +
                                              int(expected_storage) + 4096)
                    require_report(result, {
                        "cell_kind": "zb-coordinate-discovery",
                        "layout": layout,
                        "pixel_x": "37",
                        "pixel_y": "21",
                        "pitch_pixels": pitch,
                        "base_bytes": base,
                        "arm": "measure",
                        "storage_bytes": expected_storage,
                        "allocation_bytes": expected_allocation,
                    })
                    if "no submission attempted" not in result.stdout:
                        raise AssertionError("runner omitted submit-free result")
                    reports[(layout, pitch, base)] = result

        baseline = reports[("microtiled", "64", "2048")]
        require_frozen_stream_identity(
            baseline,
            "036eadd154fce784270296f1bd64b2c43def4b6de1a52500eaac18fe17a6406d")
        require_frozen_stream_identity(
            reports[("macrotiled", "64", "2048")],
            "3785aebf83d8be0cb196032b2951b7302e527dbc2743b01a4ab348d6e1f52e84")
        require_stream_identity_changed(
            baseline, reports[("macrotiled", "64", "2048")], "layout")
        require_stream_identity_changed(
            baseline, reports[("microtiled", "96", "2048")], "pitch")
        require_stream_identity_changed(
            baseline, reports[("microtiled", "64", "4096")], "base")
        moved = run(runner, evidence_dir, environment, x="36", y="20")
        require_report(moved, {
            "layout": "microtiled", "pixel_x": "36", "pixel_y": "20",
            "pitch_pixels": "64", "base_bytes": "2048", "arm": "measure",
            "storage_bytes": "16896", "allocation_bytes": "25088",
        })
        if field(moved.stdout, "scenario_blake3") == \
                field(baseline.stdout, "scenario_blake3"):
            raise AssertionError("coordinate did not move scenario identity")
        if field(moved.stdout, "ib_blake3") == field(baseline.stdout,
                                                       "ib_blake3"):
            raise AssertionError("coordinate did not move scissor stream")
        if field(moved.stdout, "initial_image_blake3") != \
                field(baseline.stdout, "initial_image_blake3"):
            raise AssertionError("coordinate changed uniform initial image")

        never = run(runner, evidence_dir, environment, arm="never")
        if field(never.stdout, "scenario_blake3") != \
                field(baseline.stdout, "scenario_blake3"):
            raise AssertionError("arm changed scenario identity")
        if field(never.stdout, "ib_blake3") == field(baseline.stdout,
                                                       "ib_blake3"):
            raise AssertionError("arm did not change stream identity")

        bad_cases = (
            ("octiled", "37", "21", "64", "2048", "measure"),
            ("microtiled", "64", "21", "64", "2048", "measure"),
            ("microtiled", "37", "64", "64", "2048", "measure"),
            ("microtiled", "37", "21", "68", "2048", "measure"),
            ("microtiled", "37", "21", "64", "32", "measure"),
            ("microtiled", "37", "21", "64", "2048", "sometimes"),
        )
        for arguments in bad_cases:
            result = run(runner, evidence_dir, environment, *arguments)
            if result.returncode != 2 or "ib_blake3=" in result.stdout:
                raise AssertionError("invalid runner declaration admitted: %r" %
                                     (arguments,))
            attended_result = subprocess.run(
                [attended, evidence_dir, *arguments], env=environment,
                capture_output=True, text=True)
            if attended_result.returncode != 2 or \
                    "verdict:" in attended_result.stdout:
                raise AssertionError("attended declaration reached execution: %r"
                                     % (arguments,))

    print("r3v_native_zb_coordinate_discovery_arming_runner_check: "
          "bounded declarations and identities hold")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print("FAIL: %s" % error, file=sys.stderr)
        sys.exit(1)
