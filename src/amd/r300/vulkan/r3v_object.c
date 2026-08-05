/*
 * SPDX-License-Identifier: MIT
 *
 * Sampler, buffer-view, query-pool, and event object lifecycle.
 *
 * r3v did not implement Create/Destroy for these four object types, so the
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
 * r3v_buffer is a raw vk_object_base (not the vk_buffer base), so the view
 * resolves the range against r3v_buffer->size itself.  Event has no runtime
 * base and carries the host signal state; on the single-queue serialized
 * CPU-replay model the host Set/Reset/GetStatus pair is the observable event
 * contract.  The descriptor and query paths read this state when they are wired.
 */

#include <stdint.h>
#include <string.h>

#include "r3v_device.h"
#include "r3v_buffer.h"
#include "r3v_entrypoints.h"
#include "r3v_format.h"
#include "r3v_object.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_query_pool.h"
#include "vk_sampler.h"

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "pipe/p_defines.h"

struct r3v_buffer_view {
   struct vk_object_base base;
   struct r3v_buffer *buffer;   /* the texel buffer this view selects */
   VkFormat              format;
   VkDeviceSize          offset;
   VkDeviceSize          range;    /* VK_WHOLE_SIZE resolved to the buffer tail */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_buffer_view, base, VkBufferView,
                               VK_OBJECT_TYPE_BUFFER_VIEW)

/* VkSamplerAddressMode -> PIPE_TEX_WRAP_x.  r300 honors every Vulkan 1.0 wrap
 * mode; MIRROR_CLAMP_TO_EDGE is gated by the samplerMirrorClampToEdge feature
 * (VK_KHR_sampler_mirror_clamp_to_edge), which the loader validates before the
 * call reaches here. */
static unsigned
vk_address_mode_to_pipe(VkSamplerAddressMode mode)
{
   switch (mode) {
   case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:      return PIPE_TEX_WRAP_MIRROR_REPEAT;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:        return PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:      return PIPE_TEX_WRAP_CLAMP_TO_BORDER;
   case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE;
   case VK_SAMPLER_ADDRESS_MODE_REPEAT:
   default:                                           return PIPE_TEX_WRAP_REPEAT;
   }
}

/* VkCompareOp shares GL's ordering (NEVER=0 .. ALWAYS=7), and PIPE_FUNC_x is the
 * same enumeration, but switch explicitly so a future divergence is a compile
 * error rather than a silent miscompare. */
static unsigned
vk_compare_op_to_pipe(VkCompareOp op)
{
   switch (op) {
   case VK_COMPARE_OP_LESS:             return PIPE_FUNC_LESS;
   case VK_COMPARE_OP_EQUAL:            return PIPE_FUNC_EQUAL;
   case VK_COMPARE_OP_LESS_OR_EQUAL:    return PIPE_FUNC_LEQUAL;
   case VK_COMPARE_OP_GREATER:          return PIPE_FUNC_GREATER;
   case VK_COMPARE_OP_NOT_EQUAL:        return PIPE_FUNC_NOTEQUAL;
   case VK_COMPARE_OP_GREATER_OR_EQUAL: return PIPE_FUNC_GEQUAL;
   case VK_COMPARE_OP_ALWAYS:           return PIPE_FUNC_ALWAYS;
   case VK_COMPARE_OP_NEVER:
   default:                             return PIPE_FUNC_NEVER;
   }
}

/* Map the full VkSamplerCreateInfo to a pipe_sampler_state.  The runtime
 * vk_sampler keeps the address modes and border color but drops the filter,
 * mipmap, and compare fields, so the mapping is done here at create time while
 * pCreateInfo is in hand, and the resulting CSO is cached on the sampler. */
static void
r3v_sampler_state_from_vk(const VkSamplerCreateInfo *ci,
                             struct pipe_sampler_state *ss)
{
   memset(ss, 0, sizeof(*ss));
   ss->wrap_s = vk_address_mode_to_pipe(ci->addressModeU);
   ss->wrap_t = vk_address_mode_to_pipe(ci->addressModeV);
   ss->wrap_r = vk_address_mode_to_pipe(ci->addressModeW);
   ss->min_img_filter = ci->minFilter == VK_FILTER_LINEAR
                        ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
   ss->mag_img_filter = ci->magFilter == VK_FILTER_LINEAR
                        ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
   ss->min_mip_filter = ci->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR
                        ? PIPE_TEX_MIPFILTER_LINEAR : PIPE_TEX_MIPFILTER_NEAREST;
   ss->unnormalized_coords = ci->unnormalizedCoordinates ? 1 : 0;
   ss->lod_bias = ci->mipLodBias;
   ss->min_lod  = ci->minLod;
   ss->max_lod  = ci->maxLod;
   ss->max_anisotropy = 1;
   if (ci->anisotropyEnable && ci->maxAnisotropy > 1.0f)
      ss->max_anisotropy = (unsigned)ci->maxAnisotropy;
   if (ci->compareEnable) {
      ss->compare_mode = PIPE_TEX_COMPARE_R_TO_TEXTURE;
      ss->compare_func = vk_compare_op_to_pipe(ci->compareOp);
   }

   VkFormat border_format;
   VkClearColorValue border_color =
      vk_sampler_border_color_value(ci, &border_format);
   memcpy(&ss->border_color, &border_color, sizeof(ss->border_color));
   ss->border_color_is_integer = vk_border_color_is_int(ci->borderColor);
   ss->border_color_format = border_format == VK_FORMAT_UNDEFINED
                              ? PIPE_FORMAT_NONE
                              : r3v_vk_format_to_pipe_format(border_format);
}

VkResult
r3v_CreateSampler(VkDevice _device,
                     const VkSamplerCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkSampler *pSampler)
{
   VK_FROM_HANDLE(r3v_device, device, _device);

   struct vk_sampler *vks =
      vk_sampler_create(&device->vk, pCreateInfo, pAllocator,
                        sizeof(struct r3v_sampler));
   if (!vks)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Build the Gallium sampler CSO now so the draw replay can bind it directly.
    * device->pipe is the same context r3v_CreateGraphicsPipelines uses to
    * create its blend/raster/dsa/vs/fs CSOs from an API thread, so a sampler CSO
    * built here follows the existing serialized-pipe-access model. */
   struct r3v_sampler *sampler = r3v_sampler_from_vk(vks);
   struct pipe_sampler_state ss;
   r3v_sampler_state_from_vk(pCreateInfo, &ss);
   sampler->pipe_cso = device->pipe->create_sampler_state(device->pipe, &ss);
   if (!sampler->pipe_cso) {
      vk_sampler_destroy(&device->vk, pAllocator, vks);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* A split (multi-tile) sampled image may stitch only through a sampler whose
    * footprint is a single point in one tile: NEAREST on every filter, every
    * address mode CLAMP_TO_EDGE (out-of-tile samples clamp to the edge and are
    * masked out by the tile select), normalized coordinates, and no compare.
    * Anything else (LINEAR straddles the seam, REPEAT wraps across tiles) is left
    * ineligible so the split-image bind keeps refusing it. */
   sampler->nearest_stitch_eligible =
      pCreateInfo->magFilter == VK_FILTER_NEAREST &&
      pCreateInfo->minFilter == VK_FILTER_NEAREST &&
      pCreateInfo->mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST &&
      pCreateInfo->addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
      pCreateInfo->addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
      pCreateInfo->addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
      !pCreateInfo->unnormalizedCoordinates &&
      !pCreateInfo->compareEnable;

   /* LINEAR variant: the overlapped halo atlas duplicates seam texels so a
    * bilinear footprint stays inside one chart, so LINEAR min/mag is eligible
    * under the same CLAMP_TO_EDGE / normalized / no-compare / no-anisotropy
    * constraints; the mipmap mode is unconstrained because the stitched
    * surface carries one mip level. */
   sampler->linear_stitch_eligible =
      pCreateInfo->magFilter == VK_FILTER_LINEAR &&
      pCreateInfo->minFilter == VK_FILTER_LINEAR &&
      pCreateInfo->addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
      pCreateInfo->addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
      pCreateInfo->addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
      !pCreateInfo->unnormalizedCoordinates &&
      !pCreateInfo->compareEnable &&
      !(pCreateInfo->anisotropyEnable && pCreateInfo->maxAnisotropy > 1.0f);

   *pSampler = vk_sampler_to_handle(vks);
   return VK_SUCCESS;
}

void
r3v_DestroySampler(VkDevice _device,
                      VkSampler _sampler,
                      const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(vk_sampler, vks, _sampler);
   if (!vks)
      return;

   struct r3v_sampler *sampler = r3v_sampler_from_vk(vks);
   if (sampler->pipe_cso)
      device->pipe->delete_sampler_state(device->pipe, sampler->pipe_cso);

   vk_sampler_destroy(&device->vk, pAllocator, vks);
}

VkResult
r3v_CreateBufferView(VkDevice _device,
                        const VkBufferViewCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkBufferView *pView)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_buffer, buffer, pCreateInfo->buffer);

   struct r3v_buffer_view *view =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*view),
                       VK_OBJECT_TYPE_BUFFER_VIEW);
   if (!view)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   view->buffer = buffer;
   view->format = pCreateInfo->format;
   view->offset = pCreateInfo->offset;
   if (pCreateInfo->range == VK_WHOLE_SIZE) {
      /* Resolve VK_WHOLE_SIZE to the buffer tail; clamp so an offset past the
       * end (invalid usage) yields 0 rather than wrapping VkDeviceSize to a
       * huge range. */
      view->range = (buffer && pCreateInfo->offset <= buffer->size)
                    ? buffer->size - pCreateInfo->offset : 0;
   } else {
      view->range = pCreateInfo->range;
   }

   *pView = r3v_buffer_view_to_handle(view);
   return VK_SUCCESS;
}

void
r3v_DestroyBufferView(VkDevice _device,
                         VkBufferView _view,
                         const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_buffer_view, view, _view);
   if (!view)
      return;

   vk_object_free(&device->vk, pAllocator, view);
}

VkResult
r3v_CreateQueryPool(VkDevice _device,
                       const VkQueryPoolCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkQueryPool *pQueryPool)
{
   VK_FROM_HANDLE(r3v_device, device, _device);

   /* Allocate the vk_query_pool base plus one r3v_query per query, so the
    * replay and GetQueryPoolResults have per-slot result + availability storage.
    * vk_query_pool_create zero-initializes the allocation. */
   const size_t pool_size = sizeof(struct r3v_query_pool);
   const size_t query_size = sizeof(struct r3v_query);
   if (pCreateInfo->queryCount > (SIZE_MAX - pool_size) / query_size)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   const size_t size = pool_size + (size_t)pCreateInfo->queryCount * query_size;
   struct vk_query_pool *pool =
      vk_query_pool_create(&device->vk, pCreateInfo, pAllocator, size);
   if (!pool)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   *pQueryPool = vk_query_pool_to_handle(pool);
   return VK_SUCCESS;
}

void
r3v_DestroyQueryPool(VkDevice _device,
                        VkQueryPool _pool,
                        const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(vk_query_pool, pool, _pool);
   if (!pool)
      return;

   vk_query_pool_destroy(&device->vk, pAllocator, pool);
}

static bool
r3v_query_result_host_slot_in_bounds(uint32_t query_index,
                                        VkDeviceSize stride,
                                        unsigned result_size,
                                        unsigned per_query,
                                        bool write_availability,
                                        size_t data_size,
                                        size_t *byte_offset)
{
   if (stride > SIZE_MAX)
      return false;
   if (stride && query_index > SIZE_MAX / (size_t)stride)
      return false;

   const size_t offset = (size_t)query_index * (size_t)stride;
   const size_t mapped_span = write_availability ? per_query : result_size;
   if (mapped_span > data_size || offset > data_size - mapped_span)
      return false;

   *byte_offset = offset;
   return true;
}

static void
r3v_store_query_result_word(uint8_t *dst, unsigned result_size,
                               uint64_t value)
{
   if (result_size == sizeof(uint64_t)) {
      const uint64_t v = value;
      memcpy(dst, &v, sizeof(v));
   } else {
      const uint32_t v = (uint32_t)value;
      memcpy(dst, &v, sizeof(v));
   }
}

static bool
r3v_get_query_pool_result_one(uint8_t *data,
                                 size_t data_size,
                                 VkDeviceSize stride,
                                 uint32_t dst_index,
                                 const struct r3v_query *query,
                                 unsigned result_size,
                                 unsigned per_query,
                                 bool write_availability,
                                 bool force_result,
                                 bool wait,
                                 VkResult *slot_result)
{
   const bool available = query->available;
   const bool write_result = available || force_result;
   *slot_result = (!available && !wait) ? VK_NOT_READY : VK_SUCCESS;

   if (!write_result && !write_availability)
      return true;

   size_t byte_off;
   if (!data ||
       !r3v_query_result_host_slot_in_bounds(dst_index, stride,
                                                result_size, per_query,
                                                write_availability,
                                                data_size, &byte_off)) {
      *slot_result = VK_NOT_READY;
      return false;
   }

   uint8_t *slot = data + byte_off;
   if (write_result)
      r3v_store_query_result_word(slot, result_size,
                                     available ? query->result : 0);
   if (write_availability)
      r3v_store_query_result_word(slot + result_size, result_size,
                                     available ? 1 : 0);

   return true;
}

/* Copy occlusion results recorded by the replay into the caller's buffer.  Each
 * query writes a result word (32- or 64-bit per VK_QUERY_RESULT_64_BIT) at
 * pData + i*stride, optionally followed by an availability word
 * (VK_QUERY_RESULT_WITH_AVAILABILITY_BIT).  The range and host-buffer layout
 * are clamped defensively before indexing either side.  The serialized
 * CPU-replay queue retires every submitted query before the host returns, so
 * VK_QUERY_RESULT_WAIT never blocks; an unavailable slot is one reset but never
 * ended. */
VkResult
r3v_GetQueryPoolResults(VkDevice _device,
                           VkQueryPool _pool,
                           uint32_t firstQuery,
                           uint32_t queryCount,
                           size_t dataSize,
                           void *pData,
                           VkDeviceSize stride,
                           VkQueryResultFlags flags)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(vk_query_pool, vk_pool, _pool);
   struct r3v_query_pool *pool = r3v_query_pool(vk_pool);
   const bool b64        = (flags & VK_QUERY_RESULT_64_BIT) != 0;
   const bool want_avail = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
   const bool force_result = (flags & (VK_QUERY_RESULT_PARTIAL_BIT |
                                       VK_QUERY_RESULT_WAIT_BIT)) != 0;
   const bool wait = (flags & VK_QUERY_RESULT_WAIT_BIT) != 0;
   const unsigned rsize  = b64 ? sizeof(uint64_t) : sizeof(uint32_t);
   const unsigned per_query = rsize * (want_avail ? 2u : 1u);
   VkResult result = VK_SUCCESS;
   (void)device;

   if (firstQuery >= vk_pool->query_count || queryCount == 0)
      return VK_SUCCESS;
   if (queryCount > vk_pool->query_count - firstQuery)
      queryCount = vk_pool->query_count - firstQuery;

   for (uint32_t i = 0; i < queryCount; i++) {
      const struct r3v_query *q = &pool->queries[firstQuery + i];
      VkResult slot_result = VK_SUCCESS;
      if (!r3v_get_query_pool_result_one(pData, dataSize, stride, i, q,
                                            rsize, per_query, want_avail,
                                            force_result, wait,
                                            &slot_result)) {
         result = slot_result;
         break;
      }
      if (slot_result != VK_SUCCESS)
         result = slot_result;
   }
   return result;
}

/* Host-side query reset (VK_EXT_host_query_reset / core vkResetQueryPool).  The
 * CPU-replay queue drains every prior submit before returning to the host, so
 * when the application calls this the recorded results are already retired and
 * no GPU-side write races the clear.  Clear the per-slot result and availability
 * over [firstQuery, firstQuery+queryCount) exactly as the
 * R3V_CMD_RESET_QUERY_POOL replay does, so a subsequent vkGetQueryPoolResults
 * sees the slots unavailable.  The range is bounds-clamped against the pool to
 * match the command-buffer reset's defensive clamp. */
void
r3v_ResetQueryPool(VkDevice _device,
                      VkQueryPool _pool,
                      uint32_t firstQuery,
                      uint32_t queryCount)
{
   VK_FROM_HANDLE(vk_query_pool, vk_pool, _pool);
   struct r3v_query_pool *pool = r3v_query_pool(vk_pool);
   (void)_device;

   if (firstQuery >= vk_pool->query_count)
      return;
   uint32_t n = queryCount;
   if (n > vk_pool->query_count - firstQuery)
      n = vk_pool->query_count - firstQuery;
   for (uint32_t i = 0; i < n; i++) {
      pool->queries[firstQuery + i].result    = 0;
      pool->queries[firstQuery + i].available = false;
   }
}

VkResult
r3v_CreateEvent(VkDevice _device,
                   const VkEventCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkEvent *pEvent)
{
   VK_FROM_HANDLE(r3v_device, device, _device);

   struct r3v_event *event =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*event),
                       VK_OBJECT_TYPE_EVENT);
   if (!event)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   event->status = VK_EVENT_RESET;
   *pEvent = r3v_event_to_handle(event);
   return VK_SUCCESS;
}

void
r3v_DestroyEvent(VkDevice _device,
                    VkEvent _event,
                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_event, event, _event);
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
r3v_GetEventStatus(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(r3v_event, event, _event);
   return event->status;
}

VkResult
r3v_SetEvent(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(r3v_event, event, _event);
   event->status = VK_EVENT_SET;
   return VK_SUCCESS;
}

VkResult
r3v_ResetEvent(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(r3v_event, event, _event);
   event->status = VK_EVENT_RESET;
   return VK_SUCCESS;
}
