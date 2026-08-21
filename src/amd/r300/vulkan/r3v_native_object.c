/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V core Vulkan 1.0 object surface: the commands whose objects the
 * native implementation constructs no route for.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"

#include "vk_log.h"

#include <string.h>

/* vkGetDeviceProcAddr reports a command absent only when the requested core
 * version does not contain it, so every core 1.0 device command carries a
 * definition even where the object behind it has no native route.  Three
 * shapes cover this file.  A creation command clears its output handle and
 * refuses, so an application never receives a handle that names nothing.  A
 * destroy or free command accepts the null handle the specification makes a
 * no-op and touches no other, since no non-null handle of that type can
 * exist.  A command that reads or writes an object of an unconstructable
 * type refuses on the same grounds as its creation command.
 *
 * VkDeviceMemory is the exception: r3v_AllocateMemory builds a real GEM
 * buffer object, so the mapped-range commands below validate and execute
 * rather than refuse.
 *
 * The definitions follow those shapes in order: creation, destruction,
 * access, then the device-memory commands.
 */

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateBufferView(VkDevice _device,
                     const VkBufferViewCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkBufferView *pView)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   *pView = VK_NULL_HANDLE;
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateSampler(VkDevice _device, const VkSamplerCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkSampler *pSampler)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   *pSampler = VK_NULL_HANDLE;
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateEvent(VkDevice _device, const VkEventCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator, VkEvent *pEvent)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   *pEvent = VK_NULL_HANDLE;
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateQueryPool(VkDevice _device,
                    const VkQueryPoolCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkQueryPool *pQueryPool)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   *pQueryPool = VK_NULL_HANDLE;
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

/* Destruction of the null handle is a specified no-op, and the creation
 * commands above hand back no other handle, so each of these performs the
 * no-op for every input a valid program can present.
 */

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyBufferView(VkDevice _device, VkBufferView bufferView,
                      const VkAllocationCallbacks *pAllocator)
{
}

VKAPI_ATTR void VKAPI_CALL
r3v_DestroySampler(VkDevice _device, VkSampler sampler,
                   const VkAllocationCallbacks *pAllocator)
{
}

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyEvent(VkDevice _device, VkEvent event,
                 const VkAllocationCallbacks *pAllocator)
{
}

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyQueryPool(VkDevice _device, VkQueryPool queryPool,
                     const VkAllocationCallbacks *pAllocator)
{
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_GetEventStatus(VkDevice _device, VkEvent event)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_SetEvent(VkDevice _device, VkEvent event)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_ResetEvent(VkDevice _device, VkEvent event)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_GetQueryPoolResults(VkDevice _device, VkQueryPool queryPool,
                        uint32_t firstQuery, uint32_t queryCount,
                        size_t dataSize, void *pData, VkDeviceSize stride,
                        VkQueryResultFlags flags)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

/* The committed size is defined for lazily-allocated memory.  Every native
 * memory type commits its whole allocation at r3v_AllocateMemory and none
 * carries VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, so no memory object this
 * command accepts exists and the report is zero.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_GetDeviceMemoryCommitment(VkDevice _device, VkDeviceMemory memory,
                              VkDeviceSize *pCommittedMemoryInBytes)
{
   *pCommittedMemoryInBytes = 0;
}

/* The mapped-range commands take real memory objects, so they validate the
 * ranges the caller names rather than refusing.  VK_WHOLE_SIZE runs from the
 * offset to the end of the allocation; every other range must close inside
 * it, and the sum is computed against the remaining size so a caller-supplied
 * size cannot wrap it.
 */
static VkResult
r3v_native_validate_mapped_ranges(struct r3v_native_device *device,
                                  uint32_t rangeCount,
                                  const VkMappedMemoryRange *pRanges)
{
   for (uint32_t i = 0; i < rangeCount; i++) {
      VK_FROM_HANDLE(r3v_native_memory, memory, pRanges[i].memory);
      if (memory == NULL)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

      const VkDeviceSize size = memory->bo.size;
      if (pRanges[i].offset > size)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      if (pRanges[i].size != VK_WHOLE_SIZE &&
          pRanges[i].size > size - pRanges[i].offset)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   }
   return VK_SUCCESS;
}

/* Type 0 is the one host-visible memory type, and it reports
 * VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, which makes host writes visible to
 * the device and device writes visible to the host without either call.  The
 * native driver holds that promise itself: the unsnooped GART mapping is
 * published and invalidated by radeon_drm_vk_bo_cache_sync around the
 * synchronous submission, the only window in which the device reads or
 * writes a mapped range.  r3v_native_memory_properties_fill grants type 1
 * VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT alone (rg --fixed-strings
 * r3v_native_memory_properties_fill src/amd/r300/vulkan/), and vkMapMemory
 * admits a host-visible type alone, so every mapped range names type 0.
 * These commands therefore carry range validation alone.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_FlushMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                            const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   return r3v_native_validate_mapped_ranges(device, memoryRangeCount,
                                            pMemoryRanges);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_InvalidateMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                                 const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   return r3v_native_validate_mapped_ranges(device, memoryRangeCount,
                                            pMemoryRanges);
}
