#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Every depth-control entry point reports its usage before reading an argument.

A mode selector read ahead of the argument count crashes on an invocation
that supplies none, and a crash is nonzero exactly as a usage refusal is,
so a test that only demands failure cannot tell them apart.  This demands
the exact usage status and a usage line on stderr, which a signal death
supplies neither of.
"""

import subprocess
import sys

USAGE_STATUS = 2


# Two argument shapes. The harness selects its mode with argv[1] and its
# surface with argv[2]; the two runners take an evidence directory in
# argv[1] and the surface in argv[2], so an unrecognized argv[1] is a
# directory name to them and only argv[2] can be unknown.
SHAPES = {
    "mode": (
        ([], "no arguments"),
        (["closed", "z24_linear", "extra"], "too many arguments"),
        (["not-a-mode"], "an unknown mode"),
        (["closed", "not-a-surface"], "an unknown surface"),
    ),
    "directory": (
        ([], "no arguments"),
        (["/nonexistent", "z24_linear", "extra"], "too many arguments"),
        (["/nonexistent", "not-a-surface"], "an unknown surface"),
    ),
}


def check(binary: str, shape: str) -> list[str]:
    failures = []
    for arguments, label in SHAPES[shape]:
        run = subprocess.run(
            [binary, *arguments], capture_output=True, text=True, timeout=60
        )
        if run.returncode != USAGE_STATUS:
            failures.append(
                f"{binary} with {label}: exit {run.returncode}, "
                f"expected {USAGE_STATUS}"
            )
        if "usage:" not in run.stderr:
            failures.append(f"{binary} with {label}: no usage line on stderr")
    return failures


def main() -> int:
    entries = sys.argv[1:]
    if not entries or any(":" not in entry for entry in entries):
        print(
            "usage: r3v_native_zb_depth_usage_check.py "
            "<binary>:mode|directory ...",
            file=sys.stderr,
        )
        return 2

    failures: list[str] = []
    for entry in entries:
        binary, _, shape = entry.rpartition(":")
        if shape not in SHAPES:
            print(f"unknown argument shape: {shape}", file=sys.stderr)
            return 2
        failures.extend(check(binary, shape))

    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(
        f"r3v_native_zb_depth_usage_check: {len(entries)} entry points "
        f"report usage on an absent, excessive, or unknown argument"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
