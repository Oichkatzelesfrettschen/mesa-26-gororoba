# SPDX-License-Identifier: MIT
#
# Calibrates radeon_noop_drm_shim_residue_check.py's identity-mismatch
# tightening against known-good and known-bad inputs, standing in for a
# real shim-test subprocess so the checker's normalize() logic is
# exercised without a build of the shim itself.
#
# A FAIL: override line naming a foreign vendor (0x10de) must fail the
# check even though digit-run normalization alone would collapse it onto
# the shim's own vendor (0x1002); this is the class that let a
# claimed-namespace mapping miss fall through to the host's own DRM node
# and pass. A decimal-only value inside an unrelated FAIL: line -- a pid,
# fd, or errno -- keeps its normalization, so the same defect class with a
# different value still matches the recorded signature.

import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
CHECKER = HERE / "radeon_noop_drm_shim_residue_check.py"
SIGNATURE = HERE / "state_token_residue_full_selector.signature"

sys.path.insert(0, str(HERE))
import radeon_noop_drm_shim_residue_check as residue_check  # noqa: E402


class CalibrationFailure(RuntimeError):
    """Reports a calibration failure without relying on assert statements."""


def require(condition, message):
    if not condition:
        raise CalibrationFailure(message)


def read_signature_lines():
    with open(SIGNATURE, encoding="utf-8") as handle:
        return [line.rstrip("\n") for line in handle if line.startswith("FAIL:")]


def emitter_command(lines):
    """A stand-in shim-test subprocess: prints the given FAIL: lines to
    stderr and exits 1, matching the contract the checker expects from
    radeon_noop_drm_shim_test.  The writes preserve embedded newlines from
    test_text_equals so the multiline diagnostic parser is exercised."""
    script = (
        "\n".join(
            "__import__('sys').stderr.write({!r})".format(
                line if line.endswith("\n") else line + "\n"
            )
            for line in lines
        )
        + "\n__import__('sys').exit(1)"
    )
    return [sys.executable, "-c", script]


def run_checker(command, signature=SIGNATURE):
    return subprocess.run(
        [sys.executable, str(CHECKER), "--signature", str(signature), "--", *command],
        capture_output=True,
        text=True,
    )


def known_good_signature_replay():
    """The recorded signature, replayed verbatim, passes."""
    result = run_checker(emitter_command(read_signature_lines()))
    require(
        result.returncode == 0,
        "known-good signature replay failed:\n{}".format(result.stderr),
    )


def multiline_identity(observed):
    return (
        'FAIL: override /sys/dev/char/226:128/device/vendor contains "{}\n" '
        'instead of "0x1002\n"\n'.format(observed)
    )


def known_bad_multiline_identity():
    """A foreign vendor in a multiline record defeats digit-only matching.

    The temporary signature carries the same multiline shape as the
    producer.  The old splitlines-based checker would normalize the first
    physical line in both records and pass; the logical-record checker keeps
    the quoted identity and must reject the 0x10de mutation.
    """
    with tempfile.TemporaryDirectory() as directory:
        signature = Path(directory) / "signature"
        signature.write_text(multiline_identity("0x1002"), encoding="utf-8")
        result = run_checker(emitter_command([multiline_identity("0x10de")]), signature)
    require(
        result.returncode == 1,
        "multiline foreign-vendor identity passed the residue check",
    )
    require(
        "failure outside the signature" in result.stderr,
        "checker did not report the multiline identity as a new "
        "failure:\n{}".format(result.stderr),
    )


def normalize_path_still_varies():
    """normalize() keeps digit-run normalization on the override path
    itself (a numbered PCI domain or char-device major:minor pair varies
    by host and enumeration order), while the quoted vendor content on
    each side of "instead of" stays exact."""
    first = residue_check.normalize(
        "FAIL: override /sys/dev/char/226:128/device/vendor contains "
        '"0x1002\n" instead of "0x1002\n"'
    )
    second = residue_check.normalize(
        "FAIL: override /sys/dev/char/226:129/device/vendor contains "
        '"0x1002\n" instead of "0x1002\n"'
    )
    require(
        first == second,
        "the override path did not keep digit-run normalization:\n"
        "{!r}\n{!r}".format(first, second),
    )
    third = residue_check.normalize(
        "FAIL: override /sys/dev/char/226:128/device/vendor contains "
        '"0x10de\n" instead of "0x1002\n"'
    )
    require(
        first != third,
        "a foreign vendor id normalized identically to the shim's own "
        "vendor id:\n{!r}\n{!r}".format(first, third),
    )


def known_good_legitimate_variance():
    """A decimal-only value inside an unrelated FAIL: line -- pid, fd, or
    errno -- keeps its normalization: the signature still matches when
    that value differs between runs."""
    lines = read_signature_lines()
    mutated = list(lines)
    substituted = False
    for index, line in enumerate(mutated):
        replaced, count = re.subn(r"\d+", "999999", line, count=1)
        if count:
            mutated[index] = replaced
            substituted = True
            break
    require(substituted, "no decimal value found to mutate for the arm")
    result = run_checker(emitter_command(mutated))
    require(
        result.returncode == 0,
        "legitimate decimal-value variance failed the residue check:\n{}".format(
            result.stderr
        ),
    )


def main():
    try:
        known_good_signature_replay()
        known_bad_multiline_identity()
        normalize_path_still_varies()
        known_good_legitimate_variance()
    except CalibrationFailure as error:
        print("FAIL: {}".format(error), file=sys.stderr)
        return 1
    print("residue-checker identity calibration: 4/4 arms passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
