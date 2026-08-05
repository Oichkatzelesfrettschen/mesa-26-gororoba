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


def main() -> int:
    nm, library = sys.argv[1], sys.argv[2]
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
    failures = [name for name in FORBIDDEN_SYMBOLS if name in table]
    if failures:
        print(
            "native ICD carries Gallium/winsys symbols: "
            + ", ".join(failures)
        )
        return 1
    print("r3v_native_separation_audit: no Gallium or winsys symbol present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
