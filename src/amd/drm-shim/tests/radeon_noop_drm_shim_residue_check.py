# SPDX-License-Identifier: MIT
#
# Calibrates the radeon drm-shim state-token residue: runs a shim test
# and requires its failure set to equal the recorded signature exactly.
#
# The residue is one architectural class. drm_shim_render_node_open hands
# out a memfd per open whose contents are the state token itself, so a
# read or pread on a render descriptor returns header bytes, an identity
# recovered by lseek/read is unavailable once the descriptor is shared,
# and a descriptor whose fd table diverged through fork or CLONE_FILES
# has no in-band identity to recover. Hiding the payload behind the
# descriptor is the design change that retires this file.
#
# The check fails on any drift in either direction: a new failure, or a
# failure that disappears because the class was repaired. A repaired
# class updates the signature in the same change that repairs it.

import argparse
import re
import subprocess
import sys


def normalize(line):
    """Drops volatile numbers so the signature names the defect, not a
    particular pid, descriptor number, or address."""
    return re.sub(r"[0-9]+", "N", line).strip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--signature", required=True)
    parser.add_argument("--expect-abort", action="store_true",
                        help="the run is expected to die on a signal")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        print("no command given", file=sys.stderr)
        return 2

    result = subprocess.run(command, capture_output=True, text=True)
    observed = sorted({normalize(line)
                       for line in result.stderr.splitlines()
                       if line.startswith("FAIL:")})

    with open(args.signature, "r", encoding="utf-8") as handle:
        expected = sorted({normalize(line) for line in handle
                           if line.startswith("FAIL:")})

    if result.returncode < 0 and not args.expect_abort:
        print("FAIL: run died on signal {}".format(-result.returncode),
              file=sys.stderr)
        return 1

    missing = [line for line in expected if line not in observed]
    added = [line for line in observed if line not in expected]
    if missing or added:
        for line in missing:
            print("signature line no longer observed: {}".format(line),
                  file=sys.stderr)
        for line in added:
            print("failure outside the signature: {}".format(line),
                  file=sys.stderr)
        print("residue drifted; repair the class and update {}".format(
            args.signature), file=sys.stderr)
        return 1

    print("residue matches the recorded signature ({} lines)".format(
        len(expected)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
