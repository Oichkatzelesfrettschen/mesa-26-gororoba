#!/usr/bin/env python3
"""Calibrate display-register generator validation in normal and optimized Python."""

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).parents[5]
GENERATOR = ROOT / "src/gallium/drivers/r300/r300_display_reg_gen.py"
DISPLAY_HEADER = ROOT / "src/gallium/drivers/r300/r300_display_reg.h"
R300_REG_HEADER = ROOT / "src/amd/r300/common/r300_reg.h"


def load_generator():
    spec = importlib.util.spec_from_file_location("r300_display_reg_gen", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def expect_parse_rejection(module, lines, text):
    try:
        module.parse_blocks(lines)
    except module.GeneratorError as error:
        if text not in str(error):
            raise AssertionError(str(error))
    else:
        raise AssertionError("accepted invalid source: %s" % text)


def run_generator(source, output, *extra):
    interpreter = [sys.executable]
    if sys.flags.optimize:
        interpreter.append("-O")
    return subprocess.run(
        interpreter + [str(GENERATOR), str(source), str(output), *map(str, extra)],
        capture_output=True,
        text=True,
        check=False,
    )


def run_cases():
    module = load_generator()
    module.parse_blocks(
        ["#define RADEON_CRTC_FOO 0x10", "#define RADEON_CRTC_FOO 0x10"]
    )
    expect_parse_rejection(
        module,
        ["#define RADEON_CRTC_FOO 0x10", "#define RADEON_CRTC_FOO 0x20"],
        "conflicting duplicate definition",
    )
    expect_parse_rejection(
        module,
        ["#define RADEON_CRTC_FOO RADEON_BASE + 4"],
        "unsupported register expression",
    )
    expect_parse_rejection(
        module,
        [
            "#define RADEON_CRTC_FOO 0x10",
            "#       define RADEON_CRTC_FOO (1 << 0)",
        ],
        "conflicting duplicate definition",
    )
    expect_parse_rejection(
        module,
        [
            "#define RADEON_CRTC_A 0x10",
            "#       define RADEON_CRTC_SHARED (1 << 0)",
            "#define RADEON_CRTC_B 0x20",
            "#       define RADEON_CRTC_SHARED (1 << 0)",
        ],
        "register-name collision",
    )

    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        valid_output = directory / "valid.h"
        result = run_generator(DISPLAY_HEADER, valid_output, R300_REG_HEADER)
        if result.returncode != 0:
            raise AssertionError(result.stderr)
        if valid_output.read_bytes() != DISPLAY_HEADER.read_bytes():
            raise AssertionError("valid regeneration changed the generated header")

        moved_source = directory / "moved.h"
        moved_source.write_bytes(DISPLAY_HEADER.read_bytes() + b"\n#define RADEON_CRTC_EXTRA 0x7000\n")
        result = run_generator(moved_source, directory / "count.h")
        if result.returncode == 0 or "register count" not in result.stderr:
            raise AssertionError(result.stderr)

        collision_header = directory / "collision.h"
        collision_header.write_text("#define RADEON_CRTC_GEN_CNTL 0x0050\n")
        result = run_generator(DISPLAY_HEADER, directory / "collision-out.h", collision_header)
        if result.returncode == 0 or "name collisions" not in result.stderr:
            raise AssertionError(result.stderr)


def main():
    if os.environ.get("R300_DISPLAY_REG_GEN_CHILD") == "1":
        run_cases()
        return
    for optimized in (False, True):
        environment = os.environ.copy()
        environment["R300_DISPLAY_REG_GEN_CHILD"] = "1"
        command = [sys.executable]
        if optimized:
            command.append("-O")
        command.append(__file__)
        result = subprocess.run(command, env=environment, capture_output=True, text=True)
        if result.returncode != 0:
            raise AssertionError(result.stderr)


if __name__ == "__main__":
    main()
