/*
 * SPDX-License-Identifier: MIT
 *
 * r3v descriptor-set machinery -- object lifecycle for VkDescriptorSetLayout,
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

#include "r3v_descriptor.h"
#include "r3v_buffer.h"
#include "r3v_device.h"
#include "r3v_entrypoints.h"

#include "vk_alloc.h"
#include "vk_descriptor_set_layout.h"
#include "vk_descriptor_update_template.h"
#include "vk_log.h"

#include <stdlib.h>
#include <string.h>

static int compare_binding_index(const void *a, const void *b)
{
   uint32_t ba = ((const struct r3v_dsl_binding *)a)->binding;
   uint32_t bb = ((const struct r3v_dsl_binding *)b)->binding;
   return (ba < bb) ? -1 : (ba > bb) ? 1 : 0;
}

static bool
descriptor_type_is_dynamic(VkDescriptorType t)
{
   return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
          t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

bool
r3v_descriptor_type_supports_immutable_samplers(VkDescriptorType t)
{
   return t == VK_DESCRIPTOR_TYPE_SAMPLER ||
          t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

VkResult
r3v_CreateDescriptorSetLayout(VkDevice _device,
                                 const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkDescriptorSetLayout *pSetLayout)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   const uint32_t n = pCreateInfo->bindingCount;
   uint32_t immutable_sampler_count = 0;

   for (uint32_t i = 0; i < n; i++) {
      const VkDescriptorSetLayoutBinding *src = &pCreateInfo->pBindings[i];
      if (r3v_descriptor_type_supports_immutable_samplers(src->descriptorType) &&
          src->pImmutableSamplers)
         immutable_sampler_count += src->descriptorCount;
   }

   const size_t size = sizeof(struct r3v_descriptor_set_layout) +
                       n * sizeof(struct r3v_dsl_binding) +
                       immutable_sampler_count * sizeof(VkSampler);

   struct r3v_descriptor_set_layout *layout =
      vk_descriptor_set_layout_zalloc(&device->vk, size, pCreateInfo);
   if (!layout)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   layout->binding_count = n;
   uint32_t dynamic_count = 0;
   VkSampler *immutable_samplers = (VkSampler *)&layout->bindings[n];
   for (uint32_t i = 0; i < n; i++) {
      const VkDescriptorSetLayoutBinding *src = &pCreateInfo->pBindings[i];
      layout->bindings[i] = (struct r3v_dsl_binding){
         .binding      = src->binding,
         .type         = src->descriptorType,
         .count        = src->descriptorCount,
         .stage_flags  = src->stageFlags,
      };
      if (r3v_descriptor_type_supports_immutable_samplers(src->descriptorType) &&
          src->pImmutableSamplers) {
         layout->bindings[i].immutable_samplers = immutable_samplers;
         memcpy(immutable_samplers, src->pImmutableSamplers,
                src->descriptorCount * sizeof(*immutable_samplers));
         immutable_samplers += src->descriptorCount;
      }
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

   *pSetLayout = r3v_descriptor_set_layout_to_handle(layout);
   return VK_SUCCESS;
}

VkResult
r3v_CreateDescriptorPool(VkDevice _device,
                            const VkDescriptorPoolCreateInfo *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkDescriptorPool *pDescriptorPool)
{
   VK_FROM_HANDLE(r3v_device, device, _device);

   struct r3v_descriptor_pool *pool =
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

   *pDescriptorPool = r3v_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

/* Tear down one allocated slot and clear its bookkeeping.  Used by the
 * AllocateDescriptorSets failure rollback, FreeDescriptorSets, and
 * ResetDescriptorPool.  set->descriptors was allocated with the device alloc
 * (no pAllocator), so the free call matches that side; any caller-supplied
 * pAllocator only owns the pool struct itself. */
static void
release_set_slot(struct r3v_device *device, struct r3v_descriptor_set *set)
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
r3v_DestroyDescriptorPool(VkDevice _device, VkDescriptorPool _pool,
                             const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_descriptor_pool, pool, _pool);
   if (!pool)
      return;
   for (uint32_t i = 0; i < pool->max_sets; i++)
      release_set_slot(device, &pool->sets[i]);
   vk_free2(&device->vk.alloc, pAllocator, pool->sets);
   vk_object_base_finish(&pool->base);
   vk_free2(&device->vk.alloc, pAllocator, pool);
}

VkResult
r3v_ResetDescriptorPool(VkDevice _device, VkDescriptorPool _pool,
                           VkDescriptorPoolResetFlags flags)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_descriptor_pool, pool, _pool);
   (void)flags;
   for (uint32_t i = 0; i < pool->max_sets; i++)
      release_set_slot(device, &pool->sets[i]);
   pool->allocated_sets = 0;
   return VK_SUCCESS;
}

VkResult
r3v_AllocateDescriptorSets(VkDevice _device,
                              const VkDescriptorSetAllocateInfo *pAllocateInfo,
                              VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_descriptor_pool, pool, pAllocateInfo->descriptorPool);
   const uint32_t n = pAllocateInfo->descriptorSetCount;

   for (uint32_t i = 0; i < n; i++) {
      VK_FROM_HANDLE(r3v_descriptor_set_layout, layout,
                     pAllocateInfo->pSetLayouts[i]);

      /* Find a free slot in the pool's set array.  A linear scan is fine for
       * the typical max_sets <= 1024 a compute kernel needs. */
      struct r3v_descriptor_set *set = NULL;
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
            } else {
               r3v_initialize_immutable_sampler_descriptors(set);
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
            VK_FROM_HANDLE(r3v_descriptor_set, prev, pDescriptorSets[j]);
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
      pDescriptorSets[i] = r3v_descriptor_set_to_handle(set);
   }
   return VK_SUCCESS;
}

VkResult
r3v_FreeDescriptorSets(VkDevice _device, VkDescriptorPool _pool,
                          uint32_t descriptorSetCount,
                          const VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_descriptor_pool, pool, _pool);
   for (uint32_t i = 0; i < descriptorSetCount; i++) {
      VK_FROM_HANDLE(r3v_descriptor_set, set, pDescriptorSets[i]);
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
static const struct r3v_dsl_binding *
find_binding(const struct r3v_descriptor_set_layout *layout, uint32_t dst_binding)
{
   for (uint32_t i = 0; i < layout->binding_count; i++)
      if (layout->bindings[i].binding == dst_binding)
         return &layout->bindings[i];
   return NULL;
}

static const struct r3v_dsl_binding *
find_binding_for_slot(const struct r3v_descriptor_set_layout *layout,
                      uint32_t slot, uint32_t *array_index)
{
   for (uint32_t i = 0; i < layout->binding_count; i++) {
      const struct r3v_dsl_binding *b = &layout->bindings[i];
      if (slot >= b->offset && slot < b->offset + b->count) {
         *array_index = slot - b->offset;
         return b;
      }
   }
   return NULL;
}

static uint32_t
descriptor_update_span(const struct r3v_descriptor_set_layout *layout,
                       const struct r3v_dsl_binding *binding,
                       uint32_t array_element,
                       VkDescriptorType descriptor_type,
                       uint32_t descriptor_count)
{
   if (binding->type != descriptor_type ||
       array_element >= binding->count ||
       descriptor_count == 0)
      return 0;

   uint32_t span = binding->count - array_element;
   if (span >= descriptor_count)
      return descriptor_count;

   const uint32_t binding_index =
      (uint32_t)(binding - layout->bindings);
   uint32_t next_binding_number = binding->binding;

   for (uint32_t i = binding_index + 1;
        i < layout->binding_count && span < descriptor_count; i++) {
      if (next_binding_number == UINT32_MAX)
         break;
      next_binding_number++;

      const struct r3v_dsl_binding *next = &layout->bindings[i];
      if (next->binding != next_binding_number ||
          next->type != descriptor_type)
         break;

      if (next->count >= descriptor_count - span)
         return descriptor_count;
      span += next->count;
   }

   return span;
}

VkSampler
r3v_descriptor_slot_sampler(const struct r3v_descriptor_set_layout *layout,
                            uint32_t slot, VkSampler written_sampler)
{
   uint32_t array_index = 0;
   const struct r3v_dsl_binding *b =
      find_binding_for_slot(layout, slot, &array_index);

   if (b && b->immutable_samplers && array_index < b->count)
      return b->immutable_samplers[array_index];

   return written_sampler;
}

void
r3v_initialize_immutable_sampler_descriptors(struct r3v_descriptor_set *set)
{
   const struct r3v_descriptor_set_layout *layout = set->layout;

   for (uint32_t b = 0; b < layout->binding_count; b++) {
      const struct r3v_dsl_binding *binding = &layout->bindings[b];
      if (!r3v_descriptor_type_supports_immutable_samplers(binding->type) ||
          !binding->immutable_samplers)
         continue;

      for (uint32_t d = 0; d < binding->count; d++) {
         struct r3v_descriptor *slot =
            &set->descriptors[binding->offset + d];
         slot->type = binding->type;
         slot->img.sampler = binding->immutable_samplers[d];
      }
   }
}

void
r3v_copy_descriptors_preserving_immutable_samplers(
   struct r3v_descriptor_set *dst_set, uint32_t dst_base,
   const struct r3v_descriptor_set *src_set, uint32_t src_base,
   uint32_t count)
{
   for (uint32_t d = 0; d < count; d++) {
      struct r3v_descriptor *dst = &dst_set->descriptors[dst_base + d];
      *dst = src_set->descriptors[src_base + d];
      /* Only sampler-typed slots can carry immutable samplers; skip the
       * layout walk for other descriptor types. */
      if (!r3v_descriptor_type_supports_immutable_samplers(dst->type))
         continue;
      uint32_t array_index = 0;
      const struct r3v_dsl_binding *binding =
         find_binding_for_slot(dst_set->layout, dst_base + d, &array_index);
      if (binding && binding->immutable_samplers &&
          array_index < binding->count) {
         dst->type = binding->type;
         dst->img.sampler = binding->immutable_samplers[array_index];
      }
   }
}

void
r3v_UpdateDescriptorSets(VkDevice _device,
                            uint32_t descriptorWriteCount,
                            const VkWriteDescriptorSet *pDescriptorWrites,
                            uint32_t descriptorCopyCount,
                            const VkCopyDescriptorSet *pDescriptorCopies)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   (void)device;

   for (uint32_t w = 0; w < descriptorWriteCount; w++) {
      const VkWriteDescriptorSet *write = &pDescriptorWrites[w];
      VK_FROM_HANDLE(r3v_descriptor_set, set, write->dstSet);
      const struct r3v_dsl_binding *b = find_binding(set->layout,
                                                        write->dstBinding);
      if (!b)
         continue;
      /* Vulkan descriptor writes may spill only through consecutive bindings
       * of the same descriptor type.  Keep the linear descriptors[] walk inside
       * that allowed run so mixed-type layouts cannot be overwritten by a write
       * that started in an earlier binding. */
      uint32_t base = b->offset + write->dstArrayElement;
      uint32_t span = descriptor_update_span(set->layout, b,
                                             write->dstArrayElement,
                                             write->descriptorType,
                                             write->descriptorCount);
      if (span == 0)
         continue;
      for (uint32_t d = 0; d < span; d++) {
         struct r3v_descriptor *slot = &set->descriptors[base + d];
         /* Stamp the descriptor type after the matching payload is accepted. */
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
             * the buffer's allocated pages; the compute-as-raster
             * orchestrator's wrap helper would then read garbage at the
             * upper GART range.  R300 PFS has no shader-emitted flat
             * address, so an out-of-bounds descriptor offset cannot be
             * clamped in NIR; the defense surface lives at descriptor
             * binding, not in NIR.
             *
             * VK_WHOLE_SIZE (~0ull) means "from offset to end of
             * buffer", which is by construction in-bounds; the Mesa
             * runtime does not resolve it earlier than this site, so
             * accept it.  An out-of-bounds binding is silently
             * skipped (the previous slot's value, NULL after
             * AllocateDescriptorSets's zalloc, persists). */
            VK_FROM_HANDLE(r3v_buffer, buf, bi->buffer);
            bool in_bounds = (buf == NULL); /* NULL-buffer = unbind, fine */
            if (buf) {
               in_bounds = (bi->offset <= buf->size) &&
                           (bi->range == VK_WHOLE_SIZE ||
                            (bi->range <= buf->size - bi->offset));
            }
            if (!in_bounds) {
               /* Preserve the previous accepted descriptor. */
               break;
            }
            slot->buf.buffer = bi->buffer;
            slot->buf.offset = bi->offset;
            slot->buf.range  = bi->range;
            slot->type = write->descriptorType;
            break;
         }
         case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
         case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
            const VkDescriptorImageInfo *ii = &write->pImageInfo[d];
            slot->img.image_view = ii->imageView;
            slot->img.layout     = ii->imageLayout;
            slot->img.sampler    = VK_NULL_HANDLE;
            slot->type = write->descriptorType;
            break;
         }
         case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
            const VkDescriptorImageInfo *ii = &write->pImageInfo[d];
            slot->img.image_view = ii->imageView;
            slot->img.layout     = ii->imageLayout;
            slot->img.sampler    =
               r3v_descriptor_slot_sampler(set->layout, base + d, ii->sampler);
            slot->type = write->descriptorType;
            break;
         }
         case VK_DESCRIPTOR_TYPE_SAMPLER: {
            const VkDescriptorImageInfo *ii = &write->pImageInfo[d];
            slot->img.sampler =
               r3v_descriptor_slot_sampler(set->layout, base + d, ii->sampler);
            slot->type = write->descriptorType;
            break;
         }
         default:
            /* Texel buffers and inline-uniform-block are not yet wired into
             * the gallium-binding stage; the slot's recorded type makes the
             * unsupported case visible to the dispatch replay so it can
             * reject early rather than dispatch with stale data. */
            slot->type = write->descriptorType;
            break;
         }
      }
   }

   for (uint32_t c = 0; c < descriptorCopyCount; c++) {
      const VkCopyDescriptorSet *cp = &pDescriptorCopies[c];
      VK_FROM_HANDLE(r3v_descriptor_set, src_set, cp->srcSet);
      VK_FROM_HANDLE(r3v_descriptor_set, dst_set, cp->dstSet);
      const struct r3v_dsl_binding *src_b =
         find_binding(src_set->layout, cp->srcBinding);
      const struct r3v_dsl_binding *dst_b =
         find_binding(dst_set->layout, cp->dstBinding);
      if (!src_b || !dst_b)
         continue;
      if (src_b->type != dst_b->type)
         continue;

      uint32_t src_base = src_b->offset + cp->srcArrayElement;
      uint32_t dst_base = dst_b->offset + cp->dstArrayElement;
      uint32_t src_span = descriptor_update_span(src_set->layout, src_b,
                                                 cp->srcArrayElement,
                                                 src_b->type,
                                                 cp->descriptorCount);
      uint32_t dst_span = descriptor_update_span(dst_set->layout, dst_b,
                                                 cp->dstArrayElement,
                                                 dst_b->type,
                                                 cp->descriptorCount);
      uint32_t span = src_span < dst_span ? src_span : dst_span;
      if (span == 0)
         continue;
      r3v_copy_descriptors_preserving_immutable_samplers(
         dst_set, dst_base, src_set, src_base, span);
   }
}

/* VK_KHR_descriptor_update_template.  vk_common_CreateDescriptorUpdateTemplate
 * builds the vk_descriptor_update_template -- the entry array carrying each
 * entry's binding, array element, count, type, and a source offset/stride into
 * pData -- but the update writes driver descriptor state, so the runtime leaves
 * it to the driver.  Each entry names array_count descriptors laid out in pData
 * at offset + j*stride; applying each as a single-descriptor
 * VkWriteDescriptorSet through r3v_UpdateDescriptorSets reuses that path's
 * binding lookup, linear-span capping, and per-type bounds checks.  Reading
 * each descriptor at its own pData address makes this correct for an arbitrary
 * stride, which a tightly packed pImageInfo/pBufferInfo array could not honour. */
void
r3v_UpdateDescriptorSetWithTemplate(VkDevice _device,
                                       VkDescriptorSet descriptorSet,
                                       VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                       const void *pData)
{
   VK_FROM_HANDLE(vk_descriptor_update_template, templ, descriptorUpdateTemplate);

   for (uint32_t e = 0; e < templ->entry_count; e++) {
      const struct vk_descriptor_template_entry *entry = &templ->entries[e];

      for (uint32_t j = 0; j < entry->array_count; j++) {
         const void *src = (const uint8_t *)pData + entry->offset +
                           (size_t)j * entry->stride;

         VkWriteDescriptorSet write = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = descriptorSet,
            .dstBinding      = entry->binding,
            .dstArrayElement = entry->array_element + j,
            .descriptorCount = 1,
            .descriptorType  = entry->type,
         };

         switch (entry->type) {
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            write.pBufferInfo = src;
            break;
         case VK_DESCRIPTOR_TYPE_SAMPLER:
         case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
         case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            write.pImageInfo = src;
            break;
         case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
         case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            write.pTexelBufferView = src;
            break;
         default:
            /* An unsupported descriptor type (e.g. inline uniform block) records
             * slot->type and reads no data in r3v_UpdateDescriptorSets'
             * default case, so leave the info pointers NULL. */
            break;
         }

         r3v_UpdateDescriptorSets(_device, 1, &write, 0, NULL);
      }
   }
}
