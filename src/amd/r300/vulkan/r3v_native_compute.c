/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V compute surface: storage-buffer descriptors, direct SPIR-V
 * admission, dispatch recording, the default CPU execution route, and the
 * separately gated R2VB identity-map GPU route.  The surface stands behind
 * the exact R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL=1 opt-in the queue-family compute
 * bit advertises under.  Every out-of-contract input refuses before device
 * work rather than executing as a no-op.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"
#include "r3v_physical_device.h"

#include "amd/r300/common/r300_compute_identity_carrier.h"
#include "amd/r300/vulkan/r3v_compute_spirv.h"
#include "amd/r300/common/r300_compute_verb.h"
#include "amd/r300/common/r300_operation_route.h"
#include "amd/r300/common/r300_r2vb_producer_pass.h"
#include "amd/r300/cpu/r300_cpu_compute_job.h"

#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_pipeline_layout.h"
#include "vk_shader_module.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <radeon_drm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
poison(VkCommandBuffer commandBuffer, VkResult error)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   vk_command_buffer_set_error(cmd_buffer, error);
}

/* Every core descriptor type admits at layout creation inside the
 * binding and array-count bounds; the layout records each binding's
 * type, count, and stages so pipeline creation proves the job's
 * bindings carry the executing shape before any set binds.
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

   struct r3v_native_descriptor_layout_binding
      bindings[R3V_NATIVE_DESCRIPTOR_BINDING_MAX] = { { false } };
   uint32_t type_counts[R3V_NATIVE_DESCRIPTOR_TYPE_COUNT] = { 0 };
   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
      const VkDescriptorSetLayoutBinding *binding =
         &pCreateInfo->pBindings[i];
      /* A zero-count binding is a reserved number the spec admits; it
       * consumes no pool capacity and never binds.  Immutable samplers
       * name sampler objects the executing surface has no route for.
       */
      if (binding->binding >= R3V_NATIVE_DESCRIPTOR_BINDING_MAX ||
          bindings[binding->binding].present ||
          (uint32_t)binding->descriptorType >=
             R3V_NATIVE_DESCRIPTOR_TYPE_COUNT ||
          binding->descriptorCount > R3V_NATIVE_DESCRIPTOR_COUNT_MAX ||
          binding->pImmutableSamplers != NULL)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      bindings[binding->binding] =
         (struct r3v_native_descriptor_layout_binding){
            .present = true,
            .type = binding->descriptorType,
            .count = binding->descriptorCount,
            .stages = binding->stageFlags,
         };
      type_counts[binding->descriptorType] += binding->descriptorCount;
   }

   struct r3v_native_descriptor_set_layout *layout =
      vk_descriptor_set_layout_zalloc(&device->vk, sizeof(*layout),
                                      pCreateInfo);
   if (layout == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   memcpy(layout->bindings, bindings, sizeof(bindings));
   memcpy(layout->type_counts, type_counts, sizeof(type_counts));

   *pSetLayout = r3v_native_descriptor_set_layout_to_handle(layout);
   return VK_SUCCESS;
}

/* A pool carries a set count and a per-type descriptor capacity;
 * allocation deducts both and refuses with VK_ERROR_OUT_OF_POOL_MEMORY
 * when either runs out, and free and reset return what they release.
 */
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
   uint32_t type_capacity[R3V_NATIVE_DESCRIPTOR_TYPE_COUNT] = { 0 };
   for (uint32_t i = 0; i < pCreateInfo->poolSizeCount; i++) {
      const VkDescriptorPoolSize *size = &pCreateInfo->pPoolSizes[i];
      if ((uint32_t)size->type >= R3V_NATIVE_DESCRIPTOR_TYPE_COUNT ||
          size->descriptorCount >
             UINT32_MAX - type_capacity[size->type])
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      type_capacity[size->type] += size->descriptorCount;
   }

   struct r3v_native_descriptor_pool *pool =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*pool),
                       VK_OBJECT_TYPE_DESCRIPTOR_POOL);
   if (pool == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   pool->sets = vk_zalloc2(&device->vk.alloc, pAllocator,
                           sizeof(*pool->sets) * pCreateInfo->maxSets, 8,
                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pool->sets == NULL) {
      vk_object_free(&device->vk, pAllocator, pool);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   pool->max_sets = pCreateInfo->maxSets;
   memcpy(pool->type_capacity, type_capacity, sizeof(type_capacity));

   *pDescriptorPool = r3v_native_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

static void
pool_release_capacity(struct r3v_native_descriptor_pool *pool,
                      const struct r3v_native_descriptor_set_layout *layout)
{
   for (uint32_t t = 0; t < R3V_NATIVE_DESCRIPTOR_TYPE_COUNT; t++)
      pool->type_used[t] -= layout->type_counts[t];
}

static bool
pool_reserve_capacity(struct r3v_native_descriptor_pool *pool,
                      const struct r3v_native_descriptor_set_layout *layout)
{
   for (uint32_t t = 0; t < R3V_NATIVE_DESCRIPTOR_TYPE_COUNT; t++) {
      if (layout->type_counts[t] >
          pool->type_capacity[t] - pool->type_used[t])
         return false;
   }
   for (uint32_t t = 0; t < R3V_NATIVE_DESCRIPTOR_TYPE_COUNT; t++)
      pool->type_used[t] += layout->type_counts[t];
   return true;
}

static void
release_set(struct r3v_native_device *device,
            struct r3v_native_descriptor_pool *pool,
            struct r3v_native_descriptor_set *set)
{
   pool_release_capacity(pool, set->layout);
   vk_descriptor_set_layout_unref(&device->vk, &set->layout->vk);
   vk_object_free(&device->vk, NULL, set);
}

static void
release_pool_sets(struct r3v_native_device *device,
                  struct r3v_native_descriptor_pool *pool)
{
   for (uint32_t i = 0; i < pool->set_count; i++)
      release_set(device, pool, pool->sets[i]);
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
   vk_free2(&device->vk.alloc, pAllocator, pool->sets);
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
      VkResult failure = VK_SUCCESS;
      struct r3v_native_descriptor_set *set = NULL;
      if (layout == NULL) {
         failure = R3V_NATIVE_REFUSAL_RESULT;
      } else if (!pool_reserve_capacity(pool, layout)) {
         failure = VK_ERROR_OUT_OF_POOL_MEMORY;
      } else {
         set = vk_object_zalloc(&device->vk, NULL, sizeof(*set),
                                VK_OBJECT_TYPE_DESCRIPTOR_SET);
         if (set == NULL) {
            pool_release_capacity(pool, layout);
            failure = VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }
      if (failure != VK_SUCCESS) {
         /* The failure clause frees every set this call created and
          * hands back VK_NULL_HANDLE in every slot, so no handle to a
          * freed set survives in the application's array.
          */
         r3v_FreeDescriptorSets(_device, pAllocateInfo->descriptorPool, i,
                                pDescriptorSets);
         for (uint32_t k = 0; k < i; k++)
            pDescriptorSets[k] = VK_NULL_HANDLE;
         return vk_error(device, failure);
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
         release_set(device, pool, set);
         break;
      }
   }
   return VK_SUCCESS;
}

static bool
descriptor_type_is_buffer(VkDescriptorType type)
{
   return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
          type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
          type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
          type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static bool
descriptor_type_is_texel_buffer(VkDescriptorType type)
{
   return type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
          type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}

/* vkUpdateDescriptorSets returns void, so a write outside its layout
 * bindings' type and array bounds, or one whose payload pointer the
 * type requires is absent, poisons its set and the dispatch recording
 * refuses it.  A write's descriptorCount may run past its binding into
 * the consecutive bindings of the same type (the update rule for
 * consecutive binding updates), so the write walks the bindings it
 * spans.  A single-element storage-buffer binding written at index
 * zero binds the executing shape; a write of any other admitted shape
 * leaves the binding unbound.  Copies apply after writes; a copy
 * carries a same-type single-element binding, propagates a poisoned
 * source, and poisons on a type or bound mismatch.
 */
static bool
descriptor_write_span_ok(const struct r3v_native_descriptor_set_layout *layout,
                         uint32_t binding, uint32_t element,
                         uint32_t count, VkDescriptorType type)
{
   while (count > 0) {
      if (binding >= R3V_NATIVE_DESCRIPTOR_BINDING_MAX)
         return false;
      const struct r3v_native_descriptor_layout_binding *decl =
         &layout->bindings[binding];
      if (!decl->present || decl->type != type || element > decl->count)
         return false;
      const uint32_t here = decl->count - element;
      if (here == 0 && count > 0 && decl->count != 0)
         return false;
      count -= here < count ? here : count;
      binding++;
      element = 0;
   }
   return true;
}

VKAPI_ATTR void VKAPI_CALL
r3v_UpdateDescriptorSets(VkDevice _device, uint32_t descriptorWriteCount,
                         const VkWriteDescriptorSet *pDescriptorWrites,
                         uint32_t descriptorCopyCount,
                         const VkCopyDescriptorSet *pDescriptorCopies)
{
   (void)_device;

   for (uint32_t i = 0; i < descriptorWriteCount; i++) {
      const VkWriteDescriptorSet *write = &pDescriptorWrites[i];
      VK_FROM_HANDLE(r3v_native_descriptor_set, set, write->dstSet);
      if (set == NULL)
         continue;
      const bool buffer_type = descriptor_type_is_buffer(write->descriptorType);
      const bool texel_type =
         descriptor_type_is_texel_buffer(write->descriptorType);
      const bool image_type = !buffer_type && !texel_type;
      if (write->descriptorCount == 0 ||
          !descriptor_write_span_ok(set->layout, write->dstBinding,
                                    write->dstArrayElement,
                                    write->descriptorCount,
                                    write->descriptorType) ||
          (buffer_type && write->pBufferInfo == NULL) ||
          (texel_type && write->pTexelBufferView == NULL) ||
          (image_type && write->pImageInfo == NULL)) {
         set->poisoned = true;
         continue;
      }
      /* Walk the spanned bindings; each single-element storage-buffer
       * binding the write covers at element zero binds its buffer.
       */
      uint32_t binding = write->dstBinding;
      uint32_t element = write->dstArrayElement;
      uint32_t remaining = write->descriptorCount;
      uint32_t payload = 0;
      while (remaining > 0) {
         const struct r3v_native_descriptor_layout_binding *decl =
            &set->layout->bindings[binding];
         const uint32_t here_max = decl->count - element;
         const uint32_t here = here_max < remaining ? here_max : remaining;
         if (write->descriptorType ==
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
             decl->count == 1 && element == 0 && here == 1) {
            /* The executing sampling shape: a view over a sampled-usage
             * image in a layout the sampling read admits, with the
             * write's own sampler; the draw admission holds the pair
             * to the TX program it emits.
             */
            const VkDescriptorImageInfo *info = &write->pImageInfo[payload];
            VK_FROM_HANDLE(r3v_native_image_view, view, info->imageView);
            VK_FROM_HANDLE(r3v_native_sampler, sampler, info->sampler);
            if (view == NULL || sampler == NULL ||
                (view->image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0 ||
                (info->imageLayout != VK_IMAGE_LAYOUT_GENERAL &&
                 info->imageLayout !=
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
               set->poisoned = true;
               break;
            }
            set->bindings[binding] =
               (struct r3v_native_descriptor_binding){
                  .bound = true,
                  .image_view = view,
                  .sampler = sampler,
               };
         }
         if (write->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
             decl->count == 1 && element == 0 && here == 1) {
            const VkDescriptorBufferInfo *info =
               &write->pBufferInfo[payload];
            VK_FROM_HANDLE(r3v_native_buffer, buffer, info->buffer);
            if (buffer == NULL || info->offset > buffer->vk.size ||
                (info->range != VK_WHOLE_SIZE &&
                 info->range > buffer->vk.size - info->offset)) {
               set->poisoned = true;
               break;
            }
            const VkDeviceSize range =
               info->range == VK_WHOLE_SIZE
                  ? buffer->vk.size - info->offset
                  : info->range;
            set->bindings[binding] =
               (struct r3v_native_descriptor_binding){
                  .bound = true,
                  .buffer = buffer,
                  .offset = info->offset,
                  .range = range,
               };
         }
         payload += here;
         remaining -= here;
         binding++;
         element = 0;
      }
   }

   for (uint32_t i = 0; i < descriptorCopyCount; i++) {
      const VkCopyDescriptorSet *copy = &pDescriptorCopies[i];
      VK_FROM_HANDLE(r3v_native_descriptor_set, dst, copy->dstSet);
      VK_FROM_HANDLE(r3v_native_descriptor_set, src, copy->srcSet);
      if (dst == NULL)
         continue;
      if (src == NULL || src->poisoned ||
          copy->srcBinding >= R3V_NATIVE_DESCRIPTOR_BINDING_MAX ||
          copy->dstBinding >= R3V_NATIVE_DESCRIPTOR_BINDING_MAX) {
         dst->poisoned = true;
         continue;
      }
      const struct r3v_native_descriptor_layout_binding *s =
         &src->layout->bindings[copy->srcBinding];
      const struct r3v_native_descriptor_layout_binding *d =
         &dst->layout->bindings[copy->dstBinding];
      if (!s->present || !d->present || s->type != d->type ||
          copy->descriptorCount == 0 ||
          copy->srcArrayElement > s->count ||
          copy->descriptorCount > s->count - copy->srcArrayElement ||
          copy->dstArrayElement > d->count ||
          copy->descriptorCount > d->count - copy->dstArrayElement) {
         dst->poisoned = true;
         continue;
      }
      /* The executing shape is one element at index zero, so a whole
       * single-element copy carries the bound buffer; any other slice
       * leaves the destination binding unbound.
       */
      if (s->count == 1 && d->count == 1)
         dst->bindings[copy->dstBinding] = src->bindings[copy->srcBinding];
      else
         dst->bindings[copy->dstBinding] =
            (struct r3v_native_descriptor_binding){ .bound = false };
   }
}

/* A compute job binding executes through one storage-buffer element
 * visible to the compute stage; a layout binding of any other type,
 * count, or stage set is a declared object with no executing route.
 */
static bool
compute_binding_executes(
   const struct r3v_native_descriptor_set_layout *layout, uint32_t index)
{
   if (index >= R3V_NATIVE_DESCRIPTOR_BINDING_MAX)
      return false;
   const struct r3v_native_descriptor_layout_binding *b =
      &layout->bindings[index];
   return b->present && b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
          b->count == 1 && (b->stages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
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

   if (!device->pdevice->compute_queue_claimed)
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
   if (!r3v_compute_job_from_spirv((const uint32_t *)module->data,
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
   if (verb == NULL ||
       !r300_operation_has_executing_route(
          verb->operation_id, R300_OPERATION_ROUTE_EXECUTOR_HOST)) {
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
       !compute_binding_executes(set_layout, job.input_binding) ||
       !compute_binding_executes(set_layout, job.output_binding))
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   struct r3v_native_pipeline *pipeline =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*pipeline),
                       VK_OBJECT_TYPE_PIPELINE);
   if (pipeline == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline->is_compute = true;
   pipeline->compute_job = job;
   memcpy(pipeline->set0_bindings, set_layout->bindings,
          sizeof(pipeline->set0_bindings));

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
   /* The dispatch consumes the set through the pipeline's layout, so
    * the set's layout must be identically defined with the layout the
    * pipeline was created against.
    */
   if (pipeline == NULL || set == NULL || set->poisoned ||
       !r3v_native_descriptor_bindings_equal(pipeline->set0_bindings,
                                             set->layout->bindings) ||
       cmd_buffer->pass_target != NULL || cmd_buffer->draw_recorded ||
       cmd_buffer->deferred_draw_count != 0 ||
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

   if ((pipelineBindPoint != VK_PIPELINE_BIND_POINT_COMPUTE &&
        pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) ||
       firstSet != 0 || descriptorSetCount != 1 ||
       dynamicOffsetCount != 0) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   VK_FROM_HANDLE(r3v_native_descriptor_set, set, pDescriptorSets[0]);
   VK_FROM_HANDLE(vk_pipeline_layout, bind_layout, layout);
   /* The bind names a pipeline layout whose set 0 the set's own layout
    * must be compatible with; identically defined reduces to equal
    * typed binding maps.
    */
   if (set == NULL || bind_layout == NULL || bind_layout->set_count != 1) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   const struct r3v_native_descriptor_set_layout *bind_set_layout =
      container_of(bind_layout->set_layouts[0],
                   struct r3v_native_descriptor_set_layout, vk);
   if (!r3v_native_descriptor_layouts_equal(bind_set_layout,
                                            set->layout)) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
      cmd_buffer->bound_graphics_set = set;
   else
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

static enum r3v_native_cell_kind
r3v_native_compute_cell_kind(enum r300_gpu_route_contract_id contract_id)
{
   switch (contract_id) {
   case R300_GPU_ROUTE_CONTRACT_R2VB_COMPUTE_IDENTITY_CARRIER:
      return R3V_NATIVE_CELL_KIND_COMPUTE_IDENTITY_CARRIER;
   /* The RB2D linear fill has a plan and contracts and no executing route,
    * so it names no cell kind and the admission below declines it; a cell
    * kind lands with the receipt that makes the route execute. */
   case R300_GPU_ROUTE_CONTRACT_RB2D_LINEAR_SOLID_FILL:
   case R300_GPU_ROUTE_CONTRACT_NONE:
   case R300_GPU_ROUTE_CONTRACT_COUNT:
      return R3V_NATIVE_CELL_KIND_UNDECLARED;
   }
   return R3V_NATIVE_CELL_KIND_UNDECLARED;
}

/* The identity verb's GPU route admission.  The route is selected by the
 * compute gate (the device exists only under it) and the verb's own
 * exact gate; with the verb gate closed the dispatch keeps the CPU
 * route.  Every admission check precedes every allocation, reference,
 * IB, and write, so a refusal leaves the recording as recorded and the
 * output untouched (the ledger's refuse-before-write clause).
 */
VkResult
r3v_native_deferred_dispatch_admit_gpu(struct r3v_native_device *device,
                                       struct r3v_native_cmd_buffer *cmd_buffer)
{
   struct r3v_native_deferred_dispatch *dispatch =
      &cmd_buffer->deferred_dispatch;
   if (!dispatch->pending || dispatch->gpu_carrier_delivery)
      return VK_SUCCESS;
   /* The job resolves to a verb, the verb to an operation, and the
    * operation to at most one eligible executing GPU route.  Selection
    * reads each route's own gate, so an open gate on one route never makes
    * another eligible and an open gate on a candidate selects nothing; two
    * eligible routes refuse rather than letting table order decide. */
   const struct r300_compute_verb_row *verb =
      r300_compute_verb_for_job(&dispatch->job);
   if (verb == NULL)
      return VK_SUCCESS;

   bool gate_open[R300_OPERATION_ROUTE_COUNT] = { false };
   for (uint32_t r = 0; r < R300_OPERATION_ROUTE_COUNT; r++)
      gate_open[r] = device->compute_route_gates[r] != NULL;

   const struct r300_operation_route_row *route =
      r300_operation_select_route(verb->operation_id,
                                  R300_OPERATION_ROUTE_EXECUTOR_GPU,
                                  gate_open, NULL);
   if (route == NULL)
      return VK_SUCCESS;

   /* The selected route's own contracts must be the carrier's, so the
    * identity-carrier plan is compared against that exact row rather than
    * against the semantic verb, which no longer carries contracts. */
   const struct r300_compute_identity_carrier_contract *contract =
      &r300_compute_identity_carrier_contract;
   const enum r3v_native_cell_kind cell_kind =
      r3v_native_compute_cell_kind(route->gpu_route_contract_id);
   if (route->operation_id != contract->operation_id ||
       route->implementation_id != contract->implementation_id ||
       route->gpu_route_contract_id != contract->gpu_route_contract_id ||
       route->admission_id != contract->admission_id ||
       contract->admission_id != R300_ROUTE_ADMISSION_R2VB_FP24_IDENTITY ||
       cell_kind == R3V_NATIVE_CELL_KIND_UNDECLARED)
      return VK_SUCCESS;
   if (device->gpu_compute_quarantined) {
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: GPU compute capability is quarantined "
                       "on this device; a completed delivery diverged from "
                       "the CPU oracle");
   }

   /* The carrier moves whole F32_4 records inside the single-row
    * ceiling; a dispatch outside that shape refuses by name under the
    * open gate rather than taking a route the gate did not select. */
   if (dispatch->byte_count % contract->record_bytes != 0 ||
       dispatch->byte_count / contract->record_bytes >
          contract->max_records) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: compute identity carrier moves whole "
                       "F32_4 records, at most %u; the dispatch names %"
                       PRIu64 " bytes",
                       contract->max_records,
                       dispatch->byte_count);
   }
   const uint32_t records =
      (uint32_t)(dispatch->byte_count / contract->record_bytes);
   struct r3v_native_memory *input_memory = dispatch->input_buffer->memory;
   struct r3v_native_memory *output_memory =
      dispatch->output_buffer->memory;
   if (input_memory == NULL || output_memory == NULL) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: dispatch buffer is unbound at "
                       "admission");
   }
   /* The three relocations resolve one BO each: the reloc list folds
    * duplicate handles into one entry, which would shift the chunk
    * index the pass payloads name. */
   if (input_memory->bo.handle == output_memory->bo.handle) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: compute identity carrier binds the "
                       "input and output as two relocations; one buffer "
                       "object holds both");
   }
   if (dispatch->input_memory_offset > UINT32_MAX ||
       dispatch->output_memory_offset > UINT32_MAX ||
       dispatch->input_memory_offset % contract->input_alignment != 0 ||
       dispatch->output_memory_offset % contract->output_alignment != 0) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: compute identity carrier takes a "
                       "dword-granular input offset and a 32-byte-aligned "
                       "output offset inside the 32-bit pointer; the "
                       "dispatch binds %" PRIu64 " and %" PRIu64,
                       dispatch->input_memory_offset,
                       dispatch->output_memory_offset);
   }
   struct r300_r2vb_producer_layout layout;
   if (r300_compute_identity_carrier_layout(records, &layout) != 0 ||
       r300_compute_identity_carrier_output_bytes(&layout) >
          output_memory->bo.size - dispatch->output_memory_offset) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: the output memory holds fewer bytes "
                       "past the offset than the %u-slot row's color "
                       "bound", layout.pitch_pixels);
   }

   /* The oracle: the FP24 identity over the input records -- the bytes
    * the fetch, the US datapath, and the C4_32_FP export reproduce --
    * refusing a word outside the window with -EDOM; inside the window
    * the result is the bit copy the CPU route writes. */
   uint8_t *input_map = NULL;
   bool input_owned = false;
   VkResult result = map_memory(device, input_memory, &input_map,
                                &input_owned);
   if (result != VK_SUCCESS)
      return result;
   const uint32_t expected_dwords =
      records * contract->record_dwords;
   uint32_t *expected = vk_alloc(&cmd_buffer->vk.pool->alloc,
                                 (size_t)expected_dwords * sizeof(uint32_t),
                                 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (expected == NULL) {
      release_memory(device, input_memory, input_owned);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   const uint32_t *input_words =
      (const uint32_t *)(input_map + dispatch->input_memory_offset);
   const int modeled = r300_compute_identity_carrier_expected(
      input_words, records, expected, expected_dwords);
   const bool bit_copy_agrees =
      modeled == 0 &&
      memcmp(expected, input_words,
             (size_t)expected_dwords * sizeof(uint32_t)) == 0;
   release_memory(device, input_memory, input_owned);
   if (modeled != 0 || !bit_copy_agrees) {
      vk_free(&cmd_buffer->vk.pool->alloc, expected);
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       modeled == -EDOM
                          ? "r3v-native: compute identity carrier oracle "
                            "refused: an input word is outside the FP24 "
                            "fixed-point window"
                       : modeled != 0
                          ? "r3v-native: compute identity carrier oracle "
                            "refused (%d)"
                          : "r3v-native: compute identity carrier oracle "
                            "diverged from the CPU bit copy (%d)",
                       modeled);
   }

   /* The pass over the bound geometry. */
   const uint64_t slot_bytes =
      ((uint64_t)records * R300_R2VB_FETCHED_PRODUCER_SLOT_RECORD_BYTES +
       4095) & ~(uint64_t)4095;
   const struct r300_compute_identity_carrier_params params = {
      .record_count = records,
      .carrier_offset = (uint32_t)dispatch->output_memory_offset,
      .carrier_bo_size_bytes = output_memory->bo.size,
      .source_offset = (uint32_t)dispatch->input_memory_offset,
      .source_bo_size_bytes = input_memory->bo.size,
      .slot_offset_bytes = 0,
      .slot_bo_size_bytes = slot_bytes,
   };
   struct r300_r2vb_fetched_producer_ib pass;
   const int emitted = r300_compute_identity_carrier_emit(&params, &pass);
   if (emitted != 0) {
      vk_free(&cmd_buffer->vk.pool->alloc, expected);
      return vk_errorf(device, r3v_native_cell_vk_result_from_errno(emitted),
                       "r3v-native: compute identity carrier emission "
                       "refused (%d)", emitted);
   }

   /* The slot BO: allocated per command buffer at the records' size,
    * its positions rewritten on every admission. */
   struct r3v_native_memory *slot = cmd_buffer->owned_slot;
   if (slot != NULL && slot->bo.size < slot_bytes) {
      radeon_drm_vk_bo_free(&device->drm, &slot->bo);
      vk_free(&cmd_buffer->vk.pool->alloc, slot);
      slot = NULL;
      cmd_buffer->owned_slot = NULL;
   }
   if (slot == NULL) {
      slot = vk_zalloc(&cmd_buffer->vk.pool->alloc, sizeof(*slot), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (slot == NULL) {
         r300_r2vb_fetched_producer_release(&pass);
         vk_free(&cmd_buffer->vk.pool->alloc, expected);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      if (radeon_drm_vk_bo_create(&device->drm, slot_bytes,
                                  R3V_NATIVE_MEMORY_ALIGNMENT,
                                  RADEON_GEM_DOMAIN_GTT, 0, false,
                                  &slot->bo) != 0) {
         vk_free(&cmd_buffer->vk.pool->alloc, slot);
         r300_r2vb_fetched_producer_release(&pass);
         vk_free(&cmd_buffer->vk.pool->alloc, expected);
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
   }
   void *slot_map = NULL;
   if (radeon_drm_vk_bo_map(&device->drm, &slot->bo, &slot_map) != 0) {
      if (cmd_buffer->owned_slot == NULL) {
         radeon_drm_vk_bo_free(&device->drm, &slot->bo);
         vk_free(&cmd_buffer->vk.pool->alloc, slot);
      }
      r300_r2vb_fetched_producer_release(&pass);
      vk_free(&cmd_buffer->vk.pool->alloc, expected);
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: slot memory is not CPU-mappable at "
                       "admission");
   }
   const uint32_t slot_dwords =
      records * (R300_R2VB_FETCHED_PRODUCER_SLOT_RECORD_BYTES / 4);
   ASSERTED const int positioned = r300_r2vb_fetched_producer_slot_positions(
      records, slot_map, slot_dwords);
   assert(positioned == 0);
   radeon_drm_vk_bo_cache_sync(&device->drm, slot_map,
                               (size_t)slot_dwords * 4);
   radeon_drm_vk_bo_unmap(&device->drm, &slot->bo, slot_map);

   struct r3v_native_bo_reference *references =
      calloc(3, sizeof(*references));
   if (references == NULL) {
      if (cmd_buffer->owned_slot == NULL) {
         radeon_drm_vk_bo_free(&device->drm, &slot->bo);
         vk_free(&cmd_buffer->vk.pool->alloc, slot);
      }
      r300_r2vb_fetched_producer_release(&pass);
      vk_free(&cmd_buffer->vk.pool->alloc, expected);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   references[0] = (struct r3v_native_bo_reference){
      .handle = output_memory->bo.handle,
      .read_domains = 0,
      .write_domain = RADEON_GEM_DOMAIN_GTT,
      .memory = output_memory,
   };
   references[1] = (struct r3v_native_bo_reference){
      .handle = slot->bo.handle,
      .read_domains = RADEON_GEM_DOMAIN_GTT,
      .write_domain = 0,
      .memory = slot,
   };
   references[2] = (struct r3v_native_bo_reference){
      .handle = input_memory->bo.handle,
      .read_domains = RADEON_GEM_DOMAIN_GTT,
      .write_domain = 0,
      .memory = input_memory,
   };
   /* Every fallible step has completed: install the pass. */
   cmd_buffer->owned_slot = slot;
   r3v_native_cmd_buffer_install_ib(
      cmd_buffer, cell_kind, pass.ib, pass.ib_size_dwords, references, 3);
   pass.ib = NULL;
   r300_r2vb_fetched_producer_release(&pass);
   dispatch->gpu_carrier_delivery = true;
   dispatch->gpu_record_count = records;
   dispatch->gpu_expected = expected;
   return VK_SUCCESS;
}

static void
retain_words(const char *dir, const char *name, const void *bytes,
             size_t size)
{
   char path[4096];
   const int length = snprintf(path, sizeof(path), "%s/%s", dir, name);
   if (length <= 0 || (size_t)length >= sizeof(path))
      return;
   FILE *file = fopen(path, "wb");
   if (file == NULL)
      return;
   fwrite(bytes, 1, size, file);
   fclose(file);
}

VkResult
r3v_native_deferred_dispatch_verify_gpu(struct r3v_native_device *device,
                                        struct r3v_native_cmd_buffer *cmd_buffer)
{
   struct r3v_native_deferred_dispatch *dispatch =
      &cmd_buffer->deferred_dispatch;
   if (!dispatch->gpu_carrier_delivery)
      return VK_SUCCESS;
   assert(dispatch->gpu_expected != NULL && dispatch->gpu_record_count != 0);

   struct r3v_native_memory *output_memory =
      dispatch->output_buffer->memory;
   const size_t bytes = (size_t)dispatch->gpu_record_count *
                        r300_compute_identity_carrier_contract.record_bytes;
   uint8_t *output_map = NULL;
   bool output_owned = false;
   if (map_memory(device, output_memory, &output_map, &output_owned) !=
       VK_SUCCESS) {
      device->gpu_compute_quarantined = true;
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: compute output read-back mapping "
                       "failed; GPU compute capability quarantined");
   }
   /* The device wrote the output past the host's cache, so the read
    * extent invalidates before the compare (the rule r3v_MapMemory
    * states for the public path). */
   uint8_t *observed = output_map + dispatch->output_memory_offset;
   radeon_drm_vk_bo_cache_sync_for_bo(&device->drm, &output_memory->bo,
                                      observed, bytes);
   const bool matches = memcmp(observed, dispatch->gpu_expected, bytes) == 0;
   if (device->manifest_dir != NULL) {
      retain_words(device->manifest_dir, "gpu_compute_observed.bin",
                   observed, bytes);
      retain_words(device->manifest_dir, "gpu_compute_expected.bin",
                   dispatch->gpu_expected, bytes);
   }
   release_memory(device, output_memory, output_owned);
   if (!matches) {
      device->gpu_compute_quarantined = true;
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: compute identity carrier output "
                       "diverged from the CPU oracle; capability "
                       "quarantined and the observed bytes retained");
   }
   return VK_SUCCESS;
}
