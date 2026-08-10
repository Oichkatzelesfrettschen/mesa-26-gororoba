/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V device memory and buffers over GEM buffer objects.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_util.h"

#include <radeon_drm.h>

/* Memory type 0 is the GTT host-visible pool and type 1 the device-local
 * pool, matching the physical-device table.  RS48x is UMA, so the
 * device-local pool also admits GTT placement.
 */
static void
r3v_native_memory_type_policy(uint32_t type_index, uint32_t *domains,
                              uint32_t *flags)
{
   if (type_index == 0) {
      *domains = RADEON_GEM_DOMAIN_GTT;
      *flags = RADEON_GEM_CPU_ACCESS;
   } else {
      *domains = RADEON_GEM_DOMAIN_VRAM | RADEON_GEM_DOMAIN_GTT;
      *flags = RADEON_GEM_NO_CPU_ACCESS;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_AllocateMemory(VkDevice _device,
                   const VkMemoryAllocateInfo *pAllocateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkDeviceMemory *pMem)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);

   struct r3v_native_memory *memory =
      vk_device_memory_create(&device->vk, pAllocateInfo, pAllocator,
                              sizeof(*memory));
   if (memory == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   uint32_t domains, flags;
   r3v_native_memory_type_policy(pAllocateInfo->memoryTypeIndex, &domains,
                                 &flags);

   if (radeon_drm_vk_bo_create(&device->drm, pAllocateInfo->allocationSize,
                               4096, domains, flags, false,
                               &memory->bo) != 0) {
      vk_device_memory_destroy(&device->vk, pAllocator, &memory->vk);
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   memory->map = NULL;

   *pMem = r3v_native_memory_to_handle(memory);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
r3v_FreeMemory(VkDevice _device, VkDeviceMemory _memory,
               const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_memory, memory, _memory);

   if (memory == NULL)
      return;

   if (memory->map != NULL)
      radeon_drm_vk_bo_unmap(&device->drm, &memory->bo, memory->map);
   radeon_drm_vk_bo_free(&device->drm, &memory->bo);
   vk_device_memory_destroy(&device->vk, pAllocator, &memory->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_MapMemory(VkDevice _device, VkDeviceMemory _memory, VkDeviceSize offset,
              VkDeviceSize size, VkMemoryMapFlags flags, void **ppData)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_memory, memory, _memory);

   if (memory->map == NULL) {
      if (radeon_drm_vk_bo_map(&device->drm, &memory->bo,
                               &memory->map) != 0) {
         return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
      }
      /* A fresh mapping may alias cache lines from an earlier map window
       * while the GPU wrote the pages past the cache; drop them before
       * the host reads through the new mapping.
       */
      radeon_drm_vk_bo_cache_sync(&device->drm, memory->map,
                                  memory->bo.size);
   }
   *ppData = (char *)memory->map + offset;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
r3v_UnmapMemory(VkDevice _device, VkDeviceMemory _memory)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_memory, memory, _memory);

   if (memory == NULL || memory->map == NULL)
      return;
   /* Host writes publish while the address is still live; munmap leaves
    * dirty lines behind and is not a publication mechanism.
    */
   radeon_drm_vk_bo_cache_sync(&device->drm, memory->map, memory->bo.size);
   radeon_drm_vk_bo_unmap(&device->drm, &memory->bo, memory->map);
   memory->map = NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateBuffer(VkDevice _device, const VkBufferCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);

   struct r3v_native_buffer *buffer =
      vk_buffer_create(&device->vk, pCreateInfo, pAllocator, sizeof(*buffer));
   if (buffer == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   buffer->memory = NULL;
   buffer->offset = 0;

   *pBuffer = r3v_native_buffer_to_handle(buffer);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyBuffer(VkDevice _device, VkBuffer _buffer,
                  const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_buffer, buffer, _buffer);

   if (buffer == NULL)
      return;
   vk_buffer_destroy(&device->vk, pAllocator, &buffer->vk);
}

/* Native buffers bind slices of explicitly allocated BOs, so their memory
 * requirements permit shared allocations and do not request a dedicated BO.
 */
static void
r3v_native_fill_buffer_dedicated_requirements(
   VkMemoryRequirements2 *pMemoryRequirements)
{
   VkMemoryDedicatedRequirements *dedicated =
      vk_find_struct(pMemoryRequirements->pNext, MEMORY_DEDICATED_REQUIREMENTS);
   if (dedicated != NULL) {
      dedicated->prefersDedicatedAllocation = VK_FALSE;
      dedicated->requiresDedicatedAllocation = VK_FALSE;
   }
}

VKAPI_ATTR void VKAPI_CALL
r3v_GetDeviceBufferMemoryRequirements(
   VkDevice _device, const VkDeviceBufferMemoryRequirements *pInfo,
   VkMemoryRequirements2 *pMemoryRequirements)
{
   (void)_device;
   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements){
      .size = pInfo->pCreateInfo->size,
      .alignment = 4096,
      .memoryTypeBits = 0x3,
   };
   r3v_native_fill_buffer_dedicated_requirements(pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL
r3v_GetBufferMemoryRequirements2(VkDevice _device,
                                 const VkBufferMemoryRequirementsInfo2 *pInfo,
                                 VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(r3v_native_buffer, buffer, pInfo->buffer);

   /* Type 0 alone: the draw's submission-time gather reads the bound
    * buffer through a CPU mapping, and type 1 allocates with
    * RADEON_GEM_NO_CPU_ACCESS, so an allocation the requirement admits
    * is always one the gather can map.
    */
   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements){
      .size = buffer->vk.size,
      .alignment = 4096,
      .memoryTypeBits = 0x1,
   };
   r3v_native_fill_buffer_dedicated_requirements(pMemoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_BindBufferMemory2(VkDevice _device, uint32_t bindInfoCount,
                      const VkBindBufferMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      VK_FROM_HANDLE(r3v_native_buffer, buffer, pBindInfos[i].buffer);
      VK_FROM_HANDLE(r3v_native_memory, memory, pBindInfos[i].memory);
      buffer->memory = memory;
      buffer->offset = pBindInfos[i].memoryOffset;
   }
   return VK_SUCCESS;
}
