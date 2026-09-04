# SPDX-License-Identifier: MIT
"""Keep the reserved refusal result out of the preparation layer's signals.

`R3V_NATIVE_REFUSAL_RESULT` is `VK_ERROR_UNKNOWN`, and its whole meaning is
public: it is the one error every native command's registry entry permits, so
an application reading it learns that the driver declined the command.  A
helper that returns it to its own caller spends that meaning on an internal
hand-off, and the caller can no longer tell a declined command from a
propagated placeholder.

The preparation layer therefore signals with typed verdicts --
`enum r3v_route_decision`, `enum r3v_submit_refusal`, and a `const char
**reason` -- and converts a verdict into a result class at its edge.  The API
boundary alone spells a `VkResult`.  This audit reads the layer's translation
units with comments removed and fails on any Vulkan result token, so a helper
that starts returning one reports here rather than at the first application
that reads an unexplained error.

Usage:
  r3v_internal_refusal_signal_audit.py --source PATH [--source PATH ...]
  r3v_internal_refusal_signal_audit.py --selftest
Exit 0 when no listed source names a Vulkan result.
"""

import argparse
import re
import sys
from pathlib import Path

# Tokens that name a Vulkan result in code.  VkResult is the type, VK_SUCCESS
# and the VK_ERROR_* set are its values, and vk_error/vk_errorf are the
# runtime helpers that produce one.
RESULT_PATTERN = re.compile(
    r"\b(VkResult|VK_SUCCESS|VK_ERROR_[A-Z0-9_]+|vk_errorf?|"
    r"vk_command_buffer_set_error)\b")


class AuditFailure(Exception):
    """A preparation-layer source names a Vulkan result."""


def strip_comments(text):
    """Remove C comments so a token named in prose reads as absent."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def named_results(text):
    """Every distinct Vulkan result token the code names, in sorted order."""
    return sorted({m.group(0) for m in RESULT_PATTERN.finditer(
        strip_comments(text))})


def audit(sources):
    """Verdict string, or AuditFailure naming the first source that carries one.

    sources is a sequence of (name, text) pairs.
    """
    if not sources:
        raise AuditFailure("no source named; the audit would pass vacuously")
    for name, text in sources:
        found = named_results(text)
        if found:
            raise AuditFailure(
                f"{name} names {', '.join(found)}; the preparation layer "
                f"signals with typed verdicts and the API boundary alone "
                f"spells a Vulkan result")
    return f"{len(sources)} preparation-layer sources name no Vulkan result"


CLEAN_SOURCE = """
/* A refusal here is a verdict; VkResult belongs at the API boundary. */
enum r3v_submit_refusal
r3v_example_check(const struct r3v_submit_census *c, const char **reason)
{
   if (c == NULL) {
      *reason = "census is absent";
      return R3V_SUBMIT_REFUSAL_PHASE_ORDER;
   }
   return R3V_SUBMIT_ADMITTED;
}
"""


def selftest():
    """Calibrate on a clean source and on each way a result reaches the layer."""
    verdict = audit([("clean.c", CLEAN_SOURCE)])
    if "no Vulkan result" not in verdict:
        raise AuditFailure(f"clean source reported {verdict!r}")

    known_bad = {
        "reserved refusal value":
            CLEAN_SOURCE.replace("return R3V_SUBMIT_ADMITTED;",
                                 "return VK_ERROR_UNKNOWN;"),
        "result type":
            CLEAN_SOURCE.replace("enum r3v_submit_refusal", "VkResult"),
        "success value":
            CLEAN_SOURCE.replace("return R3V_SUBMIT_ADMITTED;",
                                 "return VK_SUCCESS;"),
        "runtime helper":
            CLEAN_SOURCE.replace('*reason = "census is absent";',
                                 'vk_errorf(device, "census is absent");'),
        "record poison":
            CLEAN_SOURCE.replace('*reason = "census is absent";',
                                 "vk_command_buffer_set_error(cmd, 0);"),
    }
    for label, text in known_bad.items():
        try:
            passed = audit([("mutated.c", text)])
        except AuditFailure:
            continue
        raise AuditFailure(
            f"known-bad source ({label}) admitted with {passed!r}")

    # A comment naming the type is prose, not a signal.
    audit([("commented.c",
            "/* This file names no VkResult and returns VK_ERROR_UNKNOWN "
            "nowhere. */\nint f(void) { return 0; }\n")])

    # An empty source list would pass every rule by having nothing to break.
    try:
        audit([])
    except AuditFailure:
        return ("selftest: clean source admitted, five known-bad sources and "
                "the empty list refused")
    raise AuditFailure("the empty source list was admitted")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, action="append", default=[])
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    try:
        if args.selftest:
            verdict = selftest()
        elif args.source:
            verdict = audit([(str(p), p.read_text()) for p in args.source])
        else:
            parser.error("--source PATH or --selftest")
            return 2
    except AuditFailure as exc:
        print(f"r3v-internal-refusal-signal: {exc}", file=sys.stderr)
        return 1
    print(f"r3v-internal-refusal-signal: {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
