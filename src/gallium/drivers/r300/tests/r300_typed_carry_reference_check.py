#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Typed-carry retained reference: byte identity against the digest manifest.

The corpus header embeds each module's SPIR-V words; the manifest beside it
records the sha256 of every module's little-endian byte image and of every
fixture of record (GLSL and SPIR-V assembly sources).  The checker rebuilds
each digest from the header and the source files and compares it with the
manifest, so an edited header, a regenerated module, a changed source, a
module missing from the manifest, or a manifest row naming no module all
fail.  Three manifest rows reproduce the sha256 a retained silicon bundle
recorded for the modules it replayed, which is the identity the reference
claims.
"""

from __future__ import annotations

import hashlib
import re
import struct
import sys
import tempfile
from pathlib import Path

ARRAY_RE = re.compile(
    r"static const uint32_t (\w+)_spirv\[\] = \{(.*?)\};", re.DOTALL)
WORD_RE = re.compile(r"0x([0-9a-fA-F]{1,8})")

STATUS_OK = 0
STATUS_MISMATCH = 1
STATUS_USAGE = 2


def header_modules(text: str) -> dict[str, bytes]:
    """Return {module name: little-endian SPIR-V bytes} from the header."""
    modules: dict[str, bytes] = {}
    for match in ARRAY_RE.finditer(text):
        words = [int(word, 16) for word in WORD_RE.findall(match.group(2))]
        modules[match.group(1)] = b"".join(
            struct.pack("<I", word) for word in words)
    return modules


def read_manifest(text: str) -> dict[str, str]:
    """Return {entry: sha256 hex} from sha256sum-shaped lines."""
    rows: dict[str, str] = {}
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 2 or len(parts[0]) != 64:
            raise ValueError(f"manifest line {number}: expected "
                             f"'<sha256>  <entry>', got {raw!r}")
        if parts[1] in rows:
            raise ValueError(f"manifest line {number}: duplicate entry "
                             f"{parts[1]}")
        rows[parts[1]] = parts[0].lower()
    return rows


def check(header_text: str, manifest_text: str,
          sources: dict[str, bytes]) -> list[str]:
    """Return the mismatch list for one header, manifest, and source set."""
    failures: list[str] = []
    modules = header_modules(header_text)
    if not modules:
        return ["header embeds no SPIR-V module"]
    try:
        rows = read_manifest(manifest_text)
    except ValueError as error:
        return [str(error)]

    observed: dict[str, str] = {}
    for name, data in modules.items():
        observed[f"{name}.spv"] = hashlib.sha256(data).hexdigest()
    for name, data in sources.items():
        observed[name] = hashlib.sha256(data).hexdigest()

    for entry, expected in rows.items():
        actual = observed.get(entry)
        if actual is None:
            failures.append(f"manifest entry {entry} names no header module "
                            "or fixture of record")
        elif actual != expected:
            failures.append(f"{entry}: manifest {expected} observed {actual}")
    for entry in observed:
        if entry not in rows:
            failures.append(f"{entry} has no manifest row")
    return failures


def load_sources(corpus_dir: Path) -> dict[str, bytes]:
    return {
        path.name: path.read_bytes()
        for path in sorted(corpus_dir.iterdir())
        if path.suffix in {".vert", ".spvasm"}
    }


def run(header: Path, manifest: Path, corpus_dir: Path) -> int:
    failures = check(header.read_text(encoding="utf-8"),
                     manifest.read_text(encoding="utf-8"),
                     load_sources(corpus_dir))
    if failures:
        print("\n".join(failures))
        return STATUS_MISMATCH
    print("r300_typed_carry_reference_check: header modules and fixtures "
          "of record match the retained manifest")
    return STATUS_OK


def selftest(header: Path, manifest: Path, corpus_dir: Path) -> int:
    header_text = header.read_text(encoding="utf-8")
    manifest_text = manifest.read_text(encoding="utf-8")
    sources = load_sources(corpus_dir)

    def expect(label: str, failures: list[str], want_failure: bool,
               marker: str = "") -> bool:
        if bool(failures) != want_failure:
            print(f"selftest {label}: expected "
                  f"{'failure' if want_failure else 'success'}, "
                  f"got {failures!r}")
            return False
        if marker and not any(marker in line for line in failures):
            print(f"selftest {label}: expected a failure naming {marker!r}, "
                  f"got {failures!r}")
            return False
        return True

    ok = True
    ok &= expect("known-good", check(header_text, manifest_text, sources),
                 False)

    # One SPIR-V word flipped inside the first module.
    first = ARRAY_RE.search(header_text)
    first_word = WORD_RE.search(first.group(2))
    mutated = (header_text[:first.start(2) + first_word.start()] +
               "0x0badf00d" +
               header_text[first.start(2) + first_word.end():])
    ok &= expect("mutated-word", check(mutated, manifest_text, sources),
                 True, f"{first.group(1)}.spv")

    # A module the manifest does not name.
    extra = header_text + (
        "\nstatic const uint32_t t_outside_spirv[] = {\n   0x07230203,\n};\n")
    ok &= expect("extra-module", check(extra, manifest_text, sources), True,
                 "t_outside.spv has no manifest row")

    # A manifest row naming nothing.
    ghost = manifest_text + ("0" * 64) + "  t_ghost.spv\n"
    ok &= expect("ghost-row", check(header_text, ghost, sources), True,
                 "t_ghost.spv names no header module")

    # A fixture of record edited without regeneration.
    edited = dict(sources)
    name = sorted(edited)[0]
    edited[name] = edited[name] + b"\n// edited\n"
    ok &= expect("edited-source", check(header_text, manifest_text, edited),
                 True, name)

    # A manifest row the header no longer matches after a module is removed.
    removed = ARRAY_RE.sub("", header_text, count=1)
    ok &= expect("removed-module", check(removed, manifest_text, sources),
                 True, f"{first.group(1)}.spv names no header module")

    if not ok:
        return STATUS_MISMATCH
    print("r300_typed_carry_reference_check: known-good, mutated-word, "
          "extra-module, ghost-row, edited-source, and removed-module "
          "verdicts calibrated")
    return STATUS_OK


def main(argv: list[str]) -> int:
    if len(argv) == 5 and argv[1] == "--selftest":
        return selftest(Path(argv[2]), Path(argv[3]), Path(argv[4]))
    if len(argv) == 4:
        return run(Path(argv[1]), Path(argv[2]), Path(argv[3]))
    print("usage: r300_typed_carry_reference_check.py [--selftest] "
          "<header> <manifest> <corpus-dir>", file=sys.stderr)
    return STATUS_USAGE


if __name__ == "__main__":
    sys.exit(main(sys.argv))
