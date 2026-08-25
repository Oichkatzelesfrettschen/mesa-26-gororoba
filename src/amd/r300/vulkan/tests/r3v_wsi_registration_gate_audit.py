# SPDX-License-Identifier: MIT
"""Hold the WSI surface-contract registrations to their meson guard.

`docs/hardware/r3v-wsi-denominator.md`, "Per-host-class dEQP
applicability", names a host missing `libxcb` development headers or
`Xvfb` as registering zero test binaries from
`r3v_native_wsi_surface_contract.c`, not a skip result: the
`meson.build` `if dep_xcb.found() and prog_xvfb.found()` block (`rg -n
"dep_xcb.found\\(\\) and prog_xvfb.found\\(\\)"
src/amd/r300/vulkan/meson.build`) is what makes that true, because a
registration outside the block runs unconditionally regardless of
whether the dependency it needs is present.  A dropped guard silently
turns "not registered" into "registered and failing" on every host
missing `libxcb` or `Xvfb`; this audit reads `meson.build` as text and
fails the moment any of the surface-contract registrations sits outside
the block, without needing a configured build to observe it.

`r3v-xvfb-wrapper-signal-cleanup` (`meson.build`, immediately above the
guard) tests the wrapper script's own signal-cleanup logic against
plain `true`/`false` shell commands and needs neither `libxcb` nor a
real `Xvfb` display, so it registers unconditionally by design; the
audit asserts that registration stays outside the guard exactly as it
asserts the others stay inside it, so neither direction can drift
unnoticed.

Usage:
  r3v_wsi_registration_gate_audit.py --meson-build PATH
  r3v_wsi_registration_gate_audit.py --selftest
Exit 0 when every guarded registration sits inside the block and the
unconditional one sits outside it.
"""

import argparse
import re
import sys
from pathlib import Path

GUARD_TOKENS = ("dep_xcb.found()", "prog_xvfb.found()")

# Each entry is a literal substring that names one registration this
# check locates by line; GUARDED entries must sit inside the guard
# block, UNGUARDED entries must sit outside every guard block.
GUARDED_MARKERS = [
    "r3v_native_wsi_surface_contract = executable(",
    "'r3v-native-wsi-surface-contract',",
    "'r3v-native-wsi-surface-contract-known-bad-@0@'.format(wsi_query)",
    "'r3v-xvfb-wrapper-passthrough-success',",
    "'r3v-xvfb-wrapper-passthrough-failure',",
    "'r3v-xvfb-wrapper-passthrough-skip',",
    "'r3v-xvfb-wrapper-infrastructure-refusal',",
]
UNGUARDED_MARKERS = [
    "'r3v-xvfb-wrapper-signal-cleanup',",
]

BLOCK_OPEN = {"if": "endif", "foreach": "endforeach"}
BLOCK_CLOSE = {v: k for k, v in BLOCK_OPEN.items()}


class AuditFailure(Exception):
    """A registration sits on the wrong side of the guard."""


class ParseFailure(Exception):
    """meson.build's if/foreach nesting could not be read as balanced."""


def parse_if_blocks(text):
    """Return a list of (condition, start_line, end_line) for every `if`
    block in text, 1-indexed and inclusive of the lines strictly between
    the `if` and its matching `endif`.  Nesting through `foreach` is
    tracked so an `if` inside a `foreach` (or the reverse) still finds
    its own matching close."""
    lines = text.splitlines()
    stack = []
    blocks = []
    for line_no, line in enumerate(lines, start=1):
        open_match = re.match(r"^\s*(if|foreach)\b(.*)$", line)
        close_match = re.match(r"^\s*(endif|endforeach)\b", line)
        if open_match:
            kind, rest = open_match.group(1), open_match.group(2)
            stack.append([kind, rest.split("#", 1)[0].strip(), line_no])
        elif close_match:
            kind = BLOCK_CLOSE[close_match.group(1)]
            if not stack or stack[-1][0] != kind:
                raise ParseFailure(
                    f"line {line_no}: {close_match.group(1)!r} closes no "
                    f"open {kind!r} block")
            opened_kind, condition, start_line = stack.pop()
            if opened_kind == "if":
                blocks.append((condition, start_line + 1, line_no - 1))
    if stack:
        raise ParseFailure(
            f"{len(stack)} block(s) never close: "
            + ", ".join(f"{kind} opened at line {line}"
                        for kind, _cond, line in stack))
    return blocks


def guard_blocks(blocks):
    """The if-blocks whose condition names both GUARD_TOKENS."""
    return [(start, end) for condition, start, end in blocks
            if all(token in condition for token in GUARD_TOKENS)]


def marker_line(text, marker):
    lines = text.splitlines()
    for line_no, line in enumerate(lines, start=1):
        if marker in line:
            return line_no
    return None


def inside_any(line_no, ranges):
    return any(start <= line_no <= end for start, end in ranges)


def audit(text):
    try:
        blocks = parse_if_blocks(text)
    except ParseFailure as exc:
        raise AuditFailure(f"meson.build if/foreach nesting is unreadable: "
                           f"{exc}") from exc
    guards = guard_blocks(blocks)
    if not guards:
        raise AuditFailure(
            "no if-block names both "
            + " and ".join(GUARD_TOKENS)
            + "; the guard that keeps the WSI surface-contract "
              "registrations from running on a host missing libxcb or "
              "Xvfb is gone")

    problems = []
    for marker in GUARDED_MARKERS:
        line_no = marker_line(text, marker)
        if line_no is None:
            problems.append(f"{marker!r} does not appear in meson.build")
            continue
        if not inside_any(line_no, guards):
            problems.append(
                f"line {line_no} ({marker!r}) registers outside every "
                f"dep_xcb.found()/prog_xvfb.found() guard block, so it "
                f"would run on a host missing libxcb or Xvfb")
    for marker in UNGUARDED_MARKERS:
        line_no = marker_line(text, marker)
        if line_no is None:
            problems.append(f"{marker!r} does not appear in meson.build")
            continue
        if inside_any(line_no, guards):
            problems.append(
                f"line {line_no} ({marker!r}) now registers inside a "
                f"dep_xcb.found()/prog_xvfb.found() guard block, which "
                f"skips its wrapper-script self-test on a host that "
                f"needs no real display for it")
    if problems:
        raise AuditFailure("; ".join(problems))
    return True


MESON_FIXTURE_TEMPLATE = """
prog_xvfb = find_program('Xvfb', required : false)
r3v_xvfb_wrapper = files('tests/run_under_xvfb.sh')
test(
  'r3v-xvfb-wrapper-signal-cleanup',
  prog_python,
  args : [files('tests/r3v_xvfb_wrapper_signal_cleanup.py'),
          r3v_xvfb_wrapper],
  suite : ['r3v'],
)
{guard_open}
  test(
    'r3v-xvfb-wrapper-passthrough-success',
    r3v_xvfb_wrapper,
    args : ['true'],
    suite : ['r3v'],
  )
  test(
    'r3v-xvfb-wrapper-passthrough-failure',
    r3v_xvfb_wrapper,
    args : ['false'],
    should_fail : true,
    suite : ['r3v'],
  )
  test(
    'r3v-xvfb-wrapper-passthrough-skip',
    r3v_xvfb_wrapper,
    args : ['sh', '-c', 'exit 77'],
    suite : ['r3v'],
  )
  test(
    'r3v-xvfb-wrapper-infrastructure-refusal',
    find_program('sh'),
    args : ['-c', 'true'],
    suite : ['r3v'],
  )
  r3v_native_wsi_surface_contract = executable(
    'r3v_native_wsi_surface_contract',
    files('tests/r3v_native_wsi_surface_contract.c'),
  )
  test(
    'r3v-native-wsi-surface-contract',
    r3v_xvfb_wrapper,
    args : [r3v_native_wsi_surface_contract.full_path()],
    suite : ['r3v'],
  )
  foreach wsi_query : ['capabilities', 'capabilities-error']
    test(
      'r3v-native-wsi-surface-contract-known-bad-@0@'.format(wsi_query),
      r3v_xvfb_wrapper,
      suite : ['r3v'],
    )
  endforeach
{guard_close}
"""


def selftest():
    checks = []

    def check(name, ok):
        checks.append((name, ok))

    guarded_text = MESON_FIXTURE_TEMPLATE.format(
        guard_open="if dep_xcb.found() and prog_xvfb.found()",
        guard_close="endif")
    check("passes the guarded tree", audit(guarded_text))

    def refuses(text, fragment):
        try:
            audit(text)
        except AuditFailure as exc:
            return fragment in str(exc)
        return False

    dropped_guard_text = MESON_FIXTURE_TEMPLATE.format(
        guard_open="", guard_close="")
    check("refuses a dropped guard",
          refuses(dropped_guard_text, "no if-block names both"))

    reordered_guard_text = MESON_FIXTURE_TEMPLATE.format(
        guard_open="if prog_xvfb.found() and dep_xcb.found()",
        guard_close="endif")
    check("admits the guard tokens in either order",
          audit(reordered_guard_text))

    wrong_guard_text = MESON_FIXTURE_TEMPLATE.format(
        guard_open="if dep_xcb.found()", guard_close="endif")
    check("refuses a guard missing prog_xvfb.found()",
          refuses(wrong_guard_text, "no if-block names both"))

    moved_signal_cleanup = guarded_text.replace(
        "test(\n  'r3v-xvfb-wrapper-signal-cleanup',",
        "if dep_xcb.found() and prog_xvfb.found()\ntest(\n"
        "  'r3v-xvfb-wrapper-signal-cleanup',").replace(
        "  args : [files('tests/r3v_xvfb_wrapper_signal_cleanup.py'),\n"
        "          r3v_xvfb_wrapper],\n"
        "  suite : ['r3v'],\n"
        ")\nif dep_xcb.found() and prog_xvfb.found()",
        "  args : [files('tests/r3v_xvfb_wrapper_signal_cleanup.py'),\n"
        "          r3v_xvfb_wrapper],\n"
        "  suite : ['r3v'],\n"
        ")\nendif\nif dep_xcb.found() and prog_xvfb.found()")
    check("refuses the unconditional wrapper self-test moved inside "
          "the guard",
          refuses(moved_signal_cleanup, "signal-cleanup"))

    check("refuses unbalanced if/endif nesting",
          refuses("if dep_xcb.found() and prog_xvfb.found()\n",
                  "never close"))
    check("refuses an endif with no matching if",
          refuses("endif\n", "closes no open"))

    unnamed_registration = guarded_text.replace(
        "  r3v_native_wsi_surface_contract = executable(\n"
        "    'r3v_native_wsi_surface_contract',\n"
        "    files('tests/r3v_native_wsi_surface_contract.c'),\n"
        "  )\n", "")
    check("refuses a tree where the target definition itself vanished",
          refuses(unnamed_registration,
                  "does not appear in meson.build"))

    failed = [n for n, ok in checks if not ok]
    for name, ok in checks:
        print(f"  {'ok  ' if ok else 'FAIL'} {name}")
    print(f"{len(checks) - len(failed)}/{len(checks)} checks pass")
    return 1 if failed else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--meson-build", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.meson_build is None:
        parser.error("--meson-build or --selftest is required")
    if not args.meson_build.is_file():
        print(f"r3v-wsi-registration-gate: {args.meson_build} is not a "
              f"file", file=sys.stderr)
        return 2
    try:
        audit(args.meson_build.read_text())
    except AuditFailure as exc:
        print(f"r3v-wsi-registration-gate: {exc}", file=sys.stderr)
        return 1
    print("r3v-wsi-registration-gate: every surface-contract registration "
         "sits inside the dep_xcb.found()/prog_xvfb.found() guard, and "
         "the wrapper self-test sits outside it")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
