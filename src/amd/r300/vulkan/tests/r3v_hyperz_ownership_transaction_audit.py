#!/usr/bin/env python3

"""Audit HyperZ grant rollback at the R3V submission boundary.

The kernel binds HyperZ ownership to the DRM file descriptor.  R3V may acquire
that grant while preparing a command stream, but a grant first acquired for one
submission must be released when every CS ioctl remains unaccepted.  A grant
that predates the submission, or one followed by an accepted CS ioctl, remains
owned until device destruction.

Usage:
  r3v_hyperz_ownership_transaction_audit.py --queue PATH
  r3v_hyperz_ownership_transaction_audit.py --selftest
"""

import argparse
import sys
from pathlib import Path


class AuditFailure(Exception):
    """The queue source violates the descriptor-grant transaction."""


def strip_comments(text):
    """Remove C comments so prose cannot satisfy a mechanism check."""
    output = []
    offset = 0
    while offset < len(text):
        if text.startswith("/*", offset):
            end = text.find("*/", offset + 2)
            offset = len(text) if end < 0 else end + 2
        elif text.startswith("//", offset):
            end = text.find("\n", offset)
            offset = len(text) if end < 0 else end
        else:
            output.append(text[offset])
            offset += 1
    return "".join(output)


def function_body(text, name):
    """Return the brace-matched body of one C function definition."""
    marker = f"\n{name}("
    start = text.find(marker)
    if start < 0:
        raise AuditFailure(f"{name} is absent")
    open_brace = text.find("{", start)
    if open_brace < 0:
        raise AuditFailure(f"{name} carries no body")
    depth = 0
    for offset in range(open_brace, len(text)):
        if text[offset] == "{":
            depth += 1
        elif text[offset] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace : offset + 1]
    raise AuditFailure(f"{name} has an unbalanced body")


def require_order(body, function, *tokens):
    """Require each token exactly after the preceding token."""
    offset = 0
    for token in tokens:
        found = body.find(token, offset)
        if found < 0:
            raise AuditFailure(
                f"{function} lacks ordered transaction token {token!r}"
            )
        offset = found + len(token)


def audit(queue_text):
    """Verify grant acquisition, rollback, acceptance, and release state."""
    source = strip_comments(queue_text)

    admit = function_body(source, "r3v_native_hyperz_admit")
    require_order(
        admit,
        "r3v_native_hyperz_admit",
        "if (newly_acquired == NULL)",
        "*newly_acquired = false;",
        "r300_zb_hyperz_admit_stream",
        "device->hyperz_ownership = R300_ZB_HYPERZ_OWNED;",
        "*newly_acquired = true;",
        "r300_zb_hyperz_admit_stream",
    )

    release = function_body(source, "r3v_native_hyperz_release")
    require_order(
        release,
        "r3v_native_hyperz_release",
        "device->hyperz_ownership != R300_ZB_HYPERZ_OWNED",
        "RADEON_INFO_WANT_HYPERZ",
        "result != 0 || release != 0u",
        "device->hyperz_ownership = R300_ZB_HYPERZ_UNOWNED;",
        "return true;",
    )

    prepare = function_body(source, "r3v_native_queue_prepare_submission")
    require_order(
        prepare,
        "r3v_native_queue_prepare_submission",
        "r3v_native_hyperz_admit(",
        "&prepared->hyperz_newly_acquired",
        "prepared->valid = true;",
        "prepare_fail:",
        "if (prepared->hyperz_newly_acquired)",
        "r3v_native_hyperz_release(device);",
        "memset(prepared, 0, sizeof(*prepared));",
    )

    prepared_release = function_body(source, "r3v_native_prepared_release")
    require_order(
        prepared_release,
        "r3v_native_prepared_release",
        "if (!prepared->valid)",
        "if (prepared->hyperz_newly_acquired)",
        "r3v_native_hyperz_release(device);",
        "memset(prepared, 0, sizeof(*prepared));",
    )

    commit = function_body(source, "r3v_native_queue_commit_prepared")
    require_order(
        commit,
        "r3v_native_queue_commit_prepared",
        "radeon_drm_vk_cs_submit",
        "const bool ioctl_accepted = result == 0;",
        "if (ioctl_accepted)",
        "prepared->hyperz_newly_acquired = false;",
        "r3v_native_prepared_release(device);",
    )

    positional = function_body(source, "r3v_native_queue_execute_positional_stream")
    require_order(
        positional,
        "r3v_native_queue_execute_positional_stream",
        "if (any_ioctl_accepted == NULL)",
        "*any_ioctl_accepted = false;",
        "r3v_native_queue_submit_ordered_segment",
        "*any_ioctl_accepted = progress.any_ioctl_accepted;",
        "finish:",
        "*any_ioctl_accepted = progress.any_ioctl_accepted;",
    )

    submit = function_body(source, "r3v_native_queue_submit")
    acquisition = submit.rfind("bool hyperz_newly_acquired = false;")
    if acquisition < 0:
        raise AuditFailure("r3v_native_queue_submit lacks inline acquisition state")
    inline = submit[acquisition:]
    require_order(
        inline,
        "r3v_native_queue_submit positional path",
        "r3v_native_hyperz_admit(",
        "bool any_ioctl_accepted = false;",
        "r3v_native_queue_execute_positional_stream(",
        "&any_ioctl_accepted",
        "if (hyperz_newly_acquired && any_ioctl_accepted)",
        "hyperz_newly_acquired = false;",
        "if (hyperz_newly_acquired)",
        "r3v_native_hyperz_release(device);",
    )
    single_cs = inline.find("int result = radeon_drm_vk_cs_submit")
    if single_cs < 0:
        raise AuditFailure("r3v_native_queue_submit lacks the inline CS path")
    require_order(
        inline[single_cs:],
        "r3v_native_queue_submit single-CS path",
        "int result = radeon_drm_vk_cs_submit",
        "const bool ioctl_accepted = result == 0;",
        "if (hyperz_newly_acquired && ioctl_accepted)",
        "hyperz_newly_acquired = false;",
        "if (hyperz_newly_acquired)",
        "r3v_native_hyperz_release(device);",
    )

    return "acquisition, pre-ioctl rollback, accepted-CS retention, and release checked"


SELFTEST_QUEUE = r"""
VkResult
r3v_native_hyperz_admit(struct device *device, struct cmd *cmd,
                        bool *newly_acquired)
{
   if (newly_acquired == NULL) return fail();
   *newly_acquired = false;
   verdict = r300_zb_hyperz_admit_stream();
   device->hyperz_ownership = R300_ZB_HYPERZ_OWNED;
   *newly_acquired = true;
   verdict = r300_zb_hyperz_admit_stream();
}
bool
r3v_native_hyperz_release(struct device *device)
{
   if (device->hyperz_ownership != R300_ZB_HYPERZ_OWNED) return true;
   call(RADEON_INFO_WANT_HYPERZ);
   if (result != 0 || release != 0u) return false;
   device->hyperz_ownership = R300_ZB_HYPERZ_UNOWNED;
   return true;
}
VkResult
r3v_native_queue_prepare_submission(void)
{
   r3v_native_hyperz_admit(device, cmd, &prepared->hyperz_newly_acquired);
   prepared->valid = true;
prepare_fail:
   if (prepared->hyperz_newly_acquired) r3v_native_hyperz_release(device);
   memset(prepared, 0, sizeof(*prepared));
}
void
r3v_native_prepared_release(void)
{
   if (!prepared->valid) return;
   if (prepared->hyperz_newly_acquired) r3v_native_hyperz_release(device);
   memset(prepared, 0, sizeof(*prepared));
}
VkResult
r3v_native_queue_commit_prepared(void)
{
   result = radeon_drm_vk_cs_submit();
   const bool ioctl_accepted = result == 0;
   if (ioctl_accepted) prepared->hyperz_newly_acquired = false;
   r3v_native_prepared_release(device);
}
VkResult
r3v_native_queue_execute_positional_stream(void)
{
   if (any_ioctl_accepted == NULL) return fail();
   *any_ioctl_accepted = false;
   r3v_native_queue_submit_ordered_segment();
   *any_ioctl_accepted = progress.any_ioctl_accepted;
   return success;
finish:
   *any_ioctl_accepted = progress.any_ioctl_accepted;
}
VkResult
r3v_native_queue_submit(void)
{
   bool hyperz_newly_acquired = false;
   r3v_native_hyperz_admit();
   bool any_ioctl_accepted = false;
   r3v_native_queue_execute_positional_stream(&any_ioctl_accepted);
   if (hyperz_newly_acquired && any_ioctl_accepted)
      hyperz_newly_acquired = false;
   if (hyperz_newly_acquired) r3v_native_hyperz_release(device);
   int result = radeon_drm_vk_cs_submit();
   const bool ioctl_accepted = result == 0;
   if (hyperz_newly_acquired && ioctl_accepted)
      hyperz_newly_acquired = false;
   if (hyperz_newly_acquired) r3v_native_hyperz_release(device);
}
"""


def expect_failure(source, old, new, expected):
    """Mutate one transaction edge and require a named refusal."""
    if old not in source:
        raise AuditFailure(f"selftest mutation token {old!r} is absent")
    try:
        audit(source.replace(old, new, 1))
    except AuditFailure as error:
        if expected not in str(error):
            raise AuditFailure(
                f"mutation refused with {error!r}, expected {expected!r}"
            ) from error
        return
    raise AuditFailure(f"mutation {old!r} was admitted")


def selftest():
    """Calibrate the audit against six independently broken transaction edges."""
    audit(SELFTEST_QUEUE)
    mutations = (
        ("*newly_acquired = false;", "", "*newly_acquired = false;"),
        ("*newly_acquired = true;", "", "*newly_acquired = true;"),
        ("if (result != 0 || release != 0u) return false;", "", "result != 0"),
        ("if (prepared->hyperz_newly_acquired) r3v_native_hyperz_release(device);",
         "", "if (prepared->hyperz_newly_acquired)"),
        ("if (ioctl_accepted) prepared->hyperz_newly_acquired = false;",
         "", "if (ioctl_accepted)"),
        ("*any_ioctl_accepted = progress.any_ioctl_accepted;\n   return success;",
         "return success;", "finish:"),
    )
    for old, new, expected in mutations:
        expect_failure(SELFTEST_QUEUE, old, new, expected)
    return "selftest: known-good admitted and six broken transactions refused"


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--queue", type=Path)
    parser.add_argument("--selftest", action="store_true")
    arguments = parser.parse_args(argv)
    try:
        if arguments.selftest:
            verdict = selftest()
        elif arguments.queue is not None:
            verdict = audit(arguments.queue.read_text(encoding="utf-8"))
        else:
            parser.error("--queue PATH or --selftest")
            return 2
    except AuditFailure as error:
        print(f"r3v-hyperz-ownership-transaction: {error}", file=sys.stderr)
        return 1
    print(f"r3v-hyperz-ownership-transaction: {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
