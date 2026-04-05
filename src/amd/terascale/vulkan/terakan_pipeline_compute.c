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

#include "gallium/drivers/r600/r600_shader_common.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_shader_module.h"

#include <assert.h>
#include <string.h>

VkResult
terakan_pipeline_compute_create(struct terakan_device * const device,
                                UNUSED VkPipelineCache const cache,
                                VkComputePipelineCreateInfo const * const create_info,
                                VkAllocationCallbacks const * const allocator,
                                VkPipeline * const pipeline_out)
{
   struct terakan_pipeline_compute *pipeline =
      vk_alloc2(&device->vk.alloc, allocator, sizeof(*pipeline), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   memset(pipeline, 0, sizeof(*pipeline));

   terakan_pipeline_init(&pipeline->base, device, true /* is_compute */);

   /* Get SPIR-V from the stage info */
   VkPipelineShaderStageCreateInfo const *stage_info = &create_info->stage;
   assert(stage_info->stage == VK_SHADER_STAGE_COMPUTE_BIT);

   size_t spirv_size = 0;
   uint32_t const *spirv = terakan_pipeline_stage_spirv(stage_info, &spirv_size);

   /* SPIR-V → NIR */
   nir_shader *nir = terakan_shader_spirv_to_nir(
      device, spirv_size, spirv, MESA_SHADER_COMPUTE,
      stage_info->pName ? stage_info->pName : "main",
      stage_info->pSpecializationInfo);
   if (nir == NULL) {
      vk_free2(&device->vk.alloc, allocator, pipeline);
      return VK_ERROR_UNKNOWN;
   }

   /* Extract local workgroup size from the shader */
   pipeline->local_size[0] = nir->info.workgroup_size[0];
   pipeline->local_size[1] = nir->info.workgroup_size[1];
   pipeline->local_size[2] = nir->info.workgroup_size[2];
   pipeline->group_size = pipeline->local_size[0] *
                          pipeline->local_size[1] *
                          pipeline->local_size[2];

   /* Post-link lowering and optimization */
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
      NULL /* no fragment data locations for compute */);

   /* NIR → r600 bytecode */
   union r600_shader_key key;
   memset(&key, 0, sizeof(key));

   VkResult result = terakan_shader_impl_compile(
      &pipeline->shader, device, &key, nir, allocator);

   ralloc_free(nir);

   if (result != VK_SUCCESS) {
      terakan_pipeline_finish(&pipeline->base);
      vk_free2(&device->vk.alloc, allocator, pipeline);
      return result;
   }

   /* Pre-compute HW register values from the compiled shader */
   pipeline->sq_pgm_start_cs = pipeline->shader.static_state.program_va_shr8;
   memcpy(pipeline->sq_pgm_resources_cs,
          pipeline->shader.static_state.sq_pgm_resources,
          sizeof(pipeline->sq_pgm_resources_cs));

   /* LDS allocation: (lds_size_dwords | num_waves << 14)
    * num_waves = ceil(group_size / (16 * num_pipes))
    * For now use a conservative 1 wave. */
   uint32_t lds_size_dwords = 0 /* LDS set at dispatch from NIR workgroup_size */;
   pipeline->sq_lds_alloc = lds_size_dwords | (1 << 14);

   *pipeline_out = terakan_pipeline_to_handle(&pipeline->base);
   return VK_SUCCESS;
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
                               UNUSED VkPipelineCache const pipelineCache,
                               uint32_t const createInfoCount,
                               VkComputePipelineCreateInfo const * const pCreateInfos,
                               VkAllocationCallbacks const * const pAllocator,
                               VkPipeline * const pPipelines)
{
   struct terakan_device *device = terakan_device_from_handle(deviceHandle);
   VkResult overall_result = VK_SUCCESS;

   for (uint32_t i = 0; i < createInfoCount; i++) {
      VkResult result = terakan_pipeline_compute_create(
         device, pipelineCache, &pCreateInfos[i], pAllocator, &pPipelines[i]);
      if (result != VK_SUCCESS) {
         pPipelines[i] = VK_NULL_HANDLE;
         overall_result = result;
         /* VK spec: continue creating remaining pipelines */
      }
   }

   return overall_result;
}
