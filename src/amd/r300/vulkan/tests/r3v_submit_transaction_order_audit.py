# SPDX-License-Identifier: MIT
"""Hold the native queue's executable path to its prepare/validate/commit order.

`r3v_submit_preflight.h` states the rule the submission boundary runs
inside: a refusal belongs to the prepare and validate phases, so a refused
submit leaves application memory as it found it.  The queue's own comments
make that claim in three places; nothing checked it, so a gate moved past a
write, or a new refusal added after one, would keep the comments and lose the
property.

This audit reads `r3v_native_queue.c`, takes the region of `vkQueueSubmit`
that runs a command buffer carrying a recorded stream, and splits it at the
first device-visible effect.  Every refusal ahead of that split is the rule
working.  Every refusal behind it is a site where a caller can already have
had bytes written, so each one is declared here by name with the reason it
stands, and an undeclared one fails the audit.  A declared site removed from
the source fails too: the ledger names live code, not history.

A device-visible effect is a call that writes application memory, publishes
a cache line over a live mapping, spends an authorization, or reaches the
kernel.  A refusal is a call whose failure returns from `vkQueueSubmit`.

Usage:
  r3v_submit_transaction_order_audit.py --queue PATH
  r3v_submit_transaction_order_audit.py --selftest
Exit 0 when every refusal behind the split is declared and present.
"""

import argparse
import sys
from pathlib import Path

FUNCTION = "r3v_native_queue_submit"

# The marker that opens the region running a command buffer with a recorded
# stream on the inline path.  The submit sets it twice: once where a prepared
# submission commits through its transport tail, and once where the inline
# path takes over.  The inline occurrence is the last, and it is the region
# this audit orders, because the zero-stream block above it executes host
# work as the whole submission rather than as an effect ahead of a gate.
EXECUTABLE_REGION_MARKER = "submit_has_executable_ib = true;"

# The call the inline region must reach; its presence proves the region
# selected is the one that submits rather than an earlier branch.
EXECUTABLE_REGION_IOCTL = "radeon_drm_vk_cs_submit"

# Calls that put bytes where the application or the kernel can observe them.
EFFECT_TOKENS = (
    "r3v_native_cmd_buffer_execute_deferred_copies",
    "r3v_native_cmd_buffer_execute_deferred_draws",
    "r3v_native_cmd_buffer_execute_deferred_dispatch",
    "radeon_drm_vk_bo_cache_sync",
    "radeon_drm_vk_cs_submit",
    "r3v_native_arming_disarm",
)

# Calls whose failure returns from the submit.  A refusal ahead of the first
# effect leaves the caller's memory untouched; one behind it does not.
REFUSAL_TOKENS = (
    "r3v_native_arming_evaluate",
    "r3v_native_queue_write_manifest",
    "r3v_native_queue_write_submit_object",
    "r3v_native_plan_replay_admit",
    "r3v_native_plan_replay_bind",
    "r3v_native_plan_capture_record",
    "r3v_native_plan_replay_check_ib",
    "r3v_native_submission_trace_emit",
    "r3v_native_arming_disarm",
    "r3v_native_deferred_draw_verify_gpu_producer",
    "r3v_native_deferred_dispatch_verify_gpu",
    "r3v_native_plan_capture_write",
)

# The refusals that stand behind the first device-visible effect, each with
# the mechanism that keeps it there.  A site listed here is a known cost of
# the current order, not an admission that the order is free.
DECLARED_LATE_REFUSALS = {
    "r3v_native_arming_disarm":
        "the one-shot token is spent after the recorded host writes so a "
        "refused draw spends no authorization; a failed disarm then refuses "
        "with those writes already landed",
    "r3v_native_plan_replay_check_ib":
        "the plan session proves the stream at the ioctl boundary, which is "
        "after the deferred execution that resolves which stream travels",
    "r3v_native_submission_trace_emit":
        "the transport bracket opens immediately before the ioctl, so a "
        "refused trace write lands after the recorded host writes",
    "r3v_native_deferred_draw_verify_gpu_producer":
        "the carrier read-back compares against the CPU oracle after the "
        "completion wait, so its divergence verdict is necessarily late",
    "r3v_native_deferred_dispatch_verify_gpu":
        "the compute carrier read-back compares after the completion wait "
        "for the same reason",
    "r3v_native_plan_capture_write":
        "the transcript lands on the capture cadence after a completed "
        "submission, so a short plan is refused once the submission ran",
}


class AuditFailure(Exception):
    """The executable path orders a refusal the ledger does not carry."""


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


def function_body(text, name):
    """The brace-matched body of one function definition."""
    marker = f"\n{name}("
    start = text.find(marker)
    if start < 0:
        raise AuditFailure(f"{name} is absent from the source")
    open_brace = text.find("{", start)
    if open_brace < 0:
        raise AuditFailure(f"{name} carries no body")
    depth, i, n = 0, open_brace, len(text)
    while i < n:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace:i + 1]
        i += 1
    raise AuditFailure(f"{name} has an unbalanced body")


def executable_region(body):
    """The inline part of the submit that runs a stream and reaches the ioctl."""
    marker = body.rfind(EXECUTABLE_REGION_MARKER)
    if marker < 0:
        raise AuditFailure(
            f"{FUNCTION} carries no {EXECUTABLE_REGION_MARKER!r} marker; the "
            f"executable region cannot be located")
    region = body[marker:]
    if EXECUTABLE_REGION_IOCTL not in region:
        raise AuditFailure(
            f"the region after the last {EXECUTABLE_REGION_MARKER!r} reaches "
            f"no {EXECUTABLE_REGION_IOCTL}; the executable region cannot be "
            f"located")
    return region


def occurrences(region, tokens):
    """Every (offset, token) pair, in source order."""
    found = []
    for token in tokens:
        start = 0
        while True:
            at = region.find(token, start)
            if at < 0:
                break
            found.append((at, token))
            start = at + len(token)
    found.sort()
    return found


def audit(queue_text):
    """Verdict string, or AuditFailure naming the first broken rule."""
    region = executable_region(function_body(strip_comments(queue_text),
                                             FUNCTION))

    effects = occurrences(region, EFFECT_TOKENS)
    refusals = occurrences(region, REFUSAL_TOKENS)
    if not effects:
        raise AuditFailure(
            "the executable region names no device-visible effect; the "
            "effect token list no longer matches the source")
    if not refusals:
        raise AuditFailure(
            "the executable region names no refusal; the refusal token list "
            "no longer matches the source")

    boundary = effects[0][0]
    early = sorted({token for at, token in refusals if at < boundary})
    late = sorted({token for at, token in refusals if at >= boundary})

    if not early:
        raise AuditFailure(
            "no refusal precedes the first device-visible effect; every gate "
            "now runs after application memory can have changed")

    undeclared = [token for token in late if token not in
                  DECLARED_LATE_REFUSALS]
    if undeclared:
        raise AuditFailure(
            "refusals behind the first device-visible effect are not "
            "declared: " + ", ".join(undeclared))

    absent = [token for token in DECLARED_LATE_REFUSALS if token not in late]
    if absent:
        raise AuditFailure(
            "declared late refusals no longer appear behind the first "
            "effect: " + ", ".join(sorted(absent)))

    return (f"{len(early)} refusals precede the first device-visible effect; "
            f"{len(late)} declared refusals follow it")


SELFTEST_QUEUE = """
VkResult
r3v_native_queue_submit(struct vk_queue *queue_base,
                        struct vk_queue_submit *submit)
{
   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      if (device->prepared.valid) {
         VkResult committed = r3v_native_queue_commit_prepared(device);
         if (committed != VK_SUCCESS)
            return committed;
         submit_has_executable_ib = true;
         continue;
      }
      if (cmd_buffer->ib_size_dwords == 0) {
         r3v_native_cmd_buffer_execute_deferred_copies(device, cmd_buffer);
         continue;
      }
      submit_has_executable_ib = true;
      if (r3v_native_queue_write_manifest(device) != 0)
         return VK_ERROR_DEVICE_LOST;
      enum r3v_native_arming_verdict arming =
         r3v_native_arming_evaluate(&facts);
      if (r3v_native_queue_write_submit_object(device) != 0)
         return VK_ERROR_DEVICE_LOST;
      if (r3v_native_plan_replay_bind(&device->plan_replay) != NULL)
         return VK_ERROR_DEVICE_LOST;
      if (r3v_native_plan_replay_admit(&device->plan_replay) != NULL)
         return VK_ERROR_DEVICE_LOST;
      if (r3v_native_plan_capture_record(&device->plan_capture) != 0)
         return VK_ERROR_DEVICE_LOST;
      r3v_native_cmd_buffer_execute_deferred_copies(device, cmd_buffer);
      r3v_native_cmd_buffer_execute_deferred_draws(device, cmd_buffer);
      r3v_native_cmd_buffer_execute_deferred_dispatch(device, cmd_buffer);
      radeon_drm_vk_bo_cache_sync(&device->drm, map, size);
      if (r3v_native_arming_disarm(dir, digest) != 0)
         return VK_ERROR_DEVICE_LOST;
      if (r3v_native_plan_replay_check_ib(&device->plan_replay) != NULL)
         return VK_ERROR_DEVICE_LOST;
      if (r3v_native_submission_trace_emit(device, event) != 0)
         return VK_ERROR_DEVICE_LOST;
      int result = radeon_drm_vk_cs_submit(&device->drm, &cs);
      if (r3v_native_deferred_draw_verify_gpu_producer(device) != VK_SUCCESS)
         return VK_ERROR_DEVICE_LOST;
      if (r3v_native_deferred_dispatch_verify_gpu(device) != VK_SUCCESS)
         return VK_ERROR_DEVICE_LOST;
      if (r3v_native_plan_capture_write(&device->plan_capture) != 0)
         return VK_ERROR_DEVICE_LOST;
   }
   return VK_SUCCESS;
}
"""


def selftest():
    """Calibrate the audit on a known-good body and four known-bad ones."""
    verdict = audit(SELFTEST_QUEUE)
    if "precede" not in verdict:
        raise AuditFailure(f"known-good body reported {verdict!r}")

    # An undeclared refusal behind the first effect.
    moved = SELFTEST_QUEUE.replace(
        "      if (r3v_native_queue_write_submit_object(device) != 0)\n"
        "         return VK_ERROR_DEVICE_LOST;\n", "", 1).replace(
        "      radeon_drm_vk_bo_cache_sync(&device->drm, map, size);\n",
        "      radeon_drm_vk_bo_cache_sync(&device->drm, map, size);\n"
        "      if (r3v_native_queue_write_submit_object(device) != 0)\n"
        "         return VK_ERROR_DEVICE_LOST;\n", 1)
    expect_failure(moved, "not declared")

    # A declared refusal removed: the ledger would keep naming dead code.
    dropped = SELFTEST_QUEUE.replace(
        "      if (r3v_native_plan_replay_check_ib(&device->plan_replay) "
        "!= NULL)\n         return VK_ERROR_DEVICE_LOST;\n", "", 1)
    expect_failure(dropped, "no longer appear")

    # Every gate moved behind the first effect on the inline path.  The
    # insertion lands after the last marker, so it exercises the same region
    # the audit anchors on rather than the prepared branch above it.
    at = SELFTEST_QUEUE.rfind("      submit_has_executable_ib = true;\n")
    cut = at + len("      submit_has_executable_ib = true;\n")
    all_late = (SELFTEST_QUEUE[:cut] +
                "      radeon_drm_vk_bo_cache_sync(&device->drm, map, "
                "size);\n" + SELFTEST_QUEUE[cut:])
    expect_failure(all_late, "no refusal precedes")

    # The region marker renamed away.
    expect_failure(SELFTEST_QUEUE.replace(EXECUTABLE_REGION_MARKER,
                                          "submit_carries_a_stream = true;"),
                   "executable region cannot be located")

    # The inline region no longer reaching the ioctl: the marker the audit
    # anchors on now opens a branch that submits nothing.
    expect_failure(SELFTEST_QUEUE.replace(
        "      int result = radeon_drm_vk_cs_submit(&device->drm, &cs);\n",
        "", 1), "reaches no radeon_drm_vk_cs_submit")

    # The function itself absent.
    expect_failure(SELFTEST_QUEUE.replace(FUNCTION, "r3v_other_submit"),
                   "is absent from the source")
    return "selftest: known-good admitted, six known-bad bodies refused"


def expect_failure(text, fragment):
    """Require audit(text) to refuse with fragment in its message."""
    try:
        verdict = audit(text)
    except AuditFailure as exc:
        if fragment not in str(exc):
            raise AuditFailure(
                f"known-bad body refused with {exc!r}, expected "
                f"{fragment!r}") from exc
        return
    raise AuditFailure(f"known-bad body admitted with {verdict!r}")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--queue", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    try:
        if args.selftest:
            verdict = selftest()
        elif args.queue is not None:
            verdict = audit(args.queue.read_text())
        else:
            parser.error("--queue PATH or --selftest")
            return 2
    except AuditFailure as exc:
        print(f"r3v-submit-transaction-order: {exc}", file=sys.stderr)
        return 1
    print(f"r3v-submit-transaction-order: {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
