/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_buffer.h"
#include "r3v_device.h"
#include "r3v_memory.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_util.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"

VkResult
r3v_CreateBuffer(VkDevice _device,
                    const VkBufferCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkBuffer *pBuffer)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   struct r3v_buffer *buf;

   buf = vk_zalloc2(&device->vk.alloc, pAllocator,
                    sizeof(*buf), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!buf)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &buf->base, VK_OBJECT_TYPE_BUFFER);
   buf->size  = pCreateInfo->size;
   buf->usage = pCreateInfo->usage;

   struct pipe_resource tmpl = {
      .target    = PIPE_BUFFER,
      .format    = PIPE_FORMAT_R8_UNORM,
      .bind      = PIPE_BIND_VERTEX_BUFFER,
      .usage     = PIPE_USAGE_DYNAMIC,
      .width0    = (unsigned)pCreateInfo->size,
      .height0   = 1,
      .depth0    = 1,
      .array_size = 1,
   };

   buf->resource = device->screen->resource_create(device->screen, &tmpl);
   if (!buf->resource) {
      vk_object_base_finish(&buf->base);
      vk_free2(&device->vk.alloc, pAllocator, buf);
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   *pBuffer = r3v_buffer_to_handle(buf);
   return VK_SUCCESS;
}

void
r3v_DestroyBuffer(VkDevice _device,
                     VkBuffer _buffer,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_buffer, buf, _buffer);
   if (!buf)
      return;

   /* A suballocating client recycles buffers within a live allocation; the
    * memory's per-buffer sync slice must die with the buffer, or the next
    * buffer -> host sync copies the dead resource's stale bytes back over
    * the live host map. */
   r3v_device_memory_drop_buffer_slices(device, buf->resource);
   pipe_resource_reference(&buf->resource, NULL);
   vk_object_base_finish(&buf->base);
   vk_free2(&device->vk.alloc, pAllocator, buf);
}

void
r3v_GetBufferMemoryRequirements2(VkDevice _device,
                                     const VkBufferMemoryRequirementsInfo2 *pInfo,
                                     VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(r3v_buffer, buf, pInfo->buffer);

   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements){
      .size           = buf->size,
      .alignment      = 4,
      /* Both advertised memory types back a buffer validly.  RS482 is UMA --
       * the GART aperture and the BIOS-carved shared-VRAM partition are one
       * physical pool -- and r3v_AllocateMemory records only the size; the
       * storage belongs to the buffer's own pipe resource, bound identically
       * for either type.  Reporting only the host-visible type starved
       * device-local-heap clients: zink allocates vertex/index buffers from
       * its DEVICE_LOCAL heap (type 1), and an empty intersection with
       * memoryTypeBits aborts zink_resource's allocate_bo. */
      .memoryTypeBits = 0x3,
   };

   /* VK_KHR_dedicated_allocation: a buffer suballocates from the shared GART
    * pool, so nothing forces it to own its VkDeviceMemory. */
   VkMemoryDedicatedRequirements *dedicated =
      vk_find_struct(pMemoryRequirements->pNext, MEMORY_DEDICATED_REQUIREMENTS);
   if (dedicated) {
      dedicated->prefersDedicatedAllocation  = VK_FALSE;
      dedicated->requiresDedicatedAllocation = VK_FALSE;
   }
}
