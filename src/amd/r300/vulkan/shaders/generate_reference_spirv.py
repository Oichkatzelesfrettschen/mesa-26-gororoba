#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regenerates r3v_native_reference_spirv.h from the GLSL sources.

The header's arrays are the byte-equality admission contract for the
public pipeline, so the generator is deterministic: glslangValidator -V
--target-env vulkan1.0 compiles each stage without debug information,
and the words print eight per line in fixed hex.  Run from the
repository root; a glslangValidator version change that alters the
emitted words changes the admission contract and needs the review that
accompanies a contract change.

Usage: shaders/generate_reference_spirv.py [--check]
  --check: regenerate to a buffer and fail if the checked-in header's
  arrays differ.
"""
import pathlib
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
HEADER = HERE.parent / "r3v_native_reference_spirv.h"
STAGES = (
    ("r3v_reference_vertex_spirv", HERE / "r3v_reference_triangle.vert"),
    ("r3v_reference_fragment_spirv", HERE / "r3v_reference_triangle.frag"),
    ("r3v_reference_identity_map_spirv",
     HERE / "r3v_reference_identity_map.comp"),
    ("r3v_reference_scatter_reject_spirv",
     HERE / "r3v_reference_scatter_reject.comp"),
)


def compile_words(source):
    with tempfile.NamedTemporaryFile(suffix=".spv") as out:
        subprocess.run(
            ["glslangValidator", "-V", "--target-env", "vulkan1.0",
             str(source), "-o", out.name],
            check=True, stdout=subprocess.DEVNULL)
        data = pathlib.Path(out.name).read_bytes()
    if len(data) % 4 != 0:
        raise SystemExit(f"{source}: SPIR-V size {len(data)} not whole words")
    return struct.unpack(f"<{len(data) // 4}I", data)


def array_text(name, words):
    lines = [f"static const uint32_t {name}[] = {{"]
    for i in range(0, len(words), 4):
        row = ", ".join(f"0x{w:08x}" for w in words[i:i + 4])
        lines.append(f"   {row},")
    lines.append("};")
    return "\n".join(lines)


def main():
    check = sys.argv[1:] == ["--check"]
    header = HEADER.read_text()
    for name, source in STAGES:
        text = array_text(name, compile_words(source))
        start = header.index(f"static const uint32_t {name}[")
        end = header.index("};", start) + 2
        header = header[:start] + text + header[end:]
    if check:
        if header != HEADER.read_text():
            raise SystemExit("reference SPIR-V header differs from "
                             "regenerated output")
        print("reference SPIR-V header matches regenerated output")
        return
    HEADER.write_text(header)
    print(f"wrote {HEADER}")


if __name__ == "__main__":
    main()
