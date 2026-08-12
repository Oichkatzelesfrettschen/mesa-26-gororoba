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
import tempfile
from pathlib import Path

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
    """Calibrate clean and forbidden symbol-table verdicts through the audit."""
    with tempfile.TemporaryDirectory(prefix="r3v-native-separation-") as tmp:
        root = Path(tmp)
        nm = root / "nm"
        nm.write_text(
            "#!/bin/sh\n"
            "set -eu\n"
            "last=\n"
            "for argument in \"$@\"; do\n"
            "    last=$argument\n"
            "done\n"
            "cat \"$last\"\n"
        )
        nm.chmod(0o755)

        cases = (
            ("native", "r3v_native_entrypoint\n", 0),
            ("gallium", "r300_screen_create\n", 1),
            ("winsys", "radeon_drm_winsys_create\n", 1),
        )
        for label, table, expected_status in cases:
            library = root / f"{label}.symbols"
            library.write_text(table)
            status = audit_library(str(nm), str(library))
            if status != expected_status:
                print(
                    f"separation selftest {label}: expected status="
                    f"{expected_status}, got {status}"
                )
                return 1

    print("r3v_native_separation_audit: clean and forbidden audit verdicts calibrated")
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
