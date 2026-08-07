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
r3v_CreateImage(VkDevice _device, const VkImageCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   *pImage = VK_NULL_HANDLE;
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateImageView(VkDevice _device,
                    const VkImageViewCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkImageView *pView)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   *pView = VK_NULL_HANDLE;
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

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

/* The descriptor types form one route: a set layout feeds a pipeline layout,
 * a pool allocates sets, and a set binds into a pipeline.  Recording refuses
 * every bind, so the route terminates at its first link and each member
 * refuses rather than handing back a descriptor object with no consumer.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateDescriptorSetLayout(VkDevice _device,
                              const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkDescriptorSetLayout *pSetLayout)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   *pSetLayout = VK_NULL_HANDLE;
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateDescriptorPool(VkDevice _device,
                         const VkDescriptorPoolCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkDescriptorPool *pDescriptorPool)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   *pDescriptorPool = VK_NULL_HANDLE;
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_AllocateDescriptorSets(VkDevice _device,
                           const VkDescriptorSetAllocateInfo *pAllocateInfo,
                           VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++)
      pDescriptorSets[i] = VK_NULL_HANDLE;
   return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
}

/* Destruction of the null handle is a specified no-op, and the creation
 * commands above hand back no other handle, so each of these performs the
 * no-op for every input a valid program can present.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_DestroyImage(VkDevice _device, VkImage image,
                 const VkAllocationCallbacks *pAllocator)
{
}

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyImageView(VkDevice _device, VkImageView imageView,
                     const VkAllocationCallbacks *pAllocator)
{
}

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

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyDescriptorPool(VkDevice _device, VkDescriptorPool descriptorPool,
                          const VkAllocationCallbacks *pAllocator)
{
}

/* Freeing zero sets is the one form of this command a valid program reaches,
 * and it succeeds.  vkFreeDescriptorSets permits VK_ERROR_UNKNOWN and
 * VK_ERROR_VALIDATION_FAILED alone, so a refusal here would report an
 * implementation failure for a call that asks nothing.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_FreeDescriptorSets(VkDevice _device, VkDescriptorPool descriptorPool,
                       uint32_t descriptorSetCount,
                       const VkDescriptorSet *pDescriptorSets)
{
   return VK_SUCCESS;
}

/* Resetting a pool recycles the sets it allocated.  No pool exists, so the
 * recycling has no subject and the command succeeds having done it.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_ResetDescriptorPool(VkDevice _device, VkDescriptorPool descriptorPool,
                        VkDescriptorPoolResetFlags flags)
{
   return VK_SUCCESS;
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

/* Writing zero descriptors is the one form a valid program reaches, and the
 * command returns void, so it performs that write and returns.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_UpdateDescriptorSets(VkDevice _device, uint32_t descriptorWriteCount,
                         const VkWriteDescriptorSet *pDescriptorWrites,
                         uint32_t descriptorCopyCount,
                         const VkCopyDescriptorSet *pDescriptorCopies)
{
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

/* Both memory types report VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, which makes
 * host writes visible to the device and device writes visible to the host
 * without either call.  The native driver holds that promise itself: the
 * unsnooped GART mapping is published and invalidated by
 * radeon_drm_vk_bo_cache_sync around the synchronous submission, the only
 * window in which the device reads or writes a mapped range.  These commands
 * therefore validate their ranges and add no maintenance of their own.
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
