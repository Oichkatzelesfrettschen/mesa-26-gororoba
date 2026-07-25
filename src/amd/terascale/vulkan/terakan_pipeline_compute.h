/*
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

   /* Per-baseArrayLayer storage-image shader variants. When enabled,
    * pipeline creation compiles one variant per layer with the storage-image
    * slice and matching layer-index scalar baked as NIR literals.
    *
    * NULL slot means "variant not compiled / not needed".  Layer 0 is
    * intentionally a separate variant (rather than reusing the base)
    * because the base shader was compiled with the wedged kcache path,
    * while variant[0] has coord.z = 0 baked as a literal -- they are
    * not bytecode-identical. */
   struct terakan_shader_impl *storage_image_layer_variants[8];
   bool storage_image_layer_variants_enabled;

   /* Local workgroup size from the shader's execution mode */
   uint32_t local_size[3];

   /* Precomputed HW register values */
   uint32_t sq_pgm_start_cs;        /* program address >> 8 */
   uint32_t sq_pgm_resources_cs[2]; /* GPR count, stack size, etc. */
   uint32_t sq_lds_alloc;           /* LDS allocation (size | num_waves << 14) */
   uint32_t group_size;             /* local_size[0] * [1] * [2] */
   uint32_t shared_size_dwords;     /* shader shared memory allocation */
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

void terakan_storage_image_set_compile_layer(int layer);
void terakan_storage_image_set_compile_layer_index(int value);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_PIPELINE_COMPUTE_H */
