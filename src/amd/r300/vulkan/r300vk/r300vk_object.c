/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Sampler, buffer-view, query-pool, and event object lifecycle.
 *
 * r300vk did not implement Create/Destroy for these four object types, so the
 * loader's dispatch table left the slots NULL (vk_cmd_enqueue fills only command
 * entrypoints; vk_common_device_entrypoints does not carry these object
 * Create/Destroy entrypoints for this device config).  A bare vkDestroySampler/
 * BufferView/QueryPool/Event -- including the spec-mandated no-op on
 * VK_NULL_HANDLE -- therefore jumped through a NULL pointer and crashed
 * (dEQP-VK.api.null_handle.*, api.object_management.*, api.buffer_view.*,
 * api.command_buffers.* state transitions, api.descriptor_set.* sampler setup).
 *
 * Sampler and query pool wrap the Mesa runtime base objects (vk_sampler /
 * vk_query_pool), whose create/destroy helpers own the vk_object_base lifecycle
 * and parse the create info.  Buffer view and event are plain vk_object_base:
 * the runtime vk_buffer_view base validates the range against a vk_buffer, but
 * r300vk_buffer is a raw vk_object_base (not the vk_buffer base), so the view
 * resolves the range against r300vk_buffer->size itself.  Event has no runtime
 * base and carries the host signal state; on the single-queue serialized
 * CPU-replay model the host Set/Reset/GetStatus pair is the observable event
 * contract.  The descriptor and query paths read this state when they are wired.
 */

#include "r300vk_device.h"
#include "r300vk_buffer.h"
#include "r300vk_entrypoints.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_query_pool.h"
#include "vk_sampler.h"

/* vk_query_pool.h defines the base struct but not its handle casts (unlike
 * vk_sampler.h / vk_buffer_view.h), so declare them here. */
VK_DEFINE_NONDISP_HANDLE_CASTS(vk_query_pool, base, VkQueryPool,
                               VK_OBJECT_TYPE_QUERY_POOL)

struct r300vk_event {
   struct vk_object_base base;
   VkResult              status;   /* VK_EVENT_SET or VK_EVENT_RESET */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_event, base, VkEvent,
                               VK_OBJECT_TYPE_EVENT)

struct r300vk_buffer_view {
   struct vk_object_base base;
   struct r300vk_buffer *buffer;   /* the texel buffer this view selects */
   VkFormat              format;
   VkDeviceSize          offset;
   VkDeviceSize          range;    /* VK_WHOLE_SIZE resolved to the buffer tail */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_buffer_view, base, VkBufferView,
                               VK_OBJECT_TYPE_BUFFER_VIEW)

VkResult
r300vk_CreateSampler(VkDevice _device,
                     const VkSamplerCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkSampler *pSampler)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);

   struct vk_sampler *sampler =
      vk_sampler_create(&device->vk, pCreateInfo, pAllocator, sizeof(*sampler));
   if (!sampler)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   *pSampler = vk_sampler_to_handle(sampler);
   return VK_SUCCESS;
}

void
r300vk_DestroySampler(VkDevice _device,
                      VkSampler _sampler,
                      const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(vk_sampler, sampler, _sampler);
   if (!sampler)
      return;

   vk_sampler_destroy(&device->vk, pAllocator, sampler);
}

VkResult
r300vk_CreateBufferView(VkDevice _device,
                        const VkBufferViewCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkBufferView *pView)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_buffer, buffer, pCreateInfo->buffer);

   struct r300vk_buffer_view *view =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*view),
                       VK_OBJECT_TYPE_BUFFER_VIEW);
   if (!view)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   view->buffer = buffer;
   view->format = pCreateInfo->format;
   view->offset = pCreateInfo->offset;
   view->range  = (pCreateInfo->range == VK_WHOLE_SIZE && buffer)
                  ? buffer->size - pCreateInfo->offset
                  : pCreateInfo->range;

   *pView = r300vk_buffer_view_to_handle(view);
   return VK_SUCCESS;
}

void
r300vk_DestroyBufferView(VkDevice _device,
                         VkBufferView _view,
                         const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_buffer_view, view, _view);
   if (!view)
      return;

   vk_object_free(&device->vk, pAllocator, view);
}

VkResult
r300vk_CreateQueryPool(VkDevice _device,
                       const VkQueryPoolCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkQueryPool *pQueryPool)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);

   struct vk_query_pool *pool =
      vk_query_pool_create(&device->vk, pCreateInfo, pAllocator, sizeof(*pool));
   if (!pool)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   *pQueryPool = vk_query_pool_to_handle(pool);
   return VK_SUCCESS;
}

void
r300vk_DestroyQueryPool(VkDevice _device,
                        VkQueryPool _pool,
                        const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(vk_query_pool, pool, _pool);
   if (!pool)
      return;

   vk_query_pool_destroy(&device->vk, pAllocator, pool);
}

VkResult
r300vk_CreateEvent(VkDevice _device,
                   const VkEventCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkEvent *pEvent)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);

   struct r300vk_event *event =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*event),
                       VK_OBJECT_TYPE_EVENT);
   if (!event)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   event->status = VK_EVENT_RESET;
   *pEvent = r300vk_event_to_handle(event);
   return VK_SUCCESS;
}

void
r300vk_DestroyEvent(VkDevice _device,
                    VkEvent _event,
                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_event, event, _event);
   if (!event)
      return;

   vk_object_free(&device->vk, pAllocator, event);
}

/* Host event signal state.  The CPU-replay queue serializes every submit, so a
 * host wait already observes all prior GPU work; the host Set/Reset/GetStatus
 * trio is the full observable contract for an event used outside a command
 * buffer (dEQP-VK.api.command_buffers state-transition cases set and poll an
 * event from the host). */
VkResult
r300vk_GetEventStatus(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(r300vk_event, event, _event);
   return event->status;
}

VkResult
r300vk_SetEvent(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(r300vk_event, event, _event);
   event->status = VK_EVENT_SET;
   return VK_SUCCESS;
}

VkResult
r300vk_ResetEvent(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(r300vk_event, event, _event);
   event->status = VK_EVENT_RESET;
   return VK_SUCCESS;
}
