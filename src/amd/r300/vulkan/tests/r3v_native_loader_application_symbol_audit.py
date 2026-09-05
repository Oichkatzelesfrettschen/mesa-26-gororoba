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

A binary that compiles in a pure test helper names it with --allow, which
carves that one prefix out of a forbidden one.  The carve-out keeps the
forbidden set at the whole driver surface: dropping "r3v_" to admit
"r3v_public_" would admit twenty other driver prefixes with it.
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


def parse_prefix_options(args):
    """--forbid PREFIX replaces the default forbidden set; --allow PREFIX
    carves a prefix out of it.  Returns (forbidden, allowed) or None when
    the options are malformed."""
    forbidden, allowed = [], []
    i = 0
    while i < len(args):
        if i + 1 >= len(args) or args[i] not in ("--forbid", "--allow"):
            return None
        (forbidden if args[i] == "--forbid" else allowed).append(args[i + 1])
        i += 2
    return (tuple(forbidden) if forbidden else FORBIDDEN_PREFIXES,
            tuple(allowed))


def main() -> int:
    args = sys.argv[1:]
    usage = (f"usage: {sys.argv[0]} <nm> <binary> "
             f"[--forbid PREFIX]... [--allow PREFIX]...")
    if len(args) < 2:
        print(usage, file=sys.stderr)
        return 2
    parsed = parse_prefix_options(args[2:])
    if parsed is None:
        print(usage, file=sys.stderr)
        return 2
    forbidden_prefixes, allowed_prefixes = parsed
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
            and not any(fields[-1].startswith(p) for p in allowed_prefixes)
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
