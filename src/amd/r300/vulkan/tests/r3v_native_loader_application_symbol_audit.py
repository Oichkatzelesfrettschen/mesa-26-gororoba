# SPDX-License-Identifier: MIT
"""Symbol audit for the loader-only application.

The application's black-box claim rests on its link surface: every
Vulkan command arrives through the installed loader, so the binary
defines or references no driver symbol.  The audit walks the full nm
table (defined and undefined, local and global) for the driver prefixes
and requires the loader import that proves the Vulkan surface is
dynamic.  The known-bad meson leg runs the same audit against a harness
that links the implementation, so a prefix list that stopped matching
fails there.
"""

import subprocess
import sys

FORBIDDEN_PREFIXES = (
    "r3v_native_",
    "r3v_Cmd",
    "r300_tcl_bypass_",
    "r300_cpu_vertex_",
    "radeon_drm_vk_",
)

REQUIRED_UNDEFINED = "vkCreateInstance"


def main() -> int:
    nm, binary = sys.argv[1], sys.argv[2]
    table = subprocess.run(
        [nm, binary], check=False, capture_output=True, text=True
    ).stdout

    forbidden = sorted(
        {
            fields[-1]
            for line in table.splitlines()
            if (fields := line.split())
            and any(fields[-1].startswith(p) for p in FORBIDDEN_PREFIXES)
        }
    )
    if forbidden:
        print(
            "loader application carries driver symbols: "
            + ", ".join(forbidden)
        )
        return 1

    imports_loader = any(
        len(fields) >= 2
        and fields[-2] == "U"
        and fields[-1] == REQUIRED_UNDEFINED
        for line in table.splitlines()
        if (fields := line.split())
    )
    if not imports_loader:
        print(
            f"loader application does not import {REQUIRED_UNDEFINED}; "
            "the Vulkan surface is not resolved through the loader"
        )
        return 1

    print(
        "r3v_native_loader_application_symbol_audit: no driver symbol, "
        "Vulkan surface imported from the loader"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
