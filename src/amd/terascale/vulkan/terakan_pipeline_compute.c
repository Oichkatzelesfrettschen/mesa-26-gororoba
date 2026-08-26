/*
 * SPDX-License-Identifier: MIT
 *
 * Vulkan compute pipeline implementation for Terakan (TeraScale-2/Evergreen).
 *
 * This provides the native vk_shader abstraction for compute shaders,
 * enabling GPU-driven rendering via vkCmdDispatch to indirect draw buffer
 * population. The pipeline compiles SPIR-V to NIR to r600 bytecode and
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
 * Allocate and initialize the base compute pipeline.
 */
static VkResult
terakan_pipeline_compute_init(struct terakan_device * const device,
                              VkAllocationCallbacks const * const allocator,
                              struct terakan_pipeline_compute ** const pipeline_out)
{
   /* vk_zalloc2 atomically allocates and zero-initializes the whole pipeline
    * struct.  See the matching comment in terakan_pipeline_graphics_init(). */
   struct terakan_pipeline_compute *pipeline =
      vk_zalloc2(&device->vk.alloc, allocator, sizeof(*pipeline), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   terakan_pipeline_init(&pipeline->base, device, true);

   *pipeline_out = pipeline;
   return VK_SUCCESS;
}

/*
 * Compile the compute shader with cache integration.
 *
 * Invariants (mirrors terakan_pipeline_graphics_compile_shaders):
 *
 * Invariant 2 (Ownership isolation): Compile into a stack-local
 * terakan_shader_impl.  On success, commit via struct copy to
 * pipeline->shader.  On failure, finish() the local in-place and leave
 * pipeline->shader untouched (still zeroed from vk_zalloc2).
 *
 * Invariant 3 (Cache hit ≡ miss): Pre-compile fields are populated by
 * post-link lowering BEFORE cache lookup.
 *
 * Invariant 4 (Post-link ordering barrier): Cache key construction
 * follows terakan_shader_lower_and_optimize_post_link().
 *
 * RD-4 note: Extracts local_size[3], group_size, and shared_size from
 * NIR BEFORE ralloc_free because these are metadata on the NIR shader
 * and cannot be reconstructed afterward. They are written directly into
 * the pipeline struct (not into local_shader) because they live on the
 * parent pipeline_compute, not on the shader_impl.
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
   pipeline->shared_size_dwords =
      DIV_ROUND_UP(nir->info.shared_size, sizeof(uint32_t));

   /* Invariant 2: compile into a stack-local; commit on success. */
   struct terakan_shader_impl local_shader;
   memset(&local_shader, 0, sizeof(local_shader));

   /* Seed app push-constant extent from the pipeline layout so that
    * terakan_push_constants_apply uploads the application-supplied
    * vkCmdPushConstants payload into the compute KCACHE bank
    * (TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) at dispatch time.  Without
    * this, push_constants_usage.app_extent_bytes stays at zero, the
    * app_constants memcpy in terakan_push_constants_apply is skipped,
    * and the shader reads zero out of the bank-15 slot.  The graphics
    * pipeline uses the same metadata seed before shader lowering. */
   if (create_info->layout != VK_NULL_HANDLE) {
      struct terakan_pipeline_layout const * const compute_layout =
         terakan_pipeline_layout_from_handle(create_info->layout);
      local_shader.push_constants_usage.app_extent_bytes =
         compute_layout->shader_app_push_constants_extents_bytes[MESA_SHADER_COMPUTE];
   }

   /* Compute the effective robustness flag for this stage.  See the matching
    * comment in terakan_pipeline_graphics_compile_shaders. */
   struct vk_pipeline_robustness_state stage_rs;
   vk_pipeline_robustness_state_fill(&device->vk.robustness_state, &stage_rs,
                                     create_info->pNext, stage_info->pNext);
   bool const stage_robust_buffer_access =
      device->vk.enabled_features.robustBufferAccess ||
      stage_rs.storage_buffers ==
         VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT ||
      stage_rs.storage_buffers ==
         VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT ||
      stage_rs.uniform_buffers ==
         VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT ||
      stage_rs.uniform_buffers ==
         VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT;

   /* Post-link lowering populates pre-compile metadata (Invariant 3).
    * The reject flags are sampled before binding lowering consumes the
    * original CMPXCHG and image atomic intrinsics. */
   bool cmpxchg_strict_reject = false;
   bool image_atomic_reject = false;
   terakan_shader_lower_and_optimize_post_link(
      nir,
      create_info->layout != VK_NULL_HANDLE
         ? terakan_pipeline_layout_from_handle(create_info->layout)
         : NULL,
      local_shader.resources_needed,
      &local_shader.samplers_needed,
      local_shader.uavs_for_mutable_resources_needed,
      local_shader.robustness_metadata_for_mutable_resources_needed,
      &local_shader.push_constants_usage.driver_constants,
      &local_shader.kcache_needed,
      NULL,
      stage_robust_buffer_access,
      terakan_device_physical_device(device)->chip_info.chip_family,
      &cmpxchg_strict_reject,
      &image_atomic_reject);

   if (cmpxchg_strict_reject || image_atomic_reject) {
      ralloc_free(nir);
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   /* Build cache key (Invariant 4: only after post-link lowering). */
   VkPipelineCreateFlags2KHR const pipeline_flags =
      terakan_pipeline_create_flags(create_info->flags, create_info->pNext);

   struct terakan_shader_stage_key stage_key;
   terakan_shader_stage_key_fill(&stage_key, device, stage_info, pipeline_flags);

   union r600_shader_key shader_key;
   memset(&shader_key, 0, sizeof(shader_key));

   blake3_hash spirv_hash;
   struct vk_pipeline_robustness_state rs;
   vk_pipeline_robustness_state_fill(&device->vk.robustness_state, &rs,
                                     create_info->pNext, stage_info->pNext);
   vk_pipeline_hash_shader_stage(pipeline_flags, stage_info, &rs, spirv_hash);

   blake3_hash cache_key;
   terakan_pipeline_cache_hash_shader(cache_key, device, &stage_key,
                                      &shader_key, spirv_hash,
                                      NULL, 0);

   /* Cache lookup skips compilation on hit (Invariant 3). */
   struct terakan_cached_shader *cached =
      terakan_pipeline_cache_lookup(cache, cache_key);
   VkResult result;
   if (cached != NULL) {
      result = terakan_cached_shader_restore(cached, &local_shader, device, allocator);
      vk_pipeline_cache_object_unref(&device->vk, &cached->base);
      ralloc_free(nir);
      if (result != VK_SUCCESS) {
         /* Invariant 2: clean local in-place; pipeline->shader untouched. */
         terakan_shader_impl_finish(&local_shader, allocator);
         return result;
      }
   } else {
      /* Cache miss requires compilation.  If the app set
       * FAIL_ON_PIPELINE_COMPILE_REQUIRED, bail out immediately.
       * VK_PIPELINE_COMPILE_REQUIRED is a non-error success code (>0)
       * but counts as non-VK_SUCCESS for EARLY_RETURN_ON_FAILURE. */
      if (pipeline_flags &
          VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR) {
         ralloc_free(nir);
         terakan_shader_impl_finish(&local_shader, allocator);
         return VK_PIPELINE_COMPILE_REQUIRED;
      }

      result = terakan_shader_impl_compile(
         &local_shader, device, &shader_key, nir, allocator);
      size_t const program_size_bytes =
         sizeof(uint32_t) * local_shader.shader.bc.ndw;
      ralloc_free(nir);
      if (result != VK_SUCCESS) {
         /* Invariant 2: finish() cleans whatever compile partially allocated
          * on the local; pipeline->shader is still zeroed from vk_zalloc2. */
         terakan_shader_impl_finish(&local_shader, allocator);
         return result;
      }
      terakan_pipeline_cache_insert(cache, cache_key, &local_shader,
                                    MESA_SHADER_COMPUTE, program_size_bytes, device);
   }

   /* Invariant 2 commit: transfer ownership via struct copy.  local_shader
    * is now "moved-from" and must not be finished. */
   pipeline->shader = local_shader;
   return VK_SUCCESS;
}

static VkResult
terakan_pipeline_compute_compile_storage_image_layer_variant(
   struct terakan_pipeline_compute * const pipeline,
   struct terakan_device * const device,
   VkComputePipelineCreateInfo const * const create_info,
   VkAllocationCallbacks const * const allocator,
   int const layer)
{
   assert(layer >= 0 && layer < 8);

   VkPipelineShaderStageCreateInfo const *stage_info = &create_info->stage;

   size_t spirv_size = 0;
   uint32_t const *spirv = terakan_pipeline_stage_spirv(stage_info, &spirv_size);

   nir_shader *nir = terakan_shader_spirv_to_nir(
      device, spirv_size, spirv, MESA_SHADER_COMPUTE,
      stage_info->pName ? stage_info->pName : "main",
      stage_info->pSpecializationInfo);
   if (nir == NULL)
      return VK_ERROR_UNKNOWN;

   struct terakan_shader_impl *variant =
      vk_zalloc2(&device->vk.alloc, allocator, sizeof(*variant), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (variant == NULL) {
      ralloc_free(nir);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   struct vk_pipeline_robustness_state stage_rs;
   vk_pipeline_robustness_state_fill(&device->vk.robustness_state, &stage_rs,
                                     create_info->pNext, stage_info->pNext);
   bool const stage_robust_buffer_access =
      device->vk.enabled_features.robustBufferAccess ||
      stage_rs.storage_buffers ==
         VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT ||
      stage_rs.storage_buffers ==
         VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT ||
      stage_rs.uniform_buffers ==
         VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT ||
      stage_rs.uniform_buffers ==
         VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT;

   /* Seed app push-constant extent from the pipeline layout so the
    * storage-image layer variant carries the same compute KCACHE upload
    * metadata as the primary shader. */
   if (create_info->layout != VK_NULL_HANDLE) {
      struct terakan_pipeline_layout const * const variant_layout =
         terakan_pipeline_layout_from_handle(create_info->layout);
      variant->push_constants_usage.app_extent_bytes =
         variant_layout->shader_app_push_constants_extents_bytes[MESA_SHADER_COMPUTE];
   }

   terakan_storage_image_set_compile_layer(layer);
   terakan_storage_image_set_compile_layer_index(layer);

   bool cmpxchg_strict_reject_variant = false;
   bool image_atomic_reject_variant = false;
   terakan_shader_lower_and_optimize_post_link(
      nir,
      create_info->layout != VK_NULL_HANDLE
         ? terakan_pipeline_layout_from_handle(create_info->layout)
         : NULL,
      variant->resources_needed,
      &variant->samplers_needed,
      variant->uavs_for_mutable_resources_needed,
      variant->robustness_metadata_for_mutable_resources_needed,
      &variant->push_constants_usage.driver_constants,
      &variant->kcache_needed,
      NULL,
      stage_robust_buffer_access,
      terakan_device_physical_device(device)->chip_info.chip_family,
      &cmpxchg_strict_reject_variant,
      &image_atomic_reject_variant);

   if (cmpxchg_strict_reject_variant || image_atomic_reject_variant) {
      terakan_storage_image_set_compile_layer(INT32_MIN);
      terakan_storage_image_set_compile_layer_index(INT32_MIN);
      ralloc_free(nir);
      terakan_shader_impl_finish(variant, allocator);
      vk_free2(&device->vk.alloc, allocator, variant);
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   /* Clear the NIR-lowering literals before backend emission starts. */
   terakan_storage_image_set_compile_layer(INT32_MIN);
   terakan_storage_image_set_compile_layer_index(INT32_MIN);

   union r600_shader_key shader_key;
   memset(&shader_key, 0, sizeof(shader_key));

   VkResult result = terakan_shader_impl_compile(
      variant, device, &shader_key, nir, allocator);
   ralloc_free(nir);
   if (result != VK_SUCCESS) {
      terakan_shader_impl_finish(variant, allocator);
      vk_free2(&device->vk.alloc, allocator, variant);
      return result;
   }

   pipeline->storage_image_layer_variants[layer] = variant;

   if (debug_get_bool_option("TERAKAN_DEBUG_STORAGE_IMAGE_VARIANTS", false)) {
      fprintf(stderr,
         "terakan/storage_image_variant: compiled layer=%d program_va_shr8=0x%x\n",
         layer, variant->static_state.program_va_shr8);
   }

   return VK_SUCCESS;
}

/*
 * Precompute hardware register values from the compiled shader. Workgroup
 * dimensions and shared memory size are captured before NIR ownership moves to
 * the backend compiler.
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
   pipeline->sq_lds_alloc = pipeline->shared_size_dwords | (1 << 14);
}

/*
 * Thin caller with a single destroy error path (Invariant 2).
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

   if (debug_get_bool_option("TERAKAN_STORAGE_IMAGE_LAYER_VARIANTS", false)) {
      pipeline->storage_image_layer_variants_enabled = true;
      for (int layer = 0; layer < 8; layer++) {
         VkResult variant_result =
            terakan_pipeline_compute_compile_storage_image_layer_variant(
            pipeline, device, create_info, allocator, layer);
         if (variant_result != VK_SUCCESS) {
            fprintf(stderr,
               "terakan/storage_image_variant: layer %d compile failed (rc=%d)\n",
               layer, variant_result);
            result = variant_result;
            goto fail;
         }
      }
   }

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

   for (int layer = 0; layer < 8; layer++) {
      if (pipeline->storage_image_layer_variants[layer] != NULL) {
         terakan_shader_impl_finish(
            pipeline->storage_image_layer_variants[layer], allocator);
         vk_free2(&device->vk.alloc, allocator,
                  pipeline->storage_image_layer_variants[layer]);
         pipeline->storage_image_layer_variants[layer] = NULL;
      }
   }

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
            ? vk_pipeline_cache_from_handle(pipelineCache) : device->vk.mem_cache;
      VkResult result = terakan_pipeline_compute_create(
         device, cache, &pCreateInfos[i], pAllocator, &pPipelines[i]);
      if (result != VK_SUCCESS) {
         pPipelines[i] = VK_NULL_HANDLE;
         overall_result = result;
         if (terakan_pipeline_create_flags(pCreateInfos[i].flags,
                                           pCreateInfos[i].pNext) &
             VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR) {
            /* Remaining pipelines must be VK_NULL_HANDLE per spec. */
            for (++i; i < createInfoCount; i++)
               pPipelines[i] = VK_NULL_HANDLE;
            break;
         }
      }
   }

   return overall_result;
}
