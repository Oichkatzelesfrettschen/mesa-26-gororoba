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
    ("r3v_reference_fragment_blue_spirv",
     HERE / "r3v_reference_triangle_blue.frag"),
    ("r3v_reference_identity_map_spirv",
     HERE / "r3v_reference_identity_map.comp"),
    ("r3v_reference_scatter_reject_spirv",
     HERE / "r3v_reference_scatter_reject.comp"),
    ("r3v_reference_bitwise_not_spirv",
     HERE / "r3v_reference_bitwise_not.comp"),
    ("r3v_reference_vertex_arith_spirv",
     HERE / "r3v_reference_vertex_arith.vert"),
    ("r3v_reference_vertex_varying_spirv",
     HERE / "r3v_reference_vertex_varying.vert"),
    ("r3v_reference_fragment_varying_spirv",
     HERE / "r3v_reference_fragment_varying.frag"),
    ("r3v_reference_vertex_two_attributes_spirv",
     HERE / "r3v_reference_vertex_two_attributes.vert"),
    ("r3v_reference_vertex_instance_offset_spirv",
     HERE / "r3v_reference_vertex_instance_offset.vert"),
    ("r3v_reference_vertex_instance_index_spirv",
     HERE / "r3v_reference_vertex_instance_index.vert"),
    ("r3v_reference_vertex_vertex_index_spirv",
     HERE / "r3v_reference_vertex_vertex_index.vert"),
    ("r3v_reference_vertex_flat_spirv", HERE / "r3v_reference_vertex_flat.vert"),
    ("r3v_reference_fragment_flat_spirv", HERE / "r3v_reference_fragment_flat.frag"),
    ("r3v_reference_vertex_flat_saturated_spirv", HERE / "r3v_reference_vertex_flat_saturated.vert"),
    ("r3v_reference_vertex_flat_rgba_spirv", HERE / "r3v_reference_vertex_flat_rgba.vert"),
    ("r3v_reference_vertex_mixed_spirv", HERE / "r3v_reference_vertex_mixed.vert"),
    ("r3v_reference_fragment_mixed_spirv", HERE / "r3v_reference_fragment_mixed.frag"),
    ("r3v_reference_fragment_noperspective_spirv", HERE / "r3v_reference_fragment_noperspective.frag"),
    ("r3v_reference_vertex_clip_cull_spirv", HERE / "r3v_reference_vertex_clip_cull.vert"),
    ("r3v_reference_vertex_block_spirv", HERE / "r3v_reference_vertex_block.vert"),
    ("r3v_reference_fragment_block_spirv", HERE / "r3v_reference_fragment_block.frag"),
    ("r3v_reference_vertex_scalar_spirv", HERE / "r3v_reference_vertex_scalar.vert"),
    ("r3v_reference_fragment_scalar_spirv", HERE / "r3v_reference_fragment_scalar.frag"),
    ("r3v_reference_vertex_int_spirv", HERE / "r3v_reference_vertex_int.vert"),
    ("r3v_reference_fragment_int_spirv", HERE / "r3v_reference_fragment_int.frag"),
    ("r3v_reference_vertex_components_spirv", HERE / "r3v_reference_vertex_components.vert"),
    ("r3v_reference_fragment_components_spirv", HERE / "r3v_reference_fragment_components.frag"),
    ("r3v_reference_vertex_two_attributes_vec3_spirv", HERE / "r3v_reference_vertex_two_attributes_vec3.vert"),
    ("r3v_reference_vertex_two_attributes_vec2_spirv", HERE / "r3v_reference_vertex_two_attributes_vec2.vert"),
    ("r3v_reference_vertex_two_attributes_float_spirv", HERE / "r3v_reference_vertex_two_attributes_float.vert"),
    ("r3v_reference_vertex_varying_vec2_component1_spirv", HERE / "r3v_reference_vertex_varying_vec2_component1.vert"),
    ("r3v_reference_fragment_noperspective_vec3_spirv", HERE / "r3v_reference_fragment_noperspective_vec3.frag"),
    ("r3v_reference_fragment_noperspective_vec2_spirv", HERE / "r3v_reference_fragment_noperspective_vec2.frag"),
    ("r3v_reference_fragment_noperspective_float_spirv", HERE / "r3v_reference_fragment_noperspective_float.frag"),
    ("r3v_reference_fragment_noperspective_vec2_component1_spirv", HERE / "r3v_reference_fragment_noperspective_vec2_component1.frag"),
    ("r3v_reference_vertex_varying_vec3_spirv", HERE / "r3v_reference_vertex_varying_vec3.vert"),
    ("r3v_reference_vertex_varying_vec2_spirv", HERE / "r3v_reference_vertex_varying_vec2.vert"),
    ("r3v_reference_vertex_varying_float_spirv", HERE / "r3v_reference_vertex_varying_float.vert"),
    ("r3v_reference_fragment_smooth_vec3_spirv", HERE / "r3v_reference_fragment_smooth_vec3.frag"),
    ("r3v_reference_vertex_mixed_carrier_spirv", HERE / "r3v_reference_vertex_mixed_carrier.vert"),
    ("r3v_reference_fragment_mixed_carrier_spirv", HERE / "r3v_reference_fragment_mixed_carrier.frag"),
    ("r3v_reference_vertex_two_attributes_mixed_carrier_spirv", HERE / "r3v_reference_vertex_two_attributes_mixed_carrier.vert"),
    ("r3v_reference_fragment_flat_mixed_carrier_spirv", HERE / "r3v_reference_fragment_flat_mixed_carrier.frag"),
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
