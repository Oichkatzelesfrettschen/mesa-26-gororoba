/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V fail-closed recording surface: every core Vulkan 1.0 command
 * outside the qualified draw subset poisons its command buffer.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"

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
   if (image->transfer_family)
      return r3v_native_transfer_source_layout_ok(layout) ||
             r3v_native_transfer_destination_layout_ok(layout);
   return r3v_native_render_layout_ok(layout);
}

/* The synchronous host executor has no hardware image-layout register:
 * host mappings execute in recorded order, and every destination publishes
 * before submission completion.  The API layout tokens still carry a
 * contract.  Transfer image copies accept only the source or destination
 * layouts the executor implements, while image barriers admit transitions
 * from Vulkan's undefined or preinitialized states.  Render-family barriers
 * use the general and color-attachment layouts accepted by the render pass
 * path.
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
 * outside the render pass into a command buffer carrying no draw --
 * the buffer holds either the qualified pass or copies, so execution
 * order between the two never arises -- and each region admits at
 * record time: the color aspect's single mip and layer, offsets and
 * extents inside the image, the buffer byte footprint inside the
 * buffer's created size, and the usage bit of each direction.  A
 * refused region, an overflowing op list, or a mixed buffer poisons
 * the recording.
 */
static struct r3v_native_deferred_copy *
r3v_native_copy_slot(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   if (cmd_buffer->pass_target != NULL || cmd_buffer->draw_recorded) {
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

static bool
r3v_native_copy_subresource_ok(const VkImageSubresourceLayers *sub)
{
   return sub->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
          sub->mipLevel == 0 && sub->baseArrayLayer == 0 &&
          sub->layerCount == 1;
}

/* The offset admits non-negative and the rectangle inside the image.
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
   return image != NULL && image->transfer_family &&
          image->memory != NULL && offset.x >= 0 && offset.y >= 0 &&
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
         4;
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

VKAPI_ATTR void VKAPI_CALL
r3v_CmdBeginQuery(
   VkCommandBuffer commandBuffer,
   VkQueryPool queryPool,
   uint32_t query,
   VkQueryControlFlags flags)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdBindDescriptorSets(
   VkCommandBuffer commandBuffer,
   VkPipelineBindPoint pipelineBindPoint,
   VkPipelineLayout layout,
   uint32_t firstSet,
   uint32_t descriptorSetCount,
   const VkDescriptorSet *pDescriptorSets,
   uint32_t dynamicOffsetCount,
   const uint32_t *pDynamicOffsets)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdBindIndexBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer buffer,
   VkDeviceSize offset,
   VkIndexType indexType)
{
   r3v_native_cmd_poison(commandBuffer);
}

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
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdClearAttachments(
   VkCommandBuffer commandBuffer,
   uint32_t attachmentCount,
   const VkClearAttachment *pAttachments,
   uint32_t rectCount,
   const VkClearRect *pRects)
{
   r3v_native_cmd_poison(commandBuffer);
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
    * full extent.  The float clear value converts to B8G8R8A8_UNORM by
    * clamp then round-to-nearest, packed in the little-endian texel
    * order the render family's readback proved on silicon: B in byte
    * 0, G, R, then A.  A NaN component converts as zero -- every
    * ordered comparison on NaN is false, so it slips both clamp arms,
    * and the float-to-integer cast of NaN has no defined value.
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
      uint32_t packed = 0;
      static const unsigned lane_byte[4] = { 2, 1, 0, 3 };
      for (unsigned c = 0; c < 4; c++) {
         float f = pColor->float32[c];
         if (f != f)
            f = 0.0f;
         f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
         const uint32_t unorm = (uint32_t)(f * 255.0f + 0.5f);
         packed |= unorm << (lane_byte[c] * 8);
      }
      *op = (struct r3v_native_deferred_copy){
         .kind = R3V_NATIVE_COPY_CLEAR_IMAGE,
         .dst_image = image,
         .width = image->width,
         .height = image->height,
         .clear_dword = packed,
      };
      cmd_buffer->deferred_copy_count++;
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
             region->size)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      *op = (struct r3v_native_deferred_copy){
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
                                     region, &row_length)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      *op = (struct r3v_native_deferred_copy){
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
          (dst->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      *op = (struct r3v_native_deferred_copy){
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
                                     region, &row_length)) {
         r3v_native_cmd_poison(commandBuffer);
         return;
      }
      *op = (struct r3v_native_deferred_copy){
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
r3v_CmdDispatch(
   VkCommandBuffer commandBuffer,
   uint32_t groupCountX,
   uint32_t groupCountY,
   uint32_t groupCountZ)
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
r3v_CmdDrawIndexed(
   VkCommandBuffer commandBuffer,
   uint32_t indexCount,
   uint32_t instanceCount,
   uint32_t firstIndex,
   int32_t vertexOffset,
   uint32_t firstInstance)
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
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdExecuteCommands(
   VkCommandBuffer commandBuffer,
   uint32_t commandBufferCount,
   const VkCommandBuffer *pCommandBuffers)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdFillBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer dstBuffer,
   VkDeviceSize dstOffset,
   VkDeviceSize size,
   uint32_t data)
{
   r3v_native_cmd_poison(commandBuffer);
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
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdResetQueryPool(
   VkCommandBuffer commandBuffer,
   VkQueryPool queryPool,
   uint32_t firstQuery,
   uint32_t queryCount)
{
   r3v_native_cmd_poison(commandBuffer);
}

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

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetEvent(
   VkCommandBuffer commandBuffer,
   VkEvent event,
   VkPipelineStageFlags stageMask)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetLineWidth(
   VkCommandBuffer commandBuffer,
   float lineWidth)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetScissor(
   VkCommandBuffer commandBuffer,
   uint32_t firstScissor,
   uint32_t scissorCount,
   const VkRect2D *pScissors)
{
   r3v_native_cmd_poison(commandBuffer);
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

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetViewport(
   VkCommandBuffer commandBuffer,
   uint32_t firstViewport,
   uint32_t viewportCount,
   const VkViewport *pViewports)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdUpdateBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer dstBuffer,
   VkDeviceSize dstOffset,
   VkDeviceSize dataSize,
   const void *pData)
{
   r3v_native_cmd_poison(commandBuffer);
}

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
   r3v_native_cmd_poison(commandBuffer);
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
