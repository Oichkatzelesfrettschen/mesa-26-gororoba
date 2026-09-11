/* SPDX-License-Identifier: MIT */

#include "r3v_native.h"

#include "amd/r300/common/r300_rb2d_copy.h"
#include "amd/r300/common/r300_zb_aspect_copy.h"
#include "amd/r300/common/r300_zb_tile_copy.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

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

static bool
r3v_native_depth_copy_aspects_valid(VkImageAspectFlags aspects)
{
   return aspects == VK_IMAGE_ASPECT_DEPTH_BIT ||
          aspects == VK_IMAGE_ASPECT_STENCIL_BIT ||
          aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
}

static bool
r3v_native_depth_copy_resolve_bound_pixel(
   const struct r3v_native_depth_image_bound *bound, uint32_t x, uint32_t y,
   uint64_t *byte_offset_out)
{
   if (bound == NULL || bound->contract == NULL || byte_offset_out == NULL ||
       bound->binding_offset_bytes > bound->bo_bytes ||
       bound->surface_base_bytes < bound->binding_offset_bytes)
      return false;
   const uint64_t relative_base =
      bound->surface_base_bytes - bound->binding_offset_bytes;
   const uint64_t relative_bytes = bound->bo_bytes -
                                   bound->binding_offset_bytes;
   uint64_t relative_offset;
   if (r300_zb_depth_address_checked(
          &bound->contract->surface, relative_base, relative_bytes, x, y,
          &relative_offset) != 0 ||
       relative_offset > UINT64_MAX - bound->binding_offset_bytes)
      return false;
   *byte_offset_out = relative_offset + bound->binding_offset_bytes;
   return *byte_offset_out < bound->bo_bytes;
}

struct r3v_native_depth_copy_range {
   uint64_t offset_bytes;
   uint32_t byte_count;
};

static bool
r3v_native_depth_copy_region_ranges_disjoint(
   const struct r3v_native_image *source_image,
   const struct r3v_native_image *destination_image,
   VkImageAspectFlags aspects, const VkImageCopy *region)
{
   const uint32_t byte_count =
      aspects == VK_IMAGE_ASPECT_DEPTH_BIT ? 3u : 1u;
   const uint32_t component_offset =
      aspects == VK_IMAGE_ASPECT_DEPTH_BIT ? 1u : 0u;
   const size_t pixel_count =
      (size_t)region->extent.width * (size_t)region->extent.height;
   struct r3v_native_depth_copy_range *source_ranges =
      calloc(pixel_count, sizeof(*source_ranges));
   struct r3v_native_depth_copy_range *destination_ranges =
      calloc(pixel_count, sizeof(*destination_ranges));
   if (source_ranges == NULL || destination_ranges == NULL) {
      free(source_ranges);
      free(destination_ranges);
      return false;
   }

   size_t range_index = 0u;
   bool valid = true;
   for (uint32_t y = 0u; y < region->extent.height && valid; y++) {
      for (uint32_t x = 0u; x < region->extent.width; x++) {
         uint64_t source_pixel;
         uint64_t destination_pixel;
         if (!r3v_native_depth_copy_resolve_bound_pixel(
                &source_image->depth_bound,
                (uint32_t)region->srcOffset.x + x,
                (uint32_t)region->srcOffset.y + y, &source_pixel) ||
             !r3v_native_depth_copy_resolve_bound_pixel(
                &destination_image->depth_bound,
                (uint32_t)region->dstOffset.x + x,
                (uint32_t)region->dstOffset.y + y, &destination_pixel) ||
             source_pixel > UINT64_MAX - component_offset ||
             destination_pixel > UINT64_MAX - component_offset) {
            valid = false;
            break;
         }
         source_ranges[range_index] = (struct r3v_native_depth_copy_range){
            .offset_bytes = source_pixel + component_offset,
            .byte_count = byte_count,
         };
         destination_ranges[range_index] =
            (struct r3v_native_depth_copy_range){
               .offset_bytes = destination_pixel + component_offset,
               .byte_count = byte_count,
            };
         if (source_image->memory->bo.size < byte_count ||
             destination_image->memory->bo.size < byte_count ||
             source_ranges[range_index].offset_bytes >
                source_image->memory->bo.size - byte_count ||
             destination_ranges[range_index].offset_bytes >
                destination_image->memory->bo.size - byte_count) {
            valid = false;
            break;
         }
         range_index++;
      }
   }
   for (size_t source_index = 0u; source_index < range_index && valid;
        source_index++) {
      const struct r3v_native_depth_copy_range source_range =
         source_ranges[source_index];
      const uint64_t source_end =
         source_range.offset_bytes + source_range.byte_count;
      for (size_t destination_index = 0u; destination_index < range_index;
           destination_index++) {
         const struct r3v_native_depth_copy_range destination_range =
            destination_ranges[destination_index];
         const uint64_t destination_end =
            destination_range.offset_bytes + destination_range.byte_count;
         if (source_range.offset_bytes < destination_end &&
             destination_range.offset_bytes < source_end) {
            valid = false;
            break;
         }
      }
   }
   free(source_ranges);
   free(destination_ranges);
   return valid;
}

static VkResult
r3v_native_record_depth_image_tile_copy(
   VkCommandBuffer command_buffer, struct r3v_native_image *source_image,
   struct r3v_native_image *destination_image, VkImageAspectFlags aspects,
   uint32_t source_tile_x, uint32_t source_tile_y, uint32_t destination_tile_x,
   uint32_t destination_tile_y)
{
   struct r300_zb_tile_copy_mapping source_mapping;
   struct r300_zb_tile_copy_mapping destination_mapping;
   if (r3v_native_depth_image_contract_copy_mapping(
          &source_image->depth_bound, source_tile_x, source_tile_y,
          &source_mapping) != 0 ||
       r3v_native_depth_image_contract_copy_mapping(
          &destination_image->depth_bound, destination_tile_x,
          destination_tile_y, &destination_mapping) != 0)
      return VK_ERROR_INITIALIZATION_FAILED;

   const bool same_storage =
      source_image->memory == destination_image->memory ||
      source_image->memory->bo.handle == destination_image->memory->bo.handle;
   const struct r300_zb_tile_copy_request request = {
      .source = source_mapping,
      .destination = destination_mapping,
      .same_buffer = same_storage,
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
          destination_image->memory->bo.size, same_storage, segment_storage,
          &copy_plan) != R300_RB2D_COPY_OK)
      return VK_ERROR_INITIALIZATION_FAILED;

   const uint32_t write_mask =
      aspects == VK_IMAGE_ASPECT_DEPTH_BIT
         ? 0xffffff00u
         : aspects == VK_IMAGE_ASPECT_STENCIL_BIT ? 0x000000ffu : UINT32_MAX;
   return r3v_native_record_rb2d_copy(
      command_buffer, r3v_native_memory_to_handle(source_image->memory),
      r3v_native_memory_to_handle(destination_image->memory), &copy_plan,
      write_mask);
}

static VkResult
r3v_native_record_depth_image_partial_copy(
   VkCommandBuffer command_buffer, struct r3v_native_image *source_image,
   struct r3v_native_image *destination_image, VkImageAspectFlags aspects,
   const VkImageCopy *region)
{
   const bool depth = aspects == VK_IMAGE_ASPECT_DEPTH_BIT;
   const bool stencil = aspects == VK_IMAGE_ASPECT_STENCIL_BIT;
   const uint32_t byte_count = depth ? 3u : stencil ? 1u : 4u;
   const uint32_t source_component_offset = depth ? 1u : 0u;
   const uint32_t destination_component_offset = source_component_offset;
   const bool same_storage =
      source_image->memory == destination_image->memory ||
      source_image->memory->bo.handle == destination_image->memory->bo.handle;
   struct r300_rb2d_copy_segment segments[R300_RB2D_COPY_MAX_SEGMENTS];
   uint32_t segment_count = 0u;

   for (uint32_t y = 0u; y < region->extent.height; y++) {
      for (uint32_t x = 0u; x < region->extent.width; x++) {
         uint64_t source_pixel;
         uint64_t destination_pixel;
         if (!r3v_native_depth_copy_resolve_bound_pixel(
                &source_image->depth_bound,
                (uint32_t)region->srcOffset.x + x,
                (uint32_t)region->srcOffset.y + y, &source_pixel) ||
             !r3v_native_depth_copy_resolve_bound_pixel(
                &destination_image->depth_bound,
                (uint32_t)region->dstOffset.x + x,
                (uint32_t)region->dstOffset.y + y, &destination_pixel) ||
             source_pixel > UINT64_MAX - source_component_offset ||
             destination_pixel > UINT64_MAX - destination_component_offset) {
            return VK_ERROR_INITIALIZATION_FAILED;
         }
         segments[segment_count] = (struct r300_rb2d_copy_segment){
            .source_offset_bytes = source_pixel + source_component_offset,
            .destination_offset_bytes =
               destination_pixel + destination_component_offset,
            .byte_count = byte_count,
         };
         segment_count++;
         if (segment_count != R300_RB2D_COPY_MAX_SEGMENTS &&
             (x + 1u != region->extent.width ||
              y + 1u != region->extent.height))
            continue;

         const struct r300_rb2d_copy_plan plan = {
            .source_buffer_bytes = source_image->memory->bo.size,
            .destination_buffer_bytes = destination_image->memory->bo.size,
            .same_buffer = same_storage,
            .segments = segments,
            .segment_count = segment_count,
            .byte_carrier = byte_count != 4u,
         };
         const VkResult result = r3v_native_record_rb2d_copy(
            command_buffer, r3v_native_memory_to_handle(source_image->memory),
            r3v_native_memory_to_handle(destination_image->memory), &plan,
            UINT32_MAX);
         if (result != VK_SUCCESS)
            return result;
         segment_count = 0u;
      }
   }
   return VK_SUCCESS;
}

static bool
r3v_native_depth_image_to_image_copy_valid(
   const struct r3v_native_image *source_image,
   VkImageLayout source_layout,
   const struct r3v_native_image *destination_image,
   VkImageLayout destination_layout, const VkImageCopy *region)
{
   const VkImageAspectFlags source_aspects =
      region != NULL ? region->srcSubresource.aspectMask : 0u;
   const VkImageAspectFlags destination_aspects =
      region != NULL ? region->dstSubresource.aspectMask : 0u;
   const bool valid =
      source_image != NULL && destination_image != NULL && region != NULL &&
          source_image->depth_family && destination_image->depth_family &&
          source_image->memory != NULL && destination_image->memory != NULL &&
          source_image->format == VK_FORMAT_D24_UNORM_S8_UINT &&
          destination_image->format == source_image->format &&
          (source_image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0u &&
          (destination_image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0u &&
          (source_layout == VK_IMAGE_LAYOUT_GENERAL ||
           source_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) &&
          (destination_layout == VK_IMAGE_LAYOUT_GENERAL ||
           destination_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) &&
          (source_aspects == VK_IMAGE_ASPECT_DEPTH_BIT ||
           source_aspects == VK_IMAGE_ASPECT_STENCIL_BIT) &&
          destination_aspects == source_aspects &&
          region->srcSubresource.mipLevel == 0u &&
          region->dstSubresource.mipLevel == 0u &&
          region->srcSubresource.baseArrayLayer == 0u &&
          region->dstSubresource.baseArrayLayer == 0u &&
          region->srcSubresource.layerCount == 1u &&
          region->dstSubresource.layerCount == 1u &&
          region->srcOffset.x >= 0 && region->srcOffset.y >= 0 &&
          region->srcOffset.z == 0 && region->dstOffset.x >= 0 &&
          region->dstOffset.y >= 0 && region->dstOffset.z == 0 &&
          region->extent.width != 0u && region->extent.height != 0u &&
          region->extent.depth == 1u &&
          source_image->depth_contract.layout.macrotile_width == 32u &&
          source_image->depth_contract.layout.macrotile_height == 16u &&
          destination_image->depth_contract.layout.macrotile_width == 32u &&
          destination_image->depth_contract.layout.macrotile_height == 16u &&
          (uint64_t)(uint32_t)region->srcOffset.x + region->extent.width <=
             source_image->width &&
          (uint64_t)(uint32_t)region->srcOffset.y + region->extent.height <=
             source_image->height &&
          (uint64_t)(uint32_t)region->dstOffset.x + region->extent.width <=
             destination_image->width &&
          (uint64_t)(uint32_t)region->dstOffset.y + region->extent.height <=
             destination_image->height;
   if (!valid)
      return false;
   if (source_image->memory != destination_image->memory &&
       source_image->memory->bo.handle != destination_image->memory->bo.handle)
      return true;
   return r3v_native_depth_copy_region_ranges_disjoint(
      source_image, destination_image, source_aspects, region);
}

bool
r3v_native_validate_depth_image_to_image_copy(
   VkImage source_image_handle, VkImageLayout source_layout,
   VkImage destination_image_handle, VkImageLayout destination_layout,
   const VkImageCopy *region)
{
   VK_FROM_HANDLE(r3v_native_image, source_image, source_image_handle);
   VK_FROM_HANDLE(r3v_native_image, destination_image,
                  destination_image_handle);
   return r3v_native_depth_image_to_image_copy_valid(
      source_image, source_layout, destination_image, destination_layout,
      region);
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
   if (!r3v_native_depth_image_to_image_copy_valid(
          source_image, source_layout, destination_image, destination_layout,
          region))
      return VK_ERROR_INITIALIZATION_FAILED;

   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, command_buffer);
   const VkImageAspectFlags source_aspects = region->srcSubresource.aspectMask;
   const struct r3v_native_zmask_plan_request source_request = {
      .operation = R3V_NATIVE_ZMASK_PLAN_OPERATION_TRANSFER_READ,
      .aspect_mask = r3v_native_depth_plan_aspect_mask(source_aspects),
      .x = (uint32_t)region->srcOffset.x,
      .y = (uint32_t)region->srcOffset.y,
      .width = region->extent.width,
      .height = region->extent.height,
      .logical_width = source_image->depth_contract.logical_extent.width,
      .logical_height = source_image->depth_contract.logical_extent.height,
   };
   const struct r3v_native_zmask_plan_request destination_request = {
      .operation = R3V_NATIVE_ZMASK_PLAN_OPERATION_STORE,
      .aspect_mask =
         r3v_native_depth_plan_aspect_mask(region->dstSubresource.aspectMask),
      .x = (uint32_t)region->dstOffset.x,
      .y = (uint32_t)region->dstOffset.y,
      .width = region->extent.width,
      .height = region->extent.height,
      .logical_width = destination_image->depth_contract.logical_extent.width,
      .logical_height = destination_image->depth_contract.logical_extent.height,
   };
   VkResult backing_result = r3v_native_cmd_buffer_require_ordinary_depth_backing(
      cmd_buffer, source_image, &source_request, NULL);
   if (backing_result == VK_SUCCESS)
      backing_result = r3v_native_cmd_buffer_require_ordinary_depth_backing(
         cmd_buffer, destination_image, &destination_request, NULL);
   if (backing_result != VK_SUCCESS)
      return backing_result;

   const bool aligned = ((uint32_t)region->srcOffset.x % 32u) == 0u &&
                        ((uint32_t)region->srcOffset.y % 16u) == 0u &&
                        ((uint32_t)region->dstOffset.x % 32u) == 0u &&
                        ((uint32_t)region->dstOffset.y % 16u) == 0u &&
                        region->extent.width % 32u == 0u &&
                        region->extent.height % 16u == 0u;
   if (!aligned)
      return r3v_native_record_depth_image_partial_copy(
         command_buffer, source_image, destination_image, source_aspects,
         region);

   for (uint32_t y = 0u; y < region->extent.height; y += 16u) {
      for (uint32_t x = 0u; x < region->extent.width; x += 32u) {
         const VkResult result = r3v_native_record_depth_image_tile_copy(
            command_buffer, source_image, destination_image, source_aspects,
            ((uint32_t)region->srcOffset.x + x) / 32u,
            ((uint32_t)region->srcOffset.y + y) / 16u,
            ((uint32_t)region->dstOffset.x + x) / 32u,
            ((uint32_t)region->dstOffset.y + y) / 16u);
         if (result != VK_SUCCESS)
            return result;
      }
   }
   return VK_SUCCESS;
}

static bool
r3v_native_depth_copy_ranges_overlap(
   const struct r300_rb2d_copy_segment *segment)
{
   const uint64_t source_end =
      segment->source_offset_bytes + segment->byte_count;
   const uint64_t destination_end =
      segment->destination_offset_bytes + segment->byte_count;
   return segment->source_offset_bytes < destination_end &&
          segment->destination_offset_bytes < source_end;
}

bool
r3v_native_validate_depth_image_copy(
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
       native_buffer->memory == NULL || cmd->deferred_dispatch.pending ||
       cmd->pass_target != NULL ||
       region->imageSubresource.mipLevel != 0u ||
       region->imageSubresource.baseArrayLayer != 0u ||
       region->imageSubresource.layerCount != 1u ||
       !r3v_native_depth_copy_aspects_valid(
          region->imageSubresource.aspectMask) ||
       region->imageSubresource.aspectMask ==
          (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ||
       region->imageOffset.x < 0 || region->imageOffset.y < 0 ||
       region->imageOffset.z != 0 || region->imageExtent.depth != 1u ||
       region->imageExtent.width == 0u || region->imageExtent.height == 0u ||
       (uint64_t)(uint32_t)region->imageOffset.x + region->imageExtent.width >
          image->width ||
       (uint64_t)(uint32_t)region->imageOffset.y + region->imageExtent.height >
          image->height ||
       (region->bufferOffset & 3u) != 0u ||
       region->bufferOffset > native_buffer->vk.size ||
       native_buffer->offset > native_buffer->memory->bo.size ||
       native_buffer->vk.size >
          native_buffer->memory->bo.size - native_buffer->offset)
      return false;

   const bool depth = region->imageSubresource.aspectMask ==
                      VK_IMAGE_ASPECT_DEPTH_BIT;
   const uint32_t texel_bytes = depth ? 4u : 1u;
   const uint32_t row_length = region->bufferRowLength != 0u
                                  ? region->bufferRowLength
                                  : region->imageExtent.width;
   const uint32_t image_height = region->bufferImageHeight != 0u
                                    ? region->bufferImageHeight
                                    : region->imageExtent.height;
   if (row_length < region->imageExtent.width ||
       image_height < region->imageExtent.height)
      return false;
   const uint64_t row_bytes = (uint64_t)row_length * texel_bytes;
   const uint64_t row_count = (uint64_t)region->imageExtent.height - 1u;
   const uint64_t texel_count = row_count * row_length +
                                region->imageExtent.width;
   if (row_bytes > UINT32_MAX ||
       texel_count > UINT64_MAX / texel_bytes ||
       region->bufferOffset > native_buffer->vk.size ||
       texel_count * texel_bytes >
          native_buffer->vk.size - region->bufferOffset ||
       native_buffer->offset > UINT64_MAX - region->bufferOffset)
      return false;

   const bool same_storage =
      native_buffer->memory == image->memory ||
      native_buffer->memory->bo.handle == image->memory->bo.handle;
   for (uint32_t y = 0u; y < region->imageExtent.height; y++) {
      for (uint32_t x = 0u; x < region->imageExtent.width; x++) {
         struct r300_rb2d_copy_segment segment;
         const enum r300_zb_aspect_copy_refusal refusal =
            r300_zb_aspect_copy_plan(
               &image->depth_contract.surface,
               image->depth_contract.surface_base_bytes,
               image->depth_contract.binding_bytes,
               (uint32_t)region->imageOffset.x + x,
               (uint32_t)region->imageOffset.y + y, x, y,
               native_buffer->offset + region->bufferOffset,
               (uint32_t)row_bytes, native_buffer->memory->bo.size,
               depth ? R300_ZB_ASPECT_COPY_DEPTH :
                       R300_ZB_ASPECT_COPY_STENCIL,
               buffer_to_image ? R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE :
                                 R300_ZB_ASPECT_COPY_SURFACE_TO_BUFFER,
               &segment);
         if (refusal != R300_ZB_ASPECT_COPY_OK ||
             !r3v_native_depth_copy_apply_binding_offset(
                &image->depth_bound, buffer_to_image, &segment) ||
             (same_storage && r3v_native_depth_copy_ranges_overlap(&segment)))
            return false;
      }
   }
   return true;
}

VkResult
r3v_native_record_depth_image_copy(
   VkCommandBuffer command_buffer, VkBuffer buffer, VkImage image_handle,
   const VkBufferImageCopy *region, VkImageLayout layout, bool buffer_to_image)
{
   VK_FROM_HANDLE(r3v_native_buffer, native_buffer, buffer);
   VK_FROM_HANDLE(r3v_native_image, image, image_handle);
   if (!r3v_native_validate_depth_image_copy(
          command_buffer, buffer, image_handle, region, layout,
          buffer_to_image))
      return VK_ERROR_INITIALIZATION_FAILED;

   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, command_buffer);
   const struct r3v_native_zmask_plan_request plan_request = {
      .operation = buffer_to_image
                      ? R3V_NATIVE_ZMASK_PLAN_OPERATION_STORE
                      : R3V_NATIVE_ZMASK_PLAN_OPERATION_TRANSFER_READ,
      .aspect_mask =
         r3v_native_depth_plan_aspect_mask(region->imageSubresource.aspectMask),
      .x = (uint32_t)region->imageOffset.x,
      .y = (uint32_t)region->imageOffset.y,
      .width = region->imageExtent.width,
      .height = region->imageExtent.height,
      .logical_width = image->depth_contract.logical_extent.width,
      .logical_height = image->depth_contract.logical_extent.height,
   };
   const VkResult backing_result =
      r3v_native_cmd_buffer_require_ordinary_depth_backing(
         cmd_buffer, image, &plan_request, NULL);
   if (backing_result != VK_SUCCESS)
      return backing_result;

   const bool depth = region->imageSubresource.aspectMask ==
                      VK_IMAGE_ASPECT_DEPTH_BIT;
   const uint32_t texel_bytes = depth ? 4u : 1u;
   const uint32_t row_length = region->bufferRowLength != 0u
                                  ? region->bufferRowLength
                                  : region->imageExtent.width;
   const uint32_t row_bytes = row_length * texel_bytes;
   const size_t pixel_count = (size_t)region->imageExtent.width *
                              (size_t)region->imageExtent.height;
   struct r300_rb2d_copy_segment *all_segments =
      malloc(pixel_count * sizeof(*all_segments));
   if (all_segments == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   size_t segment_index = 0u;
   for (uint32_t y = 0u; y < region->imageExtent.height; y++) {
      for (uint32_t x = 0u; x < region->imageExtent.width; x++) {
         const enum r300_zb_aspect_copy_refusal refusal =
            r300_zb_aspect_copy_plan(
               &image->depth_contract.surface,
               image->depth_contract.surface_base_bytes,
               image->depth_contract.binding_bytes,
               (uint32_t)region->imageOffset.x + x,
               (uint32_t)region->imageOffset.y + y, x, y,
               native_buffer->offset + region->bufferOffset, row_bytes,
               native_buffer->memory->bo.size,
               depth ? R300_ZB_ASPECT_COPY_DEPTH :
                       R300_ZB_ASPECT_COPY_STENCIL,
               buffer_to_image ? R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE :
                                 R300_ZB_ASPECT_COPY_SURFACE_TO_BUFFER,
               &all_segments[segment_index]);
         if (refusal != R300_ZB_ASPECT_COPY_OK ||
             !r3v_native_depth_copy_apply_binding_offset(
                &image->depth_bound, buffer_to_image,
                &all_segments[segment_index])) {
            free(all_segments);
            return VK_ERROR_INITIALIZATION_FAILED;
         }
         segment_index++;
      }
   }

   const bool same_storage =
      native_buffer->memory == image->memory ||
      native_buffer->memory->bo.handle == image->memory->bo.handle;
   VkResult result = VK_SUCCESS;
   for (size_t first = 0u; first < segment_index;
        first += R300_RB2D_COPY_MAX_SEGMENTS) {
      const uint32_t count =
         (uint32_t)((segment_index - first > R300_RB2D_COPY_MAX_SEGMENTS)
                       ? R300_RB2D_COPY_MAX_SEGMENTS
                       : segment_index - first);
      struct r300_rb2d_copy_plan plan = {
         .source_buffer_bytes = buffer_to_image
                                    ? native_buffer->memory->bo.size
                                    : image->memory->bo.size,
         .destination_buffer_bytes = buffer_to_image
                                        ? image->memory->bo.size
                                        : native_buffer->memory->bo.size,
         .same_buffer = same_storage,
         .segments = &all_segments[first],
         .segment_count = count,
         .byte_carrier = true,
      };
      result = r3v_native_record_rb2d_copy(
         command_buffer,
         buffer_to_image ? r3v_native_memory_to_handle(native_buffer->memory) :
                           r3v_native_memory_to_handle(image->memory),
         buffer_to_image ? r3v_native_memory_to_handle(image->memory) :
                           r3v_native_memory_to_handle(native_buffer->memory),
         &plan, UINT32_MAX);
      if (result != VK_SUCCESS)
         break;
   }
   free(all_segments);
   return result;
}
