# SPDX-License-Identifier: MIT
"""Calibrate Xvfb wrapper cleanup and inherited-stdin preservation."""

import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time


def fail(message):
    print(f"r3v_xvfb_wrapper_signal_cleanup: {message}", file=sys.stderr)
    return 1


def process_is_live(pid):
    """Return whether a PID still represents a running process."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    stat_path = pathlib.Path(f"/proc/{pid}/stat")
    try:
        fields = stat_path.read_text().split()
    except FileNotFoundError:
        return True
    return len(fields) < 3 or fields[2] != "Z"


def wait_for_pid_exit(pid):
    for _ in range(100):
        if not process_is_live(pid):
            return True
        time.sleep(0.05)
    return False


def write_mock(root):
    mock = root / "mock-xvfb"
    mock.write_text(
        "#!/bin/sh\n"
        'echo $$ > "$MOCK_PID_FILE"\n'
        "printf '99\\n' >&9\n"
        "trap 'exit 0' TERM INT\n"
        "while :; do sleep 1; done\n"
    )
    mock.chmod(0o755)
    return mock


def wait_for_file(path):
    for _ in range(100):
        if path.exists() and path.read_text().strip():
            return True
        time.sleep(0.05)
    return False


def run_signal_cleanup(wrapper):
    with tempfile.TemporaryDirectory(prefix="r3v-xvfb-signal-") as tmp:
        root = pathlib.Path(tmp)
        mock_pid_file = root / "mock.pid"
        descendant_pid_file = root / "descendant.pid"
        mock = write_mock(root)
        environment = os.environ | {
            "MOCK_PID_FILE": str(mock_pid_file),
            "COMMAND_DESCENDANT_PID_FILE": str(descendant_pid_file),
            "R3V_XVFB_BINARY": str(mock),
            "TMPDIR": str(root),
        }
        command = (
            "sleep 30 & child=$!; "
            'printf \'%s\\n\' "$child" > "$COMMAND_DESCENDANT_PID_FILE"; '
            'wait "$child"'
        )
        process = subprocess.Popen(
            [str(wrapper), "sh", "-c", command],
            env=environment,
        )
        mock_pid = None
        descendant_pid = None
        try:
            if not wait_for_file(mock_pid_file):
                return fail("mock Xvfb did not start")
            if not wait_for_file(descendant_pid_file):
                return fail("wrapped descendant did not start")
            mock_pid = int(mock_pid_file.read_text())
            descendant_pid = int(descendant_pid_file.read_text())
            process.send_signal(signal.SIGTERM)
            status = process.wait(timeout=5)
            if status != 143:
                return fail(f"wrapper returned {status}, expected 143")
            if not wait_for_pid_exit(mock_pid):
                return fail("mock Xvfb remains after wrapper SIGTERM")
            if not wait_for_pid_exit(descendant_pid):
                return fail("wrapped descendant remains after wrapper SIGTERM")
            if list(root.glob("r3v-xvfb.*")):
                return fail("displayfd directory remains after wrapper SIGTERM")
            print("r3v_xvfb_wrapper_signal_cleanup: process-group cleanup OK")
            return 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            for pid in (mock_pid, descendant_pid):
                if pid is not None and process_is_live(pid):
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass


def run_stdin_case(wrapper, payload, expected_status):
    with tempfile.TemporaryDirectory(prefix="r3v-xvfb-stdin-") as tmp:
        root = pathlib.Path(tmp)
        mock_pid_file = root / "mock.pid"
        mock = write_mock(root)
        environment = os.environ | {
            "MOCK_PID_FILE": str(mock_pid_file),
            "R3V_XVFB_BINARY": str(mock),
            "TMPDIR": str(root),
        }
        command = 'IFS= read -r value && [ "$value" = payload ]'
        process = subprocess.Popen(
            [str(wrapper), "sh", "-c", command],
            env=environment,
            stdin=subprocess.PIPE if payload is not None else subprocess.DEVNULL,
        )
        mock_pid = None
        try:
            if not wait_for_file(mock_pid_file):
                return fail("mock Xvfb did not start for stdin case")
            mock_pid = int(mock_pid_file.read_text())
            if payload is None:
                status = process.wait(timeout=5)
            else:
                process.communicate(input=payload.encode(), timeout=5)
                status = process.returncode
            if status != expected_status:
                return fail(f"stdin case returned {status}, expected {expected_status}")
            if list(root.glob("r3v-xvfb.*")):
                return fail("displayfd directory remains after stdin case")
            if not wait_for_pid_exit(mock_pid):
                return fail("mock Xvfb remains after stdin case")
            return 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            if mock_pid is not None and process_is_live(mock_pid):
                try:
                    os.kill(mock_pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        return fail("expected the wrapper path")

    wrapper = pathlib.Path(argv[0])
    if run_signal_cleanup(wrapper) != 0:
        return 1
    if run_stdin_case(wrapper, "payload\n", 0) != 0:
        return 1
    if run_stdin_case(wrapper, None, 1) != 0:
        return 1
    print("r3v_xvfb_wrapper_signal_cleanup: stdin preservation OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
