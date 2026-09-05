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
    "r3v_",
    "r300_",
    "radeon_drm_vk_",
)

# The reference SPIR-V data header is the one driver artifact the
# application compiles in; its two const word arrays are the only
# admitted names under the forbidden prefixes.
ALLOWED_SYMBOLS = (
    "r3v_reference_vertex_spirv",
    "r3v_reference_fragment_spirv",
)

REQUIRED_UNDEFINED = "vkCreateInstance"


def main() -> int:
    args = sys.argv[1:]
    forbidden_prefixes = FORBIDDEN_PREFIXES
    if len(args) > 2:
        # --forbid PREFIX pairs after the positionals replace the default
        # prefix set; the attended application admits its r3v_public_
        # helpers while every driver and shim prefix stays refused.
        if len(args) % 2 != 0 or any(a != "--forbid" for a in args[2::2]):
            print(f"usage: {sys.argv[0]} <nm> <binary> [--forbid PREFIX]...",
                  file=sys.stderr)
            return 2
        forbidden_prefixes = tuple(args[3::2])
    if len(args) < 2:
        print(f"usage: {sys.argv[0]} <nm> <binary> [--forbid PREFIX]...",
              file=sys.stderr)
        return 2
    nm, binary = args[0], args[1]
    result = subprocess.run(
        [nm, binary], check=False, capture_output=True, text=True
    )
    if result.returncode != 0:
        print(
            f"nm failed with status {result.returncode}: {result.stderr}",
            file=sys.stderr,
        )
        return 2
    table = result.stdout

    forbidden = sorted(
        {
            fields[-1]
            for line in table.splitlines()
            if (fields := line.split())
            and any(fields[-1].startswith(p) for p in forbidden_prefixes)
            and fields[-1] not in ALLOWED_SYMBOLS
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
