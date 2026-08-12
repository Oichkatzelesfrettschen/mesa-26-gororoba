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

AUDIT_OK = 0
AUDIT_FORBIDDEN_SYMBOL = 1
AUDIT_USAGE = 2
AUDIT_NM_FAILURE = 3


def find_forbidden_symbols(table: str) -> list[str]:
    """Return native-library symbols that belong to Gallium or winsys."""
    return [name for name in FORBIDDEN_SYMBOLS if name in table]


def audit_library(nm: str, library: str) -> int:
    """Run the symbol audit against one native library."""
    try:
        output = subprocess.run(
            [nm, "--defined-only", library],
            check=False,
            capture_output=True,
            text=True,
        )
        full = subprocess.run(
            [nm, library], check=False, capture_output=True, text=True
        )
    except OSError as error:
        print(f"nm command failed for {library}: {error}", file=sys.stderr)
        return AUDIT_NM_FAILURE
    for label, result in (("defined-only", output), ("full", full)):
        if result.returncode != 0:
            print(
                f"nm {label} command failed for {library}: "
                f"status {result.returncode}",
                file=sys.stderr,
            )
            return AUDIT_NM_FAILURE
        if not result.stdout.strip():
            print(
                f"nm {label} command produced no symbols for {library}",
                file=sys.stderr,
            )
            return AUDIT_NM_FAILURE
    table = output.stdout + full.stdout
    failures = find_forbidden_symbols(table)
    if failures:
        print(
            "native ICD carries Gallium/winsys symbols: "
            + ", ".join(failures)
        )
        return AUDIT_FORBIDDEN_SYMBOL
    print("r3v_native_separation_audit: no Gallium or winsys symbol present")
    return AUDIT_OK


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
            "case \"$last\" in\n"
            "    *nm-error.symbols) exit 2 ;;\n"
            "esac\n"
            "cat \"$last\"\n"
        )
        nm.chmod(0o755)

        cases = (
            ("native", "r3v_native_entrypoint\n", AUDIT_OK),
            ("gallium", "r300_screen_create\n", AUDIT_FORBIDDEN_SYMBOL),
            ("winsys", "radeon_drm_winsys_create\n", AUDIT_FORBIDDEN_SYMBOL),
            ("empty", "", AUDIT_NM_FAILURE),
            ("nm-error", "r3v_native_entrypoint\n", AUDIT_NM_FAILURE),
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

    print(
        "r3v_native_separation_audit: clean, forbidden, empty-output, and "
        "nm-error verdicts calibrated"
    )
    return 0


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        return selftest()
    if len(sys.argv) == 5 and sys.argv[1] == "--expect-status":
        try:
            expected_status = int(sys.argv[2])
        except ValueError:
            print("expected audit status must be an integer", file=sys.stderr)
            return AUDIT_USAGE
        actual_status = audit_library(sys.argv[3], sys.argv[4])
        if actual_status != expected_status:
            print(
                f"separation audit expected status={expected_status}, "
                f"got {actual_status}",
                file=sys.stderr,
            )
            return 1
        print(
            f"r3v_native_separation_audit: exact status {actual_status} "
            "matched"
        )
        return AUDIT_OK
    if len(sys.argv) != 3:
        print("usage: r3v_native_separation_audit.py <nm> <library>")
        return AUDIT_USAGE
    return audit_library(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    sys.exit(main())
