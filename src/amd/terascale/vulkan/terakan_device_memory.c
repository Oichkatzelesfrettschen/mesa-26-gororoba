/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terakan_device_memory.h"

#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_dmabuf_carrier.h"
#include "terakan_entrypoints.h"
#include "terakan_format.h"
#include "terakan_image.h"
#include "terakan_physical_device.h"

#include "util/cache_ops.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_util.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline VkMemoryPropertyFlags
terakan_device_memory_flags(struct terakan_device const * const device,
                            struct terakan_device_memory const * const device_memory)
{
   return terakan_device_physical_device(device)
      ->memory_properties.memoryTypes[device_memory->vk.memory_type_index]
      .propertyFlags;
}

static void
terakan_device_memory_flush_or_invalidate_mapped_range(
   struct terakan_device const * const device,
   struct terakan_device_memory * const device_memory, VkDeviceSize const offset,
   VkDeviceSize const size, bool const invalidate)
{
   if (size == 0 || device_memory->bo->mapping == NULL || !util_has_cache_ops()) {
      return;
   }

   /* Host-cached mappings may require explicit CPU cache maintenance before GPU-visible use. */
   if (!(terakan_device_memory_flags(device, device_memory) &
         VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
      return;
   }

   VkDeviceSize flush_size = size;
   if (flush_size > (VkDeviceSize)SIZE_MAX) {
      flush_size = (VkDeviceSize)SIZE_MAX;
   }
   void * const start = (char *)device_memory->bo->mapping + offset;
   if (invalidate) {
      util_flush_inval_range(start, (size_t)flush_size);
   } else {
      util_flush_range(start, (size_t)flush_size);
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_GetMemoryFdPropertiesKHR(VkDevice const deviceHandle,
                                 VkExternalMemoryHandleTypeFlagBits const handleType, int const fd,
                                 VkMemoryFdPropertiesKHR * const pMemoryFdProperties)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);
   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);

   if (!(terakan_physical_device_supported_external_memory_types(physical_device) & handleType)) {
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   if (handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   bool vram_preferred;
   if (!device->winsys_fn->bo->get_fd_vram_preference(device, fd, &vram_preferred)) {
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   pMemoryFdProperties->memoryTypeBits =
      ((uint32_t)1 << physical_device->memory_properties.memoryTypeCount) - 1;
   /* The domain can be changed from the initial one in command submissions, so technically any
    * memory type should be fine for importing, but prefer not to move BOs between VRAM and GTT.
    */
   if (physical_device->chip_info.has_dedicated_vram) {
      VkMemoryPropertyFlags const device_local_flag_preferred =
         vram_preferred ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0;
      for (uint32_t memory_type_index = 0;
           memory_type_index < physical_device->memory_properties.memoryTypeCount;
           ++memory_type_index) {
         if ((physical_device->memory_properties.memoryTypes[memory_type_index].propertyFlags &
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != device_local_flag_preferred) {
            pMemoryFdProperties->memoryTypeBits &= ~((uint32_t)1 << memory_type_index);
         }
      }
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_GetMemoryFdKHR(VkDevice const deviceHandle, VkMemoryGetFdInfoKHR const * const pGetFdInfo,
                       int * const pFd)
{
   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_bo * const bo = terakan_device_memory_from_handle(pGetFdInfo->memory)->bo;

   int const fd = device->winsys_fn->bo->export_fd(
      bo, pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
   if (fd < 0) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   *pFd = fd;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_FlushMappedMemoryRanges(VkDevice const deviceHandle,
                                uint32_t const memoryRangeCount,
                                VkMappedMemoryRange const * const pMemoryRanges)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   for (uint32_t range_index = 0; range_index < memoryRangeCount; ++range_index) {
      VkMappedMemoryRange const * const range = &pMemoryRanges[range_index];
      struct terakan_device_memory * const device_memory =
         terakan_device_memory_from_handle(range->memory);
      if (device_memory == NULL) {
         continue;
      }
      VkDeviceSize const size =
         range->size == VK_WHOLE_SIZE ? device_memory->vk.size - range->offset : range->size;
      terakan_device_memory_flush_or_invalidate_mapped_range(device, device_memory,
                                                             range->offset, size, false);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_InvalidateMappedMemoryRanges(VkDevice const deviceHandle,
                                     uint32_t const memoryRangeCount,
                                     VkMappedMemoryRange const * const pMemoryRanges)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   for (uint32_t range_index = 0; range_index < memoryRangeCount; ++range_index) {
      VkMappedMemoryRange const * const range = &pMemoryRanges[range_index];
      struct terakan_device_memory * const device_memory =
         terakan_device_memory_from_handle(range->memory);
      if (device_memory == NULL) {
         continue;
      }
      VkDeviceSize const size =
         range->size == VK_WHOLE_SIZE ? device_memory->vk.size - range->offset : range->size;
      terakan_device_memory_flush_or_invalidate_mapped_range(device, device_memory,
                                                             range->offset, size, true);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_UnmapMemory2KHR(VkDevice const deviceHandle,
                        VkMemoryUnmapInfoKHR const * const pMemoryUnmapInfo)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);
   struct terakan_device_memory * const device_memory =
      terakan_device_memory_from_handle(pMemoryUnmapInfo->memory);

   terakan_device_memory_flush_or_invalidate_mapped_range(device, device_memory, 0,
                                                          device_memory->vk.size, false);
   terakan_bo_unmap(device_memory->bo);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_MapMemory2KHR(VkDevice const deviceHandle, VkMemoryMapInfoKHR const * const pMemoryMapInfo,
                      void ** const ppData)
{
   struct terakan_device_memory * const device_memory =
      terakan_device_memory_from_handle(pMemoryMapInfo->memory);

   void * const mapping = terakan_bo_map(device_memory->bo);
   if (mapping == NULL) {
      return vk_error(terakan_device_from_handle(deviceHandle), VK_ERROR_MEMORY_MAP_FAILED);
   }

   *ppData = (char *)mapping + pMemoryMapInfo->offset;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
terakan_FreeMemory(VkDevice const deviceHandle, VkDeviceMemory const deviceMemory,
                   VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_device_memory * const device_memory =
      terakan_device_memory_from_handle(deviceMemory);

   if (device_memory == NULL) {
      return;
   }

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   /* The carrier (if any) is a non-owning attachment to the BO.
    * Destroy it BEFORE freeing the BO so the carrier's BO-pointer
    * clear (atomic release-store of NULL into bo->carrier) sees a
    * live BO; the BO is freed exactly once, here. */
   if (device_memory->carrier != NULL) {
      terakan_dmabuf_carrier_destroy(device, device_memory->carrier);
      device_memory->carrier = NULL;
   }

   terakan_bo_free(device_memory->bo, pAllocator);

   vk_device_memory_destroy(&device->vk, pAllocator, &device_memory->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_AllocateMemory(VkDevice const deviceHandle,
                       VkMemoryAllocateInfo const * const pAllocateInfo,
                       VkAllocationCallbacks const * pAllocator, VkDeviceMemory * const pMemory)
{
   VkResult result;

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_device_memory * const device_memory = vk_device_memory_create(
      &device->vk, pAllocateInfo, pAllocator, sizeof(struct terakan_device_memory));
   if (device_memory == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);

   /* Storage and uniform buffers in Vulkan require only the offset to be aligned, not the range,
    * but the entire range must be visible to the shader anyway. For the purpose of bounds checking,
    * the ranges are rounded up to their respective access size alignments in
    * VkPhysicalDeviceRobustness2PropertiesEXT, so make sure the BO is never smaller than the size
    * rounded up, and the validation in the kernel driver doesn't consider the binding out of
    * bounds.
    */
   VkDeviceSize bo_size =
      ALIGN_POT(device_memory->vk.size, (VkDeviceSize)TERAKAN_KCACHE_HW_LINE_BYTES);
   if (physical_device->submission_info_gfx.buffer_uav_validated_as_image) {
      /* Make it possible to create buffer UAVs at any location in the BO as long as the BO-relative
       * base address is specified at the granularity of the minimum LINEAR_ALIGNED pitch for the
       * element size, with offsetting done in shaders.
       * The maximum value `terakan_format_pitch_alignment_linear_surfels` may return is 64 elements
       * of 16 bytes each - 1024 bytes, which is larger than the maximum possible pipe interleave
       * (512 bytes).
       */
      bo_size = ALIGN_POT(bo_size, (VkDeviceSize)(sizeof(uint32_t) * 4 * 64));
   }

   VkMemoryPropertyFlags const memory_property_flags =
      physical_device->memory_properties.memoryTypes[device_memory->vk.memory_type_index]
         .propertyFlags;

   /* Implementations must validate external handle usage requirements. */
   VkExternalMemoryHandleTypeFlags const import_handle_type = device_memory->vk.import_handle_type;
   if (import_handle_type) {
      if (!(terakan_physical_device_supported_external_memory_types(physical_device) &
            import_handle_type)) {
         result = vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto fail_device_memory;
      }

      switch (device_memory->vk.import_handle_type) {
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT: {
         VkImportMemoryFdInfoKHR const * const import_fd_info =
            vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);
         if (import_fd_info == NULL) {
            result = vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
            goto fail_device_memory;
         }
         result = device->winsys_fn->bo->import_fd(
            device, import_fd_info->fd, bo_size,
            (memory_property_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0, pAllocator,
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &device_memory->bo);
         if (result != VK_SUCCESS) {
            result = vk_error(device, result);
            goto fail_device_memory;
         }

         /* For dma-buf imports under the carrier env-gate, attach a
          * Palm external-sync carrier to the imported BO.  Buffer is
          * the default Phase 0 carrier classification per the
          * cache-domain evidence matrix; non-allowed domains return
          * VK_ERROR_FEATURE_NOT_PRESENT from _attach() and log the
          * policy reason, but the BO import has already succeeded so
          * the allocation continues without a carrier. */
         if (device_memory->vk.import_handle_type ==
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
             terakan_dmabuf_carrier_enabled()) {
            struct terakan_dmabuf_carrier_desc const carrier_desc = {
               .dmabuf_fd  = import_fd_info->fd,
               .domain     = TERAKAN_CARRIER_DOMAIN_BUFFER,
               .size_bytes = bo_size,
            };
            VkResult const attach_r = terakan_dmabuf_carrier_attach(
               device, &carrier_desc, device_memory->bo,
               &device_memory->carrier);
            if (attach_r != VK_SUCCESS &&
                attach_r != VK_ERROR_FEATURE_NOT_PRESENT) {
               mesa_logw("terakan: carrier attach failed unexpectedly "
                         "(VkResult=%d); allocation continues without "
                         "carrier", (int)attach_r);
               device_memory->carrier = NULL;
            }
         }
      } break;

      default:
         assert(!"Unsupported handle type exposed by "
                 "terakan_physical_device_supported_external_memory_types");
         result = vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto fail_device_memory;
      }
   } else {
      result = device->winsys_fn->bo->allocate_device_memory(
         device, bo_size, physical_device->buffer_image_bo_alignment, memory_property_flags,
         device_memory->vk.export_handle_types, pAllocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
         &device_memory->bo);
      if (result != VK_SUCCESS) {
         result = vk_error(device, result);
         goto fail_device_memory;
      }

      /* For dma-buf-exportable allocations under the carrier env-gate,
       * attach a Palm external-sync carrier to the freshly allocated
       * BO.  This is the symmetric counterpart to the import-side
       * attach above: a producer that allocates with
       * VkExportMemoryAllocateInfo.handleTypes containing
       * DMA_BUF_BIT_EXT will get a carrier on the same BO that the
       * eventual vkGetMemoryFdKHR exports.  Buffer is the default
       * Phase 0 classification per the cache-domain evidence matrix. */
      if ((device_memory->vk.export_handle_types &
              VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) != 0 &&
          terakan_dmabuf_carrier_enabled()) {
         struct terakan_dmabuf_carrier_desc const carrier_desc = {
            .dmabuf_fd  = -1, /* fd is populated lazily on vkGetMemoryFdKHR */
            .domain     = TERAKAN_CARRIER_DOMAIN_BUFFER,
            .size_bytes = bo_size,
         };
         VkResult const attach_r = terakan_dmabuf_carrier_attach(
            device, &carrier_desc, device_memory->bo,
            &device_memory->carrier);
         if (attach_r != VK_SUCCESS &&
             attach_r != VK_ERROR_FEATURE_NOT_PRESENT) {
            mesa_logw("terakan: carrier attach failed unexpectedly "
                      "on export allocation (VkResult=%d); allocation "
                      "continues without carrier", (int)attach_r);
            device_memory->carrier = NULL;
         }
      }

      VkMemoryDedicatedAllocateInfo const * const dedicated_info =
         vk_find_struct_const(pAllocateInfo->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
      if (dedicated_info != NULL) {
         struct terakan_image const * const dedicated_image =
            terakan_image_from_handle(dedicated_info->image);
         if (dedicated_image != NULL) {
            struct terakan_image_surface_aspect const * const dedicated_image_main_aspect =
               &dedicated_image->surface.aspects[0];
            struct terakan_bo_tiling const bo_tiling = {
               .pitch_bytes =
                  (dedicated_image_main_aspect->bytes_per_block /
                   terakan_format_surfels_per_block(dedicated_image_main_aspect->bytes_per_block)) *
                  (uint32_t)dedicated_image_main_aspect->levels[0].aligned_extent_surfels[0],
               .array_mode = dedicated_image_main_aspect->levels[0].array_mode,
               /* If the image is stencil-only, pass the stencil tile split as both main aspect tile
                * split and stencil tile split so it doesn't matter which field the receiving end
                * will take it from.
                */
               .attrib_tile_split = dedicated_image_main_aspect->tiling.attrib_tile_split,
               .attrib_stencil_tile_split =
                  vk_format_has_stencil(dedicated_image->vk.format)
                     ? dedicated_image->surface
                          .aspects[terakan_format_aspect_index(
                             dedicated_image->format_info.aspect_map,
                             VK_IMAGE_ASPECT_STENCIL_BIT, 0)]
                          .tiling.attrib_tile_split
                     : 0,
               .attrib_bank_width = dedicated_image_main_aspect->tiling.attrib_bank_width,
               .attrib_bank_height = dedicated_image_main_aspect->tiling.attrib_bank_height,
               .attrib_macro_tile_aspect =
                  dedicated_image_main_aspect->tiling.attrib_macro_tile_aspect,
            };
            if (!device->winsys_fn->bo->set_tiling(device_memory->bo, &bo_tiling)) {
               result = vk_error(device, VK_ERROR_UNKNOWN);
               goto fail_bo;
            }
         }
      }
   }

   *pMemory = terakan_device_memory_to_handle(device_memory);
   return VK_SUCCESS;

fail_bo:
   if (device_memory->carrier != NULL) {
      terakan_dmabuf_carrier_destroy(device, device_memory->carrier);
      device_memory->carrier = NULL;
   }
   terakan_bo_free(device_memory->bo, pAllocator);
fail_device_memory:
   vk_device_memory_destroy(&device->vk, pAllocator, &device_memory->vk);
   return result;
}
