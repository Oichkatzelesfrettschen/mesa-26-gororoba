/* SPDX-License-Identifier: MIT */

#include "r3v_native.h"

#include "amd/r300/common/r300_rb2d_copy.h"
#include "amd/r300/common/r300_zb_aspect_copy.h"
#include "amd/r300/common/r300_zb_tile_copy.h"

#include <stdint.h>

static bool
r3v_native_depth_copy_apply_binding_offset(
   const struct r3v_native_depth_image_bound *bound, bool buffer_to_image,
   struct r300_rb2d_copy_segment *segment)
{
   uint64_t *surface_offset = buffer_to_image
                                 ? &segment->destination_offset_bytes
                                 : &segment->source_offset_bytes;
   if (*surface_offset > UINT64_MAX - bound->binding_offset_bytes)
      return false;
   *surface_offset += bound->binding_offset_bytes;
   return *surface_offset <= bound->bo_bytes &&
          segment->byte_count <= bound->bo_bytes - *surface_offset;
}

VkResult
r3v_native_record_depth_image_to_image_copy(
   VkCommandBuffer command_buffer, VkImage source_image_handle,
   VkImageLayout source_layout, VkImage destination_image_handle,
   VkImageLayout destination_layout, const VkImageCopy *region)
{
   VK_FROM_HANDLE(r3v_native_image, source_image, source_image_handle);
   VK_FROM_HANDLE(r3v_native_image, destination_image,
                  destination_image_handle);
   const VkImageAspectFlags source_aspects =
      region != NULL ? region->srcSubresource.aspectMask : 0u;
   const VkImageAspectFlags destination_aspects =
      region != NULL ? region->dstSubresource.aspectMask : 0u;
   const VkImageAspectFlags packed_aspects =
      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
   const bool aspect_copy = source_aspects == VK_IMAGE_ASPECT_DEPTH_BIT ||
                            source_aspects == VK_IMAGE_ASPECT_STENCIL_BIT ||
                            source_aspects == packed_aspects;

   if (source_image == NULL || destination_image == NULL || region == NULL ||
       !source_image->depth_family || !destination_image->depth_family ||
       source_image->memory == NULL || destination_image->memory == NULL ||
       source_image->memory == destination_image->memory ||
       source_image->memory->bo.handle == destination_image->memory->bo.handle ||
       source_image->format != VK_FORMAT_D24_UNORM_S8_UINT ||
       destination_image->format != source_image->format ||
       source_image->width != destination_image->width ||
       source_image->height != destination_image->height ||
       (source_image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0u ||
       (destination_image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0u ||
       (source_layout != VK_IMAGE_LAYOUT_GENERAL &&
        source_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) ||
       (destination_layout != VK_IMAGE_LAYOUT_GENERAL &&
        destination_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) ||
       !aspect_copy || destination_aspects != source_aspects ||
       region->srcSubresource.mipLevel != 0u ||
       region->dstSubresource.mipLevel != 0u ||
       region->srcSubresource.baseArrayLayer != 0u ||
       region->dstSubresource.baseArrayLayer != 0u ||
       region->srcSubresource.layerCount != 1u ||
       region->dstSubresource.layerCount != 1u ||
       region->srcOffset.x < 0 || region->srcOffset.y < 0 ||
       region->srcOffset.z != 0 || region->dstOffset.x < 0 ||
       region->dstOffset.y < 0 || region->dstOffset.z != 0 ||
       region->extent.depth != 1u ||
       source_image->depth_contract.layout.macrotile_width != 32u ||
       source_image->depth_contract.layout.macrotile_height != 16u ||
       destination_image->depth_contract.layout.macrotile_width != 32u ||
       destination_image->depth_contract.layout.macrotile_height != 16u ||
       region->extent.width != 32u || region->extent.height != 16u ||
       ((uint32_t)region->srcOffset.x % 32u) != 0u ||
       ((uint32_t)region->srcOffset.y % 16u) != 0u ||
       ((uint32_t)region->dstOffset.x % 32u) != 0u ||
       ((uint32_t)region->dstOffset.y % 16u) != 0u ||
       (uint64_t)(uint32_t)region->srcOffset.x + region->extent.width >
          source_image->width ||
       (uint64_t)(uint32_t)region->srcOffset.y + region->extent.height >
          source_image->height ||
       (uint64_t)(uint32_t)region->dstOffset.x + region->extent.width >
          destination_image->width ||
       (uint64_t)(uint32_t)region->dstOffset.y + region->extent.height >
          destination_image->height)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r300_zb_tile_copy_mapping source_mapping;
   struct r300_zb_tile_copy_mapping destination_mapping;
   if (r3v_native_depth_image_contract_copy_mapping(
          &source_image->depth_bound, (uint32_t)region->srcOffset.x / 32u,
          (uint32_t)region->srcOffset.y / 16u, &source_mapping) != 0 ||
       r3v_native_depth_image_contract_copy_mapping(
          &destination_image->depth_bound,
          (uint32_t)region->dstOffset.x / 32u,
          (uint32_t)region->dstOffset.y / 16u,
          &destination_mapping) != 0)
      return VK_ERROR_INITIALIZATION_FAILED;

   const struct r300_zb_tile_copy_request request = {
      .source = source_mapping,
      .destination = destination_mapping,
      .same_buffer = false,
      .same_format = true,
      .compressed = false,
      .multisample = false,
   };
   struct r300_zb_tile_copy_plan tile_plan;
   struct r300_rb2d_copy_segment segment_storage[R300_RB2D_COPY_MAX_SEGMENTS];
   struct r300_rb2d_copy_plan copy_plan;
   if (r300_zb_tile_copy_plan_build(&request, &tile_plan) !=
          R300_ZB_TILE_COPY_OK ||
       r300_rb2d_copy_plan_from_zb_tile(
          &tile_plan, source_image->memory->bo.size,
          destination_image->memory->bo.size, false, segment_storage,
          &copy_plan) != R300_RB2D_COPY_OK)
      return VK_ERROR_INITIALIZATION_FAILED;

   /* The tiled wrapper intentionally admits one complete-tile operation.
    * Image copies carry an array of independent regions, so append the
    * validated segment plan directly and leave the command buffer in the
    * segment geometry used by the common RB2D validator. */
   const uint32_t write_mask =
      source_aspects == VK_IMAGE_ASPECT_DEPTH_BIT
         ? 0xffffff00u
         : source_aspects == VK_IMAGE_ASPECT_STENCIL_BIT ? 0x000000ffu
                                                        : UINT32_MAX;
   return r3v_native_record_rb2d_copy(
      command_buffer, r3v_native_memory_to_handle(source_image->memory),
      r3v_native_memory_to_handle(destination_image->memory), &copy_plan,
      write_mask);
}

VkResult
r3v_native_record_depth_image_copy(
   VkCommandBuffer command_buffer, VkBuffer buffer, VkImage image_handle,
   const VkBufferImageCopy *region, VkImageLayout layout, bool buffer_to_image)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd, command_buffer);
   VK_FROM_HANDLE(r3v_native_buffer, native_buffer, buffer);
   VK_FROM_HANDLE(r3v_native_image, image, image_handle);
   if (cmd == NULL || native_buffer == NULL || image == NULL || region == NULL ||
       !image->depth_family || image->memory == NULL ||
       (layout != VK_IMAGE_LAYOUT_GENERAL &&
        layout != (buffer_to_image ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL :
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) ||
       !(image->usage & (buffer_to_image ? VK_IMAGE_USAGE_TRANSFER_DST_BIT :
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ||
       !(native_buffer->vk.usage & (buffer_to_image ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT :
                                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT)) ||
       native_buffer->memory == NULL ||
       cmd->deferred_copy_count != 0u || cmd->deferred_draw_count != 0u ||
       cmd->deferred_dispatch.pending || cmd->pass_target != NULL ||
       region->imageSubresource.mipLevel != 0u ||
       region->imageSubresource.baseArrayLayer != 0u ||
       region->imageSubresource.layerCount != 1u ||
       (region->imageSubresource.aspectMask != VK_IMAGE_ASPECT_DEPTH_BIT &&
        region->imageSubresource.aspectMask != VK_IMAGE_ASPECT_STENCIL_BIT) ||
       region->imageOffset.x < 0 || region->imageOffset.y < 0 ||
       region->imageOffset.z != 0 || region->imageExtent.depth != 1u ||
       region->imageExtent.width == 0u || region->imageExtent.height == 0u ||
       (uint64_t)(uint32_t)region->imageOffset.x + region->imageExtent.width >
          image->depth_contract.surface.width ||
       (uint64_t)(uint32_t)region->imageOffset.y + region->imageExtent.height >
          image->depth_contract.surface.height ||
       (region->bufferOffset & 3u) != 0u ||
       region->bufferOffset > native_buffer->vk.size ||
       native_buffer->offset > native_buffer->memory->bo.size ||
       native_buffer->vk.size >
          native_buffer->memory->bo.size - native_buffer->offset)
      return VK_ERROR_INITIALIZATION_FAILED;

   const bool depth = region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT;
   const uint32_t texel_bytes = depth ? 4u : 1u;
   const uint32_t row_length = region->bufferRowLength != 0u
                                  ? region->bufferRowLength
                                  : region->imageExtent.width;
   const uint32_t image_height = region->bufferImageHeight != 0u
                                    ? region->bufferImageHeight
                                    : region->imageExtent.height;
   if (row_length < region->imageExtent.width ||
       image_height < region->imageExtent.height)
      return VK_ERROR_INITIALIZATION_FAILED;
   const uint64_t row_bytes = (uint64_t)row_length * texel_bytes;
   const uint64_t last_byte =
      region->bufferOffset +
      ((uint64_t)(region->imageExtent.height - 1u) * row_length +
       region->imageExtent.width) * texel_bytes;
   if (row_bytes > UINT32_MAX || last_byte < region->bufferOffset ||
       last_byte > native_buffer->vk.size)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r300_rb2d_copy_segment segments[R300_RB2D_COPY_MAX_SEGMENTS];
   uint32_t segment_count = 0u;
   for (uint32_t y = 0u; y < region->imageExtent.height; y++) {
      for (uint32_t x = 0u; x < region->imageExtent.width; x++) {
         const enum r300_zb_aspect_copy_refusal aspect_refusal =
            r300_zb_aspect_copy_plan(
                &image->depth_contract.surface,
                image->depth_contract.surface_base_bytes,
                image->depth_contract.binding_bytes,
                (uint32_t)region->imageOffset.x + x,
                (uint32_t)region->imageOffset.y + y,
                x, y, native_buffer->offset + region->bufferOffset,
                (uint32_t)row_bytes, native_buffer->memory->bo.size,
                depth ? R300_ZB_ASPECT_COPY_DEPTH :
                        R300_ZB_ASPECT_COPY_STENCIL,
                buffer_to_image ? R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE :
                                  R300_ZB_ASPECT_COPY_SURFACE_TO_BUFFER,
                &segments[segment_count]);
         if (aspect_refusal != R300_ZB_ASPECT_COPY_OK ||
             !r3v_native_depth_copy_apply_binding_offset(
                &image->depth_bound, buffer_to_image,
                &segments[segment_count]))
            return VK_ERROR_INITIALIZATION_FAILED;
         segment_count++;
         if (segment_count == R300_RB2D_COPY_MAX_SEGMENTS ||
             (x + 1u == region->imageExtent.width &&
              y + 1u == region->imageExtent.height)) {
            const struct r300_rb2d_copy_plan plan = {
               .source_buffer_bytes = buffer_to_image
                                            ? native_buffer->memory->bo.size
                                            : image->memory->bo.size,
               .destination_buffer_bytes = buffer_to_image
                                               ? image->memory->bo.size
                                               : native_buffer->memory->bo.size,
               .same_buffer = false,
               .segments = segments,
               .segment_count = segment_count,
               .byte_carrier = true,
            };
            const VkResult result = r3v_native_record_rb2d_copy(
               command_buffer,
               buffer_to_image ? r3v_native_memory_to_handle(native_buffer->memory) :
                                 r3v_native_memory_to_handle(image->memory),
               buffer_to_image ? r3v_native_memory_to_handle(image->memory) :
                                 r3v_native_memory_to_handle(native_buffer->memory),
               &plan, UINT32_MAX);
            if (result != VK_SUCCESS)
               return result;
            segment_count = 0u;
         }
      }
   }
   return VK_SUCCESS;
}
