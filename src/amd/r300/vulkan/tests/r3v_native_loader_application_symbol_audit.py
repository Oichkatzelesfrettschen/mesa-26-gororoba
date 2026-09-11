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

import re
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
    "r3v_reference_fragment_blue_spirv",
)

REQUIRED_UNDEFINED = "vkCreateInstance"
NEEDED_PATTERN = re.compile(r"\(NEEDED\).*Shared library: \[([^\]]+)\]")


def parse_options(args):
    """Parse symbol-prefix and dynamic-dependency contracts."""
    forbidden, allowed = [], []
    readelf = None
    required_needed, allowed_needed = [], []
    i = 0
    while i < len(args):
        if i + 1 >= len(args):
            return None
        option, value = args[i], args[i + 1]
        if option == "--forbid":
            forbidden.append(value)
        elif option == "--allow":
            allowed.append(value)
        elif option == "--readelf":
            if readelf is not None:
                return None
            readelf = value
        elif option == "--require-needed":
            required_needed.append(value)
        elif option == "--allow-needed":
            allowed_needed.append(value)
        else:
            return None
        i += 2
    if (required_needed or allowed_needed) and readelf is None:
        return None
    return (
        tuple(forbidden) if forbidden else FORBIDDEN_PREFIXES,
        tuple(allowed),
        readelf,
        frozenset(required_needed),
        frozenset(allowed_needed),
    )


def main() -> int:
    args = sys.argv[1:]
    usage = (
        f"usage: {sys.argv[0]} <nm> <binary> "
        f"[--forbid PREFIX]... [--allow PREFIX]... "
        f"[--readelf TOOL --require-needed LIB... "
        f"--allow-needed LIB...]"
    )
    if len(args) < 2:
        print(usage, file=sys.stderr)
        return 2
    parsed = parse_options(args[2:])
    if parsed is None:
        print(usage, file=sys.stderr)
        return 2
    (forbidden_prefixes, allowed_prefixes, readelf, required_needed, allowed_needed) = (
        parsed
    )
    nm, binary = args[0], args[1]
    result = subprocess.run([nm, binary], check=False, capture_output=True, text=True)
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
        print("loader application carries driver symbols: " + ", ".join(forbidden))
        return 1

    imports_loader = any(
        len(fields) >= 2 and fields[-2] == "U" and fields[-1] == REQUIRED_UNDEFINED
        for line in table.splitlines()
        if (fields := line.split())
    )
    if not imports_loader:
        print(
            f"loader application does not import {REQUIRED_UNDEFINED}; "
            "the Vulkan surface is not resolved through the loader"
        )
        return 1

    if readelf is not None:
        dynamic = subprocess.run(
            [readelf, "-d", binary], check=False, capture_output=True, text=True
        )
        if dynamic.returncode != 0:
            print(
                f"readelf failed with status {dynamic.returncode}: {dynamic.stderr}",
                file=sys.stderr,
            )
            return 2
        needed = frozenset(NEEDED_PATTERN.findall(dynamic.stdout))
        missing = sorted(required_needed - needed)
        unexpected = sorted(needed - allowed_needed)
        if missing or unexpected:
            print(
                f"loader application dependencies {sorted(needed)}; "
                f"missing {missing}; unexpected {unexpected}"
            )
            return 1

    print(
        "r3v_native_loader_application_symbol_audit: no driver symbol, "
        "Vulkan surface imported from the loader"
        + (f", dependencies {sorted(needed)}" if readelf is not None else "")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
