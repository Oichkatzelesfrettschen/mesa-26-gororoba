/* SPDX-License-Identifier: MIT */

#include "r3v_native.h"

#include "amd/r300/common/r300_rb2d_fill.h"
#include "amd/r300/common/r300_rb2d_linear_span.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct r3v_native_clear_span {
   uint64_t byte_offset;
   uint64_t byte_size;
};

static int
compare_clear_offsets(const void *left, const void *right)
{
   const uint64_t left_offset = *(const uint64_t *)left;
   const uint64_t right_offset = *(const uint64_t *)right;
   return left_offset > right_offset ? 1 : left_offset < right_offset ? -1 : 0;
}

bool
r3v_native_depth_clear_code(float value, uint32_t *code)
{
   if (code == NULL || !isfinite(value) || value < 0.0f || value > 1.0f)
      return false;
   *code = (uint32_t)((double)value * 16777215.0 + 0.5);
   return true;
}

static enum r300_zb_combined_clear_refusal
build_plan(const struct r3v_native_image *image, uint32_t aspect_mask,
           uint32_t depth_code, uint32_t stencil,
           struct r300_zb_combined_clear_plan *plan)
{
   return r300_zb_combined_clear_plan(
      &(struct r300_zb_combined_clear_request){
         .surface = &image->depth_contract.surface,
         .surface_base_bytes = image->depth_contract.surface_base_bytes,
         .binding_offset_bytes = image->depth_bound.binding_offset_bytes,
         .mapped_surface_bytes = image->depth_bound.bo_bytes,
         .pitch_bytes = image->depth_contract.layout.pitch_bytes,
         .format = R300_RB2D_FORMAT_ARGB8888,
         .aspect_mask = aspect_mask,
         .depth_code = depth_code,
         .stencil = stencil,
      },
      plan);
}

VkResult
r3v_native_record_depth_image_clear(VkCommandBuffer command_buffer,
                                    VkImage image_handle,
                                    uint32_t aspect_mask,
                                    uint32_t depth_code, uint32_t stencil)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd, command_buffer);
   VK_FROM_HANDLE(r3v_native_image, image, image_handle);

   if (cmd == NULL || image == NULL || !image->depth_family ||
       image->memory == NULL ||
       cmd->deferred_dispatch.pending || cmd->pass_target != NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (r3v_native_cmd_buffer_reserve_ordered_operations(cmd, 1u) !=
       VK_SUCCESS)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   struct r300_zb_combined_clear_plan plan;
   if (build_plan(image, aspect_mask, depth_code, stencil, &plan) !=
       R300_ZB_COMBINED_CLEAR_OK)
      return VK_ERROR_INITIALIZATION_FAILED;

   const uint32_t capacity = R300_RB2D_FILL_DWORDS(1u);
   uint32_t *words = calloc(capacity, sizeof(*words));
   if (words == NULL) {
      free(words);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   struct r300_rb2d_fill_ib emitted;
   const int emit_result = r300_rb2d_fill_emit_into(
      &plan.fill, words, capacity, &emitted);
   if (emit_result != 0 || emitted.ib_size_dwords != capacity ||
       r300_rb2d_fill_validate_reloc_sites(&emitted) != 0) {
      free(words);
      return emit_result == -ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY
                                    : VK_ERROR_INITIALIZATION_FAILED;
   }

   const bool preserves_component = plan.fill.write_mask != UINT32_MAX;
   const struct r3v_native_bo_reference reference = {
      .handle = image->memory->bo.handle,
      .read_domains = preserves_component ? RADEON_GEM_DOMAIN_GTT : 0u,
      .write_domain = RADEON_GEM_DOMAIN_GTT,
      .memory = image->memory,
   };
   const uint32_t relocation_dword = emitted.reloc_sites[0].ib_index;
   const uint32_t relocation_reference = 0u;
   const uint32_t ib_position = cmd->ib_size_dwords;
   VkResult append_result = r3v_native_cmd_buffer_append_raw_ib(
      container_of(cmd->vk.base.device, struct r3v_native_device, vk), cmd,
      words, capacity, &reference, 1u, &relocation_dword,
      &relocation_reference, 1u);
   free(words);
   if (append_result != VK_SUCCESS)
      return append_result;

   if (ib_position == 0u) {
      cmd->cell_kind = R3V_NATIVE_CELL_KIND_ZB_DEPTH_CLEAR;
      cmd->zb_depth_clear_configured = true;
      cmd->zb_depth_clear_image = image;
      cmd->zb_depth_clear_aspect_mask = aspect_mask;
      cmd->zb_depth_clear_depth_code = depth_code;
      cmd->zb_depth_clear_stencil = stencil;
   } else {
      cmd->cell_kind = R3V_NATIVE_CELL_KIND_ORDERED_IMAGE_COMPOSITION;
   }
   return r3v_native_cmd_buffer_append_ordered_operation(
      cmd, &(struct r3v_native_ordered_operation){
              .kind = R3V_NATIVE_ORDERED_OPERATION_RB2D_DEPTH_CLEAR,
              .ib_position_dwords = ib_position,
              .payload.rb2d_depth_clear = {
                 .image = image,
                 .x = 0u,
                 .y = 0u,
                 .width = image->depth_contract.logical_extent.width,
                 .height = image->depth_contract.logical_extent.height,
                 .aspect_mask = aspect_mask,
                 .depth_code = depth_code,
                 .stencil = stencil,
              },
           });
}

VkResult
r3v_native_record_depth_image_clear_logical_rect(
   VkCommandBuffer command_buffer, VkImage image_handle, uint32_t x,
   uint32_t y, uint32_t width, uint32_t height, uint32_t aspect_mask,
   uint32_t depth_code, uint32_t stencil)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd, command_buffer);
   VK_FROM_HANDLE(r3v_native_image, image, image_handle);
   if (cmd == NULL || image == NULL || !image->depth_family ||
       image->memory == NULL || cmd->deferred_dispatch.pending ||
       aspect_mask == 0u ||
       (aspect_mask & ~R300_ZB_COMBINED_CLEAR_ASPECTS) != 0u ||
       depth_code > 0x00ffffffu || stencil > 0xffu)
      return VK_ERROR_INITIALIZATION_FAILED;

   const struct r300_zb_depth_surface *surface =
      &image->depth_contract.surface;
   const uint32_t logical_width = image->depth_contract.logical_extent.width;
   const uint32_t logical_height = image->depth_contract.logical_extent.height;
   if (logical_width == 0u || logical_height == 0u || width == 0u ||
       height == 0u || x >= logical_width || y >= logical_height ||
       width > logical_width - x || height > logical_height - y ||
       logical_width > surface->width || logical_height > surface->height)
      return VK_ERROR_INITIALIZATION_FAILED;

   /* The complete admitted surface has a retained one-fill stream.  Keep
    * that stream unchanged and use the resolver plan only for subregions. */
   if (cmd->pass_target == NULL && x == 0u && y == 0u && width == 64u &&
       height == 64u && logical_width == 64u && logical_height == 64u)
      return r3v_native_record_depth_image_clear(
         command_buffer, image_handle, aspect_mask, depth_code, stencil);

   const uint64_t pixel_count = (uint64_t)width * height;
   if (pixel_count > SIZE_MAX / sizeof(uint64_t) ||
       pixel_count > SIZE_MAX / sizeof(struct r3v_native_clear_span))
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkResult result = VK_ERROR_INITIALIZATION_FAILED;
   uint64_t *offsets = malloc((size_t)pixel_count * sizeof(*offsets));
   struct r3v_native_clear_span *spans =
      malloc((size_t)pixel_count * sizeof(*spans));
   struct r300_rb2d_fill_plan *plans = NULL;
   struct r300_rb2d_fill_rect *rects = NULL;
   uint32_t *words = NULL;
   uint32_t *relocation_dwords = NULL;
   uint32_t *relocation_references = NULL;
   if (offsets == NULL || spans == NULL) {
      free(spans);
      free(offsets);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   uint32_t packed_word;
   if (r300_zb_depth_pack(surface, depth_code, stencil, &packed_word) != 0)
      goto out;

   uint64_t offset_count = 0u;
   for (uint32_t row = y; row < y + height; row++) {
      for (uint32_t column = x; column < x + width; column++) {
         uint64_t surface_relative_offset;
         if (r300_zb_depth_address_checked(
                surface, image->depth_contract.surface_base_bytes,
                image->depth_bound.bo_bytes -
                   image->depth_bound.binding_offset_bytes,
                column, row, &surface_relative_offset) != 0 ||
             surface_relative_offset > UINT64_MAX -
                                          image->depth_bound.binding_offset_bytes)
            goto out;
         offsets[offset_count++] = surface_relative_offset +
                                   image->depth_bound.binding_offset_bytes;
      }
   }

   qsort(offsets, (size_t)pixel_count, sizeof(*offsets),
         compare_clear_offsets);
   uint32_t span_count = 0u;
   uint64_t span_start = offsets[0];
   for (uint64_t index = 1u; index <= pixel_count; index++) {
      const bool contiguous =
         index < pixel_count && offsets[index] == offsets[index - 1u] + 4u;
      if (contiguous)
         continue;
      if (index < pixel_count && offsets[index] == offsets[index - 1u])
         goto out;
      spans[span_count++] = (struct r3v_native_clear_span){
         .byte_offset = span_start,
         .byte_size = offsets[index - 1u] + 4u - span_start,
      };
      if (index < pixel_count)
         span_start = offsets[index];
   }

   const struct r300_rb2d_span_layout span_layout = {
      .pitch_bytes = R300_RB2D_SPAN_PITCH_DIRECT_WRITE,
      .format = R300_RB2D_FORMAT_ARGB8888,
   };
   uint64_t segment_capacity = 0u;
   for (uint32_t index = 0u; index < span_count; index++) {
      enum r300_rb2d_span_refusal refusal;
      const uint32_t segments = r300_rb2d_linear_span_segments(
         &(struct r300_rb2d_span){
            .byte_offset = spans[index].byte_offset,
            .byte_size = spans[index].byte_size,
            .value = packed_word,
         },
         &span_layout, image->depth_bound.bo_bytes, &refusal);
      if (segments == 0u || segment_capacity > UINT32_MAX - segments)
         goto out;
      segment_capacity += segments;
   }
   if (segment_capacity == 0u || segment_capacity > SIZE_MAX /
                                                 sizeof(struct r300_rb2d_fill_plan) ||
       segment_capacity > SIZE_MAX /
                            (R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT *
                             sizeof(struct r300_rb2d_fill_rect)))
      goto out;

   plans = calloc((size_t)segment_capacity, sizeof(*plans));
   rects = calloc(
      (size_t)segment_capacity * R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT,
      sizeof(*rects));
   if (plans == NULL || rects == NULL) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto out;
   }

   const uint32_t write_mask =
      aspect_mask == R300_ZB_COMBINED_CLEAR_ASPECTS
         ? UINT32_MAX
      : aspect_mask == R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH ? 0xffffff00u
                                                           : 0x000000ffu;
   uint32_t plan_count = 0u;
   for (uint32_t index = 0u; index < span_count; index++) {
      enum r300_rb2d_span_refusal refusal;
      const uint32_t segments = r300_rb2d_linear_span_plan(
         &(struct r300_rb2d_span){
            .byte_offset = spans[index].byte_offset,
            .byte_size = spans[index].byte_size,
            .value = packed_word,
         },
         &span_layout, image->depth_bound.bo_bytes, plans + plan_count,
         rects + (size_t)plan_count * R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT,
         (uint32_t)(segment_capacity - plan_count), &refusal);
      if (segments == 0u)
         goto out;
      for (uint32_t segment = 0u; segment < segments; segment++)
         plans[plan_count + segment].write_mask = write_mask;
      plan_count += segments;
   }

   uint32_t total_dwords;
   if (!r300_rb2d_linear_span_dwords(plans, plan_count, &total_dwords))
      goto out;
   words = calloc(total_dwords, sizeof(*words));
   relocation_dwords = calloc(plan_count, sizeof(*relocation_dwords));
   relocation_references = calloc(plan_count,
                                  sizeof(*relocation_references));
   if (words == NULL || relocation_dwords == NULL ||
       relocation_references == NULL) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto out;
   }

   uint32_t word_offset = 0u;
   for (uint32_t index = 0u; index < plan_count; index++) {
      struct r300_rb2d_fill_ib emitted;
      const uint32_t plan_dwords =
         R300_RB2D_FILL_DWORDS(plans[index].rect_count);
      if (r300_rb2d_fill_emit_into(plans + index, words + word_offset,
                                   plan_dwords, &emitted) != 0 ||
          emitted.ib_size_dwords != plan_dwords ||
          r300_rb2d_fill_validate_reloc_sites(&emitted) != 0)
         goto out;
      relocation_dwords[index] = word_offset + emitted.reloc_sites[0].ib_index;
      word_offset += plan_dwords;
   }

   if (r3v_native_cmd_buffer_reserve_ordered_operations(cmd, 1u) !=
       VK_SUCCESS) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto out;
   }

   const bool preserves_component = write_mask != UINT32_MAX;
   const struct r3v_native_bo_reference reference = {
      .handle = image->memory->bo.handle,
      .read_domains = preserves_component ? RADEON_GEM_DOMAIN_GTT : 0u,
      .write_domain = RADEON_GEM_DOMAIN_GTT,
      .memory = image->memory,
   };
   const uint32_t ib_position = cmd->ib_size_dwords;
   const VkResult append_result = r3v_native_cmd_buffer_append_raw_ib(
      container_of(cmd->vk.base.device, struct r3v_native_device, vk), cmd,
      words, total_dwords, &reference, 1u, relocation_dwords,
      relocation_references, plan_count);
   if (append_result != VK_SUCCESS) {
      result = append_result;
      goto out;
   }

   const VkResult op_result = r3v_native_cmd_buffer_append_ordered_operation(
      cmd, &(struct r3v_native_ordered_operation){
              .kind = R3V_NATIVE_ORDERED_OPERATION_RB2D_DEPTH_CLEAR,
              .ib_position_dwords = ib_position,
              .payload.rb2d_depth_clear = {
                 .image = image,
                 .x = x,
                 .y = y,
                 .width = width,
                 .height = height,
                 .aspect_mask = aspect_mask,
                 .depth_code = depth_code,
                 .stencil = stencil,
              },
           });
   if (op_result != VK_SUCCESS) {
      result = op_result;
      goto out;
   }
   cmd->cell_kind = R3V_NATIVE_CELL_KIND_ORDERED_IMAGE_COMPOSITION;

   result = VK_SUCCESS;
out:
   free(relocation_references);
   free(relocation_dwords);
   free(words);
   free(plans);
   free(rects);
   free(spans);
   free(offsets);
   return result;
}

VkResult
r3v_native_record_depth_image_clear_logical(
   VkCommandBuffer command_buffer, VkImage image_handle, uint32_t aspect_mask,
   uint32_t depth_code, uint32_t stencil)
{
   VK_FROM_HANDLE(r3v_native_image, image, image_handle);
   if (image == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   return r3v_native_record_depth_image_clear_logical_rect(
      command_buffer, image_handle, 0u, 0u,
      image->depth_contract.logical_extent.width,
      image->depth_contract.logical_extent.height, aspect_mask, depth_code,
      stencil);
}

bool
r3v_native_depth_image_clear_geometry_valid(
   const struct r3v_native_cmd_buffer *cmd)
{
   if (cmd == NULL || !cmd->zb_depth_clear_configured ||
       cmd->cell_kind != R3V_NATIVE_CELL_KIND_ZB_DEPTH_CLEAR ||
       cmd->zb_depth_clear_image == NULL ||
       cmd->zb_depth_clear_image->memory == NULL || cmd->reference_count != 1u ||
       cmd->references == NULL || cmd->ib == NULL)
      return false;

   struct r300_zb_combined_clear_plan plan;
   if (build_plan(cmd->zb_depth_clear_image,
                  cmd->zb_depth_clear_aspect_mask,
                  cmd->zb_depth_clear_depth_code,
                  cmd->zb_depth_clear_stencil,
                  &plan) != R300_ZB_COMBINED_CLEAR_OK)
      return false;

   const struct r3v_native_bo_reference *reference = &cmd->references[0];
   const bool preserves_component = plan.fill.write_mask != UINT32_MAX;
   if (reference->memory != cmd->zb_depth_clear_image->memory ||
       reference->handle != cmd->zb_depth_clear_image->memory->bo.handle ||
       reference->read_domains !=
          (preserves_component ? RADEON_GEM_DOMAIN_GTT : 0u) ||
       reference->write_domain != RADEON_GEM_DOMAIN_GTT)
      return false;

   const uint32_t capacity = R300_RB2D_FILL_DWORDS(1u);
   uint32_t expected[R300_RB2D_FILL_DWORDS(1u)];
   struct r300_rb2d_fill_ib emitted;
   return cmd->ib_size_dwords == capacity &&
          r300_rb2d_fill_emit_into(&plan.fill, expected, capacity, &emitted) ==
             0 &&
          emitted.ib_size_dwords == capacity &&
          r300_rb2d_fill_validate_reloc_sites(&emitted) == 0 &&
          memcmp(cmd->ib, expected, sizeof(expected)) == 0;
}
