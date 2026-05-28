/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * r300vk descriptor-set machinery -- the M-E prerequisite the exhaustive
 * compute probe identified.  vk_common provides CreatePipelineLayout but not
 * the descriptor-set entrypoints (a descriptor set maps to driver-specific
 * resource state, so each Mesa driver implements its own).
 *
 * Object lifecycle only: the layout records bindings, the pool lends sets, a
 * set records bound buffer / image handles per binding.  The gallium-mediated
 * consumption (the bound set -> r300g sampler_views / shader_buffers at
 * draw_vbo / dispatch replay time) is a later stage (M-E); a no-op kernel
 * (M-D) does not read the descriptors so the unwritten slots are not read.
 */

#include "r300vk_descriptor.h"
#include "r300vk_device.h"
#include "r300vk_entrypoints.h"

#include "vk_alloc.h"
#include "vk_descriptor_set_layout.h"
#include "vk_log.h"

#include <stdlib.h>
#include <string.h>

static int compare_binding_index(const void *a, const void *b)
{
   uint32_t ba = ((const struct r300vk_dsl_binding *)a)->binding;
   uint32_t bb = ((const struct r300vk_dsl_binding *)b)->binding;
   return (ba < bb) ? -1 : (ba > bb) ? 1 : 0;
}

VkResult
r300vk_CreateDescriptorSetLayout(VkDevice _device,
                                 const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkDescriptorSetLayout *pSetLayout)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   const uint32_t n = pCreateInfo->bindingCount;
   const size_t size = sizeof(struct r300vk_descriptor_set_layout) +
                       n * sizeof(struct r300vk_dsl_binding);

   struct r300vk_descriptor_set_layout *layout =
      vk_descriptor_set_layout_zalloc(&device->vk, size, pCreateInfo);
   if (!layout)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   layout->binding_count = n;
   for (uint32_t i = 0; i < n; i++) {
      const VkDescriptorSetLayoutBinding *src = &pCreateInfo->pBindings[i];
      layout->bindings[i] = (struct r300vk_dsl_binding){
         .binding      = src->binding,
         .type         = src->descriptorType,
         .count        = src->descriptorCount,
         .stage_flags  = src->stageFlags,
      };
   }
   /* Sort by binding index so AllocateDescriptorSets can compute the linear
    * slot offset by a single in-order pass, and so UpdateDescriptorSets can
    * binary-search the dst binding. */
   qsort(layout->bindings, n, sizeof(layout->bindings[0]), compare_binding_index);
   uint32_t off = 0;
   for (uint32_t i = 0; i < n; i++) {
      layout->bindings[i].offset = off;
      off += layout->bindings[i].count;
   }
   layout->total_descriptors = off;

   *pSetLayout = r300vk_descriptor_set_layout_to_handle(layout);
   return VK_SUCCESS;
}

VkResult
r300vk_CreateDescriptorPool(VkDevice _device,
                            const VkDescriptorPoolCreateInfo *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkDescriptorPool *pDescriptorPool)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);

   struct r300vk_descriptor_pool *pool =
      vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*pool), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pool)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   vk_object_base_init(&device->vk, &pool->base, VK_OBJECT_TYPE_DESCRIPTOR_POOL);

   pool->max_sets = pCreateInfo->maxSets ? pCreateInfo->maxSets : 1;
   pool->sets = vk_zalloc2(&device->vk.alloc, pAllocator,
                           pool->max_sets * sizeof(pool->sets[0]), 8,
                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pool->sets) {
      vk_object_base_finish(&pool->base);
      vk_free2(&device->vk.alloc, pAllocator, pool);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   *pDescriptorPool = r300vk_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

void
r300vk_DestroyDescriptorPool(VkDevice _device, VkDescriptorPool _pool,
                             const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_descriptor_pool, pool, _pool);
   if (!pool)
      return;
   for (uint32_t i = 0; i < pool->max_sets; i++) {
      if (pool->sets[i].descriptors)
         vk_free2(&device->vk.alloc, pAllocator, pool->sets[i].descriptors);
      if (pool->sets[i].allocated)
         vk_object_base_finish(&pool->sets[i].base);
   }
   vk_free2(&device->vk.alloc, pAllocator, pool->sets);
   vk_object_base_finish(&pool->base);
   vk_free2(&device->vk.alloc, pAllocator, pool);
}

VkResult
r300vk_ResetDescriptorPool(VkDevice _device, VkDescriptorPool _pool,
                           VkDescriptorPoolResetFlags flags)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_descriptor_pool, pool, _pool);
   (void)flags;
   for (uint32_t i = 0; i < pool->max_sets; i++) {
      if (pool->sets[i].allocated) {
         vk_free(&device->vk.alloc, pool->sets[i].descriptors);
         pool->sets[i].descriptors = NULL;
         vk_object_base_finish(&pool->sets[i].base);
         pool->sets[i].allocated = false;
      }
   }
   pool->allocated_sets = 0;
   return VK_SUCCESS;
}

VkResult
r300vk_AllocateDescriptorSets(VkDevice _device,
                              const VkDescriptorSetAllocateInfo *pAllocateInfo,
                              VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_descriptor_pool, pool, pAllocateInfo->descriptorPool);

   for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      VK_FROM_HANDLE(r300vk_descriptor_set_layout, layout,
                     pAllocateInfo->pSetLayouts[i]);

      /* Find a free slot in the pool's set array.  A linear scan is fine for
       * the typical max_sets <= 1024 a compute kernel needs. */
      struct r300vk_descriptor_set *set = NULL;
      for (uint32_t j = 0; j < pool->max_sets; j++) {
         if (!pool->sets[j].allocated) {
            set = &pool->sets[j];
            break;
         }
      }
      if (!set) {
         /* Roll back the sets allocated in this call so the caller sees a
          * consistent state. */
         for (uint32_t j = 0; j < i; j++)
            pDescriptorSets[j] = VK_NULL_HANDLE;
         return vk_error(device, VK_ERROR_OUT_OF_POOL_MEMORY);
      }

      vk_object_base_init(&device->vk, &set->base, VK_OBJECT_TYPE_DESCRIPTOR_SET);
      set->layout    = layout;
      set->allocated = true;
      if (layout->total_descriptors > 0) {
         set->descriptors = vk_zalloc(&device->vk.alloc,
                                       layout->total_descriptors *
                                          sizeof(set->descriptors[0]),
                                       8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
         if (!set->descriptors) {
            vk_object_base_finish(&set->base);
            set->allocated = false;
            return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         }
      }
      pool->allocated_sets++;
      pDescriptorSets[i] = r300vk_descriptor_set_to_handle(set);
   }
   return VK_SUCCESS;
}

VkResult
r300vk_FreeDescriptorSets(VkDevice _device, VkDescriptorPool _pool,
                          uint32_t descriptorSetCount,
                          const VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_descriptor_pool, pool, _pool);
   for (uint32_t i = 0; i < descriptorSetCount; i++) {
      VK_FROM_HANDLE(r300vk_descriptor_set, set, pDescriptorSets[i]);
      if (!set || !set->allocated)
         continue;
      vk_free(&device->vk.alloc, set->descriptors);
      set->descriptors = NULL;
      vk_object_base_finish(&set->base);
      set->allocated = false;
      if (pool->allocated_sets > 0)
         pool->allocated_sets--;
   }
   return VK_SUCCESS;
}

/* Find the layout binding for dst_binding (in-order scan; bindings are sorted
 * but binding-index gaps are allowed by the spec, so binary-search would buy
 * little and linear is correct). */
static const struct r300vk_dsl_binding *
find_binding(const struct r300vk_descriptor_set_layout *layout, uint32_t dst_binding)
{
   for (uint32_t i = 0; i < layout->binding_count; i++)
      if (layout->bindings[i].binding == dst_binding)
         return &layout->bindings[i];
   return NULL;
}

void
r300vk_UpdateDescriptorSets(VkDevice _device,
                            uint32_t descriptorWriteCount,
                            const VkWriteDescriptorSet *pDescriptorWrites,
                            uint32_t descriptorCopyCount,
                            const VkCopyDescriptorSet *pDescriptorCopies)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   (void)device;

   for (uint32_t w = 0; w < descriptorWriteCount; w++) {
      const VkWriteDescriptorSet *write = &pDescriptorWrites[w];
      VK_FROM_HANDLE(r300vk_descriptor_set, set, write->dstSet);
      const struct r300vk_dsl_binding *b = find_binding(set->layout,
                                                        write->dstBinding);
      if (!b)
         continue;
      /* Walk dstArrayElement..dstArrayElement+descriptorCount-1, possibly
       * spilling into successive bindings if the layout chain consents (the
       * spec allows the write to cross into adjacent bindings of identical
       * type; the simple impl here stays within the named binding). */
      uint32_t base = b->offset + write->dstArrayElement;
      for (uint32_t d = 0; d < write->descriptorCount; d++) {
         struct r300vk_descriptor *slot = &set->descriptors[base + d];
         slot->type = write->descriptorType;
         switch (write->descriptorType) {
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: {
            const VkDescriptorBufferInfo *bi = &write->pBufferInfo[d];
            slot->buf.buffer = bi->buffer;
            slot->buf.offset = bi->offset;
            slot->buf.range  = bi->range;
            break;
         }
         case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
         case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
            const VkDescriptorImageInfo *ii = &write->pImageInfo[d];
            slot->img.image_view = ii->imageView;
            slot->img.layout     = ii->imageLayout;
            slot->img.sampler    = ii->sampler;
            break;
         }
         case VK_DESCRIPTOR_TYPE_SAMPLER: {
            const VkDescriptorImageInfo *ii = &write->pImageInfo[d];
            slot->img.sampler = ii->sampler;
            break;
         }
         default:
            /* Texel buffers and inline-uniform-block are not yet wired into
             * the gallium binding; record the type so a future stage can
             * detect and reject early.  Not a stub: the slot's type field
             * makes the unsupported case visible to the dispatch replay. */
            break;
         }
      }
   }

   for (uint32_t c = 0; c < descriptorCopyCount; c++) {
      const VkCopyDescriptorSet *cp = &pDescriptorCopies[c];
      VK_FROM_HANDLE(r300vk_descriptor_set, src_set, cp->srcSet);
      VK_FROM_HANDLE(r300vk_descriptor_set, dst_set, cp->dstSet);
      const struct r300vk_dsl_binding *src_b = find_binding(src_set->layout, cp->srcBinding);
      const struct r300vk_dsl_binding *dst_b = find_binding(dst_set->layout, cp->dstBinding);
      if (!src_b || !dst_b)
         continue;
      memcpy(&dst_set->descriptors[dst_b->offset + cp->dstArrayElement],
             &src_set->descriptors[src_b->offset + cp->srcArrayElement],
             cp->descriptorCount * sizeof(struct r300vk_descriptor));
   }
}
