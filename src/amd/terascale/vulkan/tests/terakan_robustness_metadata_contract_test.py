#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def function_body(source: str, name: str, next_name: str) -> str:
    start = source.index(name)
    end = source.index(next_name, start)
    return source[start:end]


def verify(sources: dict[str, str]) -> None:
    shader = sources["terakan_shader.h"]
    layout = sources["terakan_pipeline_layout.c"]
    metadata = sources["terakan_robustness_metadata.c"]
    lower = sources["nir/terakan_nir_lower_abi.c"]
    gather = sources["nir/terakan_nir_apply_pipeline_layout.c"]

    assert "robustness_metadata_for_mutable_resources_needed" in shader
    assert "update_uav_robustness_metadata_shadow(" in layout
    shadow_body = function_body(
        layout,
        "update_uav_robustness_metadata_shadow(",
        "terakan_set_resource_descriptor_type(",
    )
    assert "mutable_resources[is_compute ? 1 : 0]" in shadow_body
    assert "uavs_for_mutable_resources_needed" not in shadow_body
    assert "fs_uav_index_base" not in shadow_body
    bind_body = function_body(
        layout, "terakan_CmdBindDescriptorSets(", "terakan_pipeline_layout_create"
    )
    assert bind_body.count("update_uav_robustness_metadata_shadow(") == 2

    assert "bound_compute_pipeline->shader" in metadata
    assert "bound_graphics_pipeline" in metadata
    assert "terakan_robustness_metadata_compact(" in metadata
    assert ".mutable_resources[is_compute ? 1 : 0]" in metadata

    gather_body = function_body(
        gather,
        "terakan_nir_gather_robustness_metadata_needed_instr(",
        "terakan_nir_gather_robustness_metadata_needed(",
    )
    for intrinsic in (
        "nir_intrinsic_load_ssbo",
        "nir_intrinsic_get_ssbo_size",
        "nir_intrinsic_store_ssbo",
        "nir_intrinsic_ssbo_atomic",
        "nir_intrinsic_image_deref_store",
        "nir_intrinsic_image_deref_atomic",
    ):
        assert intrinsic in gather_body
    assert "robustness_metadata_for_mutable_resources_needed" in gather_body

    for function_name, next_name in (
        ("terakan_nir_lower_bindings_instr_load_ssbo(", "terakan_nir_lower_bindings_instr_get_ssbo_size("),
        ("terakan_nir_lower_bindings_instr_get_ssbo_size(", "terakan_nir_lower_bindings_instr_store_ssbo("),
        ("terakan_nir_lower_bindings_instr_store_ssbo(", "terakan_nir_lower_bindings_instr_ssbo_atomic("),
        ("terakan_nir_lower_bindings_instr_ssbo_atomic(", "terakan_nir_lower_bindings_instr_image_deref_load("),
        ("terakan_nir_lower_bindings_instr_image_deref_store(", "terakan_nir_lower_bindings_instr_image_deref_size("),
        ("terakan_nir_lower_bindings_instr_image_deref_atomic(", "terakan_nir_lower_bindings_instr("),
    ):
        assert "terakan_nir_get_binding_robustness_metadata(" in function_body(
            lower, function_name, next_name
        )

    slot_loader = function_body(
        lower,
        "terakan_nir_load_robustness_slot_u32(",
        "terakan_nir_emit_write_guard",
    )
    assert "component_dyn" in slot_loader
    assert "vec4_index_dyn" in slot_loader


def expect_mutant_rejected(sources: dict[str, str], filename: str,
                           old: str, new: str) -> None:
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
        "terakan_shader.h",
        "terakan_pipeline_layout.c",
        "terakan_robustness_metadata.c",
        "nir/terakan_nir_lower_abi.c",
        "nir/terakan_nir_apply_pipeline_layout.c",
    )
    sources = {filename: (ROOT / filename).read_text() for filename in filenames}
    verify(sources)
    expect_mutant_rejected(
        sources,
        "terakan_robustness_metadata.c",
        "terakan_robustness_metadata_compact(",
        "terakan_robustness_metadata_compact_removed(",
    )
    expect_mutant_rejected(
        sources,
        "terakan_pipeline_layout.c",
        "mutable_resources[is_compute ? 1 : 0]",
        "mutable_resources[0]",
    )
    expect_mutant_rejected(
        sources,
        "nir/terakan_nir_apply_pipeline_layout.c",
        "state->robustness_metadata_for_mutable_resources_needed,",
        "state->uavs_for_mutable_resources_needed,",
    )


if __name__ == "__main__":
    main()
