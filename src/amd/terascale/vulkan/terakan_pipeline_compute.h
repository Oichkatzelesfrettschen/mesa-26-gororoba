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

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_PIPELINE_COMPUTE_H */
