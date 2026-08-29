# SPDX-License-Identifier: MIT
"""Gallium-separation audit for the native R3V ICD.

Native ownership requires that libvulkan_r3v carries no Gallium or
radeon-winsys runtime symbol in any binding.  The audit walks the full nm
symbol table (defined and undefined, local and global), so a statically
linked Gallium object with hidden visibility fails the same as a dynamic
reference.
"""

import contextlib
import io
import subprocess
import sys
import tempfile
from pathlib import Path

FORBIDDEN_SYMBOLS = (
    "r300_screen_create",
    "radeon_drm_winsys_create",
    "pipe_screen_create",
    "vl_create_mpeg12_decoder",
)

AUDIT_OK = 0
AUDIT_FORBIDDEN_SYMBOL = 1
AUDIT_USAGE = 2
AUDIT_NM_FAILURE = 3
AUDIT_FORBIDDEN_SOURCE = 4
AUDIT_INPUT_MISSING = 5

# Gallium include roots and runtime identifiers the native compilation
# must stay free of.  The PIPE_FORMAT_* provenance authority is
# r3v_format.h; the native dependency edge is checked separately.
FORBIDDEN_INCLUDE_PREFIXES = ("pipe/", "r300/", "winsys/", "gallium")
FORBIDDEN_IDENTIFIERS = (
    "pipe_screen",
    "pipe_context",
    "pipe_resource",
    "r300_context",
    "radeon_winsys",
)


def include_directory_is_forbidden(include_dir: str) -> bool:
    """Return whether an include path contains the Gallium root directory."""
    normalized = include_dir.replace("\\", "/")
    return any(component == "gallium" for component in normalized.split("/"))


def native_active_lines(text: str):
    """Yield every (line_number, line) pair in the source.

    No source names a backend macro, so the native compilation keeps every
    line and the scan reads them all; a conditional arm cannot hide a
    forbidden include or identifier from it.
    """
    return enumerate(text.splitlines(), start=1)


def strip_comments(text: str) -> str:
    """Remove block and line comments so citations stay out of the scan."""
    import re

    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def native_header_path(include: str, including: Path, native_root: Path) -> Path | None:
    """Resolve a quoted include that remains inside the native R300 tree."""
    candidates = [including.parent / include, native_root / include]
    if include.startswith("amd/r300/"):
        candidates.append(native_root.parents[1] / include)
    for candidate in candidates:
        if not candidate.is_file():
            continue
        resolved = candidate.resolve()
        try:
            resolved.relative_to(native_root)
        except ValueError:
            continue
        return resolved
    return None


def audit_sources(paths: list[str], native_root_name: str | None = None) -> int:
    """Scan native translation units and every reachable native header."""
    import re

    failures = []
    initial_paths = [Path(path_name).resolve() for path_name in paths]
    if native_root_name is None:
        native_root = None
    else:
        native_root = Path(native_root_name).resolve()
        if not native_root.is_dir():
            print(f"native source root missing: {native_root}", file=sys.stderr)
            return AUDIT_INPUT_MISSING
    paths_to_scan = list(initial_paths)
    seen = set()
    while paths_to_scan:
        path = paths_to_scan.pop()
        if path in seen:
            continue
        seen.add(path)
        if not path.is_file():
            print(f"source audit input missing: {path}", file=sys.stderr)
            return AUDIT_INPUT_MISSING
        stripped = strip_comments(path.read_text(encoding="utf-8"))
        for number, line in native_active_lines(stripped):
            include = re.match(r'\s*#\s*include\s+["<]([^">]+)[">]', line)
            if include and any(
                include.group(1).startswith(prefix)
                for prefix in FORBIDDEN_INCLUDE_PREFIXES
            ):
                failures.append(
                    f"{path}:{number}: forbidden include " f"{include.group(1)}"
                )
                continue
            if include and native_root is not None and '"' in line:
                header = native_header_path(include.group(1), path, native_root)
                if header is not None:
                    paths_to_scan.append(header)
            if re.match(r"\s*struct\s+\w+\s*;\s*$", line):
                # A bare forward declaration references nothing.
                continue
            for identifier in FORBIDDEN_IDENTIFIERS:
                if re.search(rf"\b{identifier}\b", line):
                    failures.append(
                        f"{path}:{number}: forbidden identifier " f"{identifier}"
                    )
    if failures:
        print("\n".join(failures))
        return AUDIT_FORBIDDEN_SOURCE
    print(
        f"r3v_native_separation_audit: {len(seen)} sources and reachable headers free of "
        "Gallium includes and identifiers"
    )
    return AUDIT_OK


def audit_compile_commands(path_name: str, object_marker: str) -> int:
    """Prove the native objects' compile commands carry no Gallium -I."""
    import json
    import shlex

    path = Path(path_name)
    if not path.is_file():
        print(f"compile_commands.json missing: {path}", file=sys.stderr)
        return AUDIT_INPUT_MISSING
    try:
        entries = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeError) as error:
        print(f"compile_commands.json unreadable: {error}", file=sys.stderr)
        return AUDIT_INPUT_MISSING
    matched = 0
    failures = []
    if not isinstance(entries, list):
        print("compile_commands.json must contain an array", file=sys.stderr)
        return AUDIT_INPUT_MISSING
    for entry in entries:
        if not isinstance(entry, dict):
            print("compile_commands.json entry is not an object", file=sys.stderr)
            return AUDIT_INPUT_MISSING
        output = entry.get("output", "")
        if not isinstance(output, str):
            print("compile command output path is not text", file=sys.stderr)
            return AUDIT_INPUT_MISSING
        if object_marker not in output:
            continue
        matched += 1
        arguments = entry.get("arguments")
        if arguments is None:
            command = entry.get("command", "")
            if not isinstance(command, str):
                print(f"compile command for {output} is not text", file=sys.stderr)
                return AUDIT_INPUT_MISSING
            try:
                arguments = shlex.split(command)
            except ValueError as error:
                print(
                    f"compile command for {output} is unreadable: {error}",
                    file=sys.stderr,
                )
                return AUDIT_INPUT_MISSING
        if not isinstance(arguments, list) or not all(
            isinstance(argument, str) for argument in arguments
        ):
            print(
                f"compile command arguments for {output} are malformed", file=sys.stderr
            )
            return AUDIT_INPUT_MISSING
        argument_index = 0
        while argument_index < len(arguments):
            argument = arguments[argument_index]
            include_dir = None
            if argument == "-I":
                argument_index += 1
                if argument_index == len(arguments) or arguments[
                    argument_index
                ].startswith("-"):
                    failures.append(f"{output}: -I has no include directory")
                    break
                include_dir = arguments[argument_index]
            elif argument.startswith("-I"):
                include_dir = argument[2:]
            if include_dir and include_directory_is_forbidden(include_dir):
                failures.append(f"{output}: forbidden include dir " f"{include_dir}")
            argument_index += 1
    if matched == 0:
        print(
            f"no compile command matches object marker {object_marker!r}",
            file=sys.stderr,
        )
        return AUDIT_INPUT_MISSING
    if failures:
        print("\n".join(failures))
        return AUDIT_FORBIDDEN_SOURCE
    print(
        f"r3v_native_separation_audit: {matched} native compile "
        "commands free of Gallium include dirs"
    )
    return AUDIT_OK


def audit_dynamic_section(readelf: str, library: str) -> int:
    """Prove the built ICD's dynamic section names no Gallium library."""
    if not Path(library).is_file():
        print(f"library missing: {library}", file=sys.stderr)
        return AUDIT_INPUT_MISSING
    try:
        result = subprocess.run(
            [readelf, "-d", library], check=False, capture_output=True, text=True
        )
    except (OSError, subprocess.SubprocessError, UnicodeError) as error:
        print(f"readelf failed for {library}: {error}", file=sys.stderr)
        return AUDIT_NM_FAILURE
    import re

    dynamic_header = re.search(
        r"(?m)^\s*Dynamic section at offset .* contains \d+ entries?:\s*$",
        result.stdout,
    )
    dynamic_tags = re.findall(
        r"(?m)^\s*0x[0-9a-fA-F]+\s+\([^)]+\)",
        result.stdout,
    )
    if result.returncode != 0 or not (dynamic_header or dynamic_tags):
        print(f"readelf produced no dynamic section for {library}", file=sys.stderr)
        return AUDIT_NM_FAILURE
    failures = [
        line.strip()
        for line in result.stdout.splitlines()
        if "NEEDED" in line and "gallium" in line.lower()
    ]
    if failures:
        print("\n".join(failures))
        return AUDIT_FORBIDDEN_SYMBOL
    print("r3v_native_separation_audit: dynamic section names no " "Gallium library")
    return AUDIT_OK


def find_forbidden_symbols(table: str) -> list[str]:
    """Return native-library symbols that belong to Gallium or winsys."""
    return [name for name in FORBIDDEN_SYMBOLS if name in table]


def run_nm(nm: str, options: list[str], library: str) -> str | None:
    """Return one nm symbol table, or refuse the audit on tool failure."""
    label = "defined-only" if options else "full"
    try:
        result = subprocess.run(
            [nm, *options, library],
            check=False,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.SubprocessError, UnicodeError) as error:
        print(f"nm {label} command failed for {library}: {error}", file=sys.stderr)
        return None

    if result.returncode != 0:
        diagnostic = result.stderr.strip()
        message = f"nm {label} command failed for {library}: status {result.returncode}"
        if diagnostic:
            message += f": {diagnostic}"
        print(message, file=sys.stderr)
        return None
    if not result.stdout.strip():
        print(
            f"nm {label} command produced no symbols for {library}",
            file=sys.stderr,
        )
        return None
    return result.stdout


def audit_library(nm: str, library: str) -> int:
    """Run the symbol audit against one native library."""
    defined = run_nm(nm, ["--defined-only"], library)
    if defined is None:
        return AUDIT_NM_FAILURE
    full = run_nm(nm, [], library)
    if full is None:
        return AUDIT_NM_FAILURE
    table = defined + full
    failures = find_forbidden_symbols(table)
    if failures:
        print("native ICD carries Gallium/winsys symbols: " + ", ".join(failures))
        return AUDIT_FORBIDDEN_SYMBOL
    print("r3v_native_separation_audit: no Gallium or winsys symbol present")
    return AUDIT_OK


def selftest() -> int:
    """Calibrate clean and forbidden symbol-table verdicts through the audit."""
    with tempfile.TemporaryDirectory(prefix="r3v-native-separation-") as tmp:
        root = Path(tmp)
        nm = root / "nm"
        nm.write_text(
            "#!/bin/sh\n"
            "set -eu\n"
            "last=\n"
            'for argument in "$@"; do\n'
            "    last=$argument\n"
            "done\n"
            'case "$last" in\n'
            "    *nm-error-defined.symbols)\n"
            '        if [ "${1:-}" = "--defined-only" ]; then\n'
            "            echo 'defined-only diagnostic' >&2\n"
            "            exit 2\n"
            "        fi\n"
            "        ;;\n"
            "    *nm-error-full.symbols)\n"
            '        if [ "${1:-}" != "--defined-only" ]; then\n'
            "            echo 'full diagnostic' >&2\n"
            "            exit 3\n"
            "        fi\n"
            "        ;;\n"
            "esac\n"
            'cat "$last"\n'
        )
        nm.chmod(0o755)

        cases = (
            ("native", nm, "r3v_native_entrypoint\n", AUDIT_OK, ""),
            (
                "gallium",
                nm,
                "r300_screen_create\n",
                AUDIT_FORBIDDEN_SYMBOL,
                "",
            ),
            (
                "winsys",
                nm,
                "radeon_drm_winsys_create\n",
                AUDIT_FORBIDDEN_SYMBOL,
                "",
            ),
            (
                "empty",
                nm,
                "",
                AUDIT_NM_FAILURE,
                "produced no symbols",
            ),
            (
                "nm-error-defined",
                nm,
                "r3v_native_entrypoint\n",
                AUDIT_NM_FAILURE,
                "defined-only diagnostic",
            ),
            (
                "nm-error-full",
                nm,
                "r3v_native_entrypoint\n",
                AUDIT_NM_FAILURE,
                "full diagnostic",
            ),
            (
                "nm-missing",
                root / "missing-nm",
                "r3v_native_entrypoint\n",
                AUDIT_NM_FAILURE,
                "nm defined-only command failed",
            ),
        )
        for label, nm_path, table, expected_status, expected_diagnostic in cases:
            library = root / f"{label}.symbols"
            library.write_text(table)
            diagnostics = io.StringIO()
            with contextlib.redirect_stderr(diagnostics):
                status = audit_library(str(nm_path), str(library))
            if status != expected_status:
                print(
                    f"separation selftest {label}: expected status="
                    f"{expected_status}, got {status}"
                )
                return 1
            actual_diagnostic = diagnostics.getvalue()
            if expected_diagnostic:
                diagnostic_matches = expected_diagnostic in actual_diagnostic
            else:
                diagnostic_matches = actual_diagnostic == ""
            if not diagnostic_matches:
                print(
                    f"separation selftest {label}: expected diagnostic="
                    f"{expected_diagnostic!r}, got {actual_diagnostic!r}"
                )
                return 1

    with tempfile.TemporaryDirectory(prefix="r3v-native-sources-") as tmp:
        root = Path(tmp)
        native_backend_macro = "R3V_" + "NATIVE_BACKEND"
        source_cases = (
            (
                "clean",
                (
                    '#include "util/format/u_formats.h"\n'
                    "struct pipe_screen;\n"
                    "/* comment names pipe_context and pipe/p_defines.h */\n"
                ),
                AUDIT_OK,
            ),
            ("include", '#include "pipe/p_defines.h"\n', AUDIT_FORBIDDEN_SOURCE),
            ("identifier", "struct pipe_screen *screen;\n", AUDIT_FORBIDDEN_SOURCE),
            (
                "conditional-arm",
                (
                    "#ifdef __SSE3__\n"
                    "int vector;\n"
                    "#else\n"
                    '#include "winsys/radeon_winsys.h"\n'
                    "#endif\n"
                ),
                AUDIT_FORBIDDEN_SOURCE,
            ),
            (
                "conditional-elif",
                (
                    "#if 0\n"
                    "int vector;\n"
                    f"#elif defined({native_backend_macro})\n"
                    '#include "winsys/radeon_winsys.h"\n'
                    "#endif\n"
                ),
                AUDIT_FORBIDDEN_SOURCE,
            ),
        )
        for label, content, expected_status in source_cases:
            source = root / f"{label}.c"
            source.write_text(content)
            diagnostics = io.StringIO()
            with contextlib.redirect_stdout(diagnostics):
                status = audit_sources([str(source)])
            if status != expected_status:
                print(
                    f"source selftest {label}: expected "
                    f"{expected_status}, got {status}"
                )
                return 1
        diagnostics = io.StringIO()
        with contextlib.redirect_stderr(diagnostics):
            status = audit_sources([str(root / "absent.c")])
            if status != AUDIT_INPUT_MISSING:
                print(f"source selftest missing-input: got {status}")
                return 1

        native_root = root / "src" / "amd" / "r300"
        nested_header = native_root / "common" / "nested.h"
        nested_header.parent.mkdir(parents=True)
        nested_header.write_text('#include "pipe/p_defines.h"\n')
        nested_source = native_root / "native.c"
        nested_source.write_text('#include "amd/r300/common/nested.h"\n')
        with contextlib.redirect_stdout(io.StringIO()):
            status = audit_sources([str(nested_source)], str(native_root))
        if status != AUDIT_FORBIDDEN_SOURCE:
            print(f"source selftest nested-header: got {status}")
            return 1

        import json

        commands = root / "compile_commands.json"
        commands.write_text(
            json.dumps(
                [
                    {
                        "output": "libr3v_native_impl.a.p/a.c.o",
                        "arguments": ["cc", "-I", "src", "-Iinclude", "-c", "a.c"],
                    },
                    {
                        "output": "other.p/b.c.o",
                        "command": "cc -Isrc/gallium/include -c b.c",
                    },
                ]
            )
        )
        with contextlib.redirect_stdout(io.StringIO()):
            status = audit_compile_commands(str(commands), "libr3v_native_impl")
        if status != AUDIT_OK:
            print(f"compile-commands selftest clean: got {status}")
            return 1
        commands.write_text(
            json.dumps(
                [
                    {
                        "output": "libr3v_native_impl.a.p/a.c.o",
                        "command": "cc -I src/gallium/include -c a.c",
                    },
                ]
            )
        )
        with contextlib.redirect_stdout(io.StringIO()):
            status = audit_compile_commands(str(commands), "libr3v_native_impl")
        if status != AUDIT_FORBIDDEN_SOURCE:
            print(f"compile-commands selftest forbidden: got {status}")
            return 1
        commands.write_text(json.dumps([]))
        with contextlib.redirect_stderr(io.StringIO()):
            status = audit_compile_commands(str(commands), "libr3v_native_impl")
        if status != AUDIT_INPUT_MISSING:
            print(f"compile-commands selftest unmatched: got {status}")
            return 1
        with contextlib.redirect_stderr(io.StringIO()):
            status = audit_compile_commands(
                str(root / "absent.json"), "libr3v_native_impl"
            )
        if status != AUDIT_INPUT_MISSING:
            print(f"compile-commands selftest missing: got {status}")
            return 1
        commands.write_text(
            json.dumps(
                [
                    {
                        "output": "libr3v_native_impl.a.p/a.c.o",
                        "arguments": ["cc", "-I", "-c", "a.c"],
                    },
                ]
            )
        )
        with contextlib.redirect_stdout(io.StringIO()):
            status = audit_compile_commands(str(commands), "libr3v_native_impl")
        if status != AUDIT_FORBIDDEN_SOURCE:
            print(f"compile-commands selftest missing -I operand: got {status}")
            return 1

        readelf = root / "readelf"
        readelf.write_text('#!/bin/sh\ncat "$2"\n')
        readelf.chmod(0o755)
        dynamic = root / "clean.so"
        dynamic.write_text(
            " 0x0000000000000001 (NEEDED) Shared library: [libdrm.so.2]\n"
        )
        with contextlib.redirect_stdout(io.StringIO()):
            status = audit_dynamic_section(str(readelf), str(dynamic))
        if status != AUDIT_OK:
            print(f"dynamic selftest clean: got {status}")
            return 1
        dynamic.write_text(
            " 0x0000000000000001 (NEEDED) Shared library: " "[libgallium.so]\n"
        )
        with contextlib.redirect_stdout(io.StringIO()):
            status = audit_dynamic_section(str(readelf), str(dynamic))
        if status != AUDIT_FORBIDDEN_SYMBOL:
            print(f"dynamic selftest forbidden: got {status}")
            return 1
        dynamic.write_text("Dynamic section at offset 0x0 contains 0 entries:\n")
        with contextlib.redirect_stdout(io.StringIO()):
            status = audit_dynamic_section(str(readelf), str(dynamic))
        if status != AUDIT_OK:
            print(f"dynamic selftest header-only: got {status}")
            return 1
        dynamic.write_text("There is no dynamic section in this file.\n")
        diagnostics = io.StringIO()
        with contextlib.redirect_stderr(diagnostics):
            status = audit_dynamic_section(str(readelf), str(dynamic))
        if status != AUDIT_NM_FAILURE:
            print(f"dynamic selftest no-section: got {status}")
            return 1
        if "no dynamic section" not in diagnostics.getvalue():
            print("dynamic selftest no-section diagnostic missing")
            return 1
        with contextlib.redirect_stderr(io.StringIO()):
            status = audit_dynamic_section(str(readelf), str(root / "absent.so"))
        if status != AUDIT_INPUT_MISSING:
            print(f"dynamic selftest missing: got {status}")
            return 1

    print(
        "r3v_native_separation_audit: clean, forbidden, empty-output, "
        "first-call error, second-call error, missing-nm, source-scan, "
        "compile-commands, and dynamic-section verdicts calibrated"
    )
    return 0


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        return selftest()
    if len(sys.argv) == 5 and sys.argv[1] == "--expect-status":
        try:
            expected_status = int(sys.argv[2])
        except ValueError:
            print("expected audit status must be an integer", file=sys.stderr)
            return AUDIT_USAGE
        actual_status = audit_library(sys.argv[3], sys.argv[4])
        if actual_status != expected_status:
            print(
                f"separation audit expected status={expected_status}, "
                f"got {actual_status}",
                file=sys.stderr,
            )
            return 1
        print(f"r3v_native_separation_audit: exact status {actual_status} matched")
        return AUDIT_OK
    if (
        len(sys.argv) >= 5
        and sys.argv[1] == "--sources"
        and sys.argv[2] == "--native-root"
    ):
        return audit_sources(sys.argv[4:], sys.argv[3])
    if len(sys.argv) == 4 and sys.argv[1] == "--compile-commands":
        return audit_compile_commands(sys.argv[2], sys.argv[3])
    if len(sys.argv) == 4 and sys.argv[1] == "--dynamic":
        return audit_dynamic_section(sys.argv[2], sys.argv[3])
    if len(sys.argv) != 3:
        print(
            "usage: r3v_native_separation_audit.py <nm> <library> | "
            "--sources --native-root <dir> <file...> | "
            "--compile-commands <json> <marker> | "
            "--dynamic <readelf> <library> | --selftest"
        )
        return AUDIT_USAGE
    return audit_library(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    sys.exit(main())
