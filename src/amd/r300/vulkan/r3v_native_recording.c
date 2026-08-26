/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V fail-closed recording surface: every core Vulkan 1.0 command
 * outside the qualified draw subset poisons its command buffer.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"

#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <limits.h>
#include <string.h>

/* The native command buffer executes only an installed fixed IB, and
 * the public surface in r3v_native_draw.c is the one route that
 * installs it through Vulkan recording.  Every other core 1.0 vkCmd*
 * entrypoint records R3V_NATIVE_REFUSAL_RESULT into the command
 * buffer: vkEndCommandBuffer returns the error, the buffer ends
 * INVALID, and the queue refuses it.  Native definitions for the whole
 * core set keep common bridges from occupying native direct-entrypoint
 * slots, while the closure audit checks every reachable bridge target.
 * Two load-bearing bridge forms show the shape: vk_common_BindImageMemory in
 * src/vulkan/runtime/vk_device.c calls dispatch_table.BindImageMemory2, and
 * vk_common_CmdBeginRenderPass in src/vulkan/runtime/vk_render_pass.c
 * calls dispatch_table.CmdBeginRenderPass2.  The native table supplies
 * r3v_BindImageMemory and r3v_CmdBeginRenderPass before the common
 * overlay, so those direct slots stay native when the common providers
 * would otherwise be selected.
 *
 * Symbol discovery uses `rg --fixed-strings SYMBOL PATH`: the overlay symbol
 * `vk_common_device_entrypoints` maps to
 * `src/amd/r300/vulkan/r3v_native_device.c`; bridge providers
 * `vk_common_BindImageMemory` maps to `src/vulkan/runtime/vk_device.c`,
 * while `vk_common_CmdBeginRenderPass` and
 * `vk_common_CmdBeginRenderPass2` map to
 * `src/vulkan/runtime/vk_render_pass.c`; native symbols
 * `r3v_BindImageMemory` and `r3v_BindImageMemory2` map to
 * `src/amd/r300/vulkan/r3v_native_image.c`, while
 * `r3v_CmdBeginRenderPass` maps to
 * `src/amd/r300/vulkan/r3v_native_draw.c`; and `r3v_entrypoints` maps to
 * `src/amd/r300/vulkan/meson.build`.  The r3v-native-entrypoint-closure
 * audit walks common providers through dispatch_table calls and requires each
 * target in the linked native or common table; an open edge names a target in
 * neither table.  Its `--drop BindBufferMemory2` case calibrates that
 * known-bad edge.
 */
static void
r3v_native_cmd_poison(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   vk_command_buffer_set_error(cmd_buffer, R3V_NATIVE_REFUSAL_RESULT);
}

static bool
r3v_native_queue_family_pair_ok(uint32_t src_queue_family,
                                uint32_t dst_queue_family)
{
   return src_queue_family == dst_queue_family &&
          (src_queue_family == 0 ||
           src_queue_family == VK_QUEUE_FAMILY_IGNORED);
}

static bool
r3v_native_transfer_source_layout_ok(VkImageLayout layout)
{
   return layout == VK_IMAGE_LAYOUT_GENERAL ||
          layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

static bool
r3v_native_transfer_destination_layout_ok(VkImageLayout layout)
{
   return layout == VK_IMAGE_LAYOUT_GENERAL ||
          layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
}

static bool
r3v_native_render_layout_ok(VkImageLayout layout)
{
   return layout == VK_IMAGE_LAYOUT_GENERAL ||
          layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

static bool
r3v_native_image_layout_ok(const struct r3v_native_image *image,
                           VkImageLayout layout)
{
   if (layout == VK_IMAGE_LAYOUT_GENERAL)
      return true;
   /* Each usage bit brings its own layouts, so a render target that also
    * carries transfer usage reaches both vocabularies.
    */
   if ((image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
       r3v_native_render_layout_ok(layout))
      return true;
   return ((image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0 &&
           r3v_native_transfer_source_layout_ok(layout)) ||
          ((image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 &&
           r3v_native_transfer_destination_layout_ok(layout));
}

/* The synchronous host executor has no hardware image-layout register:
 * host mappings execute in recorded order, and every destination publishes
 * before submission completion.  The API layout tokens still carry a
 * contract.  Transfer image copies accept only the source or destination
 * layouts the executor implements, while image barriers admit transitions
 * from Vulkan's undefined or preinitialized states.  Render-family barriers
 * use the general and color-attachment layouts accepted by the render pass
 * path.  The Vulkan 1.3 specification's `vkCmdPipelineBarrier` and
 * `VkImageMemoryBarrier` valid-usage rules define layout transitions and
 * queue-family ownership; the `vkCmdCopyBufferToImage`, `vkCmdCopyImage`,
 * and `vkCmdCopyImageToBuffer` rules bind transfer layouts to image usage.
 * The bounded predicates are r3v_native_image_barrier_layouts_ok,
 * r3v_native_image_layout_ok, r3v_native_transfer_source_layout_ok,
 * r3v_native_transfer_destination_layout_ok, r3v_native_render_layout_ok,
 * and r3v_native_queue_family_pair_ok.  Symbol discovery uses
 * `(rg --fixed-strings r3v_native_image_barrier_layouts_ok
 * src/amd/r300/vulkan/)`, `(rg --fixed-strings r3v_native_image_layout_ok
 * src/amd/r300/vulkan/)`, `(rg --fixed-strings
 * r3v_native_transfer_source_layout_ok src/amd/r300/vulkan/)`,
 * `(rg --fixed-strings r3v_native_transfer_destination_layout_ok
 * src/amd/r300/vulkan/)`, `(rg --fixed-strings r3v_native_render_layout_ok
 * src/amd/r300/vulkan/)`, and `(rg --fixed-strings
 * r3v_native_queue_family_pair_ok src/amd/r300/vulkan/)`.
 */
static bool
r3v_native_image_barrier_range_ok(const VkImageSubresourceRange *range)
{
   return range->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
          range->baseMipLevel == 0 &&
          (range->levelCount == 1 ||
           range->levelCount == VK_REMAINING_MIP_LEVELS) &&
          range->baseArrayLayer == 0 &&
          (range->layerCount == 1 ||
           range->layerCount == VK_REMAINING_ARRAY_LAYERS);
}

static bool
r3v_native_image_barrier_layouts_ok(const VkImageMemoryBarrier *barrier)
{
   VK_FROM_HANDLE(r3v_native_image, image, barrier->image);
   const bool old_layout_ok =
      barrier->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
      barrier->oldLayout == VK_IMAGE_LAYOUT_PREINITIALIZED ||
      (image != NULL &&
       r3v_native_image_layout_ok(image, barrier->oldLayout));
   const bool new_layout_ok =
      image != NULL &&
      r3v_native_image_layout_ok(image, barrier->newLayout);

   return image != NULL && image->memory != NULL &&
          r3v_native_image_barrier_range_ok(&barrier->subresourceRange) &&
          old_layout_ok && new_layout_ok;
}

/* Transfer-copy recording over the linear families.  A copy records
 * while no render pass is open, and its record position relative to the
 * deferred draw fixes the group the queue executes it in, so the pass
 * and copies share one command buffer under recorded order.  Each
 * region admits at record time: the color aspect's single mip and
 * layer, offsets and extents inside the image, the buffer byte
 * footprint inside the buffer's created size, and the usage bit of each
 * direction.  A refused region, an overflowing op
 * list, or a mixed buffer poisons the recording.
 */
static struct r3v_native_deferred_copy *
r3v_native_copy_slot(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   /* A copy inside an open pass has no place in the render family's
    * lowering, so an open pass_target refuses.  r3v_CmdEndRenderPass
    * clears pass_target while the load-op clear stays in deferred_draw
    * until submission, so deferred_draw.pending is exactly the record
    * position that puts a copy after the draw.
    * Symbol discovery uses (rg --fixed-strings r3v_CmdEndRenderPass
    * src/amd/r300/vulkan/; rg --fixed-strings pass_target
    * src/amd/r300/vulkan/; rg --fixed-strings deferred_draw.pending
    * src/amd/r300/vulkan/).
    */
   if (cmd_buffer->pass_target != NULL) {
      r3v_native_cmd_poison(commandBuffer);
      return NULL;
   }

   if (cmd_buffer->deferred_copy_count == cmd_buffer->deferred_copy_capacity) {
      const uint32_t old_capacity = cmd_buffer->deferred_copy_capacity;
      const uint32_t new_capacity = old_capacity != 0
                                       ? old_capacity * 2
                                       : R3V_NATIVE_DEFERRED_COPY_INITIAL_CAPACITY;
      if (new_capacity < old_capacity ||
          (size_t)new_capacity >
             SIZE_MAX / sizeof(*cmd_buffer->deferred_copies)) {
         vk_command_buffer_set_error(&cmd_buffer->vk,
                                     VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }

      const size_t allocation_size =
         (size_t)new_capacity * sizeof(*cmd_buffer->deferred_copies);
      struct r3v_native_deferred_copy *copies = vk_alloc(
         &cmd_buffer->vk.pool->alloc, allocation_size, 8,
         VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (copies == NULL) {
         vk_command_buffer_set_error(&cmd_buffer->vk,
                                     VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }

      if (cmd_buffer->deferred_copy_count != 0) {
         memcpy(copies, cmd_buffer->deferred_copies,
                (size_t)cmd_buffer->deferred_copy_count *
                   sizeof(*cmd_buffer->deferred_copies));
      }
      vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->deferred_copies);
      cmd_buffer->deferred_copies = copies;
      cmd_buffer->deferred_copy_capacity = new_capacity;
   }

   return &cmd_buffer->deferred_copies[cmd_buffer->deferred_copy_count];
}

/* The record position that fixes a copy's execution group: a pending
 * deferred draw means the pass already recorded its load-op clear, so
 * the copy follows the draw; every earlier copy precedes it.
 */
static enum r3v_native_copy_group
r3v_native_copy_group_at_record(const struct r3v_native_cmd_buffer *cmd_buffer)
{
   return cmd_buffer->deferred_draw.pending
             ? R3V_NATIVE_COPY_GROUP_AFTER_DRAW
             : R3V_NATIVE_COPY_GROUP_BEFORE_DRAW;
}

static bool
r3v_native_copy_subresource_ok(const VkImageSubresourceLayers *sub)
{
   return sub->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
          sub->mipLevel == 0 && sub->baseArrayLayer == 0 &&
          sub->layerCount == 1;
}

/* The offset admits non-negative and the rectangle inside the image.
 * Both families copy through the same host row walk over the image's own
 * row pitch, so the rectangle rule is the family-independent one; the
 * per-command usage-bit and layout checks decide which image each
 * direction admits.
 * The region extent arrives from the application unbounded, so the
 * containment sums widen to 64 bits; a wrapping 32-bit sum would admit
 * an extent of 2^32 - 1 texels against a 16-texel image.  The image
 * must also be bound: admission at record keeps an unbound resource
 * from surfacing as device loss at submission.
 */
static bool
r3v_native_copy_rect_ok(const struct r3v_native_image *image,
                        VkOffset3D offset, VkExtent3D extent)
{
   return image != NULL && image->memory != NULL &&
          offset.x >= 0 && offset.y >= 0 &&
          offset.z == 0 && extent.depth == 1 && extent.width >= 1 &&
          extent.height >= 1 &&
          (uint64_t)(uint32_t)offset.x + extent.width <= image->width &&
          (uint64_t)(uint32_t)offset.y + extent.height <= image->height;
}

/* Resolves the buffer-side row length in texels and proves the region's
 * byte footprint inside the buffer's created size, in 64-bit
 * arithmetic.
 */
static bool
r3v_native_copy_buffer_ok(const struct r3v_native_buffer *buffer,
                          VkBufferUsageFlags usage_bit,
                          const VkBufferImageCopy *region,
                          uint32_t texel_bytes,
                          uint32_t *row_length_out)
{
   if (buffer == NULL || (buffer->vk.usage & usage_bit) == 0)
      return false;
   /* The buffer range closes inside the bound memory, which
    * BindBufferMemory2 recorded without validating -- the invariant
    * the draw path proves for its vertex stream.  After this holds,
    * vk.size is bounded by the real allocation, so the footprint sum
    * below cannot wrap once bufferOffset is confined to vk.size.
    */
   if (buffer->memory == NULL ||
       buffer->offset > buffer->memory->bo.size ||
       buffer->vk.size > buffer->memory->bo.size - buffer->offset)
      return false;
   if (region->bufferOffset > buffer->vk.size)
      return false;
   const uint32_t row_length = region->bufferRowLength != 0
                                  ? region->bufferRowLength
                                  : region->imageExtent.width;
   if (row_length < region->imageExtent.width)
      return false;
   if (region->bufferImageHeight != 0 &&
       region->bufferImageHeight < region->imageExtent.height)
      return false;
   const uint64_t last_byte =
      region->bufferOffset +
      ((uint64_t)(region->imageExtent.height - 1) * row_length +
       region->imageExtent.width) *
         texel_bytes;
   if (last_byte > buffer->vk.size)
      return false;
   *row_length_out = row_length;
   return true;
}

static bool
r3v_native_copy_buffer_range_ok(const struct r3v_native_buffer *buffer,
                                VkBufferUsageFlags usage_bit,
                                VkDeviceSize offset, VkDeviceSize size)
{
   if (buffer == NULL || (buffer->vk.usage & usage_bit) == 0 ||
       buffer->memory == NULL || buffer->offset > buffer->memory->bo.size ||
       buffer->vk.size > buffer->memory->bo.size - buffer->offset ||
       offset > buffer->vk.size || size > buffer->vk.size - offset)
      return false;
   return true;
}

static void
r3v_native_record_event_op(VkCommandBuffer commandBuffer, VkEvent _event,
                           enum r3v_native_event_op_kind kind)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_event, event, _event);

   if (event == NULL || cmd_buffer->pass_target != NULL ||
       cmd_buffer->event_op_count == R3V_NATIVE_EVENT_OP_MAX) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   cmd_buffer->event_ops[cmd_buffer->event_op_count++] =
      (struct r3v_native_event_op){ .kind = kind, .event = event };
}

static struct r3v_native_query_op *
r3v_native_query_op_slot(struct r3v_native_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->query_op_count == R3V_NATIVE_QUERY_OP_MAX)
      return NULL;
   return &cmd_buffer->query_ops[cmd_buffer->query_op_count];
}

/* The begun query admits a span with no fragment-producing command --
 * the pass begin refuses while it is active -- so the samples-passed
 * count is exactly zero and the end publishes availability alone.
 * PRECISE requires the withheld occlusionQueryPrecise feature, and an
 * imprecise zero is exact here anyway, so a nonzero flag refuses.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdBeginQuery(
   VkCommandBuffer commandBuffer,
   VkQueryPool queryPool,
   uint32_t query,
   VkQueryControlFlags flags)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_query_pool, pool, queryPool);

   if (pool == NULL || flags != 0 || query >= pool->query_count ||
       cmd_buffer->active_query_pool != NULL ||
       cmd_buffer->pass_target != NULL) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   cmd_buffer->active_query_pool = pool;
   cmd_buffer->active_query = query;
}


/* Half-open rectangle intersection over one image's texel grid; the
 * copy admissions use it to refuse same-handle overlap, which Vulkan
 * leaves undefined.
 */
static bool
r3v_native_rects_overlap(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                         uint32_t width, uint32_t height)
{
   return ax < bx + (int32_t)width && bx < ax + (int32_t)width &&
          ay < by + (int32_t)height && by < ay + (int32_t)height;
}

/* The admitted blit is the unflipped rectangle between transfer-family
 * images of one format: the unit-scale case lowers onto the
 * image-to-image copy, and unequal extents lower onto the nearest
 * resample executor, whose sample point (x + 0.5) * src/dst matches
 * the spec's nearest filter.  A scaling blit takes VK_FILTER_NEAREST
 * and distinct images; flips and overlapping same-image rectangles
 * refuse.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdBlitImage(
   VkCommandBuffer commandBuffer,
   VkImage srcImage,
   VkImageLayout srcImageLayout,
   VkImage dstImage,
   VkImageLayout dstImageLayout,
   uint32_t regionCount,
   const VkImageBlit *pRegions,
   VkFilter filter)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_image, src, srcImage);
   VK_FROM_HANDLE(r3v_native_image, dst, dstImage);

   for (uint32_t r = 0; r < regionCount; r++) {
      const VkImageBlit *region = &pRegions[r];
      struct r3v_native_deferred_copy *op =
         r3v_native_copy_slot(commandBuffer);
      const int32_t src_w = region->srcOffsets[1].x - region->srcOffsets[0].x;
      const int32_t src_h = region->srcOffsets[1].y - region->srcOffsets[0].y;
      const int32_t dst_w = region->dstOffsets[1].x - region->dstOffsets[0].x;
      const int32_t dst_h = region->dstOffsets[1].y - region->dstOffsets[0].y;
      const VkExtent3D extent = {
         .width = (uint32_t)(src_w > 0 ? src_w : 0),
         .height = (uint32_t)(src_h > 0 ? src_h : 0),
         .depth = 1,
      };
      const VkExtent3D dst_extent = {
         .width = (uint32_t)(dst_w > 0 ? dst_w : 0),
         .height = (uint32_t)(dst_h > 0 ? dst_h : 0),
         .depth = 1,
      };
      const bool unit_scale = src_w == dst_w && src_h == dst_h;
      if (op == NULL || src_w <= 0 || src_h <= 0 ||
          dst_w <= 0 || dst_h <= 0 ||
          region->srcOffsets[0].z != 0 || region->srcOffsets[1].z != 1 ||
          region->dstOffsets[0].z != 0 || region->dstOffsets[1].z != 1 ||
          !r3v_native_transfer_source_layout_ok(srcImageLayout) ||
          !r3v_native_transfer_destination_layout_ok(dstImageLayout) ||
          !r3v_native_copy_subresource_ok(&region->srcSubresource) ||
          !r3v_native_copy_subresource_ok(&region->dstSubresource) ||
          !r3v_native_copy_rect_ok(src, region->srcOffsets[0], extent) ||
          !r3v_native_copy_rect_ok(dst, region->dstOffsets[0], dst_extent) ||
          (src->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
          (dst->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0 ||
          /* Both executors move bytes and convert nothing, so the two
           * formats must be one format. */
          src->format != dst->format ||
          (!unit_scale && (filter != VK_FILTER_NEAREST || src == dst)) ||
          (src == dst &&
           r3v_native_rects_overlap(region->srcOffsets[0].x,
                                    region->srcOffsets[0].y,
                                    region->dstOffsets[0].x,
                                    region->dstOffsets[0].y,
                                    extent.width, extent.height))) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      *op = (struct r3v_native_deferred_copy){
         .group = r3v_native_copy_group_at_record(cmd_buffer),
         .kind = unit_scale ? R3V_NATIVE_COPY_IMAGE_TO_IMAGE
                            : R3V_NATIVE_COPY_BLIT_IMAGE,
         .dst_width = dst_extent.width,
         .dst_height = dst_extent.height,
         .src_image = src,
         .dst_image = dst,
         .src_x = (uint32_t)region->srcOffsets[0].x,
         .src_y = (uint32_t)region->srcOffsets[0].y,
         .dst_x = (uint32_t)region->dstOffsets[0].x,
         .dst_y = (uint32_t)region->dstOffsets[0].y,
         .width = extent.width,
         .height = extent.height,
      };
      cmd_buffer->deferred_copy_count++;
   }
}

/* In-pass attachment clears admit over a draw-less pass alone: the
 * host applies each rectangle after the load-op clear on the zero-IB
 * path, and a pass carrying the device draw refuses the clear because
 * the draw executes after every host write and would reorder against
 * it.  Each rectangle names the one color attachment's single layer
 * and closes inside the render area, which the begin admission pinned
 * to the target extent.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdClearAttachments(
   VkCommandBuffer commandBuffer,
   uint32_t attachmentCount,
   const VkClearAttachment *pAttachments,
   uint32_t rectCount,
   const VkClearRect *pRects)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   struct r3v_native_deferred_draw *draw = &cmd_buffer->deferred_draw;

   if (cmd_buffer->pass_target == NULL || cmd_buffer->draw_recorded ||
       draw->stream_mask != 0) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   for (uint32_t a = 0; a < attachmentCount; a++) {
      const VkClearAttachment *att = &pAttachments[a];
      if (att->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
          att->colorAttachment != 0) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      /* The attachment's format selects the live VkClearColorValue
       * member and the render family's formats are UNORM, so the clear
       * reaches its texel through the color buffer's own UNORM8
       * conversion under the target's lane order.
       */
      const uint32_t packed = r300_tcl_bypass_triangle_pack_unorm8_dword(
         cmd_buffer->pass_target->lanes, att->clearValue.color.float32);
      for (uint32_t r = 0; r < rectCount; r++) {
         const VkClearRect *rect = &pRects[r];
         const VkRect2D *area = &rect->rect;
         if (draw->clear_rect_count == R3V_NATIVE_PASS_CLEAR_RECT_MAX ||
             rect->baseArrayLayer != 0 || rect->layerCount != 1 ||
             area->offset.x < 0 || area->offset.y < 0 ||
             area->extent.width == 0 || area->extent.height == 0 ||
             (uint32_t)area->offset.x > draw->target_width ||
             (uint32_t)area->offset.y > draw->target_height ||
             area->extent.width >
                draw->target_width - (uint32_t)area->offset.x ||
             area->extent.height >
                draw->target_height - (uint32_t)area->offset.y) {
            r3v_native_cmd_poison(commandBuffer);
            return;
         }
         draw->clear_rects[draw->clear_rect_count++] =
            (struct r3v_native_pass_clear_rect){
               .x = (uint32_t)area->offset.x,
               .y = (uint32_t)area->offset.y,
               .width = area->extent.width,
               .height = area->extent.height,
               .dword = util_cpu_to_le32(packed),
            };
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdClearColorImage(
   VkCommandBuffer commandBuffer,
   VkImage _image,
   VkImageLayout imageLayout,
   const VkClearColorValue *pColor,
   uint32_t rangeCount,
   const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_image, image, _image);

   /* The one-mip one-layer image has one clearable subresource, so
    * every admitted range names the whole image and the fill covers the
    * full extent.  The clear value packs per the image's format from
    * the Vulkan format registry's component layout: the two UNORM8
    * formats take the float lanes clamped and rounded to 8 bits (B, G,
    * R, A order for B8G8R8A8_UNORM; R, G, B, A for R8G8B8A8_UNORM), and
    * the UINT formats take the uint32 lanes masked to the component
    * width (8, 16, or 32 bits) in R, G, B, A order; every packed value
    * is little-endian in memory, so mapped memory holds the registry's
    * byte layout on every supported host.  A NaN float component
    * converts as zero -- every ordered comparison on NaN is false, so
    * it slips both clamp arms, and the float-to-integer cast of NaN
    * has no defined value.
    */
   for (uint32_t r = 0; r < rangeCount; r++) {
      const VkImageSubresourceRange *range = &pRanges[r];
      struct r3v_native_deferred_copy *op =
         r3v_native_copy_slot(commandBuffer);
      if (op == NULL || image == NULL || !image->transfer_family ||
          image->memory == NULL ||
          !r3v_native_transfer_destination_layout_ok(imageLayout) ||
          (image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0 ||
          range->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
          range->baseMipLevel != 0 ||
          (range->levelCount != 1 &&
           range->levelCount != VK_REMAINING_MIP_LEVELS) ||
          range->baseArrayLayer != 0 ||
          (range->layerCount != 1 &&
           range->layerCount != VK_REMAINING_ARRAY_LAYERS)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      uint8_t texel[16] = { 0 };
      switch (image->format) {
      case VK_FORMAT_B8G8R8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_UNORM: {
         static const unsigned bgra[4] = { 2, 1, 0, 3 };
         static const unsigned rgba[4] = { 0, 1, 2, 3 };
         const unsigned *lane_byte =
            image->format == VK_FORMAT_B8G8R8A8_UNORM ? bgra : rgba;
         for (unsigned c = 0; c < 4; c++) {
            float f = pColor->float32[c];
            if (f != f)
               f = 0.0f;
            f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
            texel[lane_byte[c]] = (uint8_t)(f * 255.0f + 0.5f);
         }
         break;
      }
      case VK_FORMAT_R8G8B8A8_UINT:
         for (unsigned c = 0; c < 4; c++)
            texel[c] = (uint8_t)(pColor->uint32[c] & 0xffu);
         break;
      case VK_FORMAT_R16G16B16A16_UINT:
         for (unsigned c = 0; c < 4; c++) {
            const uint16_t v = (uint16_t)(pColor->uint32[c] & 0xffffu);
            texel[2 * c] = (uint8_t)(v & 0xffu);
            texel[2 * c + 1] = (uint8_t)(v >> 8);
         }
         break;
      case VK_FORMAT_R32_UINT:
      case VK_FORMAT_R32G32B32A32_UINT: {
         const unsigned lanes = image->texel_bytes / 4;
         for (unsigned c = 0; c < lanes; c++) {
            const uint32_t v = util_cpu_to_le32(pColor->uint32[c]);
            memcpy(texel + 4 * c, &v, 4);
         }
         break;
      }
      default:
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      *op = (struct r3v_native_deferred_copy){
         .group = r3v_native_copy_group_at_record(cmd_buffer),
         .kind = R3V_NATIVE_COPY_CLEAR_IMAGE,
         .dst_image = image,
         .width = image->width,
         .height = image->height,
      };
      memcpy(op->clear_texel, texel, sizeof(texel));
      cmd_buffer->deferred_copy_count++;
   }
}

/* Depth/stencil clears name a depth or stencil aspect, and image
 * creation admits the one color format, so no image this command can
 * clear exists and the recording refuses.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdClearDepthStencilImage(
   VkCommandBuffer commandBuffer,
   VkImage image,
   VkImageLayout imageLayout,
   const VkClearDepthStencilValue *pDepthStencil,
   uint32_t rangeCount,
   const VkImageSubresourceRange *pRanges)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdCopyBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer srcBuffer,
   VkBuffer dstBuffer,
   uint32_t regionCount,
   const VkBufferCopy *pRegions)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_buffer, src, srcBuffer);
   VK_FROM_HANDLE(r3v_native_buffer, dst, dstBuffer);

   for (uint32_t r = 0; r < regionCount; r++) {
      const VkBufferCopy *region = &pRegions[r];
      struct r3v_native_deferred_copy *op =
         r3v_native_copy_slot(commandBuffer);
      if (op == NULL ||
          !r3v_native_copy_buffer_range_ok(
             src, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, region->srcOffset,
             region->size) ||
          !r3v_native_copy_buffer_range_ok(
             dst, VK_BUFFER_USAGE_TRANSFER_DST_BIT, region->dstOffset,
             region->size) ||
          /* Vulkan leaves overlapping copy regions undefined, so a
           * same-buffer region pair whose byte ranges intersect
           * refuses. */
          (src == dst &&
           region->srcOffset < region->dstOffset + region->size &&
           region->dstOffset < region->srcOffset + region->size)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      *op = (struct r3v_native_deferred_copy){
         .group = r3v_native_copy_group_at_record(cmd_buffer),
         .kind = R3V_NATIVE_COPY_BUFFER_TO_BUFFER,
         .src_buffer = src,
         .dst_buffer = dst,
         .src_offset = region->srcOffset,
         .dst_offset = region->dstOffset,
         .size = region->size,
      };
      cmd_buffer->deferred_copy_count++;
   }
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdCopyBufferToImage(
   VkCommandBuffer commandBuffer,
   VkBuffer srcBuffer,
   VkImage dstImage,
   VkImageLayout dstImageLayout,
   uint32_t regionCount,
   const VkBufferImageCopy *pRegions)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_buffer, buffer, srcBuffer);
   VK_FROM_HANDLE(r3v_native_image, image, dstImage);

   for (uint32_t r = 0; r < regionCount; r++) {
      const VkBufferImageCopy *region = &pRegions[r];
      struct r3v_native_deferred_copy *op =
         r3v_native_copy_slot(commandBuffer);
      uint32_t row_length;
      if (op == NULL ||
          !r3v_native_transfer_destination_layout_ok(dstImageLayout) ||
          !r3v_native_copy_subresource_ok(&region->imageSubresource) ||
          !r3v_native_copy_rect_ok(image, region->imageOffset,
                                   region->imageExtent) ||
          (image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0 ||
          !r3v_native_copy_buffer_ok(buffer,
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     region, image->texel_bytes,
                                     &row_length)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      *op = (struct r3v_native_deferred_copy){
         .group = r3v_native_copy_group_at_record(cmd_buffer),
         .kind = R3V_NATIVE_COPY_BUFFER_TO_IMAGE,
         .buffer = buffer,
         .dst_image = image,
         .buffer_offset = region->bufferOffset,
         .buffer_row_length = row_length,
         .dst_x = (uint32_t)region->imageOffset.x,
         .dst_y = (uint32_t)region->imageOffset.y,
         .width = region->imageExtent.width,
         .height = region->imageExtent.height,
      };
      cmd_buffer->deferred_copy_count++;
   }
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdCopyImage(
   VkCommandBuffer commandBuffer,
   VkImage srcImage,
   VkImageLayout srcImageLayout,
   VkImage dstImage,
   VkImageLayout dstImageLayout,
   uint32_t regionCount,
   const VkImageCopy *pRegions)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_image, src, srcImage);
   VK_FROM_HANDLE(r3v_native_image, dst, dstImage);

   for (uint32_t r = 0; r < regionCount; r++) {
      const VkImageCopy *region = &pRegions[r];
      struct r3v_native_deferred_copy *op =
         r3v_native_copy_slot(commandBuffer);
      if (op == NULL ||
          !r3v_native_transfer_source_layout_ok(srcImageLayout) ||
          !r3v_native_transfer_destination_layout_ok(dstImageLayout) ||
          !r3v_native_copy_subresource_ok(&region->srcSubresource) ||
          !r3v_native_copy_subresource_ok(&region->dstSubresource) ||
          !r3v_native_copy_rect_ok(src, region->srcOffset,
                                   region->extent) ||
          !r3v_native_copy_rect_ok(dst, region->dstOffset,
                                   region->extent) ||
          (src->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
          (dst->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0 ||
          /* vkCmdCopyImage requires size-compatible formats: the byte
           * copy moves texels of one size. */
          src->texel_bytes != dst->texel_bytes ||
          /* Vulkan leaves overlapping copy regions undefined, so a
           * same-image rectangle pair that intersects refuses. */
          (src == dst &&
           r3v_native_rects_overlap(region->srcOffset.x, region->srcOffset.y,
                                    region->dstOffset.x, region->dstOffset.y,
                                    region->extent.width,
                                    region->extent.height))) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      *op = (struct r3v_native_deferred_copy){
         .group = r3v_native_copy_group_at_record(cmd_buffer),
         .kind = R3V_NATIVE_COPY_IMAGE_TO_IMAGE,
         .src_image = src,
         .dst_image = dst,
         .src_x = (uint32_t)region->srcOffset.x,
         .src_y = (uint32_t)region->srcOffset.y,
         .dst_x = (uint32_t)region->dstOffset.x,
         .dst_y = (uint32_t)region->dstOffset.y,
         .width = region->extent.width,
         .height = region->extent.height,
      };
      cmd_buffer->deferred_copy_count++;
   }
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdCopyImageToBuffer(
   VkCommandBuffer commandBuffer,
   VkImage srcImage,
   VkImageLayout srcImageLayout,
   VkBuffer dstBuffer,
   uint32_t regionCount,
   const VkBufferImageCopy *pRegions)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_image, image, srcImage);
   VK_FROM_HANDLE(r3v_native_buffer, buffer, dstBuffer);

   for (uint32_t r = 0; r < regionCount; r++) {
      const VkBufferImageCopy *region = &pRegions[r];
      struct r3v_native_deferred_copy *op =
         r3v_native_copy_slot(commandBuffer);
      uint32_t row_length;
      if (op == NULL ||
          !r3v_native_transfer_source_layout_ok(srcImageLayout) ||
          !r3v_native_copy_subresource_ok(&region->imageSubresource) ||
          !r3v_native_copy_rect_ok(image, region->imageOffset,
                                   region->imageExtent) ||
          (image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
          !r3v_native_copy_buffer_ok(buffer,
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     region, image->texel_bytes,
                                     &row_length)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      *op = (struct r3v_native_deferred_copy){
         .group = r3v_native_copy_group_at_record(cmd_buffer),
         .kind = R3V_NATIVE_COPY_IMAGE_TO_BUFFER,
         .buffer = buffer,
         .src_image = image,
         .buffer_offset = region->bufferOffset,
         .buffer_row_length = row_length,
         .src_x = (uint32_t)region->imageOffset.x,
         .src_y = (uint32_t)region->imageOffset.y,
         .width = region->imageExtent.width,
         .height = region->imageExtent.height,
      };
      cmd_buffer->deferred_copy_count++;
   }
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdCopyQueryPoolResults(
   VkCommandBuffer commandBuffer,
   VkQueryPool queryPool,
   uint32_t firstQuery,
   uint32_t queryCount,
   VkBuffer dstBuffer,
   VkDeviceSize dstOffset,
   VkDeviceSize stride,
   VkQueryResultFlags flags)
{
   r3v_native_cmd_poison(commandBuffer);
}


VKAPI_ATTR void VKAPI_CALL
r3v_CmdDispatchIndirect(
   VkCommandBuffer commandBuffer,
   VkBuffer buffer,
   VkDeviceSize offset)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdDrawIndexedIndirect(
   VkCommandBuffer commandBuffer,
   VkBuffer buffer,
   VkDeviceSize offset,
   uint32_t drawCount,
   uint32_t stride)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdDrawIndirect(
   VkCommandBuffer commandBuffer,
   VkBuffer buffer,
   VkDeviceSize offset,
   uint32_t drawCount,
   uint32_t stride)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdEndQuery(
   VkCommandBuffer commandBuffer,
   VkQueryPool queryPool,
   uint32_t query)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_query_pool, pool, queryPool);

   struct r3v_native_query_op *op = r3v_native_query_op_slot(cmd_buffer);
   if (op == NULL || pool == NULL ||
       cmd_buffer->active_query_pool != pool ||
       cmd_buffer->active_query != query) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   *op = (struct r3v_native_query_op){
      .kind = R3V_NATIVE_QUERY_OP_MAKE_AVAILABLE,
      .pool = pool,
      .first_query = query,
      .query_count = 1,
   };
   cmd_buffer->query_op_count++;
   cmd_buffer->active_query_pool = NULL;
}

/* The execute appends the secondary's host-executed ops to the primary
 * in recorded order: each copy takes the group the primary's record
 * position fixes, and event and query ops publish with the primary's
 * own at submission.  A secondary holding state the append cannot
 * carry -- an IB, a deferred draw or dispatch, dynamic
 * viewport/scissor, an open query span -- or a poisoned recording
 * refuses.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdExecuteCommands(
   VkCommandBuffer commandBuffer,
   uint32_t commandBufferCount,
   const VkCommandBuffer *pCommandBuffers)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   if (cmd_buffer->pass_target != NULL ||
       cmd_buffer->active_query_pool != NULL ||
       cmd_buffer->vk.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   for (uint32_t i = 0; i < commandBufferCount; i++) {
      VK_FROM_HANDLE(r3v_native_cmd_buffer, secondary, pCommandBuffers[i]);
      if (secondary == NULL ||
          secondary->vk.level != VK_COMMAND_BUFFER_LEVEL_SECONDARY ||
          secondary->vk.record_result != VK_SUCCESS ||
          secondary->ib_size_dwords != 0 ||
          secondary->deferred_draw.pending || secondary->draw_recorded ||
          secondary->deferred_dispatch.pending ||
          secondary->active_query_pool != NULL ||
          secondary->viewport_set || secondary->scissor_set) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      if (cmd_buffer->event_op_count + secondary->event_op_count >
             R3V_NATIVE_EVENT_OP_MAX ||
          cmd_buffer->query_op_count + secondary->query_op_count >
             R3V_NATIVE_QUERY_OP_MAX) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      for (uint32_t c = 0; c < secondary->deferred_copy_count; c++) {
         struct r3v_native_deferred_copy *op =
            r3v_native_copy_slot(commandBuffer);
         if (op == NULL)
            return;
         *op = secondary->deferred_copies[c];
         op->group = r3v_native_copy_group_at_record(cmd_buffer);
         /* Each recording frees its own update_data, so the appended
          * op takes a copy rather than aliasing the secondary's. */
         if (op->update_data != NULL) {
            uint8_t *data = vk_alloc(&cmd_buffer->vk.pool->alloc,
                                     op->size, 8,
                                     VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
            if (data == NULL) {
               vk_command_buffer_set_error(&cmd_buffer->vk,
                                           VK_ERROR_OUT_OF_HOST_MEMORY);
               r3v_native_cmd_poison(commandBuffer);
               return;
            }
            memcpy(data, op->update_data, op->size);
            op->update_data = data;
         }
         cmd_buffer->deferred_copy_count++;
      }
      for (uint32_t e = 0; e < secondary->event_op_count; e++) {
         cmd_buffer->event_ops[cmd_buffer->event_op_count++] =
            secondary->event_ops[e];
      }
      for (uint32_t q = 0; q < secondary->query_op_count; q++) {
         cmd_buffer->query_ops[cmd_buffer->query_op_count++] =
            secondary->query_ops[q];
      }
   }
}

/* The fill is a dword-pattern store through the host mapping of the
 * bound range.  VK_WHOLE_SIZE runs from the offset to the buffer end
 * truncated to whole dwords; an explicit size is a dword multiple
 * inside the buffer.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdFillBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer dstBuffer,
   VkDeviceSize dstOffset,
   VkDeviceSize size,
   uint32_t data)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_buffer, dst, dstBuffer);

   struct r3v_native_deferred_copy *op =
      r3v_native_copy_slot(commandBuffer);
   VkDeviceSize fill_size = 0;
   if (op != NULL && dst != NULL && dstOffset % 4 == 0 &&
       dstOffset <= dst->vk.size) {
      fill_size = size == VK_WHOLE_SIZE
                     ? (dst->vk.size - dstOffset) & ~(VkDeviceSize)3
                     : size;
   }
   if (op == NULL || fill_size == 0 || fill_size % 4 != 0 ||
       !r3v_native_copy_buffer_range_ok(
          dst, VK_BUFFER_USAGE_TRANSFER_DST_BIT, dstOffset, fill_size)) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   *op = (struct r3v_native_deferred_copy){
      .group = r3v_native_copy_group_at_record(cmd_buffer),
      .kind = R3V_NATIVE_COPY_FILL_BUFFER,
      .dst_buffer = dst,
      .dst_offset = dstOffset,
      .size = fill_size,
      .clear_dword = util_cpu_to_le32(data),
   };
   cmd_buffer->deferred_copy_count++;
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdNextSubpass(
   VkCommandBuffer commandBuffer,
   VkSubpassContents contents)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdPipelineBarrier(
   VkCommandBuffer commandBuffer,
   VkPipelineStageFlags srcStageMask,
   VkPipelineStageFlags dstStageMask,
   VkDependencyFlags dependencyFlags,
   uint32_t memoryBarrierCount,
   const VkMemoryBarrier *pMemoryBarriers,
   uint32_t bufferMemoryBarrierCount,
   const VkBufferMemoryBarrier *pBufferMemoryBarriers,
   uint32_t imageMemoryBarrierCount,
   const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   /* The deferred ops execute in recorded order on one host thread and
    * every destination publishes before the submission retires.  The
    * executors and publication symbols resolve with (rg --fixed-strings
    * r3v_native_cmd_buffer_execute_deferred_copies src/amd/r300/vulkan/;
    * rg --fixed-strings r3v_native_cmd_buffer_execute_deferred_draw
    * src/amd/r300/vulkan/; rg --fixed-strings radeon_drm_vk_bo_cache_sync
    * src/amd/r300/vulkan/), so
    * execution dependencies, availability, and visibility all hold by
    * construction and the barrier records nothing.  Admission still
    * bounds the vocabulary to what that construction covers: barriers
    * outside a render pass (the pass's one draw has no self-dependency
    * lowering), no ownership transfer -- the device exposes one queue
    * family, so an ownership-transferring pair names a family that
    * does not exist -- and image barriers over the qualified transfer or
    * render-family color subresource with its supported layout vocabulary.
    * Equal queue-family fields still name either the native family (0) or
    * the no-ownership-transfer sentinel.
    * Vulkan 1.3 `vkCmdPipelineBarrier` valid usage binds image layout and
    * queue ownership to r3v_native_image_barrier_layouts_ok and
    * r3v_native_queue_family_pair_ok.  The copy-command valid-usage rules
    * bind transfer layouts to image usage through
    * r3v_native_image_layout_ok, which composes
    * r3v_native_transfer_source_layout_ok,
    * r3v_native_transfer_destination_layout_ok, and
    * r3v_native_render_layout_ok.  Symbol discovery uses
    * `(rg --fixed-strings r3v_native_image_barrier_layouts_ok
    * src/amd/r300/vulkan/)`, `(rg --fixed-strings r3v_native_image_layout_ok
    * src/amd/r300/vulkan/)`, and `(rg --fixed-strings
    * r3v_native_queue_family_pair_ok src/amd/r300/vulkan/)`.
    */
   if (cmd_buffer->pass_target != NULL) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   for (uint32_t i = 0; i < bufferMemoryBarrierCount; i++) {
      const VkBufferMemoryBarrier *barrier = &pBufferMemoryBarriers[i];
      if (!r3v_native_queue_family_pair_ok(barrier->srcQueueFamilyIndex,
                                            barrier->dstQueueFamilyIndex)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
   }
   for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
      const VkImageMemoryBarrier *barrier = &pImageMemoryBarriers[i];
      if (!r3v_native_queue_family_pair_ok(barrier->srcQueueFamilyIndex,
                                            barrier->dstQueueFamilyIndex) ||
          !r3v_native_image_barrier_layouts_ok(barrier)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
   }
}

/* Every pipeline layout the pipeline admissions accept carries zero
 * push-constant ranges, so no layout a push can name exists and the
 * recording refuses.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdPushConstants(
   VkCommandBuffer commandBuffer,
   VkPipelineLayout layout,
   VkShaderStageFlags stageFlags,
   uint32_t offset,
   uint32_t size,
   const void *pValues)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdResetEvent(
   VkCommandBuffer commandBuffer,
   VkEvent event,
   VkPipelineStageFlags stageMask)
{
   (void)stageMask;
   r3v_native_record_event_op(commandBuffer, event,
                              R3V_NATIVE_EVENT_OP_RESET);
}

/* The reset returns each named query to unavailable at submission, in
 * recorded order with the ends around it.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdResetQueryPool(
   VkCommandBuffer commandBuffer,
   VkQueryPool queryPool,
   uint32_t firstQuery,
   uint32_t queryCount)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_query_pool, pool, queryPool);

   struct r3v_native_query_op *op = r3v_native_query_op_slot(cmd_buffer);
   if (op == NULL || pool == NULL || cmd_buffer->pass_target != NULL ||
       cmd_buffer->active_query_pool == pool ||
       firstQuery >= pool->query_count ||
       queryCount > pool->query_count - firstQuery) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   *op = (struct r3v_native_query_op){
      .kind = R3V_NATIVE_QUERY_OP_RESET,
      .pool = pool,
      .first_query = firstQuery,
      .query_count = queryCount,
   };
   cmd_buffer->query_op_count++;
}

/* Resolve reads a multisampled source, and image creation admits
 * VK_SAMPLE_COUNT_1_BIT alone, so no image a resolve can name exists
 * and the recording refuses.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdResolveImage(
   VkCommandBuffer commandBuffer,
   VkImage srcImage,
   VkImageLayout srcImageLayout,
   VkImage dstImage,
   VkImageLayout dstImageLayout,
   uint32_t regionCount,
   const VkImageResolve *pRegions)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetBlendConstants(
   VkCommandBuffer commandBuffer,
   const float blendConstants[4])
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetDepthBias(
   VkCommandBuffer commandBuffer,
   float depthBiasConstantFactor,
   float depthBiasClamp,
   float depthBiasSlopeFactor)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetDepthBounds(
   VkCommandBuffer commandBuffer,
   float minDepthBounds,
   float maxDepthBounds)
{
   r3v_native_cmd_poison(commandBuffer);
}

/* The recorded set executes at submission after every earlier
 * recorded operation has completed on the one synchronous timeline,
 * so any stage mask's completion is already implied and the mask
 * itself records nothing.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetEvent(
   VkCommandBuffer commandBuffer,
   VkEvent event,
   VkPipelineStageFlags stageMask)
{
   (void)stageMask;
   r3v_native_record_event_op(commandBuffer, event,
                              R3V_NATIVE_EVENT_OP_SET);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetLineWidth(
   VkCommandBuffer commandBuffer,
   float lineWidth)
{
   r3v_native_cmd_poison(commandBuffer);
}

/* The one scissor slot records for the dynamic-state pipeline; the
 * draw holds the value to the cell shape, so the set itself admits any
 * rectangle.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetScissor(
   VkCommandBuffer commandBuffer,
   uint32_t firstScissor,
   uint32_t scissorCount,
   const VkRect2D *pScissors)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   if (firstScissor != 0 || scissorCount != 1) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   cmd_buffer->dynamic_scissor = pScissors[0];
   cmd_buffer->scissor_set = true;
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetStencilCompareMask(
   VkCommandBuffer commandBuffer,
   VkStencilFaceFlags faceMask,
   uint32_t compareMask)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetStencilReference(
   VkCommandBuffer commandBuffer,
   VkStencilFaceFlags faceMask,
   uint32_t reference)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetStencilWriteMask(
   VkCommandBuffer commandBuffer,
   VkStencilFaceFlags faceMask,
   uint32_t writeMask)
{
   r3v_native_cmd_poison(commandBuffer);
}

/* The one viewport slot records for the dynamic-state pipeline,
 * mirroring the scissor's record-then-judge contract.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetViewport(
   VkCommandBuffer commandBuffer,
   uint32_t firstViewport,
   uint32_t viewportCount,
   const VkViewport *pViewports)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   if (firstViewport != 0 || viewportCount != 1) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   cmd_buffer->dynamic_viewport = pViewports[0];
   cmd_buffer->viewport_set = true;
}

/* The update captures the application bytes at record into storage the
 * recording owns, so the source may be dead at submission; the Vulkan
 * inline-update ceiling is 65536 bytes and offset and size are dword
 * multiples.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdUpdateBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer dstBuffer,
   VkDeviceSize dstOffset,
   VkDeviceSize dataSize,
   const void *pData)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_buffer, dst, dstBuffer);

   struct r3v_native_deferred_copy *op =
      r3v_native_copy_slot(commandBuffer);
   if (op == NULL || pData == NULL || dataSize == 0 ||
       dataSize > 65536 || dataSize % 4 != 0 || dstOffset % 4 != 0 ||
       !r3v_native_copy_buffer_range_ok(
          dst, VK_BUFFER_USAGE_TRANSFER_DST_BIT, dstOffset, dataSize)) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   uint8_t *data = vk_alloc(&cmd_buffer->vk.pool->alloc, dataSize, 8,
                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (data == NULL) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   memcpy(data, pData, dataSize);
   *op = (struct r3v_native_deferred_copy){
      .group = r3v_native_copy_group_at_record(cmd_buffer),
      .kind = R3V_NATIVE_COPY_UPDATE_BUFFER,
      .dst_buffer = dst,
      .dst_offset = dstOffset,
      .size = dataSize,
      .update_data = data,
   };
   cmd_buffer->deferred_copy_count++;
}

/* The wait carries synchronization alone: barrier work travels
 * through vkCmdPipelineBarrier's admitted vocabulary, so a wait
 * naming barriers refuses.  At submission the wait checks its events
 * in recorded order; an unsignaled event is a dependency no later
 * work can satisfy on the synchronous timeline, and the submission
 * reports device loss.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdWaitEvents(
   VkCommandBuffer commandBuffer,
   uint32_t eventCount,
   const VkEvent *pEvents,
   VkPipelineStageFlags srcStageMask,
   VkPipelineStageFlags dstStageMask,
   uint32_t memoryBarrierCount,
   const VkMemoryBarrier *pMemoryBarriers,
   uint32_t bufferMemoryBarrierCount,
   const VkBufferMemoryBarrier *pBufferMemoryBarriers,
   uint32_t imageMemoryBarrierCount,
   const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   (void)srcStageMask;
   (void)dstStageMask;
   if (eventCount == 0 || memoryBarrierCount != 0 ||
       bufferMemoryBarrierCount != 0 || imageMemoryBarrierCount != 0) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   for (uint32_t i = 0; i < eventCount; i++)
      r3v_native_record_event_op(commandBuffer, pEvents[i],
                                 R3V_NATIVE_EVENT_OP_WAIT);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdWriteTimestamp(
   VkCommandBuffer commandBuffer,
   VkPipelineStageFlagBits pipelineStage,
   VkQueryPool queryPool,
   uint32_t query)
{
   r3v_native_cmd_poison(commandBuffer);
}

/* The one image shape is a linear color target with its whole
 * allocation committed at bind, so its sparse requirement set is
 * empty.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_GetImageSparseMemoryRequirements(
   VkDevice _device, VkImage image,
   uint32_t *pSparseMemoryRequirementCount,
   VkSparseImageMemoryRequirements *pSparseMemoryRequirements)
{
   *pSparseMemoryRequirementCount = 0;
}
