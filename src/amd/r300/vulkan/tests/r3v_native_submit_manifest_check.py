# SPDX-License-Identifier: MIT
#
# Checks that the submit-object manifest binds evidence to the executable
# that issued the ioctl-shaped submission and remains parser-valid JSON.

import json
import os
import re
import subprocess
import sys
import tempfile


def fail(message, *outputs):
    print("FAIL: " + message, file=sys.stderr)
    for output in outputs:
        print(output, file=sys.stderr)
    return 1


def main():
    if len(sys.argv) != 2:
        print("usage: r3v_native_submit_manifest_check.py <harness>",
              file=sys.stderr)
        return 2
    harness = sys.argv[1]
    environment = dict(os.environ)

    with tempfile.TemporaryDirectory() as manifest_dir:
        environment["R3V_NATIVE_MANIFEST_DIR"] = manifest_dir
        run = subprocess.run([harness, "open"], env=environment,
                             capture_output=True, text=True)
        if run.returncode != 0:
            return fail("drm-shim open leg failed", run.stdout, run.stderr)

        manifest_path = os.path.join(manifest_dir, "submit_manifest.json")
        try:
            with open(manifest_path, encoding="utf-8") as manifest_file:
                manifest_text = manifest_file.read()
            manifest = json.loads(manifest_text)
        except (OSError, json.JSONDecodeError) as error:
            return fail("submit manifest is not parser-valid JSON", error)

        if manifest.get("object") != "submit-object":
            return fail("manifest names no submit object", manifest)
        elf_path = manifest.get("driver_elf_path")
        if not isinstance(elf_path, str) or not os.path.isabs(elf_path):
            return fail("manifest carries no absolute issuer path", elf_path)
        if elf_path == "unresolved":
            return fail("manifest carries unresolved issuer identity")
        try:
            same_issuer = os.path.samefile(elf_path, harness)
        except OSError as error:
            return fail("manifest issuer path is not present", error)
        if not same_issuer:
            return fail("manifest issuer differs from the submitting "
                        "executable", elf_path, harness)

        digest = manifest.get("driver_elf_blake3")
        if not isinstance(digest, str) or \
                re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            return fail("manifest issuer digest is not lowercase BLAKE3",
                        digest)

        # A literal control byte is the calibrated bad JSON mutant.  The
        # parser must reject it, so a check that only searches for field
        # names cannot stand in for JSON escaping.
        mutant = manifest_text.replace(
            '"driver_elf_path": "', '"driver_elf_path": "bad\n', 1)
        if mutant == manifest_text:
            return fail("manifest mutant did not alter the issuer field")
        try:
            json.loads(mutant)
        except json.JSONDecodeError:
            pass
        else:
            return fail("malformed JSON mutant was accepted")

    print("r3v_native_submit_manifest_check: issuer identity and JSON "
          "escaping hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
