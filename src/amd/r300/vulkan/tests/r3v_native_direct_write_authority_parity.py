# SPDX-License-Identifier: MIT
#
# Authority-parity check for the direct-write control: one stream digest
# names the cell across every authority that produces or retains it --
# the standalone manifest writer, the non-submitting arming runner, and
# the queue-retained IB from both drm-shim harness legs -- and the two
# attended runners each call only their own cell's recorder.  A
# disagreement anywhere means two authorities describe two streams, and
# an authorization built from one would submit the other.

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile


def fail(message, *outputs):
    print("FAIL: " + message, file=sys.stderr)
    for output in outputs:
        print(output, file=sys.stderr)
    return 1


def _constant_preprocessor_condition(expression):
    """Evaluate the literal preprocessor conditions used by fixtures."""
    value = expression.strip().split(None, 1)[0] if expression.strip() else ""
    if value in {"0", "0L", "0U", "0UL"}:
        return False
    if value in {"1", "1L", "1U", "1UL"}:
        return True
    return True


def mask_inactive_preprocessor(source):
    """Blank literal-disabled preprocessor branches while keeping line spans."""
    active = True
    frames = []
    masked = []
    directive_re = re.compile(
        r"^([ \t]*)#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")
    for line in source.splitlines(keepends=True):
        match = directive_re.match(line)
        if match is not None:
            directive, expression = match.group(2), match.group(3)
            if directive == "if" or directive == "ifdef" or directive == "ifndef":
                condition = (directive == "ifdef" or directive == "ifndef" or
                             _constant_preprocessor_condition(expression))
                frames.append({"parent": active, "taken": condition})
                active = active and condition
            elif directive == "elif":
                if not frames:
                    active = True
                else:
                    frame = frames[-1]
                    condition = _constant_preprocessor_condition(expression)
                    active = frame["parent"] and not frame["taken"] and condition
                    frame["taken"] = frame["taken"] or condition
            elif directive == "else":
                if not frames:
                    active = True
                else:
                    frame = frames[-1]
                    active = frame["parent"] and not frame["taken"]
                    frame["taken"] = True
            elif directive == "endif":
                if frames:
                    active = frames.pop()["parent"]
            masked.append("\n" if line.endswith("\n") else "")
        elif active:
            masked.append(line)
        else:
            masked.append("".join("\n" if char == "\n" else " "
                                 for char in line))
    return "".join(masked)


def _call_is_declaration(source, match):
    """Identify a prototype match so declarations do not count as calls."""
    line_start = source.rfind("\n", 0, match.start()) + 1
    prefix = source[line_start:match.start()]
    if not re.fullmatch(r"[ \t]*(?:[A-Za-z_]\w*[ \t]+)+", prefix):
        return False
    close = source.find(")", match.end())
    return close >= 0 and re.match(r"[ \t]*;", source[close + 1:])


def has_recorder_call(source, symbol):
    """Match an executable recorder call, not a forward declaration."""
    source = mask_inactive_preprocessor(
        re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.DOTALL))
    pattern = re.compile(rf"\b{re.escape(symbol)}[ \t]*\(")
    return any(not _call_is_declaration(source, match)
               for match in pattern.finditer(source))


def has_indirect_recorder_call(source, symbol):
    """Match a call through a function pointer assigned to the recorder."""
    source = mask_inactive_preprocessor(
        re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.DOTALL))
    aliases = re.findall(
        rf"\b([A-Za-z_]\w*)[ \t]*=[ \t]*&?[ \t]*"
        rf"{re.escape(symbol)}\b", source)
    return any(re.search(rf"\b{re.escape(alias)}[ \t]*\(", source)
               for alias in aliases)


def has_recorder_use(source, symbol):
    """Match direct or indirect executable use of a recorder."""
    return (has_recorder_call(source, symbol) or
            has_indirect_recorder_call(source, symbol))


def calibrate_recorder_call_predicate():
    """Calibrate direct, indirect, conditional, and inactive call forms."""
    good = ("VkResult record(VkCommandBuffer command);\n"
            "VkResult result = record(cmd);\n"
            "if (record(cmd) != VK_SUCCESS) return 1;\n")
    declaration_only = "VkResult record(VkCommandBuffer command);\n"
    indirect = ("record_fn record_cell = record;\n"
                "return record_cell(cmd);\n")
    inactive = "#if 0\nrecord(cmd);\n#endif\n"
    return (has_recorder_use(good, "record") and
            not has_recorder_use(declaration_only, "record") and
            has_recorder_use(indirect, "record") and
            not has_recorder_use(inactive, "record"))


def main():
    if len(sys.argv) != 6:
        print("usage: r3v_native_direct_write_authority_parity.py "
              "<manifest-writer> <arming-runner> <harness> "
              "<attended-direct-write-source> <attended-triangle-source>",
              file=sys.stderr)
        return 2
    writer, runner, harness, dw_source, triangle_source = sys.argv[1:6]

    if not calibrate_recorder_call_predicate():
        return fail("recorder call-site predicate calibration failed")

    environment = dict(os.environ)
    for declaration in (
        "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED",
        "R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
        "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE",
        "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
    ):
        environment.pop(declaration, None)

    with tempfile.TemporaryDirectory() as root:
        # The standalone manifest writer publishes the reference
        # artifacts; its manifest digest is the comparison baseline.
        writer_dir = os.path.join(root, "writer")
        os.mkdir(writer_dir)
        wrote = subprocess.run([writer, writer_dir], env=environment,
                               capture_output=True, text=True)
        if wrote.returncode != 0:
            return fail("manifest writer failed", wrote.stdout, wrote.stderr)
        with open(os.path.join(writer_dir, "manifest.json")) as manifest:
            written = json.load(manifest)
        baseline_digest = written["ib_blake3"]
        if re.fullmatch(r"[0-9a-f]{64}", baseline_digest) is None:
            return fail("manifest digest is not a 64-hex value",
                        baseline_digest)
        with open(os.path.join(writer_dir, "ib.bin"), "rb") as reference:
            baseline_ib = reference.read()

        # The arming runner reports the digest an authorization would
        # declare; it must be the writer's digest.
        armed = subprocess.run([runner, root], env=environment,
                               capture_output=True, text=True)
        runner_digest = re.search(r"^ib_blake3=([0-9a-f]{64})$",
                                  armed.stdout, re.MULTILINE)
        if runner_digest is None:
            return fail("arming runner reports no digest", armed.stdout)
        if runner_digest.group(1) != baseline_digest:
            return fail("arming runner digest differs from the manifest "
                        "writer's", runner_digest.group(1), baseline_digest)

        # Both harness legs drive the recorder and queue over the shim
        # fixture; the queue-retained IB and its manifest digest must
        # match the baseline byte for byte.
        for leg in ("closed", "open"):
            leg_dir = os.path.join(root, leg)
            os.mkdir(leg_dir)
            leg_env = dict(environment)
            leg_env["R3V_NATIVE_MANIFEST_DIR"] = leg_dir
            ran = subprocess.run([harness, leg], env=leg_env,
                                 capture_output=True, text=True)
            if ran.returncode != 0:
                return fail("harness %s leg failed" % leg, ran.stdout,
                            ran.stderr)
            with open(os.path.join(leg_dir, "ib.bin"), "rb") as retained:
                retained_ib = retained.read()
            if retained_ib != baseline_ib:
                return fail("harness %s leg retained an IB that differs "
                            "from the manifest writer's" % leg)
            with open(os.path.join(leg_dir, "manifest.json")) as manifest:
                retained_manifest = json.load(manifest)
            if retained_manifest["ib_blake3"] != baseline_digest:
                return fail("harness %s leg manifest digest differs from "
                            "the baseline" % leg)

        # When a host BLAKE3 tool exists, recompute the digest from the
        # published bytes and calibrate the comparison on a mutated copy;
        # the manifest-integration test carries this leg on hosts with
        # b3sum, so its absence here reports not run.
        b3sum = shutil.which("b3sum")
        if b3sum is not None:
            recomputed = subprocess.run(
                [b3sum, "--no-names", os.path.join(writer_dir, "ib.bin")],
                capture_output=True, text=True)
            if recomputed.returncode != 0 or \
                    recomputed.stdout.strip() != baseline_digest:
                return fail("b3sum recomputation differs from the manifest "
                            "digest", recomputed.stdout, baseline_digest)
            mutated_path = os.path.join(root, "ib_mutated.bin")
            mutated = bytearray(baseline_ib)
            mutated[0] ^= 1
            with open(mutated_path, "wb") as mutant:
                mutant.write(mutated)
            mutated_sum = subprocess.run([b3sum, "--no-names", mutated_path],
                                         capture_output=True, text=True)
            if mutated_sum.stdout.strip() == baseline_digest:
                return fail("digest comparison is vacuous: a mutated IB "
                            "reproduced the baseline digest")
        else:
            print("b3sum recomputation not run: no b3sum on PATH "
                  "(the manifest-integration test carries this leg)")

        # Recorder-call audit: each attended runner lowers only its own
        # cell, so neither cell's authorization can execute the other.
        with open(dw_source) as source:
            dw_text = source.read()
        with open(triangle_source) as source:
            triangle_text = source.read()
        if not has_recorder_use(dw_text, "r3v_native_record_direct_write"):
            return fail("attended direct-write runner does not call the "
                         "direct-write recorder")
        if has_recorder_use(dw_text, "r3v_native_record_tcl_bypass_triangle"):
            return fail("attended direct-write runner names the triangle "
                        "recorder")
        if not has_recorder_use(
                triangle_text, "r3v_native_record_tcl_bypass_triangle"):
            return fail("attended triangle runner does not call the "
                        "triangle recorder")
        if has_recorder_use(triangle_text, "r3v_native_record_direct_write"):
            return fail("attended triangle runner names the direct-write "
                        "recorder")

    print("r3v_native_direct_write_authority_parity: one digest names "
          "the cell across every authority")
    return 0


if __name__ == "__main__":
    sys.exit(main())
