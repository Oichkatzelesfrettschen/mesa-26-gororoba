/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V fail-closed recording surface: every core Vulkan 1.0 command
 * outside the qualified draw subset poisons its command buffer.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"

#include "amd/r300/common/r300_reg.h"
#include "amd/r300/common/radeon_legacy_2d_reg.h"

#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <limits.h>
#include <stdlib.h>
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

static uint32_t
r3v_native_image_barrier_visibility(VkPipelineStageFlags stages,
                                    VkAccessFlags access)
{
   uint32_t visibility = 0u;
   if ((access & (VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT)) != 0u ||
       (stages & VK_PIPELINE_STAGE_HOST_BIT) != 0u)
      visibility |= R3V_NATIVE_IMAGE_VISIBLE_HOST;
   if ((access & (VK_ACCESS_TRANSFER_READ_BIT |
                  VK_ACCESS_TRANSFER_WRITE_BIT)) != 0u ||
       (stages & VK_PIPELINE_STAGE_TRANSFER_BIT) != 0u)
      visibility |= R3V_NATIVE_IMAGE_VISIBLE_RB2D;
   if ((access & (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)) !=
          0u ||
       (stages & (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)) != 0u)
      visibility |= R3V_NATIVE_IMAGE_VISIBLE_RB3D;
   if ((access & (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)) != 0u ||
       (stages & (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)) != 0u)
      visibility |= R3V_NATIVE_IMAGE_VISIBLE_ZB;
   return visibility;
}

static VkResult
r3v_native_append_global_dependency(
   struct r3v_native_cmd_buffer *cmd_buffer, uint32_t *ib_position_out)
{
   *ib_position_out = cmd_buffer->ib_size_dwords;
   const uint32_t dependency[] = {
      CP_PACKET0(R300_ZB_ZCACHE_CTLSTAT, 0),
      R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
         R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE,
      CP_PACKET0(R300_RB3D_DSTCACHE_CTLSTAT, 0),
      R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
         R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS,
      CP_PACKET0(RADEON_DSTCACHE_CTLSTAT, 0),
      RADEON_RB2D_DC_FLUSH_ALL,
      CP_PACKET0(RADEON_WAIT_UNTIL, 0),
      RADEON_WAIT_3D_IDLECLEAN | RADEON_WAIT_2D_IDLECLEAN |
         RADEON_WAIT_HOST_IDLECLEAN | RADEON_WAIT_DMA_GUI_IDLE,
   };
   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);
   VkResult result = r3v_native_cmd_buffer_append_raw_ib(
      device, cmd_buffer, dependency, ARRAY_SIZE(dependency), NULL, 0u,
      NULL, NULL, 0u);
   if (result == VK_SUCCESS)
      cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_ORDERED_IMAGE_COMPOSITION;
   return result;
}

static VkResult
r3v_native_append_depth_image_dependency(
   struct r3v_native_cmd_buffer *cmd_buffer,
   const struct r3v_native_image *image, uint32_t *ib_position_out)
{
   *ib_position_out = cmd_buffer->ib_size_dwords;
   if (image == NULL || !image->depth_family)
      return VK_SUCCESS;
   return r3v_native_append_global_dependency(cmd_buffer, ib_position_out);
}

static bool
r3v_native_depth_layout_ok(const struct r3v_native_image *image,
                           VkImageLayout layout)
{
   if (layout == VK_IMAGE_LAYOUT_GENERAL)
      return true;
   if ((image->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0u &&
       (layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL))
      return true;
   return ((image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0u &&
           layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) ||
          ((image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0u &&
           layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

static bool
r3v_native_image_layout_ok(const struct r3v_native_image *image,
                           VkImageLayout layout)
{
   if (layout == VK_IMAGE_LAYOUT_GENERAL)
      return true;
   if (image->depth_family)
      return r3v_native_depth_layout_ok(image, layout);
   /* Each usage bit brings its own layouts, so a render target that also
    * carries transfer usage reaches both vocabularies.
    */
   if ((image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
       r3v_native_render_layout_ok(layout))
      return true;
   if ((image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 &&
       layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
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
r3v_native_image_barrier_range_ok(const struct r3v_native_image *image,
                                  const VkImageSubresourceRange *range)
{
   const VkImageAspectFlags required_aspects = image->depth_family
      ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
      : VK_IMAGE_ASPECT_COLOR_BIT;
   return range->aspectMask == required_aspects &&
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
          r3v_native_image_barrier_range_ok(image,
                                             &barrier->subresourceRange) &&
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

   /* A recorded dispatch and a recorded copy in one command buffer is a
    * shape neither executor orders: r3v_CmdDispatch refuses a buffer that
    * already carries a copy, so the copy side holds the same rule and the
    * pair is refused from whichever side arrives second.
    * Symbol discovery uses (rg --fixed-strings deferred_dispatch.pending
    * src/amd/r300/vulkan/).
    */
   if (cmd_buffer->deferred_dispatch.pending) {
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

static void
r3v_native_commit_deferred_copy(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   const uint32_t copy_index = cmd_buffer->deferred_copy_count++;
   if (r3v_native_cmd_buffer_append_ordered_operation(
          cmd_buffer, &(struct r3v_native_ordered_operation){
                         .kind = R3V_NATIVE_ORDERED_OPERATION_HOST_COPY,
                         .ib_position_dwords = cmd_buffer->ib_size_dwords,
                         .payload.host_copy = {
                            .deferred_copy_index = copy_index,
                         },
                      }) != VK_SUCCESS)
      r3v_native_cmd_poison(commandBuffer);
}

/* The record position that fixes a copy's execution group: a pending
 * deferred draw means the pass already recorded its load-op clear, so
 * the copy follows the draw; every earlier copy precedes it.
 */
static enum r3v_native_copy_group
r3v_native_copy_group_at_record(const struct r3v_native_cmd_buffer *cmd_buffer)
{
   return cmd_buffer->deferred_draw_count != 0
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
   /* bufferImageHeight is the distance to the next slice.  A depth-one
    * region accesses imageExtent.height rows in the current slice, so the
    * final row uses that extent while the slice stride remains available to
    * the multi-slice layout contract. */
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

static bool
r3v_native_buffer_barrier_range_ok(const struct r3v_native_buffer *buffer,
                                   VkDeviceSize offset, VkDeviceSize size,
                                   VkDeviceSize *normalized_size)
{
   if (buffer == NULL || buffer->memory == NULL ||
       buffer->offset > buffer->memory->bo.size ||
       buffer->vk.size > buffer->memory->bo.size - buffer->offset ||
       offset > buffer->vk.size)
      return false;
   if (size == VK_WHOLE_SIZE)
      size = buffer->vk.size - offset;
   if (size > buffer->vk.size - offset || normalized_size == NULL)
      return false;
   *normalized_size = size;
   return true;
}

static bool
r3v_native_bound_ranges_overlap(const struct r3v_native_memory *source_memory,
                                uint64_t source_offset,
                                const struct r3v_native_memory *destination_memory,
                                uint64_t destination_offset, uint64_t byte_count)
{
   if (source_memory == NULL || destination_memory == NULL ||
       (source_memory != destination_memory &&
        source_memory->bo.handle != destination_memory->bo.handle))
      return false;
   if (source_offset > UINT64_MAX - byte_count ||
       destination_offset > UINT64_MAX - byte_count)
      return true;
   return source_offset < destination_offset + byte_count &&
          destination_offset < source_offset + byte_count;
}

static bool
r3v_native_linear_image_ranges_disjoint(
   const struct r3v_native_image *source, uint32_t source_x,
   uint32_t source_y, const struct r3v_native_image *destination,
   uint32_t destination_x, uint32_t destination_y, uint32_t width,
   uint32_t height)
{
   if (source == NULL || destination == NULL || source->memory == NULL ||
       destination->memory == NULL ||
       (source->memory != destination->memory &&
        source->memory->bo.handle != destination->memory->bo.handle))
      return true;
   const uint64_t byte_count =
      (uint64_t)width * source->texel_bytes;
   for (uint32_t row = 0u; row < height; row++) {
      const uint64_t source_offset =
         source->memory_offset +
         (uint64_t)(source_y + row) * source->row_pitch_bytes +
         (uint64_t)source_x * source->texel_bytes;
      const uint64_t destination_offset =
         destination->memory_offset +
         (uint64_t)(destination_y + row) * destination->row_pitch_bytes +
         (uint64_t)destination_x * destination->texel_bytes;
      if (r3v_native_bound_ranges_overlap(
             source->memory, source_offset, destination->memory,
             destination_offset, byte_count))
         return false;
   }
   return true;
}

static bool
r3v_native_linear_image_buffer_ranges_disjoint(
   const struct r3v_native_image *image, uint32_t image_x,
   uint32_t image_y, const struct r3v_native_buffer *buffer,
   uint64_t buffer_offset, uint32_t buffer_row_length, uint32_t width,
   uint32_t height, bool buffer_to_image)
{
   if (image == NULL || buffer == NULL || image->memory == NULL ||
       buffer->memory == NULL ||
       (image->memory != buffer->memory &&
        image->memory->bo.handle != buffer->memory->bo.handle))
      return true;
   const uint64_t byte_count = (uint64_t)width * image->texel_bytes;
   for (uint32_t row = 0u; row < height; row++) {
      const uint64_t image_offset =
         image->memory_offset +
         (uint64_t)(image_y + row) * image->row_pitch_bytes +
         (uint64_t)image_x * image->texel_bytes;
      const uint64_t buffer_row =
         buffer->offset + buffer_offset +
         (uint64_t)row * buffer_row_length * image->texel_bytes;
      const uint64_t source_offset = buffer_to_image ? buffer_row : image_offset;
      const uint64_t destination_offset = buffer_to_image ? image_offset : buffer_row;
      if (r3v_native_bound_ranges_overlap(
             buffer_to_image ? buffer->memory : image->memory, source_offset,
             buffer_to_image ? image->memory : buffer->memory, destination_offset,
             byte_count))
         return false;
   }
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
   const uint32_t event_index = cmd_buffer->event_op_count++;
   cmd_buffer->event_ops[event_index] =
      (struct r3v_native_event_op){ .kind = kind, .event = event };
   if (r3v_native_cmd_buffer_append_ordered_operation(
          cmd_buffer, &(struct r3v_native_ordered_operation){
                         .kind = R3V_NATIVE_ORDERED_OPERATION_EVENT,
                         .ib_position_dwords = cmd_buffer->ib_size_dwords,
                         .payload.event = { .event_index = event_index },
                      }) != VK_SUCCESS)
      r3v_native_cmd_poison(commandBuffer);
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
      r3v_native_commit_deferred_copy(commandBuffer);
   }
}

/* In-pass attachment clears preserve command order at recording. Color
 * rectangles remain deferred beside the host color path and carry an
 * ordered operation so a clear before or after a draw executes at that
 * position. Depth/stencil rectangles become resolver-addressed RB2D
 * commands at their stream position, with a component write mask
 * preserving the unselected aspect.
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
   /* An open pass_target means CmdBeginRenderPass filled the last
    * record, which is the pass this clear lands in.
    */
   struct r3v_native_deferred_draw *draw =
      cmd_buffer->deferred_draw_count != 0
         ? &cmd_buffer->deferred_draws[cmd_buffer->deferred_draw_count - 1]
         : &cmd_buffer->deferred_draws[0];
   struct r3v_native_image *depth_target = cmd_buffer->pass_depth_target;
   if (depth_target == NULL && cmd_buffer->pass_target != NULL &&
       cmd_buffer->pass_target->depth_family)
      depth_target = cmd_buffer->pass_target;

   if (cmd_buffer->pass_target == NULL ||
       (attachmentCount != 0u && pAttachments == NULL) ||
       (rectCount != 0u && pRects == NULL) ||
       draw->clear_rect_count > R3V_NATIVE_PASS_CLEAR_RECT_MAX) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   uint64_t color_rect_count = draw->clear_rect_count;
   for (uint32_t a = 0; a < attachmentCount; a++) {
      const VkClearAttachment *att = &pAttachments[a];
      const bool color = att->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
      const bool depth_stencil =
         att->aspectMask != 0u &&
         (att->aspectMask & ~(VK_IMAGE_ASPECT_DEPTH_BIT |
                              VK_IMAGE_ASPECT_STENCIL_BIT)) == 0u;
      if ((!color && !depth_stencil) ||
          (color && att->colorAttachment != 0) ||
          (color && (cmd_buffer->pass_target == NULL ||
                     cmd_buffer->pass_target->depth_family)) ||
          (depth_stencil && depth_target == NULL)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      if (depth_stencil &&
          (att->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0u &&
          !r3v_native_depth_clear_code(att->clearValue.depthStencil.depth,
                                       &(uint32_t){0})) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      for (uint32_t r = 0; r < rectCount; r++) {
         const VkClearRect *rect = &pRects[r];
         const VkRect2D *area = &rect->rect;
         if (rect->baseArrayLayer != 0 || rect->layerCount != 1 ||
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
         if (color && ++color_rect_count > R3V_NATIVE_PASS_CLEAR_RECT_MAX) {
            r3v_native_cmd_poison(commandBuffer);
            return;
         }
      }
   }

   for (uint32_t a = 0; a < attachmentCount; a++) {
      const VkClearAttachment *att = &pAttachments[a];
      const bool color = att->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
      if (color) {
         const uint32_t first_rect = draw->clear_rect_count;
         const uint32_t packed =
            r300_tcl_bypass_triangle_pack_unorm8_dword(
               cmd_buffer->pass_target->lanes,
               att->clearValue.color.float32);
         for (uint32_t r = 0; r < rectCount; r++) {
            const VkRect2D *area = &pRects[r].rect;
            draw->clear_rects[draw->clear_rect_count++] =
            (struct r3v_native_pass_clear_rect){
               .x = (uint32_t)area->offset.x,
               .y = (uint32_t)area->offset.y,
               .width = area->extent.width,
               .height = area->extent.height,
               .aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT,
               .dword = util_cpu_to_le32(packed),
            };
         }
         if (r3v_native_cmd_buffer_append_ordered_operation(
                cmd_buffer, &(struct r3v_native_ordered_operation){
                   .kind = R3V_NATIVE_ORDERED_OPERATION_COLOR_CLEAR,
                   .ib_position_dwords = cmd_buffer->ib_size_dwords,
                   .payload.color_clear = {
                      .deferred_draw_index =
                         cmd_buffer->deferred_draw_count - 1u,
                      .first_rect = first_rect,
                      .rect_count = rectCount,
                   },
                }) != VK_SUCCESS) {
            r3v_native_cmd_poison(commandBuffer);
            return;
         }
         continue;
      }

      uint32_t depth_code = 0u;
      uint32_t aspect_mask = 0u;
      if ((att->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0u) {
         (void)r3v_native_depth_clear_code(att->clearValue.depthStencil.depth,
                                           &depth_code);
         aspect_mask |= R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH;
      }
      if ((att->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0u)
         aspect_mask |= R300_ZB_COMBINED_CLEAR_ASPECT_STENCIL;
      const uint32_t stencil =
         att->clearValue.depthStencil.stencil & UINT8_MAX;
      for (uint32_t r = 0; r < rectCount; r++) {
         const VkRect2D *area = &pRects[r].rect;
         if (r3v_native_record_depth_image_clear_logical_rect(
                commandBuffer,
                r3v_native_image_to_handle(depth_target),
                (uint32_t)area->offset.x, (uint32_t)area->offset.y,
                area->extent.width, area->extent.height, aspect_mask,
                depth_code, stencil) != VK_SUCCESS) {
            r3v_native_cmd_poison(commandBuffer);
            return;
         }
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
      r3v_native_commit_deferred_copy(commandBuffer);
   }
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdClearDepthStencilImage(
   VkCommandBuffer commandBuffer,
   VkImage image,
   VkImageLayout imageLayout,
   const VkClearDepthStencilValue *pDepthStencil,
   uint32_t rangeCount,
   const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(r3v_native_image, native_image, image);
   if (native_image == NULL || !native_image->depth_family ||
       native_image->memory == NULL ||
       !(native_image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
       !r3v_native_transfer_destination_layout_ok(imageLayout) ||
       pDepthStencil == NULL || rangeCount == 0u || pRanges == NULL) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }

   const VkImageAspectFlags valid_aspects =
      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
   VkImageAspectFlags requested_aspects = 0u;
   for (uint32_t range_index = 0u; range_index < rangeCount; range_index++) {
      const VkImageSubresourceRange *range = &pRanges[range_index];
      if (range->aspectMask == 0u ||
          (range->aspectMask & ~valid_aspects) != 0u ||
          range->baseMipLevel != 0u ||
          (range->levelCount != 1u &&
           range->levelCount != VK_REMAINING_MIP_LEVELS) ||
          range->baseArrayLayer != 0u ||
          (range->layerCount != 1u &&
           range->layerCount != VK_REMAINING_ARRAY_LAYERS)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      requested_aspects |= range->aspectMask;
   }

   uint32_t aspect_mask = 0u;
   uint32_t depth_code = 0u;
   uint32_t stencil = 0u;
   if ((requested_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0u) {
      aspect_mask |= R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH;
      if (!r3v_native_depth_clear_code(pDepthStencil->depth, &depth_code)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
   }
   if ((requested_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0u) {
      aspect_mask |= R300_ZB_COMBINED_CLEAR_ASPECT_STENCIL;
      /* The API value is uint32_t, while the packed D24S8 destination
       * carries the low eight bits of stencil.  The complete range and
       * aspect request was validated above before conversion. */
      stencil = pDepthStencil->stencil & UINT8_MAX;
   }

   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   if (r3v_native_cmd_buffer_require_image_layout(
          cmd_buffer, native_image, imageLayout,
          R3V_NATIVE_IMAGE_PRODUCER_RB2D, true) != VK_SUCCESS ||
       r3v_native_record_depth_image_clear(
          commandBuffer, image, aspect_mask, depth_code, stencil) !=
       VK_SUCCESS)
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

   if (pRegions == NULL && regionCount != 0u) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   for (uint32_t r = 0u; r < regionCount; r++) {
      const VkBufferCopy *region = &pRegions[r];
      if (!r3v_native_copy_buffer_range_ok(
             src, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, region->srcOffset,
             region->size) ||
          !r3v_native_copy_buffer_range_ok(
             dst, VK_BUFFER_USAGE_TRANSFER_DST_BIT, region->dstOffset,
             region->size) ||
          r3v_native_bound_ranges_overlap(
             src != NULL ? src->memory : NULL,
             src != NULL && src->memory != NULL &&
                   src->offset <= UINT64_MAX - region->srcOffset
                ? src->offset + region->srcOffset
                : UINT64_MAX,
             dst != NULL ? dst->memory : NULL,
             dst != NULL && dst->memory != NULL &&
                   dst->offset <= UINT64_MAX - region->dstOffset
                ? dst->offset + region->dstOffset
                : UINT64_MAX,
             region->size)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
   }
   for (uint32_t r = 0; r < regionCount; r++) {
      const VkBufferCopy *region = &pRegions[r];
      struct r3v_native_deferred_copy *op =
         r3v_native_copy_slot(commandBuffer);
      if (op == NULL) {
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
      r3v_native_commit_deferred_copy(commandBuffer);
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

   if (pRegions == NULL && regionCount != 0u) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   if (regionCount == 0u)
      return;
   if (image != NULL && image->depth_family) {
      for (uint32_t r = 0u; r < regionCount; r++) {
         if (!r3v_native_validate_depth_image_copy(
                commandBuffer, srcBuffer, dstImage, &pRegions[r],
                dstImageLayout, true)) {
            r3v_native_cmd_poison(commandBuffer);
            return;
         }
      }
      if (r3v_native_cmd_buffer_require_image_layout(
             cmd_buffer, image, dstImageLayout,
             R3V_NATIVE_IMAGE_PRODUCER_RB2D, true) != VK_SUCCESS) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      for (uint32_t r = 0u; r < regionCount; r++) {
         if (r3v_native_record_depth_image_copy(
                commandBuffer, srcBuffer, dstImage, &pRegions[r],
                dstImageLayout, true) != VK_SUCCESS) {
            r3v_native_cmd_poison(commandBuffer);
            return;
         }
      }
      return;
   }

   for (uint32_t r = 0u; r < regionCount; r++) {
      uint32_t row_length;
      const VkBufferImageCopy *region = &pRegions[r];
      if (!r3v_native_transfer_destination_layout_ok(dstImageLayout) ||
          !r3v_native_copy_subresource_ok(&region->imageSubresource) ||
          !r3v_native_copy_rect_ok(image, region->imageOffset,
                                   region->imageExtent) ||
          (image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0 ||
          !r3v_native_copy_buffer_ok(buffer,
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     region, image->texel_bytes,
                                     &row_length) ||
          !r3v_native_linear_image_buffer_ranges_disjoint(
             image, (uint32_t)region->imageOffset.x,
             (uint32_t)region->imageOffset.y, buffer,
             region->bufferOffset, row_length, region->imageExtent.width,
             region->imageExtent.height, true)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
   }
   for (uint32_t r = 0; r < regionCount; r++) {
      const VkBufferImageCopy *region = &pRegions[r];
      struct r3v_native_deferred_copy *op =
         r3v_native_copy_slot(commandBuffer);
      uint32_t row_length;
      if (op == NULL) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      row_length = region->bufferRowLength != 0u
                      ? region->bufferRowLength
                      : region->imageExtent.width;
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
      r3v_native_commit_deferred_copy(commandBuffer);
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

   if (pRegions == NULL && regionCount != 0u) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   if (regionCount == 0u)
      return;

   if ((src != NULL && src->depth_family) ||
       (dst != NULL && dst->depth_family)) {
      if (pRegions == NULL && regionCount != 0u) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      if (regionCount == 0u)
         return;
      if (src == NULL || dst == NULL || !src->depth_family ||
          !dst->depth_family ||
          (regionCount != 0u &&
           !r3v_native_validate_depth_image_to_image_copy(
              srcImage, srcImageLayout, dstImage, dstImageLayout,
              &pRegions[0]))) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      for (uint32_t r = 1u; r < regionCount; r++) {
         if (!r3v_native_validate_depth_image_to_image_copy(
                srcImage, srcImageLayout, dstImage, dstImageLayout,
                &pRegions[r])) {
            r3v_native_cmd_poison(commandBuffer);
            return;
         }
      }
      if (r3v_native_cmd_buffer_require_image_layout(
             cmd_buffer, src, srcImageLayout,
             R3V_NATIVE_IMAGE_PRODUCER_RB2D, false) != VK_SUCCESS ||
          r3v_native_cmd_buffer_require_image_layout(
             cmd_buffer, dst, dstImageLayout,
             R3V_NATIVE_IMAGE_PRODUCER_RB2D, true) != VK_SUCCESS) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      for (uint32_t r = 0; r < regionCount; r++) {
         if (r3v_native_record_depth_image_to_image_copy(
                commandBuffer, srcImage, srcImageLayout, dstImage,
                dstImageLayout, &pRegions[r]) != VK_SUCCESS) {
            r3v_native_cmd_poison(commandBuffer);
            break;
         }
      }
      return;
   }

   for (uint32_t r = 0u; r < regionCount; r++) {
      const VkImageCopy *region = &pRegions[r];
      if (!r3v_native_transfer_source_layout_ok(srcImageLayout) ||
          !r3v_native_transfer_destination_layout_ok(dstImageLayout) ||
          !r3v_native_copy_subresource_ok(&region->srcSubresource) ||
          !r3v_native_copy_subresource_ok(&region->dstSubresource) ||
          !r3v_native_copy_rect_ok(src, region->srcOffset,
                                   region->extent) ||
          !r3v_native_copy_rect_ok(dst, region->dstOffset,
                                   region->extent) ||
          (src->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
          (dst->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0 ||
          src->texel_bytes != dst->texel_bytes ||
          !r3v_native_linear_image_ranges_disjoint(
             src, (uint32_t)region->srcOffset.x,
             (uint32_t)region->srcOffset.y, dst,
             (uint32_t)region->dstOffset.x,
             (uint32_t)region->dstOffset.y, region->extent.width,
             region->extent.height) ||
          (src == dst &&
           r3v_native_rects_overlap(region->srcOffset.x, region->srcOffset.y,
                                    region->dstOffset.x, region->dstOffset.y,
                                    region->extent.width,
                                    region->extent.height))) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
   }
   for (uint32_t r = 0; r < regionCount; r++) {
      const VkImageCopy *region = &pRegions[r];
      struct r3v_native_deferred_copy *op =
         r3v_native_copy_slot(commandBuffer);
      if (op == NULL) {
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
      r3v_native_commit_deferred_copy(commandBuffer);
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

   if (pRegions == NULL && regionCount != 0u) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   if (regionCount == 0u)
      return;
   if (image != NULL && image->depth_family) {
      for (uint32_t r = 0u; r < regionCount; r++) {
         if (!r3v_native_validate_depth_image_copy(
                commandBuffer, dstBuffer, srcImage, &pRegions[r],
                srcImageLayout, false)) {
            r3v_native_cmd_poison(commandBuffer);
            return;
         }
      }
      if (r3v_native_cmd_buffer_require_image_layout(
             cmd_buffer, image, srcImageLayout,
             R3V_NATIVE_IMAGE_PRODUCER_RB2D, false) != VK_SUCCESS) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      for (uint32_t r = 0u; r < regionCount; r++) {
         if (r3v_native_record_depth_image_copy(
                commandBuffer, dstBuffer, srcImage, &pRegions[r],
                srcImageLayout, false) != VK_SUCCESS) {
            r3v_native_cmd_poison(commandBuffer);
            return;
         }
      }
      return;
   }

   for (uint32_t r = 0u; r < regionCount; r++) {
      uint32_t row_length;
      const VkBufferImageCopy *region = &pRegions[r];
      if (!r3v_native_transfer_source_layout_ok(srcImageLayout) ||
          !r3v_native_copy_subresource_ok(&region->imageSubresource) ||
          !r3v_native_copy_rect_ok(image, region->imageOffset,
                                   region->imageExtent) ||
          (image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
          !r3v_native_copy_buffer_ok(buffer,
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     region, image->texel_bytes,
                                     &row_length) ||
          !r3v_native_linear_image_buffer_ranges_disjoint(
             image, (uint32_t)region->imageOffset.x,
             (uint32_t)region->imageOffset.y, buffer,
             region->bufferOffset, row_length, region->imageExtent.width,
             region->imageExtent.height, false)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
   }
   for (uint32_t r = 0; r < regionCount; r++) {
      const VkBufferImageCopy *region = &pRegions[r];
      struct r3v_native_deferred_copy *op =
         r3v_native_copy_slot(commandBuffer);
      uint32_t row_length;
      if (op == NULL) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      row_length = region->bufferRowLength != 0u
                      ? region->bufferRowLength
                      : region->imageExtent.width;
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
      r3v_native_commit_deferred_copy(commandBuffer);
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
   const uint32_t query_index = cmd_buffer->query_op_count++;
   if (r3v_native_cmd_buffer_append_ordered_operation(
          cmd_buffer, &(struct r3v_native_ordered_operation){
                         .kind = R3V_NATIVE_ORDERED_OPERATION_QUERY,
                         .ib_position_dwords = cmd_buffer->ib_size_dwords,
                         .payload.query = { .query_index = query_index },
                      }) != VK_SUCCESS)
      r3v_native_cmd_poison(commandBuffer);
   cmd_buffer->active_query_pool = NULL;
}

static VkResult
r3v_native_merge_secondary_image_states(
   struct r3v_native_cmd_buffer *primary,
   const struct r3v_native_cmd_buffer *secondary)
{
   for (uint32_t index = 0u; index < secondary->image_state_count; index++) {
      const struct r3v_native_cmd_image_state *source =
         &secondary->image_states[index];
      struct r3v_native_cmd_image_state *destination = NULL;
      VkResult result = r3v_native_cmd_buffer_append_image_state(
         primary, source->image, &destination);
      if (result != VK_SUCCESS)
         return result;
      if (destination->representation != source->representation)
         return VK_ERROR_INITIALIZATION_FAILED;
      if (source->required_layout_set) {
         if (destination->current_layout_set &&
             source->required_layout != R3V_NATIVE_IMAGE_API_LAYOUT_UNDEFINED &&
             destination->current_layout != source->required_layout &&
             (!source->current_layout_set ||
              destination->current_layout != source->current_layout))
            return VK_ERROR_INITIALIZATION_FAILED;
         if (!destination->required_layout_set) {
            destination->required_layout = source->required_layout;
            destination->required_layout_set = true;
         }
      }
      if (source->current_layout_set) {
         destination->current_layout = source->current_layout;
         destination->current_layout_set = true;
      }
      if (source->producer_set) {
         destination->producer = source->producer;
         destination->producer_set = true;
      }
      if (source->visibility_set) {
         destination->visible_to = source->visible_to;
         destination->visibility_set = true;
      }
      if (source->content_set) {
         destination->content = source->content;
         destination->content_set = true;
      }
   }
   return VK_SUCCESS;
}

/* Secondary replay has several independently owned recording arrays.  A
 * shallow command-buffer copy would let a failed replay free or overwrite the
 * primary's arrays, so the transaction owns private copies of every array a
 * replay operation can extend. */
static void
r3v_native_secondary_transaction_release(
   struct r3v_native_cmd_buffer *transaction)
{
   if (transaction->deferred_copies != NULL) {
      for (uint32_t index = 0u; index < transaction->deferred_copy_count;
           index++)
         vk_free(&transaction->vk.pool->alloc,
                 transaction->deferred_copies[index].update_data);
   }
   vk_free(&transaction->vk.pool->alloc, transaction->deferred_copies);
   vk_free(&transaction->vk.pool->alloc, transaction->ordered_operations);
   vk_free(&transaction->vk.pool->alloc, transaction->image_states);
   free(transaction->rb2d_copy_operations);
   free(transaction->ib);
   free(transaction->references);
   transaction->deferred_copies = NULL;
   transaction->ordered_operations = NULL;
   transaction->image_states = NULL;
   transaction->rb2d_copy_operations = NULL;
   transaction->ib = NULL;
   transaction->references = NULL;
}

static void
r3v_native_secondary_primary_release_arrays(
   struct r3v_native_cmd_buffer *primary)
{
   if (primary->deferred_copies != NULL) {
      for (uint32_t index = 0u; index < primary->deferred_copy_count;
           index++)
         vk_free(&primary->vk.pool->alloc,
                 primary->deferred_copies[index].update_data);
   }
   vk_free(&primary->vk.pool->alloc, primary->deferred_copies);
   vk_free(&primary->vk.pool->alloc, primary->ordered_operations);
   vk_free(&primary->vk.pool->alloc, primary->image_states);
   free(primary->rb2d_copy_operations);
   free(primary->ib);
   free(primary->references);
}

static VkResult
r3v_native_secondary_transaction_begin(
   const struct r3v_native_cmd_buffer *primary,
   struct r3v_native_cmd_buffer *transaction)
{
   *transaction = *primary;
   transaction->deferred_copies = NULL;
   transaction->ordered_operations = NULL;
   transaction->image_states = NULL;
   transaction->rb2d_copy_operations = NULL;
   transaction->ib = NULL;
   transaction->references = NULL;

   if (primary->deferred_copy_count > primary->deferred_copy_capacity ||
       primary->ordered_operation_count > primary->ordered_operation_capacity ||
       primary->image_state_count > primary->image_state_capacity ||
       primary->rb2d_copy_operation_count >
          primary->rb2d_copy_operation_capacity)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (primary->ib_size_dwords != 0u) {
      if (primary->ib == NULL ||
          (size_t)primary->ib_size_dwords > SIZE_MAX / sizeof(*primary->ib))
         goto invalid;
      transaction->ib = malloc((size_t)primary->ib_size_dwords *
                               sizeof(*primary->ib));
      if (transaction->ib == NULL)
         goto out_of_memory;
      memcpy(transaction->ib, primary->ib,
             (size_t)primary->ib_size_dwords * sizeof(*primary->ib));
   }
   if (primary->reference_count != 0u) {
      if (primary->references == NULL ||
          (size_t)primary->reference_count >
             SIZE_MAX / sizeof(*primary->references))
         goto invalid;
      transaction->references = malloc((size_t)primary->reference_count *
                                        sizeof(*primary->references));
      if (transaction->references == NULL)
         goto out_of_memory;
      memcpy(transaction->references, primary->references,
             (size_t)primary->reference_count * sizeof(*primary->references));
   }
   if (primary->deferred_copy_capacity != 0u) {
      if (primary->deferred_copies == NULL ||
          (size_t)primary->deferred_copy_capacity >
             SIZE_MAX / sizeof(*primary->deferred_copies))
         goto invalid;
      transaction->deferred_copies = vk_alloc(
         &transaction->vk.pool->alloc,
         (size_t)primary->deferred_copy_capacity *
            sizeof(*primary->deferred_copies),
         8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (transaction->deferred_copies == NULL)
         goto out_of_memory;
      memcpy(transaction->deferred_copies, primary->deferred_copies,
             (size_t)primary->deferred_copy_count *
                sizeof(*primary->deferred_copies));
      for (uint32_t index = 0u; index < primary->deferred_copy_count; index++)
         transaction->deferred_copies[index].update_data = NULL;
      for (uint32_t index = 0u; index < primary->deferred_copy_count; index++) {
         const struct r3v_native_deferred_copy *source =
            &primary->deferred_copies[index];
         if (source->update_data == NULL || source->size == 0u)
            continue;
         if (source->size > SIZE_MAX)
            goto out_of_memory;
         transaction->deferred_copies[index].update_data = vk_alloc(
            &transaction->vk.pool->alloc, (size_t)source->size, 8,
            VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
         if (transaction->deferred_copies[index].update_data == NULL)
            goto out_of_memory;
         memcpy(transaction->deferred_copies[index].update_data,
                source->update_data, (size_t)source->size);
      }
   }
   if (primary->ordered_operation_capacity != 0u) {
      if (primary->ordered_operations == NULL ||
          (size_t)primary->ordered_operation_capacity >
             SIZE_MAX / sizeof(*primary->ordered_operations))
         goto invalid;
      transaction->ordered_operations = vk_alloc(
         &transaction->vk.pool->alloc,
         (size_t)primary->ordered_operation_capacity *
            sizeof(*primary->ordered_operations),
         8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (transaction->ordered_operations == NULL)
         goto out_of_memory;
      memcpy(transaction->ordered_operations, primary->ordered_operations,
             (size_t)primary->ordered_operation_count *
                sizeof(*primary->ordered_operations));
   }
   if (primary->image_state_capacity != 0u) {
      if (primary->image_states == NULL ||
          (size_t)primary->image_state_capacity >
             SIZE_MAX / sizeof(*primary->image_states))
         goto invalid;
      transaction->image_states = vk_alloc(
         &transaction->vk.pool->alloc,
         (size_t)primary->image_state_capacity *
            sizeof(*primary->image_states),
         8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (transaction->image_states == NULL)
         goto out_of_memory;
      memcpy(transaction->image_states, primary->image_states,
             (size_t)primary->image_state_count *
                sizeof(*primary->image_states));
   }
   if (primary->rb2d_copy_operation_capacity != 0u) {
      if (primary->rb2d_copy_operations == NULL ||
          (size_t)primary->rb2d_copy_operation_capacity >
             SIZE_MAX / sizeof(*primary->rb2d_copy_operations))
         goto invalid;
      transaction->rb2d_copy_operations = malloc(
         (size_t)primary->rb2d_copy_operation_capacity *
         sizeof(*primary->rb2d_copy_operations));
      if (transaction->rb2d_copy_operations == NULL)
         goto out_of_memory;
      memcpy(transaction->rb2d_copy_operations,
             primary->rb2d_copy_operations,
             (size_t)primary->rb2d_copy_operation_count *
                sizeof(*primary->rb2d_copy_operations));
   }
   return VK_SUCCESS;

invalid:
   r3v_native_secondary_transaction_release(transaction);
   return VK_ERROR_INITIALIZATION_FAILED;

out_of_memory:
   r3v_native_secondary_transaction_release(transaction);
   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void
r3v_native_secondary_transaction_commit(
   struct r3v_native_cmd_buffer *primary,
   struct r3v_native_cmd_buffer *transaction)
{
   const struct vk_command_buffer primary_vk = primary->vk;
   r3v_native_secondary_primary_release_arrays(primary);
   *primary = *transaction;
   primary->vk = primary_vk;
   transaction->deferred_copies = NULL;
   transaction->ordered_operations = NULL;
   transaction->image_states = NULL;
   transaction->rb2d_copy_operations = NULL;
   transaction->ib = NULL;
   transaction->references = NULL;
}

static VkResult
r3v_native_append_secondary_operation(
   VkCommandBuffer commandBuffer,
   const struct r3v_native_cmd_buffer *secondary,
   const struct r3v_native_ordered_operation *source_operation)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, primary, commandBuffer);
   switch (source_operation->kind) {
   case R3V_NATIVE_ORDERED_OPERATION_HOST_COPY: {
      const uint32_t source_index =
         source_operation->payload.host_copy.deferred_copy_index;
      if (source_index >= secondary->deferred_copy_count)
         return VK_ERROR_INITIALIZATION_FAILED;
      struct r3v_native_deferred_copy *destination =
         r3v_native_copy_slot(commandBuffer);
      if (destination == NULL)
         return primary->vk.record_result;
      *destination = secondary->deferred_copies[source_index];
      destination->group = r3v_native_copy_group_at_record(primary);
      if (destination->update_data != NULL) {
         const uint8_t *source_data = destination->update_data;
         destination->update_data = NULL;
         void *data = vk_alloc(&primary->vk.pool->alloc, destination->size, 8,
                               VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
         if (data == NULL)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         memcpy(data, source_data, destination->size);
         destination->update_data = data;
      }
      r3v_native_commit_deferred_copy(commandBuffer);
      return primary->vk.record_result;
   }
   case R3V_NATIVE_ORDERED_OPERATION_RB2D_DEPTH_CLEAR:
      return r3v_native_record_depth_image_clear_logical_rect(
         commandBuffer,
         r3v_native_image_to_handle(
            source_operation->payload.rb2d_depth_clear.image),
         source_operation->payload.rb2d_depth_clear.x,
         source_operation->payload.rb2d_depth_clear.y,
         source_operation->payload.rb2d_depth_clear.width,
         source_operation->payload.rb2d_depth_clear.height,
         source_operation->payload.rb2d_depth_clear.aspect_mask,
         source_operation->payload.rb2d_depth_clear.depth_code,
         source_operation->payload.rb2d_depth_clear.stencil);
   case R3V_NATIVE_ORDERED_OPERATION_RB2D_COPY: {
      const uint32_t source_index =
         source_operation->payload.rb2d_copy.rb2d_copy_index;
      if (source_index >= secondary->rb2d_copy_operation_count)
         return VK_ERROR_INITIALIZATION_FAILED;
      const struct r3v_native_rb2d_copy_operation *source =
         &secondary->rb2d_copy_operations[source_index];
      const struct r300_rb2d_copy_plan plan = {
         .segments = source->segments,
         .segment_count = source->segment_count,
         .source_buffer_bytes = source->source_buffer_bytes,
         .destination_buffer_bytes = source->destination_buffer_bytes,
         .same_buffer = false,
         .byte_carrier = source->byte_carrier,
      };
      return r3v_native_record_rb2d_copy(
         commandBuffer, r3v_native_memory_to_handle(source->source_memory),
         r3v_native_memory_to_handle(source->destination_memory), &plan,
         source->write_mask);
   }
   case R3V_NATIVE_ORDERED_OPERATION_IMAGE_BARRIER: {
      const struct r3v_native_image_barrier_record *barrier =
         &source_operation->payload.image_barrier;
      VkResult result = r3v_native_cmd_buffer_transition_image_layout(
         primary, barrier->image, (VkImageLayout)barrier->old_layout,
         (VkImageLayout)barrier->new_layout);
      if (result != VK_SUCCESS)
         return result;
      struct r3v_native_cmd_image_state *state =
         r3v_native_cmd_buffer_find_image_state(primary, barrier->image);
      state->visible_to = r3v_native_image_barrier_visibility(
         barrier->dst_stage_mask, barrier->dst_access_mask);
      state->visibility_set = true;
      struct r3v_native_ordered_operation operation = *source_operation;
      VkResult dependency_result = r3v_native_append_depth_image_dependency(
         primary, barrier->image, &operation.ib_position_dwords);
      if (dependency_result != VK_SUCCESS)
         return dependency_result;
      return r3v_native_cmd_buffer_append_ordered_operation(primary,
                                                             &operation);
   }
   case R3V_NATIVE_ORDERED_OPERATION_MEMORY_BARRIER:
   case R3V_NATIVE_ORDERED_OPERATION_BUFFER_BARRIER: {
      struct r3v_native_ordered_operation operation = *source_operation;
      operation.ib_position_dwords = primary->ib_size_dwords;
      return r3v_native_cmd_buffer_append_ordered_operation(primary,
                                                             &operation);
   }
   case R3V_NATIVE_ORDERED_OPERATION_EVENT: {
      const uint32_t source_index = source_operation->payload.event.event_index;
      if (source_index >= secondary->event_op_count ||
          primary->event_op_count == R3V_NATIVE_EVENT_OP_MAX)
         return VK_ERROR_INITIALIZATION_FAILED;
      const uint32_t destination_index = primary->event_op_count++;
      primary->event_ops[destination_index] = secondary->event_ops[source_index];
      return r3v_native_cmd_buffer_append_ordered_operation(
         primary, &(struct r3v_native_ordered_operation){
                     .kind = R3V_NATIVE_ORDERED_OPERATION_EVENT,
                     .ib_position_dwords = primary->ib_size_dwords,
                     .payload.event = { .event_index = destination_index },
                  });
   }
   case R3V_NATIVE_ORDERED_OPERATION_QUERY: {
      const uint32_t source_index = source_operation->payload.query.query_index;
      if (source_index >= secondary->query_op_count ||
          primary->query_op_count == R3V_NATIVE_QUERY_OP_MAX)
         return VK_ERROR_INITIALIZATION_FAILED;
      const uint32_t destination_index = primary->query_op_count++;
      primary->query_ops[destination_index] = secondary->query_ops[source_index];
      return r3v_native_cmd_buffer_append_ordered_operation(
         primary, &(struct r3v_native_ordered_operation){
                     .kind = R3V_NATIVE_ORDERED_OPERATION_QUERY,
                     .ib_position_dwords = primary->ib_size_dwords,
                     .payload.query = { .query_index = destination_index },
                  });
   }
   case R3V_NATIVE_ORDERED_OPERATION_RENDER_PASS_BEGIN:
   case R3V_NATIVE_ORDERED_OPERATION_COLOR_CLEAR:
   case R3V_NATIVE_ORDERED_OPERATION_DRAW:
   case R3V_NATIVE_ORDERED_OPERATION_RENDER_PASS_END:
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   return VK_ERROR_INITIALIZATION_FAILED;
}

static bool
r3v_native_secondary_shape_valid(
   const struct r3v_native_cmd_buffer *secondary)
{
   return secondary->ordered_operation_count <=
             secondary->ordered_operation_capacity &&
          (secondary->ordered_operation_count == 0u ||
           secondary->ordered_operations != NULL) &&
          secondary->deferred_copy_count <=
             secondary->deferred_copy_capacity &&
          (secondary->deferred_copy_count == 0u ||
           secondary->deferred_copies != NULL) &&
          secondary->image_state_count <= secondary->image_state_capacity &&
          (secondary->image_state_count == 0u ||
           secondary->image_states != NULL) &&
          secondary->rb2d_copy_operation_count <=
             secondary->rb2d_copy_operation_capacity &&
          (secondary->rb2d_copy_operation_count == 0u ||
           secondary->rb2d_copy_operations != NULL) &&
          secondary->event_op_count <= R3V_NATIVE_EVENT_OP_MAX &&
          secondary->query_op_count <= R3V_NATIVE_QUERY_OP_MAX;
}

/* Secondary transfer, dependency, event, and query records are replayed at
 * the primary's execute position.  Replay assigns primary-owned payload
 * indices and PM4 positions while preserving the secondary's operation order.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdExecuteCommands(
   VkCommandBuffer commandBuffer,
   uint32_t commandBufferCount,
   const VkCommandBuffer *pCommandBuffers)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   if (commandBufferCount != 0u && pCommandBuffers == NULL) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }

   if (cmd_buffer->pass_target != NULL ||
       cmd_buffer->active_query_pool != NULL ||
       cmd_buffer->vk.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   if (commandBufferCount == 0u)
      return;

   struct r3v_native_cmd_buffer transaction;
   VkResult transaction_result = r3v_native_secondary_transaction_begin(
      cmd_buffer, &transaction);
   if (transaction_result != VK_SUCCESS) {
      vk_command_buffer_set_error(
         &cmd_buffer->vk,
         transaction_result == VK_ERROR_OUT_OF_HOST_MEMORY
            ? transaction_result
            : R3V_NATIVE_REFUSAL_RESULT);
      return;
   }

   for (uint32_t i = 0; i < commandBufferCount; i++) {
      VK_FROM_HANDLE(r3v_native_cmd_buffer, secondary, pCommandBuffers[i]);
      if (secondary == NULL ||
          secondary->vk.level != VK_COMMAND_BUFFER_LEVEL_SECONDARY ||
          secondary->vk.record_result != VK_SUCCESS ||
          !r3v_native_secondary_shape_valid(secondary) ||
          secondary->deferred_draw_count != 0 || secondary->draw_recorded ||
          secondary->deferred_dispatch.pending ||
          secondary->active_query_pool != NULL ||
          secondary->viewport_set || secondary->scissor_set) {
         transaction_result = VK_ERROR_INITIALIZATION_FAILED;
         goto replay_failed;
      }
      for (uint32_t operation_index = 0u;
           operation_index < secondary->ordered_operation_count;
           operation_index++) {
         const VkResult append_result =
            r3v_native_append_secondary_operation(
               r3v_native_cmd_buffer_to_handle(&transaction), secondary,
               &secondary->ordered_operations[operation_index]);
         if (append_result != VK_SUCCESS) {
            transaction_result = transaction.vk.record_result != VK_SUCCESS
                                    ? transaction.vk.record_result
                                    : append_result;
            goto replay_failed;
         }
      }
      transaction_result = r3v_native_merge_secondary_image_states(
         &transaction, secondary);
      if (transaction_result != VK_SUCCESS)
         goto replay_failed;
   }
   r3v_native_secondary_transaction_commit(cmd_buffer, &transaction);
   return;

replay_failed:
   r3v_native_secondary_transaction_release(&transaction);
   vk_command_buffer_set_error(
      &cmd_buffer->vk,
      transaction_result == VK_ERROR_OUT_OF_HOST_MEMORY
         ? transaction_result
         : R3V_NATIVE_REFUSAL_RESULT);
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
   r3v_native_commit_deferred_copy(commandBuffer);
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

   if (cmd_buffer->pass_target != NULL ||
       (memoryBarrierCount != 0u && pMemoryBarriers == NULL) ||
       (bufferMemoryBarrierCount != 0u && pBufferMemoryBarriers == NULL) ||
       (imageMemoryBarrierCount != 0u && pImageMemoryBarriers == NULL)) {
      r3v_native_cmd_poison(commandBuffer);
      return;
   }
   for (uint32_t i = 0u; i < memoryBarrierCount; i++) {
      const VkMemoryBarrier *barrier = &pMemoryBarriers[i];
      if (barrier->sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER ||
          barrier->pNext != NULL)
         goto refuse;
   }
   for (uint32_t i = 0; i < bufferMemoryBarrierCount; i++) {
      const VkBufferMemoryBarrier *barrier = &pBufferMemoryBarriers[i];
      VkDeviceSize normalized_size;
      VK_FROM_HANDLE(r3v_native_buffer, buffer, barrier->buffer);
      if (!r3v_native_queue_family_pair_ok(barrier->srcQueueFamilyIndex,
                                            barrier->dstQueueFamilyIndex) ||
          barrier->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER ||
          barrier->pNext != NULL ||
          !r3v_native_buffer_barrier_range_ok(
             buffer, barrier->offset, barrier->size, &normalized_size))
         goto refuse;
   }
   for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
      const VkImageMemoryBarrier *barrier = &pImageMemoryBarriers[i];
      if (!r3v_native_queue_family_pair_ok(barrier->srcQueueFamilyIndex,
                                            barrier->dstQueueFamilyIndex) ||
          barrier->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER ||
          barrier->pNext != NULL ||
          !r3v_native_image_barrier_layouts_ok(barrier)) {
         goto refuse;
      }

      VK_FROM_HANDLE(r3v_native_image, image, barrier->image);
      enum r3v_native_image_api_layout effective_layout =
         R3V_NATIVE_IMAGE_API_LAYOUT_UNDEFINED;
      bool effective_layout_set = false;
      const struct r3v_native_cmd_image_state *recorded_state =
         r3v_native_cmd_buffer_find_image_state(cmd_buffer, image);
      if (recorded_state != NULL && recorded_state->current_layout_set) {
         effective_layout = recorded_state->current_layout;
         effective_layout_set = true;
      }
      for (uint32_t previous = 0u; previous < i; previous++) {
         if (pImageMemoryBarriers[previous].image == barrier->image) {
            effective_layout = (enum r3v_native_image_api_layout)
               pImageMemoryBarriers[previous].newLayout;
            effective_layout_set = true;
         }
      }
      if (barrier->oldLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
          effective_layout_set &&
          effective_layout != (enum r3v_native_image_api_layout)
                                 barrier->oldLayout) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
   }

   const uint64_t ordered_count = (uint64_t)memoryBarrierCount +
                                  bufferMemoryBarrierCount +
                                  imageMemoryBarrierCount;
   if (ordered_count > UINT32_MAX ||
       r3v_native_cmd_buffer_reserve_ordered_operations(
          cmd_buffer, (uint32_t)ordered_count) != VK_SUCCESS)
      goto refuse;

   if (memoryBarrierCount != 0u || bufferMemoryBarrierCount != 0u) {
      for (uint32_t i = 0u; i < memoryBarrierCount; i++) {
         const VkMemoryBarrier *barrier = &pMemoryBarriers[i];
         if (r3v_native_cmd_buffer_append_ordered_operation(
                cmd_buffer, &(struct r3v_native_ordered_operation){
                   .kind = R3V_NATIVE_ORDERED_OPERATION_MEMORY_BARRIER,
                   .ib_position_dwords = cmd_buffer->ib_size_dwords,
                   .payload.memory_barrier = {
                      .src_stage_mask = srcStageMask,
                      .dst_stage_mask = dstStageMask,
                      .src_access_mask = barrier->srcAccessMask,
                      .dst_access_mask = barrier->dstAccessMask,
                   },
                }) != VK_SUCCESS)
            goto refuse;
      }
      for (uint32_t i = 0u; i < bufferMemoryBarrierCount; i++) {
         const VkBufferMemoryBarrier *barrier = &pBufferMemoryBarriers[i];
         VK_FROM_HANDLE(r3v_native_buffer, buffer, barrier->buffer);
         VkDeviceSize normalized_size;
         if (!r3v_native_buffer_barrier_range_ok(
                buffer, barrier->offset, barrier->size, &normalized_size) ||
             r3v_native_cmd_buffer_append_ordered_operation(
                cmd_buffer, &(struct r3v_native_ordered_operation){
                   .kind = R3V_NATIVE_ORDERED_OPERATION_BUFFER_BARRIER,
                   .ib_position_dwords = cmd_buffer->ib_size_dwords,
                   .payload.memory_barrier = {
                      .src_stage_mask = srcStageMask,
                      .dst_stage_mask = dstStageMask,
                      .src_access_mask = barrier->srcAccessMask,
                      .dst_access_mask = barrier->dstAccessMask,
                      .buffer = buffer,
                      .offset = barrier->offset,
                      .size = normalized_size,
                   },
                }) != VK_SUCCESS)
            goto refuse;
      }
   }
   for (uint32_t i = 0u; i < imageMemoryBarrierCount; i++) {
      const VkImageMemoryBarrier *barrier = &pImageMemoryBarriers[i];
      VK_FROM_HANDLE(r3v_native_image, image, barrier->image);
      if (r3v_native_cmd_buffer_transition_image_layout(
             cmd_buffer, image, barrier->oldLayout,
             barrier->newLayout) != VK_SUCCESS) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      struct r3v_native_cmd_image_state *image_state =
         r3v_native_cmd_buffer_find_image_state(cmd_buffer, image);
      image_state->visible_to = r3v_native_image_barrier_visibility(
         dstStageMask, barrier->dstAccessMask);
      image_state->visibility_set = true;
      uint32_t ib_position = 0u;
      if (r3v_native_append_depth_image_dependency(
             cmd_buffer, image, &ib_position) != VK_SUCCESS) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      if (r3v_native_cmd_buffer_append_ordered_operation(
             cmd_buffer,
             &(struct r3v_native_ordered_operation){
                .kind = R3V_NATIVE_ORDERED_OPERATION_IMAGE_BARRIER,
                .ib_position_dwords = ib_position,
                .payload.image_barrier = {
                   .image = image,
                   .old_layout = (enum r3v_native_image_api_layout)
                      barrier->oldLayout,
                   .new_layout = (enum r3v_native_image_api_layout)
                      barrier->newLayout,
                   .aspect_mask = barrier->subresourceRange.aspectMask,
                   .src_stage_mask = srcStageMask,
                   .dst_stage_mask = dstStageMask,
                   .src_access_mask = barrier->srcAccessMask,
                   .dst_access_mask = barrier->dstAccessMask,
                },
             }) != VK_SUCCESS) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
   }
   return;

refuse:
   r3v_native_cmd_poison(commandBuffer);
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
   const uint32_t query_index = cmd_buffer->query_op_count++;
   if (r3v_native_cmd_buffer_append_ordered_operation(
          cmd_buffer, &(struct r3v_native_ordered_operation){
                         .kind = R3V_NATIVE_ORDERED_OPERATION_QUERY,
                         .ib_position_dwords = cmd_buffer->ib_size_dwords,
                         .payload.query = { .query_index = query_index },
                      }) != VK_SUCCESS)
      r3v_native_cmd_poison(commandBuffer);
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
   r3v_native_commit_deferred_copy(commandBuffer);
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
