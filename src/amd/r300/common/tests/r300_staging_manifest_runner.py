# SPDX-License-Identifier: MIT
"""Run the staging unit and generated-artifact integration legs together."""

import subprocess
import sys


def run_leg(label: str, command: list[str]) -> None:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.stdout:
        print(f"{label} stdout:\n{result.stdout}", end="")
    if result.stderr:
        print(f"{label} stderr:\n{result.stderr}", file=sys.stderr, end="")
    if result.returncode != 0:
        raise SystemExit(f"{label} exited with status {result.returncode}")


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: r300_staging_manifest_runner.py "
            "<unit-test> <manifest-check> <manifest-tool>")
    unit_test, manifest_check, manifest_tool = sys.argv[1:]
    run_leg("staging unit", [unit_test])
    run_leg("manifest integration", [sys.executable, manifest_check,
                                      manifest_tool])
    print("r300 staging manifest: unit and generated-artifact legs held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
