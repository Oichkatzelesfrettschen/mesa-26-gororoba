/*
 * Copyright © 2026 steinmarder project
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_PIPELINE_COMPUTE_H
#define TERAKAN_PIPELINE_COMPUTE_H

#include "terakan_pipeline.h"
#include "terakan_shader.h"

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_pipeline_compute {
   struct terakan_pipeline base;

   struct terakan_shader_impl shader;

   /* FIX-W (Q-2026-04-20): per-baseArrayLayer compiled shader variants.
    * When fix_w_enabled is set (gated by TERAKAN_FIX_W_PRODUCTION=1),
    * the create path also compiles 8 variants with the FIX-K base
    * array layer baked as a NIR literal at compile time.  At dispatch,
    * terakan_dispatch.c selects fix_w_variants[bound_layer] instead of
    * the base shader and emits the variant's SQ_PGM_START_CS.
    *
    * NULL slot means "variant not compiled / not needed".  Layer 0 is
    * intentionally a separate variant (rather than reusing the base)
    * because the base shader was compiled with the wedged kcache path,
    * while variant[0] has coord.z = 0 baked as a literal -- they are
    * not bytecode-identical. */
   struct terakan_shader_impl *fix_w_variants[8];
   bool fix_w_enabled;

   /* Local workgroup size from the shader's execution mode */
   uint32_t local_size[3];

   /* Precomputed HW register values */
   uint32_t sq_pgm_start_cs;        /* program address >> 8 */
   uint32_t sq_pgm_resources_cs[2]; /* GPR count, stack size, etc. */
   uint32_t sq_lds_alloc;           /* LDS allocation (size | num_waves << 14) */
   uint32_t group_size;             /* local_size[0] * [1] * [2] */
};

struct terakan_device;
struct vk_pipeline_cache;

VkResult terakan_pipeline_compute_create(struct terakan_device *device,
                                         struct vk_pipeline_cache *cache,
                                         VkComputePipelineCreateInfo const *create_info,
                                         VkAllocationCallbacks const *allocator,
                                         VkPipeline *pipeline_out);

void terakan_pipeline_compute_destroy(struct terakan_pipeline_compute *pipeline,
                                      VkAllocationCallbacks const *allocator);

/* FIX-W (Q-2026-04-20): set the compile-time baseArrayLayer literal for
 * the next NIR lowering pass.  Set INT32_MIN to disable.  Defined in
 * src/amd/terascale/vulkan/nir/terakan_nir_lower_abi.c. */
void terakan_fix_w_set_compile_layer(int layer);
void terakan_fix_w_set_compile_ubo(int value);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_PIPELINE_COMPUTE_H */
