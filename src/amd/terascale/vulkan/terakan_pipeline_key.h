/*
 * Copyright © 2026 steinmarder project
 * SPDX-License-Identifier: MIT
 *
 * Pipeline cache key structures for Terakan (TeraScale-2 / Evergreen).
 *
 * These structs capture ALL state that affects shader compilation output.
 * They are fed directly into BLAKE3 hashing to produce pipeline cache keys,
 * so struct layout MUST be padding-free and zero-initialized before population.
 *
 * Design:
 *   - Fields ordered by descending alignment to eliminate implicit padding
 *   - Boolean flags packed into uint32_t bitfields (no inter-type padding)
 *   - static_assert on sizeof to catch ABI drift
 *   - Fill functions zero-init the struct at entry
 */

#ifndef TERAKAN_PIPELINE_KEY_H
#define TERAKAN_PIPELINE_KEY_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#include "compiler/shader_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-shader-stage compilation key.
 *
 * Captures VkPipelineShaderStageCreateInfo flags and device-level settings
 * that affect code generation.  Mirrors RADV's radv_shader_stage_key adapted
 * for R600 capabilities (no mesh/task shaders, fixed wave64).
 *
 * 4 bytes, zero padding.
 */
struct terakan_shader_stage_key {
   uint32_t storage_robustness2 : 1;
   uint32_t uniform_robustness2 : 1;
   uint32_t vertex_robustness1 : 1;
   uint32_t optimisations_disabled : 1;
   uint32_t keep_statistic_info : 1;
   /*
    * Shader version counter — bump to force recompilation after driver
    * changes when using build-ID override.  3 bits = versions 0..7.
    */
   uint32_t version : 3;
   uint32_t reserved : 24;
};

static_assert(sizeof(struct terakan_shader_stage_key) == 4,
              "terakan_shader_stage_key must be exactly 4 bytes for deterministic hashing");

/*
 * Graphics pipeline state key.
 *
 * Captures VkGraphicsPipelineCreateInfo state that feeds into r600_shader_key
 * population and NIR lowering decisions.  Every field here potentially
 * changes the compiled shader binary; if two pipelines differ only in state
 * NOT captured here, they MUST produce identical shaders.
 *
 * 8 bytes, zero padding.
 */
struct terakan_graphics_state_key {
   /* --- 32-bit flags word --- */
   uint32_t enable_remove_point_size : 1;
   uint32_t provoking_vtx_last : 1;
   uint32_t color_two_side : 1;
   uint32_t alpha_to_one : 1;
   uint32_t apply_sample_id_mask : 1;
   uint32_t dual_source_blend : 1;
   uint32_t sample_shading_enable : 1;
   /* Tessellation / geometry shader presence (affect VS output mode) */
   uint32_t vs_as_es : 1;
   uint32_t vs_as_ls : 1;
   uint32_t vs_as_gs_a : 1;
   uint32_t reserved_flags : 22;

   /* --- uint8_t fields (4 bytes, naturally packed) --- */
   uint8_t ia_topology;                /* VkPrimitiveTopology */
   uint8_t ps_nr_cbufs;               /* color buffer count for PS exports */
   uint8_t rs_cull_mode;              /* VkCullModeFlags (low 2 bits) */
   uint8_t ms_rasterization_samples;  /* log2(sample count) */
};

static_assert(sizeof(struct terakan_graphics_state_key) == 8,
              "terakan_graphics_state_key must be exactly 8 bytes for deterministic hashing");

struct terakan_device;

/*
 * Fill a shader stage key from pipeline create info.
 * Zero-initializes the struct before population.
 */
void
terakan_shader_stage_key_fill(struct terakan_shader_stage_key *key,
                              struct terakan_device const *device,
                              VkPipelineShaderStageCreateInfo const *stage_info,
                              VkPipelineCreateFlags2KHR pipeline_flags);

/*
 * Fill a graphics state key from pipeline create info.
 *
 * ps_nr_cbufs comes from NIR analysis (fragment_data_uncompacted_locations),
 * so this must be called AFTER spirv_to_nir + post-link lowering but BEFORE
 * r600 backend compilation.
 *
 * Zero-initializes the struct before population.
 */
void
terakan_graphics_state_key_fill(struct terakan_graphics_state_key *key,
                                VkGraphicsPipelineCreateInfo const *create_info,
                                VkShaderStageFlags shader_stages,
                                uint8_t ps_nr_cbufs);

/*
 * Populate an r600_shader_key from the graphics state key.
 *
 * Bridges the new key infrastructure to the existing r600 backend interface.
 */
union r600_shader_key;

void
terakan_r600_shader_key_from_state(union r600_shader_key *r600_key,
                                   struct terakan_graphics_state_key const *state_key,
                                   mesa_shader_stage stage);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_PIPELINE_KEY_H */
