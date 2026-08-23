/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V compute surface: storage-buffer descriptors, compute
 * pipeline creation through the direct SPIR-V admission, the dispatch
 * recording, and the CPU compute route that executes it at queue
 * submission.  The surface stands behind the exact
 * R3V_HYBRID_COMPUTE_EXPERIMENTAL=1 opt-in the queue-family compute
 * bit advertises under, and every out-of-contract input refuses at
 * pipeline creation or recording rather than executing as a no-op.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"
#include "r3v_physical_device.h"

#include "amd/r300/common/r300_compute_spirv.h"
#include "amd/r300/common/r300_compute_verb.h"
#include "amd/r300/cpu/r300_cpu_compute_job.h"

#include "vk_log.h"
#include "vk_pipeline_layout.h"
#include "vk_shader_module.h"

#include <string.h>

static void
poison(VkCommandBuffer commandBuffer, VkResult error)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   vk_command_buffer_set_error(cmd_buffer, error);
}

/* The admitted set-layout shape: storage-buffer bindings alone, one
 * descriptor each, visible to the compute stage, inside the binding
 * bound.  The layout records presence per binding number, so pipeline
 * creation can prove the job's bindings exist before any set binds.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateDescriptorSetLayout(VkDevice _device,
                              const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkDescriptorSetLayout *pSetLayout)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   (void)pAllocator;

   *pSetLayout = VK_NULL_HANDLE;
   if (pCreateInfo->flags != 0 ||
       pCreateInfo->bindingCount > R3V_NATIVE_DESCRIPTOR_BINDING_MAX)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   bool present[R3V_NATIVE_DESCRIPTOR_BINDING_MAX] = { false };
   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
      const VkDescriptorSetLayoutBinding *binding =
         &pCreateInfo->pBindings[i];
      if (binding->binding >= R3V_NATIVE_DESCRIPTOR_BINDING_MAX ||
          present[binding->binding] ||
          binding->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
          binding->descriptorCount != 1 ||
          binding->stageFlags != VK_SHADER_STAGE_COMPUTE_BIT ||
          binding->pImmutableSamplers != NULL)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      present[binding->binding] = true;
   }

   struct r3v_native_descriptor_set_layout *layout =
      vk_descriptor_set_layout_zalloc(&device->vk, sizeof(*layout),
                                      pCreateInfo);
   if (layout == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   memcpy(layout->binding_present, present, sizeof(present));

   *pSetLayout = r3v_native_descriptor_set_layout_to_handle(layout);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateDescriptorPool(VkDevice _device,
                         const VkDescriptorPoolCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkDescriptorPool *pDescriptorPool)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);

   *pDescriptorPool = VK_NULL_HANDLE;
   /* FREE_DESCRIPTOR_SET_BIT changes which release commands a valid
    * program uses, and both release paths below free the same way, so
    * both flag states admit; any other flag refuses.
    */
   if ((pCreateInfo->flags &
        ~(VkDescriptorPoolCreateFlags)
           VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) != 0 ||
       pCreateInfo->maxSets == 0 ||
       pCreateInfo->maxSets > R3V_NATIVE_DESCRIPTOR_POOL_SET_MAX)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   for (uint32_t i = 0; i < pCreateInfo->poolSizeCount; i++) {
      if (pCreateInfo->pPoolSizes[i].type !=
          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   }

   struct r3v_native_descriptor_pool *pool =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*pool),
                       VK_OBJECT_TYPE_DESCRIPTOR_POOL);
   if (pool == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   pool->max_sets = pCreateInfo->maxSets;

   *pDescriptorPool = r3v_native_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

static void
release_set(struct r3v_native_device *device,
            struct r3v_native_descriptor_set *set)
{
   vk_descriptor_set_layout_unref(&device->vk, &set->layout->vk);
   vk_object_free(&device->vk, NULL, set);
}

static void
release_pool_sets(struct r3v_native_device *device,
                  struct r3v_native_descriptor_pool *pool)
{
   for (uint32_t i = 0; i < pool->set_count; i++)
      release_set(device, pool->sets[i]);
   pool->set_count = 0;
}

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyDescriptorPool(VkDevice _device, VkDescriptorPool _pool,
                          const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_descriptor_pool, pool, _pool);

   if (pool == NULL)
      return;
   release_pool_sets(device, pool);
   vk_object_free(&device->vk, pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_ResetDescriptorPool(VkDevice _device, VkDescriptorPool _pool,
                        VkDescriptorPoolResetFlags flags)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_descriptor_pool, pool, _pool);

   (void)flags;
   if (pool != NULL)
      release_pool_sets(device, pool);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_AllocateDescriptorSets(VkDevice _device,
                           const VkDescriptorSetAllocateInfo *pAllocateInfo,
                           VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_descriptor_pool, pool,
                  pAllocateInfo->descriptorPool);

   for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++)
      pDescriptorSets[i] = VK_NULL_HANDLE;
   if (pool == NULL)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   if (pAllocateInfo->descriptorSetCount >
       pool->max_sets - pool->set_count)
      return vk_error(device, VK_ERROR_OUT_OF_POOL_MEMORY);

   for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      VK_FROM_HANDLE(r3v_native_descriptor_set_layout, layout,
                     pAllocateInfo->pSetLayouts[i]);
      if (layout == NULL) {
         r3v_FreeDescriptorSets(_device, pAllocateInfo->descriptorPool, i,
                                pDescriptorSets);
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      }
      struct r3v_native_descriptor_set *set =
         vk_object_zalloc(&device->vk, NULL, sizeof(*set),
                          VK_OBJECT_TYPE_DESCRIPTOR_SET);
      if (set == NULL) {
         r3v_FreeDescriptorSets(_device, pAllocateInfo->descriptorPool, i,
                                pDescriptorSets);
         return vk_error(device, VK_ERROR_OUT_OF_POOL_MEMORY);
      }
      set->layout = layout;
      vk_descriptor_set_layout_ref(&layout->vk);
      pool->sets[pool->set_count++] = set;
      pDescriptorSets[i] = r3v_native_descriptor_set_to_handle(set);
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_FreeDescriptorSets(VkDevice _device, VkDescriptorPool _pool,
                       uint32_t descriptorSetCount,
                       const VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_descriptor_pool, pool, _pool);

   for (uint32_t i = 0; i < descriptorSetCount; i++) {
      VK_FROM_HANDLE(r3v_native_descriptor_set, set, pDescriptorSets[i]);
      if (set == NULL || pool == NULL)
         continue;
      for (uint32_t s = 0; s < pool->set_count; s++) {
         if (pool->sets[s] != set)
            continue;
         pool->sets[s] = pool->sets[--pool->set_count];
         release_set(device, set);
         break;
      }
   }
   return VK_SUCCESS;
}

/* vkUpdateDescriptorSets returns void, so a write outside the admitted
 * contract poisons its set; the dispatch recording refuses a poisoned
 * set, which converts the silent surface into a fail-closed one.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_UpdateDescriptorSets(VkDevice _device, uint32_t descriptorWriteCount,
                         const VkWriteDescriptorSet *pDescriptorWrites,
                         uint32_t descriptorCopyCount,
                         const VkCopyDescriptorSet *pDescriptorCopies)
{
   (void)_device;

   for (uint32_t i = 0; i < descriptorCopyCount; i++) {
      VK_FROM_HANDLE(r3v_native_descriptor_set, dst,
                     pDescriptorCopies[i].dstSet);
      if (dst != NULL)
         dst->poisoned = true;
   }

   for (uint32_t i = 0; i < descriptorWriteCount; i++) {
      const VkWriteDescriptorSet *write = &pDescriptorWrites[i];
      VK_FROM_HANDLE(r3v_native_descriptor_set, set, write->dstSet);
      if (set == NULL)
         continue;
      if (write->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
          write->descriptorCount != 1 || write->dstArrayElement != 0 ||
          write->dstBinding >= R3V_NATIVE_DESCRIPTOR_BINDING_MAX ||
          !set->layout->binding_present[write->dstBinding] ||
          write->pBufferInfo == NULL) {
         set->poisoned = true;
         continue;
      }
      VK_FROM_HANDLE(r3v_native_buffer, buffer,
                     write->pBufferInfo->buffer);
      if (buffer == NULL ||
          write->pBufferInfo->offset > buffer->vk.size ||
          (write->pBufferInfo->range != VK_WHOLE_SIZE &&
           write->pBufferInfo->range >
              buffer->vk.size - write->pBufferInfo->offset)) {
         set->poisoned = true;
         continue;
      }
      const VkDeviceSize range =
         write->pBufferInfo->range == VK_WHOLE_SIZE
            ? buffer->vk.size - write->pBufferInfo->offset
            : write->pBufferInfo->range;
      set->bindings[write->dstBinding] =
         (struct r3v_native_descriptor_binding){
            .bound = true,
            .buffer = buffer,
            .offset = write->pBufferInfo->offset,
            .range = range,
         };
   }
}

/* Compute pipeline creation: the gate, the layout, and the module all
 * prove before the object allocates.  The direct SPIR-V admission is
 * the semantic front end, so an out-of-subset kernel refuses here with
 * the reason logged -- no admitted pipeline can reach an unmatched
 * no-op at dispatch.
 */
static VkResult
create_compute_pipeline(struct r3v_native_device *device,
                        const VkComputePipelineCreateInfo *info,
                        const VkAllocationCallbacks *pAllocator,
                        VkPipeline *pPipeline)
{
   *pPipeline = VK_NULL_HANDLE;

   if (!device->pdevice->hybrid_compute_enabled)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   VK_FROM_HANDLE(vk_pipeline_layout, layout, info->layout);
   VK_FROM_HANDLE(vk_shader_module, module, info->stage.module);
   if (info->flags != 0 || layout == NULL || module == NULL ||
       layout->set_count != 1 || layout->push_range_count != 0 ||
       info->stage.stage != VK_SHADER_STAGE_COMPUTE_BIT ||
       info->stage.flags != 0 ||
       info->stage.pSpecializationInfo != NULL ||
       info->stage.pName == NULL || module->size % 4 != 0)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   struct r300_compute_job job;
   const char *reason = NULL;
   if (!r300_compute_job_from_spirv((const uint32_t *)module->data,
                                    module->size / 4, info->stage.pName,
                                    &job, &reason)) {
      return vk_errorf(device, R3V_NATIVE_REFUSAL_RESULT,
                       "r3v-native: compute module refused: %s", reason);
   }
   if (r300_cpu_compute_job_validate(&job) != 0)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   /* The verb ledger is the precommitted admission authority: the job
    * realizes a ledger row whose CPU route executes, or the pipeline
    * refuses (R300_COMPUTE_FAILURE_REFUSE_AT_ADMISSION). */
   const struct r300_compute_verb_row *verb = r300_compute_verb_for_job(&job);
   if (verb == NULL || verb->cpu_route != R300_COMPUTE_VERB_ROUTE_EXECUTING) {
      return vk_errorf(device, R3V_NATIVE_REFUSAL_RESULT,
                       "r3v-native: compute verb %s has no executing route",
                       verb ? verb->name : "outside the ledger");
   }

   /* The pipeline layout's set 0 carries both job bindings, so a
    * bound set of that layout can always name the two buffers.
    */
   struct r3v_native_descriptor_set_layout *set_layout =
      container_of(layout->set_layouts[0],
                   struct r3v_native_descriptor_set_layout, vk);
   if (set_layout == NULL ||
       job.input_binding >= R3V_NATIVE_DESCRIPTOR_BINDING_MAX ||
       job.output_binding >= R3V_NATIVE_DESCRIPTOR_BINDING_MAX ||
       !set_layout->binding_present[job.input_binding] ||
       !set_layout->binding_present[job.output_binding])
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   struct r3v_native_pipeline *pipeline =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*pipeline),
                       VK_OBJECT_TYPE_PIPELINE);
   if (pipeline == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline->is_compute = true;
   pipeline->compute_job = job;

   *pPipeline = r3v_native_pipeline_to_handle(pipeline);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateComputePipelines(VkDevice _device, VkPipelineCache pipelineCache,
                           uint32_t createInfoCount,
                           const VkComputePipelineCreateInfo *pCreateInfos,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   (void)pipelineCache;

   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < createInfoCount; i++) {
      VkResult one = create_compute_pipeline(device, &pCreateInfos[i],
                                             pAllocator, &pPipelines[i]);
      if (one != VK_SUCCESS && result == VK_SUCCESS)
         result = one;
   }
   return result;
}

/* The dispatch recording: one dispatch on a dispatch-only command
 * buffer, admitted whole at record time -- gate, pipeline, set,
 * buffers, usage, and both range footprints -- so submission's own
 * failure surface is the mapping and the memory binding.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX,
                uint32_t groupCountY, uint32_t groupCountZ)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   const struct r3v_native_pipeline *pipeline =
      cmd_buffer->bound_compute_pipeline;
   const struct r3v_native_descriptor_set *set =
      cmd_buffer->bound_compute_set;
   if (pipeline == NULL || set == NULL || set->poisoned ||
       cmd_buffer->pass_target != NULL || cmd_buffer->draw_recorded ||
       cmd_buffer->deferred_draw.pending ||
       cmd_buffer->deferred_copy_count != 0 ||
       cmd_buffer->deferred_dispatch.pending) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }

   const struct r300_compute_job *job = &pipeline->compute_job;
   const uint32_t group_counts[3] = { groupCountX, groupCountY,
                                      groupCountZ };
   uint32_t invocations = 0;
   if (r300_cpu_compute_job_invocations(job, group_counts,
                                        &invocations) != 0) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   const uint64_t bytes =
      (uint64_t)invocations * R300_COMPUTE_JOB_ELEMENT_BYTES;

   const struct r3v_native_descriptor_binding *input =
      &set->bindings[job->input_binding];
   const struct r3v_native_descriptor_binding *output =
      &set->bindings[job->output_binding];
   if (!input->bound || !output->bound || input->range < bytes ||
       output->range < bytes)
   {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   /* Each buffer's bound range closes inside its memory, and its
    * usage carries the storage bit the descriptor type claims.
    */
   const struct r3v_native_buffer *buffers[2] = { input->buffer,
                                                  output->buffer };
   for (unsigned b = 0; b < 2; b++) {
      const struct r3v_native_buffer *buffer = buffers[b];
      if ((buffer->vk.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0 ||
          buffer->memory == NULL ||
          buffer->offset > buffer->memory->bo.size ||
          buffer->vk.size > buffer->memory->bo.size - buffer->offset) {
         poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
         return;
      }
   }

   cmd_buffer->deferred_dispatch = (struct r3v_native_deferred_dispatch){
      .pending = true,
      .job = *job,
      .group_counts = { groupCountX, groupCountY, groupCountZ },
      .input_buffer = input->buffer,
      .output_buffer = output->buffer,
      .input_memory_offset = input->buffer->offset + input->offset,
      .output_memory_offset = output->buffer->offset + output->offset,
      .byte_count = bytes,
   };
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                          VkPipelineBindPoint pipelineBindPoint,
                          VkPipelineLayout layout, uint32_t firstSet,
                          uint32_t descriptorSetCount,
                          const VkDescriptorSet *pDescriptorSets,
                          uint32_t dynamicOffsetCount,
                          const uint32_t *pDynamicOffsets)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   (void)layout;
   (void)pDynamicOffsets;

   if (pipelineBindPoint != VK_PIPELINE_BIND_POINT_COMPUTE ||
       firstSet != 0 || descriptorSetCount != 1 ||
       dynamicOffsetCount != 0) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   VK_FROM_HANDLE(r3v_native_descriptor_set, set, pDescriptorSets[0]);
   if (set == NULL) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   cmd_buffer->bound_compute_set = set;
}

/* The CPU compute route at submission, mirroring the deferred
 * transfer executor's mapping contract: reuse a live application
 * mapping, own a fresh one otherwise, and publish the written range
 * for the unsnooped GART while the address is live.
 */
static VkResult
map_memory(struct r3v_native_device *device,
           struct r3v_native_memory *memory, uint8_t **map_out,
           bool *owned_out)
{
   *owned_out = memory->map == NULL;
   if (*owned_out &&
       radeon_drm_vk_bo_map(&device->drm, &memory->bo, &memory->map) != 0) {
      *owned_out = false;
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: dispatch memory is not CPU-mappable "
                       "at submission");
   }
   *map_out = memory->map;
   return VK_SUCCESS;
}

static void
release_memory(struct r3v_native_device *device,
               struct r3v_native_memory *memory, bool owned)
{
   if (!owned)
      return;
   radeon_drm_vk_bo_cache_sync(&device->drm, memory->map, memory->bo.size);
   radeon_drm_vk_bo_unmap(&device->drm, &memory->bo, memory->map);
   memory->map = NULL;
}

VkResult
r3v_native_cmd_buffer_execute_deferred_dispatch(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer)
{
   struct r3v_native_deferred_dispatch *dispatch =
      &cmd_buffer->deferred_dispatch;
   if (!dispatch->pending)
      return VK_SUCCESS;
   dispatch->pending = false;

   struct r3v_native_memory *input_memory =
      dispatch->input_buffer->memory;
   struct r3v_native_memory *output_memory =
      dispatch->output_buffer->memory;
   if (input_memory == NULL || output_memory == NULL) {
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: dispatch buffer is unbound at "
                       "submission");
   }

   uint8_t *input_map = NULL, *output_map = NULL;
   bool input_owned, output_owned;
   VkResult result = map_memory(device, input_memory, &input_map,
                                &input_owned);
   if (result != VK_SUCCESS)
      return result;
   result = map_memory(device, output_memory, &output_map, &output_owned);
   if (result != VK_SUCCESS) {
      release_memory(device, input_memory, input_owned);
      return result;
   }

   const int rc = r300_cpu_compute_job_execute(
      &dispatch->job, dispatch->group_counts,
      input_map + dispatch->input_memory_offset, dispatch->byte_count,
      output_map + dispatch->output_memory_offset, dispatch->byte_count);
   if (rc == 0 && !output_owned) {
      radeon_drm_vk_bo_cache_sync(&device->drm, output_map,
                                  output_memory->bo.size);
   }
   release_memory(device, output_memory, output_owned);
   release_memory(device, input_memory, input_owned);
   if (rc != 0) {
      /* The record-time admission proved the footprints, so the one
       * refusal execution can add is the executor's alias check: two
       * descriptors resolving to overlapping bytes.
       */
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: dispatch execution refused: %d", rc);
   }
   return VK_SUCCESS;
}
