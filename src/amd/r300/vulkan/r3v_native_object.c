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
 * Two exceptions construct: VkDeviceMemory, whose r3v_AllocateMemory
 * builds a real GEM buffer object so the mapped-range commands below
 * validate and execute, and VkSampler, whose creation depends on no
 * format or route so the object records state and the descriptor
 * surface refuses its use.
 *
 * The definitions follow those shapes in order: creation, destruction,
 * access, then the device-memory commands.
 */

/* A buffer view requires a format whose buffer features contain the
 * view's texel-buffer bit, and the format table advertises
 * VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT alone, so no format a valid
 * program can present admits a view and creation refuses with a
 * cleared handle.
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

/* Sampler creation binds to no format or route, so a valid program may
 * create one freely; the object records its creation state, and the
 * descriptor surface poisons every write that names a sampler, which
 * keeps the unsampled route fail-closed at the point of use.
 * samplerAnisotropy is not advertised, so an enabled anisotropy is
 * state no valid program presents and it refuses.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateSampler(VkDevice _device, const VkSamplerCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkSampler *pSampler)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);

   *pSampler = VK_NULL_HANDLE;
   if (pCreateInfo->flags != 0 ||
       pCreateInfo->anisotropyEnable != VK_FALSE)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   struct r3v_native_sampler *sampler =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*sampler),
                       VK_OBJECT_TYPE_SAMPLER);
   if (sampler == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   sampler->info = *pCreateInfo;
   sampler->info.pNext = NULL;
   *pSampler = r3v_native_sampler_to_handle(sampler);
   return VK_SUCCESS;
}

/* An event is one signaled bit the host reads and writes immediately;
 * the device timeline observes it at queue submission, which executes
 * the recorded sequence synchronously, so both sides see one ordered
 * history.  VK_EVENT_CREATE_DEVICE_ONLY would withhold the host side
 * and refuses.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateEvent(VkDevice _device, const VkEventCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator, VkEvent *pEvent)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);

   *pEvent = VK_NULL_HANDLE;
   if (pCreateInfo->flags != 0)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   struct r3v_native_event *event =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*event),
                       VK_OBJECT_TYPE_EVENT);
   if (event == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   *pEvent = r3v_native_event_to_handle(event);
   return VK_SUCCESS;
}

/* Occlusion pools construct as availability state: the recording
 * admits a query span containing no fragment-producing command, so
 * the counted value is exactly zero and the pool stores availability
 * alone.  Pipeline statistics and timestamps stay refused -- neither
 * feature is advertised and no counter route exists.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateQueryPool(VkDevice _device,
                    const VkQueryPoolCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkQueryPool *pQueryPool)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);

   *pQueryPool = VK_NULL_HANDLE;
   if (pCreateInfo->flags != 0 ||
       pCreateInfo->queryType != VK_QUERY_TYPE_OCCLUSION ||
       pCreateInfo->queryCount < 1 ||
       pCreateInfo->queryCount > R3V_NATIVE_QUERY_POOL_MAX_QUERIES)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   struct r3v_native_query_pool *pool =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*pool),
                       VK_OBJECT_TYPE_QUERY_POOL);
   if (pool == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   pool->query_count = pCreateInfo->queryCount;

   *pQueryPool = r3v_native_query_pool_to_handle(pool);
   return VK_SUCCESS;
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
r3v_DestroySampler(VkDevice _device, VkSampler _sampler,
                   const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_sampler, sampler, _sampler);

   if (sampler == NULL)
      return;
   vk_object_free(&device->vk, pAllocator, sampler);
}

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyEvent(VkDevice _device, VkEvent _event,
                 const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_event, event, _event);

   if (event == NULL)
      return;
   vk_object_free(&device->vk, pAllocator, event);
}

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyQueryPool(VkDevice _device, VkQueryPool _pool,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_query_pool, pool, _pool);

   if (pool == NULL)
      return;
   vk_object_free(&device->vk, pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_GetEventStatus(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_event, event, _event);

   if (event == NULL)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   return event->signaled ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_SetEvent(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_event, event, _event);

   if (event == NULL)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   event->signaled = true;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_ResetEvent(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_event, event, _event);

   if (event == NULL)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   event->signaled = false;
   return VK_SUCCESS;
}

/* Availability publishes at queue submission and vkQueueSubmit
 * executes the recorded span synchronously, so WAIT never blocks: the
 * state each query holds when this call runs is its final state.  An
 * available query writes its zero count in the caller's word size; an
 * unavailable one writes nothing and the call reports VK_NOT_READY
 * unless PARTIAL asked for the zero anyway, and WITH_AVAILABILITY
 * appends the state word either way.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_GetQueryPoolResults(VkDevice _device, VkQueryPool _pool,
                        uint32_t firstQuery, uint32_t queryCount,
                        size_t dataSize, void *pData, VkDeviceSize stride,
                        VkQueryResultFlags flags)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_query_pool, pool, _pool);

   const bool wide = (flags & VK_QUERY_RESULT_64_BIT) != 0;
   const bool with_availability =
      (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
   const bool partial = (flags & VK_QUERY_RESULT_PARTIAL_BIT) != 0;
   const uint32_t word_bytes = wide ? 8 : 4;
   const uint32_t per_query_bytes = word_bytes * (with_availability ? 2 : 1);

   if (pool == NULL || firstQuery >= pool->query_count ||
       queryCount > pool->query_count - firstQuery ||
       stride < per_query_bytes || stride % word_bytes != 0 ||
       queryCount == 0 ||
       dataSize < (size_t)(queryCount - 1) * stride + per_query_bytes)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   VkResult result = VK_SUCCESS;
   for (uint32_t q = 0; q < queryCount; q++) {
      const bool available = pool->state[firstQuery + q] != 0;
      uint8_t *out = (uint8_t *)pData + (size_t)q * stride;
      if (!available && !partial && !with_availability) {
         result = VK_NOT_READY;
         continue;
      }
      if (available || partial) {
         const uint64_t zero = 0;
         memcpy(out, &zero, word_bytes);
      }
      if (with_availability) {
         const uint64_t state = available ? 1 : 0;
         memcpy(out + word_bytes, &state, word_bytes);
      }
      if (!available && !partial)
         result = VK_NOT_READY;
   }
   return result;
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
