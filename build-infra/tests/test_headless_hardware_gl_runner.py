# SPDX-License-Identifier: MIT

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
RUNNER_DOCUMENT = REPOSITORY_ROOT / "headless-hardware-gl-runner.md"
TEARDOWN_HEADING = "## Terminate the recorded server and restore the display manager"
RECORDED_MAIN_PID = "4242"
RECORDED_INVOCATION_ID = "0123456789abcdef0123456789abcdef"
RECORDED_UNIT_NAME = "mesa-headless-gl-target-0123456789abcdef0123456789abcdef.service"
DIRECT_PROCESS_SIGNAL = re.compile(r"(?m)^\s*(?:sudo\s+)?(?:/\S+/)?(?:kill|pkill)\b")


def extract_teardown_script() -> str:
    document = RUNNER_DOCUMENT.read_text(encoding="utf-8")
    section = document.split(TEARDOWN_HEADING, maxsplit=1)[1]
    section = section.split("\n## ", maxsplit=1)[0]
    code_block = re.search(r"```sh\n(?P<script>.*?)\n```", section, re.DOTALL)
    assert code_block is not None
    return code_block.group("script")


def validate_no_direct_process_signal(script: str) -> None:
    if DIRECT_PROCESS_SIGNAL.search(script):
        raise ValueError("teardown must stop the run-scoped systemd unit")


def write_executable(path: Path, body: str) -> None:
    path.write_text(body, encoding="utf-8")
    path.chmod(0o755)


def write_unit_state(
    path: Path,
    *,
    load_state: str,
    active_state: str,
    main_pid: str,
    invocation_id: str,
) -> None:
    path.write_text(
        "\n".join(
            (
                f"LoadState={load_state}",
                f"ActiveState={active_state}",
                f"MainPID={main_pid}",
                f"InvocationID={invocation_id}",
                "",
            )
        ),
        encoding="utf-8",
    )


@pytest.fixture
def command_directory(tmp_path: Path) -> Path:
    command_root = tmp_path / "commands"
    command_root.mkdir()
    write_executable(
        command_root / "sudo",
        """#!/bin/sh
set -eu
case "${1:-}" in
   kill|pkill|*/kill|*/pkill)
      printf '%s %s\n' "${1##*/}" "${2:-}" >> "$FAKE_PROCESS_MUTATION_LOG"
      exit 0
      ;;
esac
exec "$@"
""",
    )
    write_executable(
        command_root / "sleep",
        """#!/bin/sh
set -eu
exit 0
""",
    )
    for process_command in ("kill", "pkill"):
        write_executable(
            command_root / process_command,
            """#!/bin/sh
set -eu
printf '%s %s\n' "${0##*/}" "$*" >> "$FAKE_PROCESS_MUTATION_LOG"
exit 0
""",
        )
    write_executable(
        command_root / "systemctl",
        """#!/bin/sh
set -eu
printf '%s\n' "$*" >> "$FAKE_SYSTEMCTL_LOG"
operation=$1
shift
case "$operation" in
   show)
      [ "${1:-}" = "$FAKE_XORG_UNIT" ] || exit 94
      cat "$FAKE_XORG_STATE"
      ;;
   stop)
      [ "${1:-}" = --no-block ] || exit 90
      [ "${2:-}" = "$FAKE_XORG_UNIT" ] || exit 94
      case "$FAKE_STOP_MODE" in
         inactive)
            printf '%s\n' \
               'LoadState=loaded' \
               'ActiveState=inactive' \
               'MainPID=0' \
               "InvocationID=$FAKE_LIVE_INVOCATION_ID" \
               > "$FAKE_XORG_STATE"
            ;;
         timeout)
            printf '%s\n' \
               'LoadState=loaded' \
               'ActiveState=deactivating' \
               "MainPID=$FAKE_LIVE_MAIN_PID" \
               "InvocationID=$FAKE_LIVE_INVOCATION_ID" \
               > "$FAKE_XORG_STATE"
            ;;
         *) exit 91 ;;
      esac
      ;;
   reset-failed|start)
      ;;
   is-active)
      [ "${1:-}" = --quiet ] || exit 92
      ;;
   *) exit 93 ;;
esac
""",
    )
    return command_root


def run_teardown(
    tmp_path: Path,
    command_directory: Path,
    *,
    load_state: str = "loaded",
    active_state: str = "active",
    live_main_pid: str = RECORDED_MAIN_PID,
    live_invocation_id: str = RECORDED_INVOCATION_ID,
    stop_mode: str = "inactive",
    recorded_unit_name: str = RECORDED_UNIT_NAME,
) -> tuple[subprocess.CompletedProcess[str], str]:
    evidence_directory = tmp_path / "evidence"
    evidence_directory.mkdir()
    (evidence_directory / "xorg-main-pid.txt").write_text(
        f"{RECORDED_MAIN_PID}\n", encoding="utf-8"
    )
    (evidence_directory / "xorg-invocation-id.txt").write_text(
        f"{RECORDED_INVOCATION_ID}\n", encoding="utf-8"
    )
    (evidence_directory / "xorg-unit-name.txt").write_text(
        f"{recorded_unit_name}\n", encoding="utf-8"
    )
    unit_state_path = tmp_path / "unit-state.txt"
    systemctl_log_path = tmp_path / "systemctl.log"
    process_mutation_log_path = tmp_path / "process-mutation.log"
    write_unit_state(
        unit_state_path,
        load_state=load_state,
        active_state=active_state,
        main_pid=live_main_pid,
        invocation_id=live_invocation_id,
    )

    environment = os.environ.copy()
    environment.pop("XORG_MAIN_PID", None)
    environment.pop("XORG_INVOCATION_ID", None)
    environment.pop("XORG_UNIT", None)
    environment.update(
        {
            "DISPLAY_MANAGER_UNIT": "display-manager.service",
            "EVIDENCE_DIR": str(evidence_directory),
            "FAKE_LIVE_INVOCATION_ID": live_invocation_id,
            "FAKE_LIVE_MAIN_PID": live_main_pid,
            "FAKE_PROCESS_MUTATION_LOG": str(process_mutation_log_path),
            "FAKE_STOP_MODE": stop_mode,
            "FAKE_SYSTEMCTL_LOG": str(systemctl_log_path),
            "FAKE_XORG_STATE": str(unit_state_path),
            "FAKE_XORG_UNIT": RECORDED_UNIT_NAME,
            "PATH": f"{command_directory}:{environment['PATH']}",
        }
    )
    teardown_script = extract_teardown_script()
    validate_no_direct_process_signal(teardown_script)
    completed = subprocess.run(
        ["/bin/sh", "-eu"],
        input=(
            "kill() { printf 'kill %s\\n' \"$*\" >> "
            '"$FAKE_PROCESS_MUTATION_LOG"; }\n' + teardown_script
        ),
        cwd=REPOSITORY_ROOT,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    systemctl_log = (
        systemctl_log_path.read_text(encoding="utf-8")
        if systemctl_log_path.exists()
        else ""
    )
    if process_mutation_log_path.exists():
        systemctl_log += process_mutation_log_path.read_text(encoding="utf-8")
    return completed, systemctl_log


def test_matching_unit_identity_stops_through_systemd(
    tmp_path: Path, command_directory: Path
) -> None:
    completed, systemctl_log = run_teardown(tmp_path, command_directory)

    assert completed.returncode == 0, completed.stderr
    assert f"stop --no-block {RECORDED_UNIT_NAME}" in systemctl_log
    assert "start display-manager.service" in systemctl_log
    assert "kill" not in systemctl_log


@pytest.mark.parametrize(
    ("load_state", "active_state"),
    (("loaded", "inactive"), ("not-found", "inactive")),
)
def test_inactive_or_removed_unit_ignores_reused_recorded_pid(
    tmp_path: Path,
    command_directory: Path,
    load_state: str,
    active_state: str,
) -> None:
    completed, systemctl_log = run_teardown(
        tmp_path,
        command_directory,
        load_state=load_state,
        active_state=active_state,
        live_main_pid="0",
        live_invocation_id="",
    )

    assert completed.returncode == 0, completed.stderr
    assert "stop --no-block" not in systemctl_log
    assert "start display-manager.service" in systemctl_log


@pytest.mark.parametrize(
    ("live_main_pid", "live_invocation_id"),
    (
        ("7777", RECORDED_INVOCATION_ID),
        (RECORDED_MAIN_PID, "fedcba9876543210fedcba9876543210"),
    ),
)
def test_reused_pid_or_replaced_unit_fails_without_mutation(
    tmp_path: Path,
    command_directory: Path,
    live_main_pid: str,
    live_invocation_id: str,
) -> None:
    completed, systemctl_log = run_teardown(
        tmp_path,
        command_directory,
        live_main_pid=live_main_pid,
        live_invocation_id=live_invocation_id,
    )

    assert completed.returncode == 2
    assert "identity mismatch; refusing stop" in completed.stderr
    assert "stop --no-block" not in systemctl_log
    assert "start display-manager.service" not in systemctl_log


def test_stop_timeout_keeps_display_manager_stopped(
    tmp_path: Path, command_directory: Path
) -> None:
    completed, systemctl_log = run_teardown(
        tmp_path, command_directory, stop_mode="timeout"
    )

    assert completed.returncode == 2
    assert "unit stop timed out" in completed.stderr
    assert f"stop --no-block {RECORDED_UNIT_NAME}" in systemctl_log
    assert "start display-manager.service" not in systemctl_log


def test_failed_unit_is_reset_before_display_manager_restart(
    tmp_path: Path, command_directory: Path
) -> None:
    completed, systemctl_log = run_teardown(
        tmp_path,
        command_directory,
        active_state="failed",
        live_main_pid="0",
    )

    assert completed.returncode == 0, completed.stderr
    reset_position = systemctl_log.index(f"reset-failed {RECORDED_UNIT_NAME}")
    start_position = systemctl_log.index("start display-manager.service")
    assert reset_position < start_position


def test_inactive_unit_with_live_main_pid_fails_closed(
    tmp_path: Path, command_directory: Path
) -> None:
    completed, systemctl_log = run_teardown(
        tmp_path,
        command_directory,
        active_state="inactive",
        live_main_pid=RECORDED_MAIN_PID,
    )

    assert completed.returncode == 2
    assert "inactive Xorg unit retains MainPID" in completed.stderr
    assert "start display-manager.service" not in systemctl_log


def test_invalid_recorded_unit_name_fails_before_systemctl(
    tmp_path: Path, command_directory: Path
) -> None:
    completed, systemctl_log = run_teardown(
        tmp_path,
        command_directory,
        recorded_unit_name="mesa-headless-gl-target-invalid.service",
    )

    assert completed.returncode == 2
    assert "invalid recorded Xorg unit name" in completed.stderr
    assert systemctl_log == ""


def test_direct_pid_signal_mutation_is_rejected() -> None:
    mutated_script = extract_teardown_script() + '\nsudo kill -TERM "$XORG_MAIN_PID"\n'

    with pytest.raises(
        ValueError, match="teardown must stop the run-scoped systemd unit"
    ):
        validate_no_direct_process_signal(mutated_script)
