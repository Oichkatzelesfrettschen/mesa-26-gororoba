#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Audit r300_reg.h against the RS485M register evidence registry.

The registry lives in the steinmarder evidence repository; this wrapper
locates it through the STEINMARDER_ROOT environment variable (or a
--steinmarder-root argument) and runs the registry's live-tree audit
against the r300 driver directory containing this script.  The audit
reports, per finding class, where the driver header and the documented
RS480-class register census diverge: documented registers the header
lacks, name disagreements at an offset, field shifts outside documented
extents, and consumer emissions of registers no RS485M-reaching evidence
covers.

Exits 0 with a skip message when the registry checkout is absent, so
build and CI environments without the evidence repository are unaffected.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

AUDIT_SCRIPT = "src/re/r300/scripts/audit_mesa_r300_reg_against_registry.py"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--steinmarder-root",
        type=Path,
        default=os.environ.get("STEINMARDER_ROOT"),
        help="path to the steinmarder evidence repository checkout",
    )
    args = parser.parse_args()

    if not args.steinmarder_root:
        print("r300 register audit skipped: STEINMARDER_ROOT not set")
        return 0
    audit = Path(args.steinmarder_root) / AUDIT_SCRIPT
    if not audit.exists():
        print(f"r300 register audit skipped: {audit} not found")
        return 0
    repo_src = Path(__file__).resolve().parents[3]
    driver_root = repo_src / "amd" / "r300" / "common"
    return subprocess.call(
        [sys.executable, str(audit), "--mesa-root", str(driver_root)]
    )


if __name__ == "__main__":
    raise SystemExit(main())
