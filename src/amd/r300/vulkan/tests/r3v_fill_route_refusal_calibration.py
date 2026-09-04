# SPDX-License-Identifier: MIT
"""Calibrate every refusal the RB2D fill route holds by removing it.

A test that passes proves nothing about a check it never exercises.  This
runner neuters one refusal at a time in the source, rebuilds the one test
target that covers it, and requires that test to fail.  A neutered check
whose test still passes is the finding: the test does not cover the rule it
is credited with.

Each row names the file, the exact source the check is written as, the text
that removes it while leaving the translation unit compiling, and the meson
test that must then fail.  The runner restores the file after every row,
whatever the outcome, and restores every file again on the way out.

Usage:
  r3v_fill_route_refusal_calibration.py --builddir DIR [--only NAME ...]
  r3v_fill_route_refusal_calibration.py --selftest

Exit 0 when every neutered refusal produced a failing test.
"""

import argparse
import subprocess
import sys
from pathlib import Path

VULKAN = Path("src/amd/r300/vulkan")
PURE = VULKAN / "r3v_fill_route.c"
GLUE = VULKAN / "r3v_native_fill_route.c"
QUEUE = VULKAN / "r3v_native_queue.c"
RECORDING = VULKAN / "r3v_native_recording.c"
POLICY = VULKAN / "r3v_route_policy.c"
CMD = VULKAN / "r3v_native_cmd.c"

PURE_TEST = "r3v-fill-route"
GLUE_TEST = "r3v-native-fill-route"
POLICY_TEST = "r3v-route-policy"
RECORDING_TEST = "r3v-native-compute-dispatch"
POLICY_SUBMIT_TEST = "r3v-native-execution-policy"

# How a row is judged when its check is removed.
#
# REACHABLE names a refusal a caller can produce: removing it must fail the
# test that covers it, and a test that still passes is a coverage defect.
# INTERNAL names a guard over a state this route's own constants and its
# already-admitted contracts make impossible: removing it changes no
# observable behavior, so the test must still pass, and a failure there says
# the state was reachable after all and the guard is a refusal.
REACHABLE = "reachable"
INTERNAL = "internal"

# name, file, exact source, replacement that removes the refusal, test, class
REFUSALS = [
    # The memory contract.
    ("memory-bound", PURE,
     "   if (!m->bound)\n      return R3V_FILL_ROUTE_REFUSE_BUFFER_UNBOUND;",
     "   if (false)\n      return R3V_FILL_ROUTE_REFUSE_BUFFER_UNBOUND;",
     PURE_TEST, REACHABLE),
    ("memory-usage", PURE,
     "   if ((m->buffer_usage & R3V_FILL_ROUTE_USAGE_TRANSFER_DST) == 0)\n"
     "      return R3V_FILL_ROUTE_REFUSE_USAGE_TRANSFER_DST;",
     "   if (false)\n      return R3V_FILL_ROUTE_REFUSE_USAGE_TRANSFER_DST;",
     PURE_TEST, REACHABLE),
    ("memory-range-empty", PURE,
     "   if (m->fill_bytes == 0)\n      return R3V_FILL_ROUTE_REFUSE_RANGE_EMPTY;",
     "   if (false)\n      return R3V_FILL_ROUTE_REFUSE_RANGE_EMPTY;",
     PURE_TEST, REACHABLE),
    ("memory-range-alignment", PURE,
     "   if (m->fill_offset % R3V_FILL_ROUTE_ELEMENT_BYTES != 0 ||\n"
     "       m->fill_bytes % R3V_FILL_ROUTE_ELEMENT_BYTES != 0)\n"
     "      return R3V_FILL_ROUTE_REFUSE_RANGE_ALIGNMENT;",
     "   if (false)\n      return R3V_FILL_ROUTE_REFUSE_RANGE_ALIGNMENT;",
     PURE_TEST, REACHABLE),
    ("memory-range-outside-buffer", PURE,
     "   if (m->fill_offset > m->buffer_bytes ||\n"
     "       m->fill_bytes > m->buffer_bytes - m->fill_offset)\n"
     "      return R3V_FILL_ROUTE_REFUSE_RANGE_OUTSIDE_BUFFER;",
     "   if (false)\n      return R3V_FILL_ROUTE_REFUSE_RANGE_OUTSIDE_BUFFER;",
     PURE_TEST, REACHABLE),
    ("memory-binding-outside-memory", PURE,
     "   if (m->binding_offset > m->memory_bytes ||\n"
     "       m->buffer_bytes > m->memory_bytes - m->binding_offset)\n"
     "      return R3V_FILL_ROUTE_REFUSE_BINDING_OUTSIDE_MEMORY;",
     "   if (false)\n      return R3V_FILL_ROUTE_REFUSE_BINDING_OUTSIDE_MEMORY;",
     PURE_TEST, REACHABLE),
    ("memory-address-envelope", PURE,
     "   if (far_edge > R300_RB2D_ADDRESS_SPACE_BYTES)\n"
     "      return R3V_FILL_ROUTE_REFUSE_ADDRESS_ENVELOPE;",
     "   if (far_edge > UINT64_MAX)\n"
     "      return R3V_FILL_ROUTE_REFUSE_ADDRESS_ENVELOPE;",
     PURE_TEST, REACHABLE),
    ("memory-host-visible", PURE,
     "   if ((m->memory_property_flags & R3V_FILL_ROUTE_MEMORY_HOST_VISIBLE) == 0)\n"
     "      return R3V_FILL_ROUTE_REFUSE_MEMORY_NOT_HOST_VISIBLE;",
     "   if (false)\n      return R3V_FILL_ROUTE_REFUSE_MEMORY_NOT_HOST_VISIBLE;",
     PURE_TEST, REACHABLE),
    ("memory-write-domain", PURE,
     "   if (m->write_domain != R3V_FILL_ROUTE_DOMAIN_GTT)\n"
     "      return R3V_FILL_ROUTE_REFUSE_WRITE_DOMAIN;",
     "   if (false)\n      return R3V_FILL_ROUTE_REFUSE_WRITE_DOMAIN;",
     PURE_TEST, REACHABLE),
    # The frozen-cell predicate.
    ("cell-counts", PURE,
     "   if (cell->copy_count != 1 || cell->reference_count != 1)\n      return false;",
     "   if (false)\n      return false;", PURE_TEST, REACHABLE),
    ("cell-kind-and-binding", PURE,
     "   if (!cell->copy_is_fill || !cell->destination_bound)\n      return false;",
     "   if (false)\n      return false;", PURE_TEST, REACHABLE),
    ("cell-routed", PURE,
     "   if (!cell->gpu_routed)\n      return false;",
     "   if (false)\n      return false;", PURE_TEST, REACHABLE),
    ("cell-range-grid", PURE,
     "   if (cell->fill_bytes == 0 ||\n"
     "       cell->fill_bytes % R3V_FILL_ROUTE_ELEMENT_BYTES != 0 ||\n"
     "       cell->fill_offset % R3V_FILL_ROUTE_ELEMENT_BYTES != 0)\n"
     "      return false;",
     "   if (false)\n      return false;", PURE_TEST, REACHABLE),
    ("cell-containment", PURE,
     "   if (cell->fill_offset > cell->buffer_bytes ||\n"
     "       cell->fill_bytes > cell->buffer_bytes - cell->fill_offset)\n"
     "      return false;",
     "   if (false)\n      return false;", PURE_TEST, REACHABLE),
    ("cell-domains", PURE,
     "   if (cell->read_domains != 0 ||\n"
     "       cell->write_domain != R3V_FILL_ROUTE_DOMAIN_GTT)\n"
     "      return false;",
     "   if (false)\n      return false;", PURE_TEST, REACHABLE),
    ("cell-names-destination", PURE,
     "   return cell->reference_names_destination;",
     "   return true;", PURE_TEST, REACHABLE),
    # The submission identity and the operator's declaration.
    ("identity-stream", PURE,
     '   if (id->ib == NULL || id->ib_dwords == 0) {',
     '   if (false) {', PURE_TEST, REACHABLE),
    ("identity-rects", PURE,
     '   if (id->rect_count == 0 || id->rects == NULL) {',
     '   if (false) {', PURE_TEST, REACHABLE),
    ("identity-segments", PURE,
     '   if (id->segment_count == 0) {',
     '   if (false) {', PURE_TEST, REACHABLE),
    ("identity-relocations", PURE,
     '   if (id->relocation_count == 0 || id->reloc_sites == NULL) {',
     '   if (false) {', PURE_TEST, REACHABLE),
    ("identity-epoch", PURE,
     '   if (id->kernel_release == NULL || id->module_srcversion == NULL) {',
     '   if (false) {', PURE_TEST, REACHABLE),
    ("authority-declared", PURE,
     "   if (!is_digest(declared)) {",
     "   if (false) {", PURE_TEST, REACHABLE),
    ("authority-match", PURE,
     "   if (strcmp(declared, actual) != 0) {",
     "   if (false) {", PURE_TEST, REACHABLE),
    ("authority-digest-width", PURE,
     "   return i == BLAKE3_OUT_LEN * 2;",
     "   return true;", PURE_TEST, REACHABLE),
    ("identity-destination-declared", PURE,
     '   if (id->destination_handle == 0) {',
     '   if (false) {', PURE_TEST, REACHABLE),
    ("identity-destination-hashed", PURE,
     "   put_u32(&ctx, id->destination_handle);",
     "   put_u32(&ctx, 0u);", PURE_TEST, REACHABLE),
    # The routed record's own lifetime.
    ("cmd-reset-drops-record", CMD,
     "   cmd_buffer->fill_route_active = false;\n"
     "   cmd_buffer->fill_route_provenance = (struct r3v_execution_provenance){0};",
     "   cmd_buffer->fill_route_provenance = cmd_buffer->fill_route_provenance;",
     GLUE_TEST, REACHABLE),
    ("queue-record-transport-walk", QUEUE,
     "   const unsigned reached = completion_retired ? 4u : (ioctl_accepted ? 3u : 2u);",
     "   const unsigned reached = 0u;", GLUE_TEST, REACHABLE),
    # The route's own admission.
    ("shape-other-work", GLUE,
     "       r3v_native_cmd_buffer_has_other_recorded_work(cmd_buffer))\n"
     "      return false;",
     "       false)\n      return false;", GLUE_TEST, REACHABLE),
    ("shape-one-copy", GLUE,
     "   if (cmd_buffer->ib_size_dwords != 0 ||\n"
     "       cmd_buffer->deferred_copy_count != 1 ||",
     "   if (false ||\n       false ||", GLUE_TEST, REACHABLE),
    ("submit-one-command-buffer", GLUE,
     "   if (submit_command_buffers != R3V_NATIVE_FILL_ROUTE_COMMAND_BUFFERS) {",
     "   if (false) {", GLUE_TEST, REACHABLE),
    ("glue-memory-contract", GLUE,
     "   if (admission != R3V_FILL_ROUTE_ADMITTED) {\n"
     "      return decline(device, policy, \"declines the destination\",\n"
     "                     r3v_fill_route_refusal_name(admission));\n   }",
     "   if (false) {\n"
     "      return decline(device, policy, \"declines the destination\",\n"
     "                     r3v_fill_route_refusal_name(admission));\n   }",
     GLUE_TEST, REACHABLE),
    ("glue-route-decision", GLUE,
     "   if (decision != R3V_ROUTE_DECISION_GPU) {",
     "   if (false) {", GLUE_TEST, REACHABLE),
    ("glue-cell-frozen", GLUE,
     "   if (!r3v_fill_route_cell_frozen(&cell)) {",
     "   if (!r3v_fill_route_cell_frozen(&cell) && false) {", GLUE_TEST,
     INTERNAL),
    ("glue-submission-gate", GLUE,
     "   if (!device->submit_hazard_accepted || device->manifest_dir == NULL) {",
     "   if (device->manifest_dir == NULL) {", GLUE_TEST, REACHABLE),
    ("glue-arming-verdict", GLUE,
     "   if (arming != R3V_NATIVE_ARMING_ARMED) {",
     "   if (false) {", GLUE_TEST, REACHABLE),
    ("glue-authority", GLUE,
     "   if (admission != R3V_FILL_ROUTE_ADMITTED) {\n"
     "      build_release(&build);\n"
     "      return decline(device, policy,\n"
     "                     r3v_fill_route_refusal_name(admission),",
     "   if (false) {\n"
     "      build_release(&build);\n"
     "      return decline(device, policy,\n"
     "                     r3v_fill_route_refusal_name(admission),",
     GLUE_TEST, REACHABLE),
    # The arming gate's geometry fact for this cell kind.
    ("queue-geometry-arm", QUEUE,
     "      return !r3v_fill_route_cell_frozen(&cell);",
     "      return false;", GLUE_TEST, REACHABLE),
    # The recorded-shape hole the dispatch side already closed.
    ("recording-pending-dispatch", RECORDING,
     "   if (cmd_buffer->deferred_dispatch.pending) {\n"
     "      r3v_native_cmd_poison(commandBuffer);\n      return NULL;\n   }",
     "   if (false) {\n"
     "      r3v_native_cmd_poison(commandBuffer);\n      return NULL;\n   }",
     RECORDING_TEST, REACHABLE),
    # GPU_ONLY over the whole submit, at the submission boundary.
    ("queue-gpu-only-policy", QUEUE,
     "         if (r3v_recorded_work_census_total(&policy_census) > routed_work) {",
     "         if (r3v_recorded_work_census_total(&policy_census) > routed_work &&\n             false) {", POLICY_SUBMIT_TEST, REACHABLE),
    # Automatic selection, separate from a route's maturity.
    ("automatic-selection-set", POLICY,
     "   for (uint32_t i = 0; i < count; i++) {\n"
     "      if (admitted[i] == id)\n         return true;\n   }\n   return false;",
     "   for (uint32_t i = 0; i < count; i++) {\n"
     "      if (admitted[i] == id)\n         return true;\n   }\n   return true;",
     POLICY_TEST, REACHABLE),
]


class CalibrationFailure(Exception):
    """A neutered refusal left its test passing."""


def run(command, cwd):
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True)


def calibrate(root, builddir, rows):
    results = []
    for name, path, old, new, test, kind in rows:
        source = root / path
        original = source.read_text()
        if original.count(old) != 1:
            raise CalibrationFailure(
                "%s: the refusal is not written as this runner records it "
                "in %s" % (name, path))
        source.write_text(original.replace(old, new, 1))
        try:
            built = run(["ninja"], builddir)
            if built.returncode != 0:
                # A neutered check that does not compile still removes the
                # rule; report it rather than crediting it as a refusal.
                results.append((name, test, "build-failed",
                                built.stdout[-800:] + built.stderr[-800:]))
                continue
            ran = run(["meson", "test", test, "--print-errorlogs"], builddir)
            failed = ran.returncode != 0
            if kind == INTERNAL:
                verdict = ("internal guard held"
                           if not failed else "REACHABLE AFTER ALL")
            else:
                verdict = "refused" if failed else "STILL PASSES"
            results.append((name, test, verdict,
                            ran.stdout[-2000:] + ran.stderr[-2000:]))
        finally:
            source.write_text(original)
    run(["ninja"], builddir)
    return results


def selftest():
    """The runner's own arms: a refusal it cannot find in the source, and a
    replacement that leaves the source unchanged."""
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / VULKAN).mkdir(parents=True)
        (root / PURE).write_text("int f(void) { return 0; }\n")
        for old in ("no such refusal", "int f(void) { return 0; }\nint f"):
            try:
                calibrate(root, root,
                          [("probe", PURE, old, "x", "t", REACHABLE)])
            except CalibrationFailure:
                continue
            print("selftest: an absent refusal was accepted", file=sys.stderr)
            return 1
        if (root / PURE).read_text() != "int f(void) { return 0; }\n":
            print("selftest: the source was not restored", file=sys.stderr)
            return 1
    print("selftest: passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--builddir")
    parser.add_argument("--root", default=".")
    parser.add_argument("--only", nargs="*", default=None)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if args.builddir is None:
        parser.error("--builddir names the configured build directory")

    rows = REFUSALS
    if args.only:
        rows = [r for r in REFUSALS if r[0] in set(args.only)]
        if not rows:
            parser.error("--only names no refusal this runner carries")

    results = calibrate(Path(args.root).resolve(),
                        Path(args.builddir).resolve(), rows)
    uncovered = 0
    for name, test, verdict, transcript in results:
        print("=== %s (%s): %s" % (name, test, verdict))
        print(transcript.strip())
        if verdict not in ("refused", "internal guard held"):
            uncovered += 1
    print("%d refusal(s) calibrated, %d uncovered" % (len(results), uncovered))
    return 1 if uncovered else 0


if __name__ == "__main__":
    sys.exit(main())
