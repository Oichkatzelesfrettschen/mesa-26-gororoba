# SPDX-License-Identifier: MIT
"""Verify closed-arming preparation of the public ZMASK lifecycle streams."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


MODES = (
    "read-materialize-export",
    "aba-materialize-export",
)
RETAINED = (
    "ib.bin",
    "manifest.json",
    "preparation_outcome.json",
    "relocs.bin",
    "submit_manifest.json",
    "submit_relocs.bin",
)
AUTHORIZATION_ENVIRONMENT = (
    "R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
    "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE",
    "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
)
HEX64 = re.compile(r"[0-9a-f]{64}")


class Failure(Exception):
    """The preparation contract did not hold."""


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise Failure(reason)


def read_json(path: Path) -> dict[str, object]:
    record = json.loads(path.read_bytes())
    require(type(record) is dict, f"JSON object: {path.name}")
    return record


def run_preparation(application: Path, mode: str, directory: Path) -> bytes:
    environment = os.environ.copy()
    for name in AUTHORIZATION_ENVIRONMENT:
        environment.pop(name, None)
    environment.update(
        {
            "R3V_NATIVE_MANIFEST_DIR": str(directory),
            "R3V_NATIVE_ZMASK_PUBLIC_LIFECYCLE_HARDWARE": "1",
            "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED": "1",
            "R3V_NATIVE_ZMASK_FAST_CLEAR_EXPERIMENTAL": "1",
            "R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS": "1",
            "R3V_DRM_SHIM_MODULE_SRCVERSION": "FIXTURESRCVERSION0000000",
        }
    )
    result = subprocess.run(
        [str(application), f"--prepare-{mode}", str(directory)],
        env=environment,
        check=False,
        capture_output=True,
        text=True,
    )
    require(
        result.returncode == 0,
        f"{mode} preparation status {result.returncode}:\n"
        f"{result.stdout}\n{result.stderr}",
    )
    require(
        "queue_submit_calls_attempted=1" in result.stdout
        and "queue_submits_accepted=0" in result.stdout
        and "queue_executions_completed=0" in result.stdout
        and "submit_result=-4" in result.stdout
        and "shim_counter_available=1" in result.stdout
        and "shim_cs_ioctls=0" in result.stdout
        and "shim_hyperz_acquire_ioctls=0" in result.stdout
        and "shim_hyperz_release_ioctls=0" in result.stdout
        and "shim_hyperz_owned_after=0" in result.stdout,
        f"{mode} application summary",
    )
    present = tuple(sorted(path.name for path in directory.iterdir()))
    require(present == RETAINED, f"{mode} retained files: {present}")

    outcome = read_json(directory / "preparation_outcome.json")
    require(
        outcome
        == {
            "schema": "r3v-zmask-public-lifecycle-preparation-outcome/1",
            "mode": mode,
            "queue_submit_result": "VK_ERROR_DEVICE_LOST",
            "queue_submit_result_value": -4,
            "queue_submit_calls_attempted": 1,
            "queue_submits_accepted": 0,
            "queue_executions_completed": 0,
            "expected_icd_dso_mapped": True,
            "authorization_declarations_absent": True,
            "submission_refused": True,
            "submit_object_retained": True,
            "attempt_token_absent": True,
            "shim_counter_available": True,
            "shim_cs_ioctls": 0,
            "shim_hyperz_state_available": True,
            "shim_hyperz_unowned_before": True,
            "shim_hyperz_acquire_ioctls": 0,
            "shim_hyperz_release_ioctls": 0,
            "shim_hyperz_owned_after": False,
        },
        f"{mode} preparation outcome",
    )
    semantic = read_json(directory / "manifest.json")
    submit = read_json(directory / "submit_manifest.json")
    digest = semantic.get("ib_blake3")
    require(
        semantic.get("object") == "semantic-cell"
        and submit.get("object") == "submit-object"
        and type(digest) is str
        and HEX64.fullmatch(digest) is not None
        and submit.get("ib_blake3") == digest
        and semantic.get("ib_dwords") == submit.get("ib_dwords")
        and len((directory / "ib.bin").read_bytes()) == semantic.get("ib_dwords") * 4,
        f"{mode} retained IB identity",
    )
    return (directory / "ib.bin").read_bytes()


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} APPLICATION", file=sys.stderr)
        return 2
    application = Path(sys.argv[1]).resolve(strict=True)
    try:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mode_ibs: dict[str, bytes] = {}
            for mode in MODES:
                first = root / f"{mode}-first"
                second = root / f"{mode}-second"
                first.mkdir()
                second.mkdir()
                first_ib = run_preparation(application, mode, first)
                second_ib = run_preparation(application, mode, second)
                require(first_ib == second_ib, f"{mode} deterministic IB")
                mode_ibs[mode] = first_ib

            require(
                mode_ibs[MODES[0]] != mode_ibs[MODES[1]],
                "lifecycle modes produce distinct IBs",
            )

            for name in AUTHORIZATION_ENVIRONMENT:
                contaminated = root / f"contaminated-{name.lower()}"
                contaminated.mkdir()
                environment = os.environ.copy()
                for authorization_name in AUTHORIZATION_ENVIRONMENT:
                    environment.pop(authorization_name, None)
                environment[name] = "0" * 64
                result = subprocess.run(
                    [
                        str(application),
                        "--prepare-read-materialize-export",
                        str(contaminated),
                    ],
                    env=environment,
                    check=False,
                    capture_output=True,
                    text=True,
                )
                require(
                    result.returncode == 2,
                    f"{name} contamination refusal",
                )
                require(
                    not any(contaminated.iterdir()),
                    f"{name} contamination leaves no artifacts",
                )
    except (Failure, OSError, TypeError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("r3v loader ZMASK lifecycle preparation: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
