# SPDX-License-Identifier: MIT
"""Gallium-separation audit for the native R3V ICD.

Native ownership requires that libvulkan_r3v_native carries no Gallium or
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
# must stay free of.  PIPE_FORMAT_* stays admissible because it comes
# from util/format through idep_mesautil, not from a Gallium root.
FORBIDDEN_INCLUDE_PREFIXES = ("pipe/", "r300/", "winsys/", "gallium")
FORBIDDEN_IDENTIFIERS = (
    "pipe_screen",
    "pipe_context",
    "pipe_resource",
    "r300_context",
    "radeon_winsys",
)
FORBIDDEN_INCLUDE_DIR_MARKERS = ("/gallium/",)


def native_active_lines(text: str):
    """Yield (line_number, line) pairs a -DR3V_NATIVE_BACKEND compile keeps.

    A minimal conditional tracker over the two backend macros: a region
    under `#ifdef R3V_GALLIUM_BACKEND` or `#ifndef R3V_NATIVE_BACKEND`
    never compiles in the native lane, and its `#else` arm does.  Any
    other conditional stays conservatively active.
    """
    stack = []
    for number, line in enumerate(text.splitlines(), start=1):
        directive = line.strip()
        if directive.startswith("#ifdef"):
            macro = directive.split(None, 1)[1].split()[0] if len(
                directive.split()) > 1 else ""
            stack.append((macro != "R3V_GALLIUM_BACKEND", True))
            continue
        if directive.startswith("#ifndef"):
            macro = directive.split(None, 1)[1].split()[0] if len(
                directive.split()) > 1 else ""
            stack.append((macro != "R3V_NATIVE_BACKEND", True))
            continue
        if directive.startswith("#if"):
            stack.append((True, False))
            continue
        if directive.startswith("#else") and stack:
            active, tracked = stack[-1]
            stack[-1] = ((not active) if tracked else True, tracked)
            continue
        if directive.startswith("#endif") and stack:
            stack.pop()
            continue
        if all(active for active, _ in stack):
            yield number, line


def strip_comments(text: str) -> str:
    """Remove block and line comments so citations stay out of the scan."""
    import re
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def audit_sources(paths: list[str]) -> int:
    """Include and identifier scan over the native compilation's sources."""
    import re
    failures = []
    for path_name in paths:
        path = Path(path_name)
        if not path.is_file():
            print(f"source audit input missing: {path}", file=sys.stderr)
            return AUDIT_INPUT_MISSING
        stripped = strip_comments(path.read_text(encoding="utf-8"))
        for number, line in native_active_lines(stripped):
            include = re.match(r'\s*#\s*include\s+["<]([^">]+)[">]', line)
            if include and any(
                    include.group(1).startswith(prefix)
                    for prefix in FORBIDDEN_INCLUDE_PREFIXES):
                failures.append(
                    f"{path}:{number}: forbidden include "
                    f"{include.group(1)}")
                continue
            if re.match(r"\s*struct\s+\w+\s*;\s*$", line):
                # A bare forward declaration references nothing.
                continue
            for identifier in FORBIDDEN_IDENTIFIERS:
                if re.search(rf"\b{identifier}\b", line):
                    failures.append(
                        f"{path}:{number}: forbidden identifier "
                        f"{identifier}")
    if failures:
        print("\n".join(failures))
        return AUDIT_FORBIDDEN_SOURCE
    print(f"r3v_native_separation_audit: {len(paths)} sources free of "
          "Gallium includes and identifiers")
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
    for entry in entries:
        output = entry.get("output", "")
        if object_marker not in output:
            continue
        matched += 1
        arguments = entry.get("arguments")
        if arguments is None:
            arguments = shlex.split(entry.get("command", ""))
        for argument in arguments:
            include_dir = None
            if argument.startswith("-I"):
                include_dir = argument[2:]
            if include_dir and any(marker in include_dir for marker in
                                   FORBIDDEN_INCLUDE_DIR_MARKERS):
                failures.append(f"{output}: forbidden include dir "
                                f"{include_dir}")
    if matched == 0:
        print(f"no compile command matches object marker {object_marker!r}",
              file=sys.stderr)
        return AUDIT_INPUT_MISSING
    if failures:
        print("\n".join(failures))
        return AUDIT_FORBIDDEN_SOURCE
    print(f"r3v_native_separation_audit: {matched} native compile "
          "commands free of Gallium include dirs")
    return AUDIT_OK


def audit_dynamic_section(readelf: str, library: str) -> int:
    """Prove the built ICD's dynamic section names no Gallium library."""
    if not Path(library).is_file():
        print(f"library missing: {library}", file=sys.stderr)
        return AUDIT_INPUT_MISSING
    try:
        result = subprocess.run([readelf, "-d", library], check=False,
                                capture_output=True, text=True)
    except (OSError, subprocess.SubprocessError, UnicodeError) as error:
        print(f"readelf failed for {library}: {error}", file=sys.stderr)
        return AUDIT_NM_FAILURE
    if result.returncode != 0 or not result.stdout.strip():
        print(f"readelf produced no dynamic section for {library}",
              file=sys.stderr)
        return AUDIT_NM_FAILURE
    failures = [line.strip() for line in result.stdout.splitlines()
                if "NEEDED" in line and "gallium" in line.lower()]
    if failures:
        print("\n".join(failures))
        return AUDIT_FORBIDDEN_SYMBOL
    print("r3v_native_separation_audit: dynamic section names no "
          "Gallium library")
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
        source_cases = (
            ("clean", '#include "util/format/u_formats.h"\n'
             "struct pipe_screen;\n"
             "/* comment names pipe_context and pipe/p_defines.h */\n"
             "#ifdef R3V_GALLIUM_BACKEND\n"
             '#include "pipe/p_screen.h"\n'
             "struct pipe_screen *screen;\n"
             "#endif\n"
             "#ifndef R3V_NATIVE_BACKEND\n"
             '#include "pipe/p_defines.h"\n'
             "#endif\n", AUDIT_OK),
            ("include", '#include "pipe/p_defines.h"\n',
             AUDIT_FORBIDDEN_SOURCE),
            ("identifier", "struct pipe_screen *screen;\n",
             AUDIT_FORBIDDEN_SOURCE),
            ("else-arm", "#ifdef R3V_GALLIUM_BACKEND\n"
             "int gallium;\n"
             "#else\n"
             '#include "winsys/radeon_winsys.h"\n'
             "#endif\n", AUDIT_FORBIDDEN_SOURCE),
        )
        for label, content, expected_status in source_cases:
            source = root / f"{label}.c"
            source.write_text(content)
            diagnostics = io.StringIO()
            with contextlib.redirect_stdout(diagnostics):
                status = audit_sources([str(source)])
            if status != expected_status:
                print(f"source selftest {label}: expected "
                      f"{expected_status}, got {status}")
                return 1
        diagnostics = io.StringIO()
        with contextlib.redirect_stderr(diagnostics):
            status = audit_sources([str(root / "absent.c")])
        if status != AUDIT_INPUT_MISSING:
            print(f"source selftest missing-input: got {status}")
            return 1

        import json
        commands = root / "compile_commands.json"
        commands.write_text(json.dumps([
            {"output": "libr3v_native_impl.a.p/a.c.o",
             "command": "cc -Isrc -Iinclude -c a.c"},
            {"output": "other.p/b.c.o",
             "command": "cc -Isrc/gallium/include -c b.c"},
        ]))
        with contextlib.redirect_stdout(io.StringIO()):
            status = audit_compile_commands(str(commands),
                                            "libr3v_native_impl")
        if status != AUDIT_OK:
            print(f"compile-commands selftest clean: got {status}")
            return 1
        commands.write_text(json.dumps([
            {"output": "libr3v_native_impl.a.p/a.c.o",
             "command": "cc -Isrc/gallium/include -c a.c"},
        ]))
        with contextlib.redirect_stdout(io.StringIO()):
            status = audit_compile_commands(str(commands),
                                            "libr3v_native_impl")
        if status != AUDIT_FORBIDDEN_SOURCE:
            print(f"compile-commands selftest forbidden: got {status}")
            return 1
        commands.write_text(json.dumps([]))
        with contextlib.redirect_stderr(io.StringIO()):
            status = audit_compile_commands(str(commands),
                                            "libr3v_native_impl")
        if status != AUDIT_INPUT_MISSING:
            print(f"compile-commands selftest unmatched: got {status}")
            return 1
        with contextlib.redirect_stderr(io.StringIO()):
            status = audit_compile_commands(str(root / "absent.json"),
                                            "libr3v_native_impl")
        if status != AUDIT_INPUT_MISSING:
            print(f"compile-commands selftest missing: got {status}")
            return 1

        readelf = root / "readelf"
        readelf.write_text("#!/bin/sh\ncat \"$2\"\n")
        readelf.chmod(0o755)
        dynamic = root / "clean.so"
        dynamic.write_text(
            " 0x0000000000000001 (NEEDED) Shared library: [libdrm.so.2]\n")
        with contextlib.redirect_stdout(io.StringIO()):
            status = audit_dynamic_section(str(readelf), str(dynamic))
        if status != AUDIT_OK:
            print(f"dynamic selftest clean: got {status}")
            return 1
        dynamic.write_text(
            " 0x0000000000000001 (NEEDED) Shared library: "
            "[libgallium.so]\n")
        with contextlib.redirect_stdout(io.StringIO()):
            status = audit_dynamic_section(str(readelf), str(dynamic))
        if status != AUDIT_FORBIDDEN_SYMBOL:
            print(f"dynamic selftest forbidden: got {status}")
            return 1
        with contextlib.redirect_stderr(io.StringIO()):
            status = audit_dynamic_section(str(readelf),
                                           str(root / "absent.so"))
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
    if len(sys.argv) >= 3 and sys.argv[1] == "--sources":
        return audit_sources(sys.argv[2:])
    if len(sys.argv) == 4 and sys.argv[1] == "--compile-commands":
        return audit_compile_commands(sys.argv[2], sys.argv[3])
    if len(sys.argv) == 4 and sys.argv[1] == "--dynamic":
        return audit_dynamic_section(sys.argv[2], sys.argv[3])
    if len(sys.argv) != 3:
        print("usage: r3v_native_separation_audit.py <nm> <library> | "
              "--sources <file...> | --compile-commands <json> <marker> | "
              "--dynamic <readelf> <library> | --selftest")
        return AUDIT_USAGE
    return audit_library(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    sys.exit(main())
