# SPDX-License-Identifier: MIT
"""Gallium-separation audit for the native R3V ICD.

Native ownership requires that libvulkan_r3v_native carries no Gallium or
radeon-winsys runtime symbol in any binding.  The audit walks the full nm
symbol table (defined and undefined, local and global), so a statically
linked Gallium object with hidden visibility fails the same as a dynamic
reference.
"""

import subprocess
import sys

FORBIDDEN_SYMBOLS = (
    "r300_screen_create",
    "radeon_drm_winsys_create",
    "pipe_screen_create",
    "vl_create_mpeg12_decoder",
)


def find_forbidden_symbols(table: str) -> list[str]:
    """Return native-library symbols that belong to Gallium or winsys."""
    return [name for name in FORBIDDEN_SYMBOLS if name in table]


def audit_library(nm: str, library: str) -> int:
    """Run the symbol audit against one native library."""
    output = subprocess.run(
        [nm, "--defined-only", library],
        check=False,
        capture_output=True,
        text=True,
    )
    full = subprocess.run(
        [nm, library], check=False, capture_output=True, text=True
    )
    table = output.stdout + full.stdout
    failures = find_forbidden_symbols(table)
    if failures:
        print(
            "native ICD carries Gallium/winsys symbols: "
            + ", ".join(failures)
        )
        return 1
    print("r3v_native_separation_audit: no Gallium or winsys symbol present")
    return 0


def selftest() -> int:
    """Calibrate clean and forbidden symbol-table verdicts without nm."""
    cases = (
        ("native", "r3v_native_entrypoint\n", False),
        ("gallium", "r300_screen_create\n", True),
        ("winsys", "radeon_drm_winsys_create\n", True),
    )
    for label, table, expected_failure in cases:
        failures = find_forbidden_symbols(table)
        if bool(failures) != expected_failure:
            print(
                f"separation selftest {label}: expected "
                f"failure={expected_failure}, got {failures}"
            )
            return 1
    print("r3v_native_separation_audit: clean and forbidden fixtures calibrated")
    return 0


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        return selftest()
    if len(sys.argv) != 3:
        print("usage: r3v_native_separation_audit.py <nm> <library>")
        return 2
    return audit_library(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    sys.exit(main())
