/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * r300vk descriptor-set machinery -- object lifecycle for VkDescriptorSetLayout,
 * VkDescriptorPool, VkDescriptorSet.  vk_common provides CreatePipelineLayout
 * but not the descriptor-set entrypoints (a descriptor set maps to
 * driver-specific resource state, so each Mesa driver implements its own).
 *
 * Scope: object lifecycle only.  The layout records bindings; the pool is a
 * bump-allocator of max_sets slots; a set records bound buffer / image handles
 * per binding.  The gallium-binding stage (the bound set translates into r300g
 * pipe_context sampler_views / shader_buffers / shader_images / constant_buffers
 * at draw_vbo / dispatch replay time) is a separate stage; a no-op dispatch
 * kernel does not read the descriptors so the unwritten slots are not read.
 */

#include "r300vk_descriptor.h"
#include "r300vk_buffer.h"
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

static bool
descriptor_type_is_dynamic(VkDescriptorType t)
{
   return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
          t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
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
   uint32_t dynamic_count = 0;
   for (uint32_t i = 0; i < n; i++) {
      const VkDescriptorSetLayoutBinding *src = &pCreateInfo->pBindings[i];
      layout->bindings[i] = (struct r300vk_dsl_binding){
         .binding      = src->binding,
         .type         = src->descriptorType,
         .count        = src->descriptorCount,
         .stage_flags  = src->stageFlags,
      };
      if (descriptor_type_is_dynamic(src->descriptorType))
         dynamic_count += src->descriptorCount;
   }
   /* Sort by binding index so AllocateDescriptorSets can compute the linear
    * slot offset by a single in-order pass, and so find_binding can scan in
    * order. */
   qsort(layout->bindings, n, sizeof(layout->bindings[0]), compare_binding_index);
   uint32_t off = 0;
   for (uint32_t i = 0; i < n; i++) {
      layout->bindings[i].offset = off;
      off += layout->bindings[i].count;
   }
   layout->total_descriptors = off;

   /* vk_common_CreatePipelineLayout reads dynamic_descriptor_count off the
    * runtime base to compute per-set dynamic_descriptor_offset[s], the index
    * each set's dynamic offsets start at in the bind-time pDynamicOffsets
    * array.  Without this count, any pipeline-layout that contains a dynamic
    * descriptor binding computes offset 0 for every set and binds the wrong
    * data. */
   layout->base.dynamic_descriptor_count = dynamic_count;

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

/* Tear down one allocated slot and clear its bookkeeping.  Used by the
 * AllocateDescriptorSets failure rollback, FreeDescriptorSets, and
 * ResetDescriptorPool.  set->descriptors was allocated with the device alloc
 * (no pAllocator), so the free call matches that side; any caller-supplied
 * pAllocator only owns the pool struct itself. */
static void
release_set_slot(struct r300vk_device *device, struct r300vk_descriptor_set *set)
{
   if (!set->allocated)
      return;
   if (set->descriptors) {
      vk_free(&device->vk.alloc, set->descriptors);
      set->descriptors = NULL;
   }
   vk_object_base_finish(&set->base);
   set->allocated = false;
   set->layout = NULL;
}

void
r300vk_DestroyDescriptorPool(VkDevice _device, VkDescriptorPool _pool,
                             const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_descriptor_pool, pool, _pool);
   if (!pool)
      return;
   for (uint32_t i = 0; i < pool->max_sets; i++)
      release_set_slot(device, &pool->sets[i]);
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
   for (uint32_t i = 0; i < pool->max_sets; i++)
      release_set_slot(device, &pool->sets[i]);
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
   const uint32_t n = pAllocateInfo->descriptorSetCount;

   for (uint32_t i = 0; i < n; i++) {
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
      VkResult fail_result = VK_SUCCESS;
      if (!set) {
         fail_result = VK_ERROR_OUT_OF_POOL_MEMORY;
      } else {
         vk_object_base_init(&device->vk, &set->base,
                             VK_OBJECT_TYPE_DESCRIPTOR_SET);
         set->layout    = layout;
         set->allocated = true;
         if (layout->total_descriptors > 0) {
            set->descriptors =
               vk_zalloc(&device->vk.alloc,
                         layout->total_descriptors * sizeof(set->descriptors[0]),
                         8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
            if (!set->descriptors) {
               /* Roll back this half-initialized slot before falling into the
                * shared failure path. */
               vk_object_base_finish(&set->base);
               set->allocated = false;
               set->layout = NULL;
               fail_result = VK_ERROR_OUT_OF_HOST_MEMORY;
            }
         }
      }
      if (fail_result != VK_SUCCESS) {
         /* VUID-vkAllocateDescriptorSets-pDescriptorSets-00756: every
          * pDescriptorSets[k] for k in [0,n) MUST be VK_NULL_HANDLE on
          * failure.  Also release the slots committed by earlier iterations
          * so the pool's allocated_sets and per-slot state stay consistent;
          * a subsequent allocate before a reset must see the real free
          * count. */
         for (uint32_t j = 0; j < i; j++) {
            VK_FROM_HANDLE(r300vk_descriptor_set, prev, pDescriptorSets[j]);
            if (prev) {
               release_set_slot(device, prev);
               if (pool->allocated_sets > 0)
                  pool->allocated_sets--;
            }
         }
         for (uint32_t j = 0; j < n; j++)
            pDescriptorSets[j] = VK_NULL_HANDLE;
         return vk_error(device, fail_result);
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
      release_set_slot(device, set);
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
      /* Walk dstArrayElement..dstArrayElement+descriptorCount-1.  The Vulkan
       * spec allows the write to spill into adjacent bindings of identical
       * type (VUID-VkWriteDescriptorSet-dstArrayElement-00321); this impl
       * spans the linear set->descriptors[] array without checking adjacent
       * binding-type identity, which is correct for r300vk's compute kernels
       * (single binding per set) and over-permissive for multi-binding layouts
       * that mix descriptor types -- the slot->type field still records the
       * actual type written, so the consumer can detect a mismatch at replay
       * time.  Cap the write to the set's total_descriptors to keep the index
       * inside the allocated array. */
      uint32_t base = b->offset + write->dstArrayElement;
      uint32_t total = set->layout->total_descriptors;
      uint32_t span = write->descriptorCount;
      if (base >= total)
         continue;
      if (base + span > total)
         span = total - base;
      for (uint32_t d = 0; d < span; d++) {
         struct r300vk_descriptor *slot = &set->descriptors[base + d];
         slot->type = write->descriptorType;
         switch (write->descriptorType) {
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: {
            const VkDescriptorBufferInfo *bi = &write->pBufferInfo[d];
            /* Bound-check (offset, range) against the buffer's actual
             * size before stamping the slot.  Without this, a Vulkan
             * application that binds offset+range > buffer-size --
             * legal Vulkan API call from the client's perspective but
             * a runtime mistake -- gets a slot that points past
             * the buffer's allocated pages; the M-E/M-F orchestrator's
             * wrap helper would then read garbage at the upper GART
             * range.  This defense replaces the falsified GART-3
             * NIR-clamp (see steinmarder
             * src/re/r300/findings/active/2026-05-28-r300-pfs-no-flat-
             * 32bit-shader-address-falsified.md) -- R300 PFS has no
             * shader-emitted flat address, so the defense surface
             * lives at descriptor binding, not in NIR.
             *
             * VK_WHOLE_SIZE (~0ull) means "from offset to end of
             * buffer", which is by construction in-bounds; the Mesa
             * runtime does not resolve it earlier than this site, so
             * accept it.  An out-of-bounds binding is silently
             * skipped (the previous slot's value, NULL after
             * AllocateDescriptorSets's zalloc, persists). */
            VK_FROM_HANDLE(r300vk_buffer, buf, bi->buffer);
            bool in_bounds = (buf == NULL); /* NULL-buffer = unbind, fine */
            if (buf) {
               in_bounds = (bi->offset <= buf->size) &&
                           (bi->range == VK_WHOLE_SIZE ||
                            (bi->range <= buf->size - bi->offset));
            }
            if (!in_bounds) {
               slot->type = 0;
               break;
            }
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
             * the gallium-binding stage; the slot's recorded type makes the
             * unsupported case visible to the dispatch replay so it can
             * reject early rather than dispatch with stale data. */
            break;
         }
      }
   }

   for (uint32_t c = 0; c < descriptorCopyCount; c++) {
      const VkCopyDescriptorSet *cp = &pDescriptorCopies[c];
      VK_FROM_HANDLE(r300vk_descriptor_set, src_set, cp->srcSet);
      VK_FROM_HANDLE(r300vk_descriptor_set, dst_set, cp->dstSet);
      const struct r300vk_dsl_binding *src_b =
         find_binding(src_set->layout, cp->srcBinding);
      const struct r300vk_dsl_binding *dst_b =
         find_binding(dst_set->layout, cp->dstBinding);
      if (!src_b || !dst_b)
         continue;
      /* The spec permits a copy to span adjacent bindings of identical type;
       * cap descriptorCount to whichever side has fewer remaining slots in
       * the linear array so a malformed call cannot memcpy past the
       * descriptors allocation. */
      uint32_t src_total = src_set->layout->total_descriptors;
      uint32_t dst_total = dst_set->layout->total_descriptors;
      uint32_t src_base = src_b->offset + cp->srcArrayElement;
      uint32_t dst_base = dst_b->offset + cp->dstArrayElement;
      if (src_base >= src_total || dst_base >= dst_total)
         continue;
      uint32_t span = cp->descriptorCount;
      if (src_base + span > src_total) span = src_total - src_base;
      if (dst_base + span > dst_total) span = dst_total - dst_base;
      memcpy(&dst_set->descriptors[dst_base],
             &src_set->descriptors[src_base],
             span * sizeof(struct r300vk_descriptor));
   }
}
