# SPDX-License-Identifier: MIT
"""Enforce the R300 common, CPU, and native-R3V dependency boundary."""

from __future__ import annotations

import contextlib
import io
import posixpath
import re
import subprocess
import sys
import tempfile
from pathlib import Path

AUDIT_OK = 0
AUDIT_FORBIDDEN = 1
AUDIT_USAGE = 2
AUDIT_TOOL_FAILURE = 3
AUDIT_INPUT_MISSING = 4

LAYERS = ("common", "cpu", "native")

FORBIDDEN_INCLUDES = {
    "common": (
        "../compiler/",
        "amd/r300/cpu/",
        "amd/r300/compiler/",
        "amd/r300/vulkan/",
        "compiler/glsl/",
        "compiler/nir/",
        "gallium/",
        "pipe/",
        "tgsi/",
        "vulkan/",
        "winsys/",
    ),
    "cpu": (
        "../compiler/",
        "amd/r300/compiler/",
        "amd/r300/vulkan/",
        "compiler/glsl/",
        "compiler/nir/",
        "gallium/",
        "pipe/",
        "tgsi/",
        "vulkan/",
        "winsys/",
    ),
    "native": (
        "../compiler/",
        "amd/r300/compiler/",
        "compiler/glsl/",
        "compiler/nir/",
        "gallium/",
        "pipe/",
        "r300/",
        "tgsi/",
        "winsys/",
    ),
}

FORBIDDEN_API_TOKENS = (
    re.compile(r"\bnir_[A-Za-z0-9_]+\b"),
    re.compile(r"\b(?:glsl_type|gl_shader_stage)\b"),
    re.compile(r"\bvtn_[A-Za-z0-9_]+\b"),
    re.compile(r"\btgsi_[A-Za-z0-9_]+\b"),
    re.compile(r"\bspirv_to_nir[A-Za-z0-9_]*\b"),
)

FORBIDDEN_GALLIUM_TYPES = (
    "pipe_blend_state",
    "pipe_context",
    "pipe_depth_stencil_alpha_state",
    "pipe_draw_info",
    "pipe_fence_handle",
    "pipe_grid_info",
    "pipe_query",
    "pipe_rasterizer_state",
    "pipe_resource",
    "pipe_sampler_state",
    "pipe_sampler_view",
    "pipe_screen",
    "pipe_surface",
    "pipe_transfer",
    "pipe_vertex_buffer",
    "pipe_vertex_element",
    "r300_context",
    "r300_screen",
    "radeon_winsys",
)

FORBIDDEN_UNDEFINED_PREFIXES = (
    "glsl_",
    "nir_",
    "pipe_",
    "tgsi_",
    "vtn_",
)

NATIVE_BACKEND_MACROS = {
    "R3V_GALLIUM_BACKEND": False,
    "R3V_NATIVE_BACKEND": True,
}

NATIVE_PIPE_TOKEN_EXCEPTIONS = ("pipe_format",)
NATIVE_PIPE_ENUM_PREFIX_EXCEPTIONS = ("PIPE_FORMAT_",)
ROOT_ONLY_INCLUDE_PREFIXES = ("r300/",)


def strip_comments(text: str) -> str:
    """Blank comments without treating comment markers in literals as code."""
    output: list[str] = []
    state = "code"
    index = 0
    while index < len(text):
        char = text[index]
        pair = text[index:index + 2]
        if state == "code":
            if pair == "//":
                output.extend((" ", " "))
                state = "line-comment"
                index += 2
                continue
            if pair == "/*":
                output.extend((" ", " "))
                state = "block-comment"
                index += 2
                continue
            output.append(char)
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            index += 1
            continue
        if state in ("string", "character"):
            output.append(char)
            if char == "\\" and index + 1 < len(text):
                output.append(text[index + 1])
                index += 2
                continue
            if ((state == "string" and char == '"') or
                    (state == "character" and char == "'")):
                state = "code"
            index += 1
            continue
        if state == "line-comment":
            output.append("\n" if char == "\n" else " ")
            if char == "\n":
                state = "code"
            index += 1
            continue
        if pair == "*/":
            output.extend((" ", " "))
            state = "code"
            index += 2
            continue
        output.append("\n" if char == "\n" else " ")
        index += 1
    return "".join(output)


def strip_literals(line: str) -> str:
    line = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
    return re.sub(r"'(?:\\.|[^'\\])*'", "''", line)


def split_top_level(expression: str, operator: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    start = 0
    index = 0
    while index < len(expression):
        char = expression[index]
        if char == "(":
            depth += 1
        elif char == ")":
            depth = max(0, depth - 1)
        elif depth == 0 and expression.startswith(operator, index):
            parts.append(expression[start:index])
            start = index + len(operator)
            index += len(operator)
            continue
        index += 1
    if parts:
        parts.append(expression[start:])
    return parts


def strip_outer_parentheses(expression: str) -> str:
    expression = expression.strip()
    while expression.startswith("(") and expression.endswith(")"):
        depth = 0
        encloses_all = True
        for index, char in enumerate(expression):
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0 and index != len(expression) - 1:
                    encloses_all = False
                    break
        if not encloses_all or depth != 0:
            break
        expression = expression[1:-1].strip()
    return expression


def preprocessor_values(expression: str) -> set[bool]:
    """Evaluate known backend terms and retain both values for unknown terms."""
    expression = strip_outer_parentheses(expression)
    disjunction = split_top_level(expression, "||")
    if disjunction:
        values = {False}
        for part in disjunction:
            right = preprocessor_values(part)
            values = {left or item for left in values for item in right}
        return values
    conjunction = split_top_level(expression, "&&")
    if conjunction:
        values = {True}
        for part in conjunction:
            right = preprocessor_values(part)
            values = {left and item for left in values for item in right}
        return values
    if expression.startswith("!") and not expression.startswith("!="):
        return {not value for value in preprocessor_values(expression[1:])}

    defined = re.fullmatch(
        r"defined\s*(?:\(\s*([A-Za-z_]\w*)\s*\)|"
        r"\s+([A-Za-z_]\w*))",
        expression,
    )
    if defined:
        macro = defined.group(1) or defined.group(2)
        if macro in NATIVE_BACKEND_MACROS:
            return {NATIVE_BACKEND_MACROS[macro]}
        return {False, True}
    if expression in NATIVE_BACKEND_MACROS:
        return {NATIVE_BACKEND_MACROS[expression]}
    try:
        return {bool(int(expression, 0))}
    except ValueError:
        return {False, True}


def native_condition(kind: str, expression: str) -> bool | None:
    """Evaluate a backend condition, retaining uncertainty conservatively."""
    expression = expression.strip()
    if kind in ("ifdef", "ifndef"):
        fields = expression.split()
        if len(fields) != 1 or fields[0] not in NATIVE_BACKEND_MACROS:
            return None
        value = NATIVE_BACKEND_MACROS[fields[0]]
        return value if kind == "ifdef" else not value

    values = preprocessor_values(expression)
    return next(iter(values)) if len(values) == 1 else None


def native_active_lines(text: str):
    """Yield every line that can survive a native-backend preprocessing."""
    # A tracked frame is an exact R3V backend predicate.  An unrelated
    # conditional is deliberately untracked, so every arm is scanned; host
    # compiler feature macros must not hide a forbidden dependency.
    stack: list[tuple[bool, bool, bool]] = []
    for number, line in enumerate(text.splitlines(), start=1):
        directive = re.match(
            r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$", line)
        if not directive:
            if all(active for active, _, _ in stack):
                yield number, line
            continue

        kind = directive.group(1)
        expression = directive.group(2)
        if kind in ("if", "ifdef", "ifndef"):
            value = native_condition(kind, expression)
            if value is None:
                stack.append((True, False, False))
            else:
                stack.append((value, True, value))
            continue
        if kind == "elif" and stack:
            _, tracked, taken = stack[-1]
            if not tracked:
                stack[-1] = (True, False, False)
                continue
            if taken:
                stack[-1] = (False, True, True)
                continue
            value = native_condition("if", expression)
            if value is None:
                stack[-1] = (True, False, False)
            else:
                stack[-1] = (value, True, value)
            continue
        if kind == "else" and stack:
            _, tracked, taken = stack[-1]
            stack[-1] = ((not taken) if tracked else True, tracked, True)
            continue
        if kind == "endif" and stack:
            stack.pop()
            continue


def source_lines(layer: str, text: str):
    stripped = strip_comments(text)
    if layer == "native":
        return native_active_lines(stripped)
    return enumerate(stripped.splitlines(), start=1)


def include_has_prefix(include: str, prefix: str) -> bool:
    """Match project-root and relative spellings of a forbidden include."""
    include = posixpath.normpath(include.replace("\\", "/"))
    return (include.startswith(prefix) or
            (prefix not in ROOT_ONLY_INCLUDE_PREFIXES and
             f"/{prefix}" in include))


def audit_sources(layer: str, paths: list[Path]) -> int:
    if layer not in LAYERS:
        print(f"unknown boundary layer: {layer}", file=sys.stderr)
        return AUDIT_USAGE
    if not paths:
        print(f"{layer} source audit received no inputs", file=sys.stderr)
        return AUDIT_INPUT_MISSING

    failures: list[str] = []
    for path in paths:
        if not path.is_file():
            print(f"{layer} source audit input missing: {path}", file=sys.stderr)
            return AUDIT_INPUT_MISSING
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            print(f"{layer} source audit input unreadable: {path}: {error}",
                  file=sys.stderr)
            return AUDIT_INPUT_MISSING

        for number, line in source_lines(layer, text):
            include = re.match(r'\s*#\s*include\s+["<]([^">]+)[">]', line)
            if include and any(
                    include_has_prefix(include.group(1), prefix)
                    for prefix in FORBIDDEN_INCLUDES[layer]):
                failures.append(
                    f"{path}:{number}: {layer}: forbidden include "
                    f"{include.group(1)}")
                continue

            code = strip_literals(line)
            if layer != "native":
                vk_type = re.search(r"\bVk[A-Z][A-Za-z0-9_]*\b", code)
                if vk_type:
                    failures.append(
                        f"{path}:{number}: {layer}: forbidden Vulkan type "
                        f"{vk_type.group(0)}")

            for pattern in FORBIDDEN_API_TOKENS:
                token = pattern.search(code)
                if token:
                    failures.append(
                        f"{path}:{number}: {layer}: forbidden compiler token "
                        f"{token.group(0)}")

            for type_name in FORBIDDEN_GALLIUM_TYPES:
                if re.search(rf"\b{re.escape(type_name)}\b", code):
                    failures.append(
                        f"{path}:{number}: {layer}: forbidden Gallium type "
                        f"{type_name}")

            pipe_token = re.search(r"\bpipe_[A-Za-z0-9_]+\b", code)
            if pipe_token and not (
                    layer == "native" and
                    pipe_token.group(0) in NATIVE_PIPE_TOKEN_EXCEPTIONS):
                failures.append(
                    f"{path}:{number}: {layer}: forbidden Gallium token "
                    f"{pipe_token.group(0)}")

            pipe_enum = re.search(r"\bPIPE_[A-Z0-9_]+\b", code)
            if pipe_enum and not (
                    layer == "native" and any(
                        pipe_enum.group(0).startswith(prefix)
                        for prefix in NATIVE_PIPE_ENUM_PREFIX_EXCEPTIONS)):
                failures.append(
                    f"{path}:{number}: {layer}: forbidden Gallium enum "
                    f"{pipe_enum.group(0)}")

    if failures:
        print("\n".join(failures))
        return AUDIT_FORBIDDEN
    qualification = ""
    if layer == "native":
        qualification = (
            "; exact pipe_format transition retires with the dual-backend "
            "adapter")
    print(f"r300_common_boundary_audit: {layer}: {len(paths)} sources clean"
          f"{qualification}")
    return AUDIT_OK


def tree_sources(root: Path) -> list[Path]:
    if not root.is_dir():
        return []
    return [
        path for path in sorted(root.rglob("*"))
        if path.suffix in (".c", ".h") and "fixtures" not in path.parts
    ]


def undefined_symbols(table: str) -> list[str]:
    symbols: list[str] = []
    for line in table.splitlines():
        fields = line.split()
        if not fields or line.rstrip().endswith(":"):
            continue
        symbol = None
        if len(fields) >= 2 and fields[-2] in ("U", "u", "V", "v", "W", "w"):
            symbol = fields[-1]
        elif len(fields) == 1:
            symbol = fields[0]
        if symbol:
            symbols.append(symbol.lstrip("_"))
    return symbols


def forbidden_undefined(symbol: str) -> bool:
    return (any(symbol.startswith(prefix)
                for prefix in FORBIDDEN_UNDEFINED_PREFIXES) or
            "spirv_to_nir" in symbol)


def audit_objects(nm: str, objects: list[Path]) -> int:
    if not objects:
        print("object audit received no inputs", file=sys.stderr)
        return AUDIT_INPUT_MISSING

    failures: list[str] = []
    for path in objects:
        if not path.is_file():
            print(f"object audit input missing: {path}", file=sys.stderr)
            return AUDIT_INPUT_MISSING
        try:
            result = subprocess.run(
                [nm, "-u", str(path)], check=False, capture_output=True,
                text=True)
        except (OSError, subprocess.SubprocessError, UnicodeError) as error:
            print(f"nm -u failed for {path}: {error}", file=sys.stderr)
            return AUDIT_TOOL_FAILURE
        if result.returncode != 0:
            diagnostic = result.stderr.strip()
            message = f"nm -u failed for {path}: status {result.returncode}"
            if diagnostic:
                message += f": {diagnostic}"
            print(message, file=sys.stderr)
            return AUDIT_TOOL_FAILURE
        for symbol in undefined_symbols(result.stdout):
            if forbidden_undefined(symbol):
                failures.append(
                    f"{path}: forbidden undefined symbol {symbol}")

    if failures:
        print("\n".join(failures))
        return AUDIT_FORBIDDEN
    print(f"r300_common_boundary_audit: {len(objects)} object inputs clean")
    return AUDIT_OK


def expect_status(actual: int, expected_text: str, label: str,
                  output: str, expected_finding: str) -> int:
    try:
        expected = int(expected_text)
    except ValueError:
        print("expected status must be an integer", file=sys.stderr)
        return AUDIT_USAGE
    if actual != expected:
        print(f"{label}: expected status {expected}, got {actual}",
              file=sys.stderr)
        return 1
    findings = [line for line in output.splitlines() if line.strip()]
    if len(findings) != 1 or expected_finding not in findings[0]:
        print(f"{label}: expected exactly one finding containing "
              f"{expected_finding!r}, got {findings!r}", file=sys.stderr)
        return 1
    print(f"r300_common_boundary_audit: {label}: exact status {actual} matched")
    return AUDIT_OK


def selftest() -> int:
    with tempfile.TemporaryDirectory(prefix="r300-common-boundary-") as tmp:
        root = Path(tmp)
        cases = (
            ("common-clean", "common", '#include "stdint.h"\nint value;\n',
             AUDIT_OK),
            ("common-cpu", "common",
             '#include "amd/r300/cpu/r300_cpu_vertex.h"\n',
             AUDIT_FORBIDDEN),
            ("cpu-vulkan", "cpu", "VkDevice device;\n", AUDIT_FORBIDDEN),
            ("native-nir", "native", "nir_shader *shader;\n",
             AUDIT_FORBIDDEN),
            ("common-r300-compiler", "common",
             '#include "amd/r300/compiler/r300_nir.h"\n',
             AUDIT_FORBIDDEN),
            ("common-normalized-r300-compiler", "common",
             '#include ".././compiler/r300_nir.h"\n', AUDIT_FORBIDDEN),
            ("native-unlisted-pipe", "native",
             "struct pipe_shader_state *state;\n", AUDIT_FORBIDDEN),
            ("native-forward-nir", "native", "struct nir_shader;\n",
             AUDIT_FORBIDDEN),
            ("native-pipe-format-transition", "native",
             "enum pipe_format format = PIPE_FORMAT_R8_UNORM;\n", AUDIT_OK),
            ("native-inactive", "native",
             "#ifdef R3V_GALLIUM_BACKEND\n"
             '#include "pipe/p_context.h"\n'
             "struct pipe_context *context;\n"
             "#endif\n", AUDIT_OK),
            ("native-active", "native",
             "#ifdef R3V_GALLIUM_BACKEND\nint gallium;\n#else\n"
             "struct pipe_context *context;\n#endif\n", AUDIT_FORBIDDEN),
            ("native-unrelated-else", "native",
             "# ifdef __SSE3__\nint vector;\n#else\n"
             "nir_shader *bad;\n#endif\n", AUDIT_FORBIDDEN),
            ("native-defined-form", "native",
             "#if defined(R3V_GALLIUM_BACKEND)\n"
             "struct pipe_context *context;\n#endif\n", AUDIT_OK),
            ("native-composite-inactive", "native",
             "#if defined(R3V_GALLIUM_BACKEND) && FEATURE\n"
             "nir_shader *bad;\n#endif\n", AUDIT_OK),
            ("native-literal-comment-markers", "native",
             'const char *url = "http://x"; nir_shader *bad;\n',
             AUDIT_FORBIDDEN),
            ("native-block-markers-in-literals", "native",
             'const char *open = "/*";\nnir_shader *bad;\n'
             'const char *close = "*/";\n', AUDIT_FORBIDDEN),
        )
        for label, layer, content, expected in cases:
            path = root / f"{label}.c"
            path.write_text(content, encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                actual = audit_sources(layer, [path])
            if actual != expected:
                print(f"selftest {label}: expected {expected}, got {actual}")
                return 1

        with contextlib.redirect_stderr(io.StringIO()):
            empty_sources = audit_sources("common", [])
        if empty_sources != AUDIT_INPUT_MISSING:
            print("selftest empty source set did not refuse")
            return 1
        with contextlib.redirect_stderr(io.StringIO()):
            empty_objects = audit_objects("nm", [])
        if empty_objects != AUDIT_INPUT_MISSING:
            print("selftest empty object set did not refuse")
            return 1

        parsed = undefined_symbols(
            "object.o:\n                 U nir_forbidden\n"
            "                 U _pipe_context_create\n"
            "                 U glsl_forbidden\n"
            "                 U vtn_forbidden\n"
            "                 U tgsi_forbidden\n"
            "                 U r300_spirv_to_nir_forbidden\n"
            "                 U memcpy\n")
        expected_symbols = [
            "nir_forbidden",
            "pipe_context_create",
            "glsl_forbidden",
            "vtn_forbidden",
            "tgsi_forbidden",
            "r300_spirv_to_nir_forbidden",
            "memcpy",
        ]
        if parsed != expected_symbols:
            print(f"selftest symbol parser mismatch: {parsed}")
            return 1
        if not all(forbidden_undefined(symbol) for symbol in parsed[:-1]):
            print("selftest forbidden symbol family direction failed")
            return 1
        if forbidden_undefined(parsed[-1]):
            print("selftest clean symbol direction failed")
            return 1

    print("r300_common_boundary_audit: source layers, conditional native "
          "view, empty inputs, and undefined-symbol parser calibrated")
    return AUDIT_OK


def main(argv: list[str]) -> int:
    if argv == ["--selftest"]:
        return selftest()
    if len(argv) >= 3 and argv[0] == "--sources":
        return audit_sources(argv[1], [Path(value) for value in argv[2:]])
    if len(argv) == 3 and argv[0] == "--tree":
        root = Path(argv[2])
        paths = tree_sources(root)
        if not paths:
            print(f"{argv[1]} source tree has no C/header inputs: {root}",
                  file=sys.stderr)
            return AUDIT_INPUT_MISSING
        return audit_sources(argv[1], paths)
    if len(argv) >= 3 and argv[0] == "--objects":
        return audit_objects(argv[1], [Path(value) for value in argv[2:]])
    if len(argv) >= 5 and argv[0] == "--expect-source-status":
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            actual = audit_sources(
                argv[3], [Path(value) for value in argv[4:]])
        return expect_status(actual, argv[1], "known-bad source",
                             output.getvalue(), argv[2])
    if len(argv) >= 5 and argv[0] == "--expect-object-status":
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            actual = audit_objects(
                argv[3], [Path(value) for value in argv[4:]])
        return expect_status(actual, argv[1], "known-bad object",
                             output.getvalue(), argv[2])
    print(
        "usage: r300_common_boundary_audit.py --selftest | "
        "--tree <layer> <root> | --sources <layer> <file...> | "
        "--objects <nm> <object...> | "
        "--expect-source-status <status> <finding> <layer> <file...> | "
        "--expect-object-status <status> <finding> <nm> <object...>",
        file=sys.stderr)
    return AUDIT_USAGE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
