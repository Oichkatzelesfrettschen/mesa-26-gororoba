# SPDX-License-Identifier: MIT
#
# Loader-application gate wrapper: runs the loader-only application with
# a fresh manifest directory, then byte-compares the retained ib.bin
# against the reference cell the arming runner emits independently, so
# the equality claim spans the loader boundary end to end.  The
# R3V_LOADER_APP_FIXTURE_MUTATE_IB fixture flips one reference byte
# before the comparison, calibrating that the comparison still judges
# bytes.

import os
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 3:
        print(
            f"usage: {sys.argv[0]} <loader-application> <arming-runner>",
            file=sys.stderr,
        )
        return 2
    application, runner = sys.argv[1], sys.argv[2]

    with tempfile.TemporaryDirectory() as manifest_dir:
        env = dict(os.environ)
        env["R3V_NATIVE_MANIFEST_DIR"] = manifest_dir
        run = subprocess.run([application], env=env)
        if run.returncode != 0:
            print(
                f"loader application failed: status {run.returncode}",
                file=sys.stderr,
            )
            return 1

        ib_path = os.path.join(manifest_dir, "ib.bin")
        if not os.path.isfile(ib_path):
            print("submit retained no ib.bin manifest", file=sys.stderr)
            return 1
        with open(ib_path, "rb") as handle:
            recorded = handle.read()

        reference_path = os.path.join(manifest_dir, "reference-ib.bin")
        emit = subprocess.run([runner, "--emit-ib", reference_path])
        if emit.returncode != 0:
            print("reference emission failed", file=sys.stderr)
            return 2
        with open(reference_path, "rb") as handle:
            reference = handle.read()

    if os.environ.get("R3V_LOADER_APP_FIXTURE_MUTATE_IB") == "1":
        reference = bytes([reference[0] ^ 0x01]) + reference[1:]

    if len(recorded) == 0 or recorded != reference:
        print(
            f"recorded IB deviates from the reference cell: "
            f"{len(recorded)} recorded bytes, {len(reference)} reference",
            file=sys.stderr,
        )
        return 1

    print(f"loader-application IB byte-identical: {len(recorded)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
