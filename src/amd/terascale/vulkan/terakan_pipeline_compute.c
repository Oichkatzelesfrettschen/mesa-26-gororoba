/*
 * Copyright © 2026 steinmarder project
 * SPDX-License-Identifier: MIT
 *
 * Vulkan compute pipeline implementation for Terakan (TeraScale-2/Evergreen).
 *
 * This provides the native vk_shader abstraction for compute shaders,
 * enabling GPU-driven rendering via vkCmdDispatch → indirect draw buffer
 * population. The pipeline compiles SPIR-V → NIR → r600 bytecode and
 * pre-computes all HW register values for dispatch emission.
 */

#include "terakan_pipeline_compute.h"

#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_pipeline.h"
#include "terakan_shader.h"

#include "terakan_pipeline_cache.h"
#include "terakan_pipeline_key.h"

#include "gallium/drivers/r600/r600_shader_common.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_pipeline.h"
#include "vk_pipeline_cache.h"
#include "vk_shader_module.h"

#include <assert.h>
#include <string.h>


/*
 * Phase A: Allocate and initialize base compute pipeline.
 */
static VkResult
terakan_pipeline_compute_init(struct terakan_device * const device,
                              VkAllocationCallbacks const * const allocator,
                              struct terakan_pipeline_compute ** const pipeline_out)
{
   struct terakan_pipeline_compute *pipeline =
      vk_alloc2(&device->vk.alloc, allocator, sizeof(*pipeline), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   memset(pipeline, 0, sizeof(*pipeline));
   terakan_pipeline_init(&pipeline->base, device, true);

   *pipeline_out = pipeline;
   return VK_SUCCESS;
}

/*
 * Phase B: Compile compute shader with cache integration.
 *
 * Extracts local_size[3] and group_size from NIR BEFORE ralloc_free
 * (RD-4 — these cannot be reconstructed after NIR is freed, especially
 * on cache hit where we skip compilation entirely).
 */
static VkResult
terakan_pipeline_compute_compile(
   struct terakan_pipeline_compute * const pipeline,
   struct terakan_device * const device,
   VkComputePipelineCreateInfo const * const create_info,
   struct vk_pipeline_cache * const cache,
   VkAllocationCallbacks const * const allocator)
{
   VkPipelineShaderStageCreateInfo const *stage_info = &create_info->stage;
   assert(stage_info->stage == VK_SHADER_STAGE_COMPUTE_BIT);

   size_t spirv_size = 0;
   uint32_t const *spirv = terakan_pipeline_stage_spirv(stage_info, &spirv_size);

   nir_shader *nir = terakan_shader_spirv_to_nir(
      device, spirv_size, spirv, MESA_SHADER_COMPUTE,
      stage_info->pName ? stage_info->pName : "main",
      stage_info->pSpecializationInfo);
   if (nir == NULL)
      return VK_ERROR_UNKNOWN;

   /* Extract workgroup size from NIR BEFORE any path that frees it (RD-4). */
   pipeline->local_size[0] = nir->info.workgroup_size[0];
   pipeline->local_size[1] = nir->info.workgroup_size[1];
   pipeline->local_size[2] = nir->info.workgroup_size[2];
   pipeline->group_size = pipeline->local_size[0] *
                          pipeline->local_size[1] *
                          pipeline->local_size[2];

   /* Post-link lowering — populates pre-compile metadata (Invariant 3). */
   terakan_shader_lower_and_optimize_post_link(
      nir,
      create_info->layout != VK_NULL_HANDLE
         ? terakan_pipeline_layout_from_handle(create_info->layout)
         : NULL,
      pipeline->shader.resources_needed,
      &pipeline->shader.samplers_needed,
      pipeline->shader.uavs_for_mutable_resources_needed,
      &pipeline->shader.push_constants_usage.driver_constants,
      &pipeline->shader.kcache_needed,
      NULL);

   /* Build cache key (Invariant 4: only after post-link lowering). */
   VkPipelineCreateFlags2KHR const pipeline_flags =
      terakan_pipeline_create_flags(create_info->flags, create_info->pNext);

   struct terakan_shader_stage_key stage_key;
   terakan_shader_stage_key_fill(&stage_key, device, stage_info, pipeline_flags);

   union r600_shader_key shader_key;
   memset(&shader_key, 0, sizeof(shader_key));

   blake3_hash spirv_hash;
   struct vk_pipeline_robustness_state rs;
   vk_pipeline_robustness_state_fill(&device->vk, &rs,
                                     create_info->pNext, stage_info->pNext);
   vk_pipeline_hash_shader_stage(pipeline_flags, stage_info, &rs, spirv_hash);

   blake3_hash cache_key;
   terakan_pipeline_cache_hash_shader(cache_key, device, &stage_key,
                                      &shader_key, spirv_hash);

   /* Cache lookup — skip compilation on hit (Invariant 3). */
   struct terakan_cached_shader *cached =
      terakan_pipeline_cache_lookup(cache, cache_key);
   VkResult result;
   if (cached != NULL) {
      result = terakan_cached_shader_restore(cached, &pipeline->shader, device, allocator);
      vk_pipeline_cache_object_unref(&device->vk, &cached->base);
      ralloc_free(nir);
      if (result != VK_SUCCESS)
         return result;
   } else {
      result = terakan_shader_impl_compile(
         &pipeline->shader, device, &shader_key, nir, allocator);
      size_t const program_size_bytes = sizeof(uint32_t) * pipeline->shader.shader.bc.ndw;
      ralloc_free(nir);
      if (result != VK_SUCCESS) {
         /* compile internally frees arrays on failure but doesn't NULL
          * the pointer.  Clear it to prevent double-free in
          * _destroy → shader_impl_finish. */
         pipeline->shader.shader.arrays = NULL;
         return result;
      }
      terakan_pipeline_cache_insert(cache, cache_key, &pipeline->shader,
                                    MESA_SHADER_COMPUTE, program_size_bytes, device);
   }

   return VK_SUCCESS;
}

/*
 * Phase C: Pre-compute hardware register values from compiled shader.
 * local_size + group_size were already populated by _compile (RD-4).
 */
static void
terakan_pipeline_compute_fill_hw(struct terakan_pipeline_compute * const pipeline)
{
   pipeline->sq_pgm_start_cs = pipeline->shader.static_state.program_va_shr8;
   memcpy(pipeline->sq_pgm_resources_cs,
          pipeline->shader.static_state.sq_pgm_resources,
          sizeof(pipeline->sq_pgm_resources_cs));

   /* LDS allocation: (lds_size_dwords | num_waves << 14)
    * Conservative 1 wave for now. */
   uint32_t lds_size_dwords = 0;
   pipeline->sq_lds_alloc = lds_size_dwords | (1 << 14);
}

/*
 * Orchestrator — thin caller with single destroy error path (Invariant 2).
 */
VkResult
terakan_pipeline_compute_create(struct terakan_device * const device,
                                struct vk_pipeline_cache * const cache,
                                VkComputePipelineCreateInfo const * const create_info,
                                VkAllocationCallbacks const * const allocator,
                                VkPipeline * const pipeline_out)
{
   struct terakan_pipeline_compute *pipeline;
   VkResult result;

   result = terakan_pipeline_compute_init(device, allocator, &pipeline);
   if (result != VK_SUCCESS)
      return result;

   result = terakan_pipeline_compute_compile(
      pipeline, device, create_info, cache, allocator);
   if (result != VK_SUCCESS)
      goto fail;

   terakan_pipeline_compute_fill_hw(pipeline);

   *pipeline_out = terakan_pipeline_to_handle(&pipeline->base);
   return VK_SUCCESS;

fail:
   terakan_pipeline_compute_destroy(pipeline, allocator);
   return result;
}

void
terakan_pipeline_compute_destroy(struct terakan_pipeline_compute * const pipeline,
                                 VkAllocationCallbacks const * const allocator)
{
   struct terakan_device *device =
      container_of(pipeline->base.base.device, struct terakan_device, vk);

   terakan_shader_impl_finish(&pipeline->shader, allocator);
   terakan_pipeline_finish(&pipeline->base);
   vk_free2(&device->vk.alloc, allocator, pipeline);
}

/* ---- Vulkan entrypoints ---- */

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateComputePipelines(VkDevice const deviceHandle,
                               VkPipelineCache const pipelineCache,
                               uint32_t const createInfoCount,
                               VkComputePipelineCreateInfo const * const pCreateInfos,
                               VkAllocationCallbacks const * const pAllocator,
                               VkPipeline * const pPipelines)
{
   struct terakan_device *device = terakan_device_from_handle(deviceHandle);
   VkResult overall_result = VK_SUCCESS;

   for (uint32_t i = 0; i < createInfoCount; i++) {
      struct vk_pipeline_cache *cache =
         pipelineCache != VK_NULL_HANDLE
            ? vk_pipeline_cache_from_handle(pipelineCache) : NULL;
      VkResult result = terakan_pipeline_compute_create(
         device, cache, &pCreateInfos[i], pAllocator, &pPipelines[i]);
      if (result != VK_SUCCESS) {
         pPipelines[i] = VK_NULL_HANDLE;
         overall_result = result;
         /* VK spec: continue creating remaining pipelines */
      }
   }

   return overall_result;
}
