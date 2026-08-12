# SPDX-License-Identifier: MIT
"""Run the staging unit and generated-artifact integration legs together."""

import subprocess
import sys
import tempfile
from pathlib import Path


def run_leg(label: str, command: list[str]) -> None:
    result = subprocess.run(
        command, capture_output=True, text=True, check=False
    )
    if result.stdout:
        print(f"{label} stdout:\n{result.stdout}", end="")
    if result.stderr:
        print(f"{label} stderr:\n{result.stderr}", file=sys.stderr, end="")
    if result.returncode != 0:
        raise SystemExit(f"{label} exited with status {result.returncode}")


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        return run_selftest()
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: r300_staging_manifest_runner.py "
            "<unit-test> <manifest-check> <manifest-tool>"
        )
    unit_test, manifest_check, manifest_tool = sys.argv[1:]
    run_leg("staging unit", [unit_test])
    run_leg(
        "manifest integration",
        [sys.executable, manifest_check, manifest_tool],
    )
    print("r300 staging manifest: unit and generated-artifact legs held")
    return 0


def run_selftest() -> int:
    """Prove that either child leg turns the wrapper verdict red."""
    script_path = Path(__file__).resolve()
    with tempfile.TemporaryDirectory(prefix="r300-staging-runner-") as tmp:
        root = Path(tmp)
        passing = root / "passing.py"
        failing = root / "failing.py"
        passing.write_text("#!/usr/bin/env python3\n")
        failing.write_text(
            "#!/usr/bin/env python3\n"
            "raise SystemExit(7)\n"
        )
        passing.chmod(0o755)
        failing.chmod(0o755)

        cases = (
            ("staging unit", [failing, passing, passing]),
            ("manifest integration", [passing, failing, passing]),
        )
        for label, arguments in cases:
            result = subprocess.run(
                [
                    sys.executable,
                    str(script_path),
                    *(str(arg) for arg in arguments),
                ],
                capture_output=True, text=True, check=False,
            )
            marker = f"{label} exited with status 7"
            if result.returncode == 0 or marker not in result.stderr:
                raise SystemExit(
                    f"selftest: {label} failure did not reject the wrapper"
                )

    print("r300 staging manifest runner: both failing legs reject")
    return 0


if __name__ == "__main__":
    sys.exit(main())
