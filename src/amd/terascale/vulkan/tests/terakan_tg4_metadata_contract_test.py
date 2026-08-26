#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def verify(sources: dict[str, str]) -> None:
    descriptor_layout = sources["terakan_descriptor_set_layout.c"]
    pipeline_layout = sources["terakan_pipeline_layout.c"]
    metadata_upload = sources["terakan_robustness_metadata.c"]
    tg4_lowering = sources["nir/terakan_nir_lower_tg4_view_swizzle.c"]
    shader = sources["terakan_shader.c"]

    assert "layout_shader->sampled_image_count = stage_sampled_image_count" in descriptor_layout
    assert "terakan_tg4_metadata_map_add_range(" in pipeline_layout
    assert "s->first_shader_tg4_metadata[st]" in pipeline_layout
    assert "terakan_tg4_metadata_set_swizzle(" in pipeline_layout
    assert "view_swizzles,\n                            stage, metadata_index" in pipeline_layout

    assert "TERAKAN_KCACHE_HW_LINE_BYTES * MESA_SHADER_STAGES" in metadata_upload
    assert "view_swizzles[stage]" in metadata_upload
    assert "va_kcache_lines + stage_index" in metadata_upload

    assert "terakan_tg4_metadata_index(state->metadata_map, tex->texture_index" in tg4_lowering
    assert "nir_imm_int(b, metadata_index)" in tg4_lowering
    assert "nir_imm_int(b, tex->texture_index)" not in tg4_lowering
    assert "resource_index + TERAKAN_GATHER_DESCRIPTOR_SLOT_OFFSET" in tg4_lowering
    assert "shader_tg4_metadata_maps[nir->info.stage]" in shader


def expect_mutant_rejected(
    sources: dict[str, str], filename: str, old: str, new: str
) -> None:
    mutant = dict(sources)
    assert old in mutant[filename]
    mutant[filename] = mutant[filename].replace(old, new, 1)
    try:
        verify(mutant)
    except AssertionError:
        return
    raise AssertionError(f"mutant survived: {filename}: {old}")


def main() -> None:
    filenames = (
        "terakan_descriptor_set_layout.c",
        "terakan_pipeline_layout.c",
        "terakan_robustness_metadata.c",
        "nir/terakan_nir_lower_tg4_view_swizzle.c",
        "terakan_shader.c",
    )
    sources = {filename: (ROOT / filename).read_text() for filename in filenames}
    verify(sources)
    expect_mutant_rejected(
        sources,
        "terakan_pipeline_layout.c",
        "view_swizzles,\n                            stage, metadata_index",
        "view_swizzles[0],\n                            MESA_SHADER_VERTEX, metadata_index",
    )
    expect_mutant_rejected(
        sources,
        "terakan_robustness_metadata.c",
        "va_kcache_lines + stage_index",
        "va_kcache_lines",
    )
    expect_mutant_rejected(
        sources,
        "nir/terakan_nir_lower_tg4_view_swizzle.c",
        "nir_imm_int(b, metadata_index)",
        "nir_imm_int(b, tex->texture_index)",
    )


if __name__ == "__main__":
    main()
