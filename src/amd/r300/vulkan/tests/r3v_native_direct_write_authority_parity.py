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


_UNKNOWN = object()
_SIGNED_MIN = -(1 << 63)
_SIGNED_MAX = (1 << 63) - 1
_UNSIGNED_MASK = (1 << 64) - 1
_PREPROCESSOR_TOKEN = re.compile(
    r"\s*(?:(?:0[xX][0-9A-Fa-f]+|0[bB][01]+|0[0-7]*|[1-9][0-9]*)"
    r"[uUlL]*|[A-Za-z_]\w*|\|\||&&|==|!=|<=|>=|<<|>>|"
    r"[()?:~!%^&|*/+\-<>])"
)
_PREPROCESSOR_PRECEDENCE = {
    "||": 1, "&&": 2, "|": 3, "^": 4, "&": 5,
    "==": 6, "!=": 6, "<": 7, "<=": 7, ">": 7, ">=": 7,
    "<<": 8, ">>": 8, "+": 9, "-": 9, "*": 10, "/": 10,
    "%": 10,
}


class _PreprocessorInteger:
    """Carry the value and unsigned type used by preprocessor arithmetic."""

    def __init__(self, value, unsigned=False):
        self.unsigned = unsigned
        self.value = value & _UNSIGNED_MASK if unsigned else value

    def truthy(self):
        return self.value != 0

    def __eq__(self, other):
        return (isinstance(other, _PreprocessorInteger) and
                self.value == other.value and self.unsigned == other.unsigned)


def _preprocessor_result(value, unsigned=False):
    if unsigned:
        return _PreprocessorInteger(value, True)
    if value < _SIGNED_MIN or value > _SIGNED_MAX:
        return _UNKNOWN
    return _PreprocessorInteger(value)


def _preprocessor_common_type(left, right):
    unsigned = left.unsigned or right.unsigned
    if unsigned:
        return (left.value & _UNSIGNED_MASK, right.value & _UNSIGNED_MASK,
                True)
    return left.value, right.value, False


def _preprocessor_division(left, right):
    if right == 0:
        return _UNKNOWN
    quotient = abs(left) // abs(right)
    if (left < 0) != (right < 0):
        quotient = -quotient
    return quotient


def _preprocessor_tokens(expression):
    tokens = []
    position = 0
    while position < len(expression):
        match = _PREPROCESSOR_TOKEN.match(expression, position)
        if match is None:
            return None
        tokens.append(match.group(0).strip())
        position = match.end()
    return tokens


def _preprocessor_integer(token):
    suffix_match = re.search(r"[uUlL]+$", token)
    suffix = suffix_match.group(0) if suffix_match is not None else ""
    core = token[:-len(suffix)] if suffix else token
    try:
        if core.lower().startswith("0x"):
            value = int(core[2:], 16)
        elif core.lower().startswith("0b"):
            value = int(core[2:], 2)
        elif len(core) > 1 and core.startswith("0"):
            value = int(core, 8)
        else:
            value = int(core, 10)
    except ValueError:
        return _UNKNOWN
    unsigned = "u" in suffix.lower() or value > _SIGNED_MAX
    return _preprocessor_result(value, unsigned)


def _preprocessor_binary(left, right, operator):
    if left is _UNKNOWN or right is _UNKNOWN:
        if operator == "&&" and (
                left is not _UNKNOWN and not left.truthy() or
                right is not _UNKNOWN and not right.truthy()):
            return _PreprocessorInteger(0)
        if operator == "||" and (
                left is not _UNKNOWN and left.truthy() or
                right is not _UNKNOWN and right.truthy()):
            return _PreprocessorInteger(1)
        return _UNKNOWN
    left_value, right_value, unsigned = _preprocessor_common_type(left, right)
    try:
        if operator == "||":
            return _PreprocessorInteger(
                int(left_value != 0 or right_value != 0))
        if operator == "&&":
            return _PreprocessorInteger(
                int(left_value != 0 and right_value != 0))
        if operator == "|":
            return _preprocessor_result(left_value | right_value, unsigned)
        if operator == "^":
            return _preprocessor_result(left_value ^ right_value, unsigned)
        if operator == "&":
            return _preprocessor_result(left_value & right_value, unsigned)
        if operator == "==":
            return _PreprocessorInteger(int(left_value == right_value))
        if operator == "!=":
            return _PreprocessorInteger(int(left_value != right_value))
        if operator == "<":
            return _PreprocessorInteger(int(left_value < right_value))
        if operator == "<=":
            return _PreprocessorInteger(int(left_value <= right_value))
        if operator == ">":
            return _PreprocessorInteger(int(left_value > right_value))
        if operator == ">=":
            return _PreprocessorInteger(int(left_value >= right_value))
        if operator == "<<" or operator == ">>":
            if right_value < 0 or right_value >= 64:
                return _UNKNOWN
            if operator == "<<":
                value = left_value << right_value
                if not unsigned and value > _SIGNED_MAX:
                    return _UNKNOWN
            else:
                value = left_value >> right_value
            return _preprocessor_result(value, unsigned)
        if operator == "+":
            return _preprocessor_result(left_value + right_value, unsigned)
        if operator == "-":
            return _preprocessor_result(left_value - right_value, unsigned)
        if operator == "*":
            return _preprocessor_result(left_value * right_value, unsigned)
        if operator == "/":
            quotient = _preprocessor_division(left_value, right_value)
            if quotient is _UNKNOWN:
                return _UNKNOWN
            return _preprocessor_result(quotient, unsigned)
        if operator == "%":
            quotient = _preprocessor_division(left_value, right_value)
            if quotient is _UNKNOWN:
                return _UNKNOWN
            return _preprocessor_result(left_value - quotient * right_value,
                                        unsigned)
    except (OverflowError, ValueError, ZeroDivisionError):
        return _UNKNOWN
    return _UNKNOWN


class _PreprocessorExpressionParser:
    """Evaluate constant parts of a preprocessor expression conservatively."""

    def __init__(self, expression):
        self.tokens = _preprocessor_tokens(expression)
        self.position = 0

    def peek(self):
        if self.tokens is None or self.position == len(self.tokens):
            return None
        return self.tokens[self.position]

    def take(self, token=None):
        value = self.peek()
        if value is None or (token is not None and value != token):
            return None
        self.position += 1
        return value

    def parse(self):
        if self.tokens is None:
            return _UNKNOWN
        value = self._conditional()
        return value if self.position == len(self.tokens) else _UNKNOWN

    def _conditional(self):
        condition = self._expression(0)
        if self.take("?") is None:
            return condition
        when_true = self._conditional()
        if self.take(":") is None:
            return _UNKNOWN
        when_false = self._conditional()
        if condition is _UNKNOWN:
            return when_true if when_true == when_false else _UNKNOWN
        return when_true if condition.truthy() else when_false

    def _expression(self, minimum_precedence):
        left = self._unary()
        while True:
            operator = self.peek()
            precedence = _PREPROCESSOR_PRECEDENCE.get(operator, -1)
            if precedence < minimum_precedence:
                return left
            self.take()
            right = self._expression(precedence + 1)
            left = _preprocessor_binary(left, right, operator)

    def _unary(self):
        if self.peek() in {"!", "~", "+", "-"}:
            operator = self.take()
            value = self._unary()
            if value is _UNKNOWN:
                return _UNKNOWN
            if operator == "!":
                return _PreprocessorInteger(int(not value.truthy()))
            if operator == "~":
                return _preprocessor_result(~value.value, value.unsigned)
            if operator == "+":
                return value
            return _preprocessor_result(-value.value, value.unsigned)
        token = self.take()
        if token is None:
            return _UNKNOWN
        if token == "(":
            value = self._conditional()
            return value if self.take(")") is not None else _UNKNOWN
        if token == "defined":
            if self.take("(") is not None:
                self.take()
                if self.take(")") is None:
                    return _UNKNOWN
            else:
                self.take()
            return _UNKNOWN
        if re.fullmatch(
                r"(?:0[xX][0-9A-Fa-f]+|0[bB][01]+|0[0-7]*|[1-9][0-9]*)"
                r"[uUlL]*", token):
            return _preprocessor_integer(token)
        return _UNKNOWN


def _constant_preprocessor_condition(expression):
    """Return the preprocessor truth value, retaining unknown conditions."""
    value = _PreprocessorExpressionParser(expression.strip()).parse()
    return _UNKNOWN if value is _UNKNOWN else value.truthy()


def _strip_non_code_literals_and_comments(source):
    """Blank comments and literals while preserving source line spans."""
    output = []
    state = "code"
    position = 0
    while position < len(source):
        character = source[position]
        following = source[position + 1] if position + 1 < len(source) else ""
        if state == "code":
            if character == "/" and following == "/":
                output.extend((" ", " "))
                position += 2
                state = "line-comment"
            elif character == "/" and following == "*":
                output.extend((" ", " "))
                position += 2
                state = "block-comment"
            elif character == '"' or character == "'":
                output.append(" ")
                position += 1
                state = character
            else:
                output.append(character)
                position += 1
        elif state == "line-comment":
            if character == "\n":
                output.append("\n")
                position += 1
                state = "code"
            else:
                output.append(" ")
                position += 1
        elif state == "block-comment":
            if character == "*" and following == "/":
                output.extend((" ", " "))
                position += 2
                state = "code"
            else:
                output.append("\n" if character == "\n" else " ")
                position += 1
        else:
            if character == "\\" and position + 1 < len(source):
                output.append(" ")
                position += 1
                escaped = source[position]
                output.append("\n" if escaped == "\n" else " ")
                position += 1
            elif character == state:
                output.append(" ")
                position += 1
                state = "code"
            else:
                output.append("\n" if character == "\n" else " ")
                position += 1
    return "".join(output)


def _splice_preprocessor_lines(source):
    """Apply C line splicing before directives and literals are parsed."""
    return re.sub(r"\\\r?\n", "  ", source)


def _condition_may_be_true(condition):
    return condition is _UNKNOWN or condition


def _condition_or(left, right):
    if left is True or right is True:
        return True
    if left is False and right is False:
        return False
    return _UNKNOWN


def mask_inactive_preprocessor(source):
    """Blank only branches proven inactive by preprocessor evaluation."""
    source = _splice_preprocessor_lines(source)
    source = _strip_non_code_literals_and_comments(source)
    active = True
    frames = []
    masked = []
    directive_re = re.compile(
        r"^([ \t]*)#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")
    for line in source.splitlines(keepends=True):
        match = directive_re.match(line)
        if match is not None:
            directive, expression = match.group(2), match.group(3)
            if directive in {"if", "ifdef", "ifndef"}:
                condition = (_UNKNOWN if directive in {"ifdef", "ifndef"}
                             else _constant_preprocessor_condition(expression))
                frames.append({"parent": active, "taken": condition})
                active = active and _condition_may_be_true(condition)
            elif directive == "elif":
                if not frames:
                    active = True
                else:
                    frame = frames[-1]
                    condition = _constant_preprocessor_condition(expression)
                    active = (
                        frame["parent"] and
                        frame["taken"] is not True and
                        _condition_may_be_true(condition))
                    frame["taken"] = _condition_or(frame["taken"], condition)
            elif directive == "else":
                if not frames:
                    active = True
                else:
                    frame = frames[-1]
                    active = frame["parent"] and frame["taken"] is not True
                    frame["taken"] = True
            elif directive == "endif":
                if frames:
                    active = frames.pop()["parent"]
            masked.append("\n" if line.endswith("\n") else "")
        elif active:
            masked.append(line)
        else:
            masked.append("".join(
                "\n" if char == "\n" else " " for char in line))
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
    source = mask_inactive_preprocessor(source)
    pattern = re.compile(rf"\b{re.escape(symbol)}[ \t]*\(")
    return any(not _call_is_declaration(source, match)
               for match in pattern.finditer(source))


def _indirect_call_is_declaration(source, match):
    """Exclude function-pointer declarations from indirect call matches."""
    if not match.group(0).lstrip().startswith("("):
        return False
    line_start = source.rfind("\n", 0, match.start()) + 1
    prefix = source[line_start:match.start()]
    prefix = prefix.rstrip()
    if re.search(r"(?:^|[ \t])(return|if|while|for|switch|sizeof)$",
                 prefix):
        return False
    if re.search(r"[=,:;{}]", prefix):
        return False
    if not re.fullmatch(r"[ \t]*[A-Za-z_]\w*(?:[ \t]+[A-Za-z_]\w*)*[ \t]*",
                        prefix):
        return False
    close = source.find(")", match.end())
    return close >= 0 and re.match(r"[ \t]*;", source[close + 1:])


def has_indirect_recorder_call(source, symbol):
    """Match a call through a function pointer assigned to the recorder."""
    source = mask_inactive_preprocessor(source)
    aliases = re.findall(
        rf"\b([A-Za-z_]\w*)[ \t]*=[ \t]*&?[ \t]*"
        rf"{re.escape(symbol)}\b", source)
    for alias in aliases:
        call_pattern = re.compile(
            rf"(?:\b{re.escape(alias)}\b|\(\s*\*\s*"
            rf"{re.escape(alias)}\s*\))[ \t]*\(")
        if any(not _indirect_call_is_declaration(source, match)
               for match in call_pattern.finditer(source)):
            return True
    return False


def has_recorder_use(source, symbol):
    """Match direct or indirect executable use of a recorder."""
    return (has_recorder_call(source, symbol) or
            has_indirect_recorder_call(source, symbol))


def calibrate_recorder_call_predicate():
    """Calibrate executable calls, expressions, aliases, and literals."""
    good = ("VkResult record(VkCommandBuffer command);\n"
            "VkResult result = record(cmd);\n"
            "if (record(cmd) != VK_SUCCESS) return 1;\n")
    declaration_only = "VkResult record(VkCommandBuffer command);\n"
    indirect = ("record_fn record_cell = record;\n"
                "return record_cell(cmd);\n")
    dereferenced = ("record_fn record_cell = record;\n"
                    "return (*record_cell)(cmd);\n")
    active_expression = "#if 0 + 1\nrecord(cmd);\n#endif\n"
    active_unknown_expression = (
        "#if 0 || ENABLE_CELL\nrecord(cmd);\n#endif\n")
    unknown_alternate = ("#if ENABLE_CELL\nreturn 0;\n#else\n"
                         "record(cmd);\n#endif\n")
    ifdef_alternate = ("#ifdef ENABLE_CELL\nreturn 0;\n#else\n"
                       "record(cmd);\n#endif\n")
    unsigned_expression = "#if -1 > 1U\nrecord(cmd);\n#endif\n"
    negative_division = "#if -3 / 2 == -1\nrecord(cmd);\n#endif\n"
    negative_remainder = "#if -3 % 2 == -1\nrecord(cmd);\n#endif\n"
    dereference_declaration = (
        "extern VkResult (*record_cell)(VkCommandBuffer);\n"
        "record_cell = record;\n")
    continued_inactive = ("#if 0 \\\n"
                          " || 0\nrecord(cmd);\n#else\n"
                          "return 0;\n#endif\n")
    inactive = "#if 0 + 0\nrecord(cmd);\n#endif\n"
    string_literal = 'fprintf(stderr, "record(");\n'
    character_literal = "const int marker = 'record(';\n"
    return (has_recorder_use(good, "record") and
            not has_recorder_use(declaration_only, "record") and
            has_recorder_use(indirect, "record") and
            has_recorder_use(dereferenced, "record") and
            has_recorder_use(active_expression, "record") and
            has_recorder_use(active_unknown_expression, "record") and
            has_recorder_use(unknown_alternate, "record") and
            has_recorder_use(ifdef_alternate, "record") and
            has_recorder_use(unsigned_expression, "record") and
            has_recorder_use(negative_division, "record") and
            has_recorder_use(negative_remainder, "record") and
            not has_recorder_use(dereference_declaration, "record") and
            not has_recorder_use(continued_inactive, "record") and
            not has_recorder_use(string_literal, "record") and
            not has_recorder_use(character_literal, "record") and
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
