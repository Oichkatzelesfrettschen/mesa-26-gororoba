# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT_PATH = Path(__file__).resolve().parent / "mount_boundary_calibration.py"
MODULE_SPEC = importlib.util.spec_from_file_location(
    "mount_boundary_calibration", SCRIPT_PATH
)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
mount_boundary_calibration = importlib.util.module_from_spec(MODULE_SPEC)
sys.modules[MODULE_SPEC.name] = mount_boundary_calibration
MODULE_SPEC.loader.exec_module(mount_boundary_calibration)
del sys.modules[MODULE_SPEC.name]


def test_accepts_caller_mapped_private_mount_namespace() -> None:
    mount_boundary_calibration.validate_namespace_isolation(
        ["0", "1000", "1"],
        "mnt:[4026533000]",
        "mnt:[4026531832]",
    )


@pytest.mark.parametrize(
    ("uid_fields", "process_namespace", "caller_namespace", "diagnostic"),
    (
        (["0", "0", "4294967295"], "mnt:[2]", "mnt:[1]", "user namespace"),
        (["0", "1000", "1"], "mnt:[1]", "mnt:[1]", "mount namespace"),
        (["0", "1000", "1"], "mnt:[2]", "", "caller mount namespace"),
        (["0", "1000", "1"], "mnt:[2]", "pid:[1]", "caller mount namespace"),
        (["0", "1000", "1"], "invalid", "mnt:[1]", "calibration mount namespace"),
    ),
)
def test_rejects_unproven_namespace_isolation(
    uid_fields: list[str],
    process_namespace: str,
    caller_namespace: str,
    diagnostic: str,
) -> None:
    with pytest.raises(SystemExit, match=diagnostic):
        mount_boundary_calibration.validate_namespace_isolation(
            uid_fields,
            process_namespace,
            caller_namespace,
        )


def test_create_audit_root_honors_tmpdir(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    selected_temporary_root = tmp_path / "selected-temporary-root"
    selected_temporary_root.mkdir()
    monkeypatch.setenv("TMPDIR", str(selected_temporary_root))
    monkeypatch.setattr(mount_boundary_calibration.tempfile, "tempdir", None)

    audit_root = mount_boundary_calibration.create_audit_root()

    assert audit_root.parent == selected_temporary_root
    assert audit_root.name.startswith("mesa-mount-boundary.")


@pytest.mark.parametrize("checkout_parent", ("tmp", "opt", "workspace"))
@pytest.mark.parametrize("bind_behavior", ("restore", "omit", "fail"))
def test_namespace_launcher_preserves_selected_control_checkout(
    tmp_path: Path,
    checkout_parent: str,
    bind_behavior: str,
) -> None:
    """Rename-backed mounts calibrate descriptor lifetime without privileges."""
    makefile = (SCRIPT_PATH.parent.parent / "Makefile").read_text()
    launcher = makefile.split("unshare --user --map-root-user --mount bash -c '", 1)[
        1
    ].split("' \\\n", 1)[0]
    launcher = launcher.replace("$$", "$")
    for mount_name in ("tmp", "opt"):
        mount_point = tmp_path / mount_name
        mount_point.mkdir()
        launcher = launcher.replace(f"/{mount_name}", str(mount_point))

    control_root = tmp_path / checkout_parent / "selected-control"
    control_root.mkdir(parents=True)
    sentinel = control_root / "sentinel"
    sentinel.write_text("retained source bytes\n")
    sentinel_stat = sentinel.stat()
    calibration = control_root / "calibration.py"
    calibration.write_text(
        "from pathlib import Path\n"
        "import sys\n"
        "control_root = Path(sys.argv[2])\n"
        "assert (control_root / 'sentinel').read_text() == 'retained source bytes\\n'\n"
        "print('calibration reached selected control checkout')\n"
    )

    binaries = tmp_path / "bin"
    binaries.mkdir()
    mount = binaries / "mount"
    mount.write_text(
        f"#!{sys.executable}\n"
        "import os\n"
        "from pathlib import Path\n"
        "import sys\n"
        "arguments = sys.argv[1:]\n"
        "if arguments == ['--make-rprivate', '/']:\n"
        "    sys.exit(0)\n"
        "target = Path(arguments[-1])\n"
        "allowed_root = Path(os.environ['FIXTURE_ROOT'])\n"
        "assert target.is_relative_to(allowed_root)\n"
        "if arguments[0] == '-t':\n"
        "    target.rename(target.with_name(target.name + '-underlay'))\n"
        "    target.mkdir()\n"
        "elif arguments[:2] == ['--no-canonicalize', '--bind']:\n"
        "    source = Path(arguments[2])\n"
        "    assert str(source).startswith('/proc/self/fd/')\n"
        "    assert (source / 'sentinel').read_text() == 'retained source bytes\\n'\n"
        "    behavior = os.environ['BIND_BEHAVIOR']\n"
        "    if behavior == 'fail':\n"
        "        print('forced bind failure', file=sys.stderr)\n"
        "        sys.exit(19)\n"
        "    if behavior == 'restore':\n"
        "        target.rmdir()\n"
        "        target.symlink_to(source.resolve(), target_is_directory=True)\n"
        "else:\n"
        "    raise AssertionError(arguments)\n"
    )
    mount.chmod(0o755)
    environment = dict(os.environ)
    environment.update(
        PATH=str(binaries) + os.pathsep + environment["PATH"],
        FIXTURE_ROOT=str(tmp_path),
        BIND_BEHAVIOR=bind_behavior,
        PYTHON=sys.executable,
        PYTHONDONTWRITEBYTECODE="1",
    )
    result = subprocess.run(
        ["bash", "-c", launcher, "_", str(calibration), str(control_root)],
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    checkout_overlaid = checkout_parent in ("tmp", "opt")
    if checkout_overlaid and bind_behavior != "restore":
        assert result.returncode != 0
        assert "calibration reached" not in result.stdout
        if bind_behavior == "fail":
            assert "forced bind failure" in result.stderr
        else:
            assert "can't open file" in result.stderr
    else:
        assert result.returncode == 0, result.stderr
        assert "calibration reached selected control checkout" in result.stdout

    retained_root = (
        tmp_path / (checkout_parent + "-underlay") / control_root.name
        if checkout_overlaid
        else control_root
    )
    retained_sentinel = retained_root / "sentinel"
    assert retained_sentinel.read_text() == "retained source bytes\n"
    assert retained_sentinel.stat().st_ino == sentinel_stat.st_ino
    assert retained_sentinel.stat().st_dev == sentinel_stat.st_dev
