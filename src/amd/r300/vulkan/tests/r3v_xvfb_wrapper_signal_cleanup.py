# SPDX-License-Identifier: MIT
"""Calibrate Xvfb wrapper cleanup when the parent receives SIGTERM."""

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


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        return fail("expected the wrapper path")

    wrapper = pathlib.Path(argv[0])
    with tempfile.TemporaryDirectory(prefix="r3v-xvfb-signal-") as tmp:
        root = pathlib.Path(tmp)
        mock = root / "mock-xvfb"
        pid_file = root / "mock.pid"
        mock.write_text(
            "#!/bin/sh\n"
            "echo $$ > \"$MOCK_PID_FILE\"\n"
            "printf '99\\n' >&9\n"
            "trap 'exit 0' TERM INT\n"
            "while :; do sleep 1; done\n"
        )
        mock.chmod(0o755)
        environment = os.environ | {
            "MOCK_PID_FILE": str(pid_file),
            "R3V_XVFB_BINARY": str(mock),
            "TMPDIR": str(root),
        }
        process = subprocess.Popen(
            [str(wrapper), "sh", "-c", "sleep 30"],
            env=environment,
        )
        mock_pid = None
        try:
            for _ in range(100):
                if pid_file.exists() and pid_file.read_text().strip():
                    break
                time.sleep(0.05)
            if not pid_file.exists():
                return fail("mock Xvfb did not start")
            mock_pid = int(pid_file.read_text())
            process.send_signal(signal.SIGTERM)
            status = process.wait(timeout=5)
            if status != 143:
                return fail(f"wrapper returned {status}, expected 143")
            for _ in range(100):
                try:
                    os.kill(mock_pid, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.05)
            else:
                return fail("mock Xvfb remains after wrapper SIGTERM")
            if list(root.glob("r3v-xvfb.*")):
                return fail("displayfd directory remains after wrapper SIGTERM")
            print("r3v_xvfb_wrapper_signal_cleanup: signal cleanup OK")
            return 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            if mock_pid is not None:
                try:
                    os.kill(mock_pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass


if __name__ == "__main__":
    raise SystemExit(main())
