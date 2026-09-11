/* SPDX-License-Identifier: MIT */

#include "r3v_native.h"
#include "r3v_physical_device.h"

#include "amd/r300/common/r300_rb2d_fill.h"
#include "amd/r300/common/r300_rb2d_linear_span.h"
#include "amd/r300/common/r300_zb_depth_control_cell.h"
#include "amd/r300/common/r300_zmask_clear_plan.h"
#include "amd/r300/common/r300_zmask_materialize_plan.h"

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

   if (r3v_native_cmd_buffer_require_ordinary_depth_backing(cmd, image) !=
       VK_SUCCESS)
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

   if (r3v_native_cmd_buffer_require_ordinary_depth_backing(cmd, image) !=
       VK_SUCCESS)
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
r3v_native_record_zmask_materialize(VkCommandBuffer command_buffer,
                                    VkImage image_handle,
                                    enum r3v_native_image_representation
                                       source_representation,
                                    const struct r3v_native_zmask_metadata_state
                                       *source_metadata)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, command_buffer);
   VK_FROM_HANDLE(r3v_native_image, image, image_handle);
   if (cmd_buffer == NULL || image == NULL || !image->depth_family ||
       image->memory == NULL || !image->zmask_layout_admitted ||
       image->depth_bound.contract != &image->depth_contract ||
       cmd_buffer->vk.base.device == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);
   struct r3v_native_cmd_image_state *state =
      r3v_native_cmd_buffer_find_image_state(cmd_buffer, image);
   const enum r3v_native_image_representation representation =
      state != NULL && state->current_representation_set
         ? state->current_representation
         : state != NULL ? state->required_representation
                         : image->committed_submission.representation;
   const struct r3v_native_zmask_metadata_state metadata =
      state != NULL && state->current_zmask_metadata_set
         ? state->current_zmask_metadata
         : state != NULL ? state->required_zmask_metadata
                         : image->committed_submission.zmask_metadata;
   if (source_metadata == NULL || representation != source_representation ||
       !r3v_native_zmask_metadata_equal(&metadata, source_metadata) ||
       !r3v_native_zmask_metadata_valid(&metadata))
      return VK_ERROR_INITIALIZATION_FAILED;
   if (representation == R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_COMPRESSED &&
       metadata.status == R3V_NATIVE_ZMASK_METADATA_COMPRESSED)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   if (representation != R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR ||
       metadata.status != R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (!device->zmask_materialize_scratch_initialized ||
       device->zmask_materialize_vertex.bo.handle == 0u ||
       device->zmask_materialize_vertex.bo.size <
          R3V_NATIVE_MEMORY_ALIGNMENT ||
       device->zmask_materialize_color.bo.handle == 0u ||
       device->zmask_materialize_color.bo.size <
          R300_ZB_DEPTH_CONTROL_COLOR_BYTES)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r300_zmask_materialize_plan plan;
   int plan_result = r300_zmask_materialize_prefix(
      &image->depth_contract.surface, &image->zmask_layout,
      metadata.clear_depth_code, metadata.clear_stencil, &plan);
   if (plan_result == 0)
      plan_result = r300_zmask_materialize_suffix(&plan);
   const uint64_t depth_offset = image->depth_bound.surface_base_bytes;
   if (plan_result != 0 || depth_offset > UINT32_MAX ||
       depth_offset % 32u != 0u)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r300_zb_depth_control_ib cell;
   const int emit_result = r300_zb_depth_zmask_materialize_emit(
      &plan, (uint32_t)depth_offset, 0u,
      r300_rb3d_colorpitch0_pack_argb8888(64u), &cell);
   if (emit_result != 0)
      return emit_result == -ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY
                                    : VK_ERROR_INITIALIZATION_FAILED;
   if (r300_zb_depth_control_validate_reloc_sites(&cell) != 0) {
      r300_zb_depth_control_release(&cell);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   if (r3v_native_cmd_buffer_reserve_ordered_operations(cmd_buffer, 1u) !=
       VK_SUCCESS) {
      r300_zb_depth_control_release(&cell);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   const uint32_t original_image_state_count = cmd_buffer->image_state_count;
   const uint32_t original_operation_count =
      cmd_buffer->ordered_operation_count;
   const bool original_required_owner_set =
      cmd_buffer->required_zmask_owner_set;
   const bool original_current_owner_set = cmd_buffer->current_zmask_owner_set;
   const struct r3v_native_zmask_owner_state original_required_owner =
      cmd_buffer->required_zmask_owner;
   const struct r3v_native_zmask_owner_state original_current_owner =
      cmd_buffer->current_zmask_owner;
   const struct r3v_native_cmd_image_state original_state =
      state != NULL ? *state : (struct r3v_native_cmd_image_state){0};

   struct r3v_native_zmask_owner_state owner;
   const struct r3v_native_zmask_owner_state retired_owner = {0};
   const struct r3v_native_zmask_metadata_state retired_metadata = {0};
   VkResult result = r3v_native_zmask_owner_from_image(image, &metadata,
                                                       &owner);
   if (result == VK_SUCCESS)
      result = r3v_native_cmd_buffer_transition_image_representation(
         cmd_buffer, image, representation,
         R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
   if (result == VK_SUCCESS)
      result = r3v_native_cmd_buffer_transition_zmask_metadata(
         cmd_buffer, image, &metadata, &retired_metadata);
   if (result == VK_SUCCESS)
      result = r3v_native_cmd_buffer_transition_zmask_owner(
         cmd_buffer, &owner, &retired_owner);
   const struct r3v_native_bo_reference references[] = {
      [R300_ZB_DEPTH_CONTROL_SLOT_VERTEX] = {
         .handle = device->zmask_materialize_vertex.bo.handle,
         .read_domains = RADEON_GEM_DOMAIN_GTT,
         .memory = &device->zmask_materialize_vertex,
      },
      [R300_ZB_DEPTH_CONTROL_SLOT_COLOR] = {
         .handle = device->zmask_materialize_color.bo.handle,
         .write_domain = RADEON_GEM_DOMAIN_GTT,
         .memory = &device->zmask_materialize_color,
      },
      [R300_ZB_DEPTH_CONTROL_SLOT_DEPTH] = {
         .handle = image->memory->bo.handle,
         .read_domains = RADEON_GEM_DOMAIN_GTT,
         .write_domain = RADEON_GEM_DOMAIN_GTT,
         .memory = image->memory,
      },
   };
   uint32_t relocation_dwords[R300_ZB_DEPTH_CONTROL_MAX_RELOC_SITES];
   uint32_t relocation_references[R300_ZB_DEPTH_CONTROL_MAX_RELOC_SITES];
   for (uint32_t index = 0u; index < cell.reloc_site_count; index++) {
      relocation_dwords[index] = cell.reloc_sites[index].ib_index;
      relocation_references[index] = cell.reloc_sites[index].slot;
   }
   if (result == VK_SUCCESS)
      result = r3v_native_cmd_buffer_append_raw_ib(
         device, cmd_buffer, cell.ib, cell.ib_size_dwords, references,
         ARRAY_SIZE(references), relocation_dwords, relocation_references,
         cell.reloc_site_count);
   r300_zb_depth_control_release(&cell);
   if (result == VK_SUCCESS) {
      cmd_buffer->ordered_operations[cmd_buffer->ordered_operation_count++] =
         (struct r3v_native_ordered_operation){
            .kind = R3V_NATIVE_ORDERED_OPERATION_IMAGE_MATERIALIZE,
            .ib_position_dwords = cmd_buffer->ib_size_dwords,
            .payload.image_materialize = {
               .image = image,
               .source_representation = representation,
               .metadata = metadata,
            },
         };
      cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_ORDERED_IMAGE_COMPOSITION;
      return VK_SUCCESS;
   }

   cmd_buffer->ordered_operation_count = original_operation_count;
   cmd_buffer->required_zmask_owner_set = original_required_owner_set;
   cmd_buffer->current_zmask_owner_set = original_current_owner_set;
   cmd_buffer->required_zmask_owner = original_required_owner;
   cmd_buffer->current_zmask_owner = original_current_owner;
   if (state != NULL)
      *state = original_state;
   cmd_buffer->image_state_count = original_image_state_count;
   return result;
}

VkResult
r3v_native_record_zmask_ownership_only(VkCommandBuffer command_buffer)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, command_buffer);
   if (cmd_buffer == NULL || cmd_buffer->vk.base.device == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);
   if (device->zmask_ownership_gate == NULL)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   const struct r300_zmask_layout unused_layout = {0};
   struct r300_zmask_clear_plan plan;
   if (r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_OWNERSHIP_ONLY,
                                   &unused_layout, &plan) != 0 ||
       plan.dword_count != 0u || !plan.requires_hyperz_ownership ||
       plan.writes_hyperz_registers)
      return VK_ERROR_INITIALIZATION_FAILED;

   VkResult result =
      r3v_native_cmd_buffer_append_ordered_operation(
         cmd_buffer, &(struct r3v_native_ordered_operation){
                        .kind =
                           R3V_NATIVE_ORDERED_OPERATION_HYPERZ_ACQUIRE,
                        .ib_position_dwords = cmd_buffer->ib_size_dwords,
                     });
   if (result == VK_SUCCESS)
      cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_ORDERED_IMAGE_COMPOSITION;
   return result;
}

static VkResult
r3v_native_record_zmask_initialize_state(
   VkCommandBuffer command_buffer, VkImage image_handle,
   const struct r3v_native_zmask_owner_state *expected_owner,
   const struct r3v_native_zmask_metadata_state *expected_metadata,
   const struct r3v_native_zmask_metadata_state *fixed_resulting_metadata)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, command_buffer);
   VK_FROM_HANDLE(r3v_native_image, image, image_handle);
   if (cmd_buffer == NULL || image == NULL || !image->depth_family ||
       image->memory == NULL || !image->zmask_layout_admitted ||
       image->depth_bound.contract != &image->depth_contract ||
       cmd_buffer->vk.base.device == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);
   if (device->zmask_initialize_gate == NULL)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   struct r3v_native_cmd_image_state *state =
      r3v_native_cmd_buffer_find_image_state(cmd_buffer, image);
   const enum r3v_native_image_representation representation =
      state != NULL && state->current_representation_set
         ? state->current_representation
         : state != NULL ? state->required_representation
                         : image->committed_submission.representation;
   const struct r3v_native_zmask_metadata_state metadata =
      state != NULL && state->current_zmask_metadata_set
         ? state->current_zmask_metadata
         : state != NULL ? state->required_zmask_metadata
                         : image->committed_submission.zmask_metadata;
   const struct r3v_native_zmask_owner_state source_owner =
      cmd_buffer->current_zmask_owner_set
         ? cmd_buffer->current_zmask_owner
         : cmd_buffer->required_zmask_owner_set
              ? cmd_buffer->required_zmask_owner
              : device->zmask_owner;
   if (representation != R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED ||
       (metadata.status != R3V_NATIVE_ZMASK_METADATA_RETIRED &&
        metadata.status != R3V_NATIVE_ZMASK_METADATA_INITIALIZED) ||
       (expected_owner != NULL &&
        !r3v_native_zmask_owner_equal(&source_owner, expected_owner)) ||
       (expected_metadata != NULL &&
        !r3v_native_zmask_metadata_equal(&metadata, expected_metadata)))
      return VK_ERROR_INITIALIZATION_FAILED;

   const struct r3v_native_zmask_owner_state no_owner = {0};
   if (metadata.status == R3V_NATIVE_ZMASK_METADATA_RETIRED) {
      if (source_owner.image == image)
         return VK_ERROR_INITIALIZATION_FAILED;
   } else if (source_owner.image != image ||
              !r3v_native_zmask_metadata_equal(&source_owner.metadata,
                                                &metadata)) {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   if (source_owner.image != NULL && source_owner.image != image) {
      struct r3v_native_cmd_image_state *owner_state =
         r3v_native_cmd_buffer_find_image_state(cmd_buffer,
                                                source_owner.image);
      const enum r3v_native_image_representation owner_representation =
         owner_state != NULL && owner_state->current_representation_set
            ? owner_state->current_representation
            : owner_state != NULL ? owner_state->required_representation
                                  : source_owner.image->committed_submission
                                       .representation;
      const struct r3v_native_zmask_metadata_state owner_metadata =
         owner_state != NULL && owner_state->current_zmask_metadata_set
            ? owner_state->current_zmask_metadata
            : owner_state != NULL ? owner_state->required_zmask_metadata
                                  : source_owner.image->committed_submission
                                       .zmask_metadata;
      if (owner_representation !=
             R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED ||
          owner_metadata.status != R3V_NATIVE_ZMASK_METADATA_INITIALIZED ||
          !r3v_native_zmask_metadata_equal(&owner_metadata,
                                            &source_owner.metadata))
         return VK_ERROR_INITIALIZATION_FAILED;
   }

   struct r3v_native_zmask_metadata_state resulting_metadata = {
      .status = R3V_NATIVE_ZMASK_METADATA_INITIALIZED,
      .generation = fixed_resulting_metadata != NULL
                       ? fixed_resulting_metadata->generation
                       : p_atomic_inc_return(
                            &device->zmask_metadata_generation_counter),
   };
   if (fixed_resulting_metadata != NULL)
      resulting_metadata = *fixed_resulting_metadata;
   if (resulting_metadata.status != R3V_NATIVE_ZMASK_METADATA_INITIALIZED ||
       resulting_metadata.clear_depth_code != 0u ||
       resulting_metadata.clear_stencil != 0u ||
       resulting_metadata.generation == 0u)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r300_zmask_clear_plan plan;
   if (r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
                                   &image->zmask_layout, &plan) != 0)
      return VK_ERROR_INITIALIZATION_FAILED;
   if (r3v_native_cmd_buffer_reserve_ordered_operations(cmd_buffer, 1u) !=
       VK_SUCCESS)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   const uint32_t original_ib_size = cmd_buffer->ib_size_dwords;
   const uint32_t original_reference_count = cmd_buffer->reference_count;
   const uint32_t original_operation_count =
      cmd_buffer->ordered_operation_count;
   const uint32_t original_image_state_count = cmd_buffer->image_state_count;
   const enum r3v_native_cell_kind original_cell_kind = cmd_buffer->cell_kind;
   const bool original_required_owner_set =
      cmd_buffer->required_zmask_owner_set;
   const bool original_current_owner_set = cmd_buffer->current_zmask_owner_set;
   const struct r3v_native_zmask_owner_state original_required_owner =
      cmd_buffer->required_zmask_owner;
   const struct r3v_native_zmask_owner_state original_current_owner =
      cmd_buffer->current_zmask_owner;
   struct r3v_native_cmd_image_state *original_image_states = NULL;
   if (original_image_state_count != 0u) {
      original_image_states =
         malloc((size_t)original_image_state_count *
                sizeof(*original_image_states));
      if (original_image_states == NULL)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      memcpy(original_image_states, cmd_buffer->image_states,
             (size_t)original_image_state_count *
                sizeof(*original_image_states));
   }

   VkResult result = r3v_native_cmd_buffer_transition_image_representation(
      cmd_buffer, image, representation, representation);
   if (result == VK_SUCCESS && source_owner.image != NULL &&
       source_owner.image != image) {
      const struct r3v_native_zmask_metadata_state retired_metadata = {0};
      result = r3v_native_cmd_buffer_transition_zmask_metadata(
         cmd_buffer, source_owner.image, &source_owner.metadata,
         &retired_metadata);
      if (result == VK_SUCCESS)
         result = r3v_native_cmd_buffer_transition_zmask_owner(
            cmd_buffer, &source_owner, &no_owner);
   }
   if (result == VK_SUCCESS)
      result = r3v_native_cmd_buffer_transition_zmask_metadata(
         cmd_buffer, image, &metadata, &resulting_metadata);

   struct r3v_native_zmask_owner_state resulting_owner;
   if (result == VK_SUCCESS)
      result = r3v_native_zmask_owner_from_image(
         image, &resulting_metadata, &resulting_owner);
   const struct r3v_native_zmask_owner_state owner_before_initialize =
      source_owner.image != NULL && source_owner.image != image ? no_owner
                                                                : source_owner;
   if (result == VK_SUCCESS)
      result = r3v_native_cmd_buffer_transition_zmask_owner(
         cmd_buffer, &owner_before_initialize, &resulting_owner);

   const struct r3v_native_bo_reference reference = {
      .handle = image->memory->bo.handle,
      .read_domains = RADEON_GEM_DOMAIN_GTT,
      .write_domain = RADEON_GEM_DOMAIN_GTT,
      .memory = image->memory,
   };
   if (result == VK_SUCCESS)
      result = r3v_native_cmd_buffer_append_raw_ib(
         device, cmd_buffer, plan.words, plan.dword_count, &reference, 1u,
         NULL, NULL, 0u);
   if (result == VK_SUCCESS) {
      cmd_buffer->ordered_operations[cmd_buffer->ordered_operation_count++] =
         (struct r3v_native_ordered_operation){
            .kind = R3V_NATIVE_ORDERED_OPERATION_IMAGE_ZMASK_INITIALIZE,
            .ib_position_dwords = cmd_buffer->ib_size_dwords,
            .payload.image_zmask_initialize = {
               .image = image,
               .source_owner = source_owner,
               .source_metadata = metadata,
               .resulting_metadata = resulting_metadata,
            },
         };
      cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_ORDERED_IMAGE_COMPOSITION;
      free(original_image_states);
      return VK_SUCCESS;
   }

   cmd_buffer->ib_size_dwords = original_ib_size;
   cmd_buffer->reference_count = original_reference_count;
   cmd_buffer->ordered_operation_count = original_operation_count;
   cmd_buffer->image_state_count = original_image_state_count;
   cmd_buffer->cell_kind = original_cell_kind;
   cmd_buffer->required_zmask_owner_set = original_required_owner_set;
   cmd_buffer->current_zmask_owner_set = original_current_owner_set;
   cmd_buffer->required_zmask_owner = original_required_owner;
   cmd_buffer->current_zmask_owner = original_current_owner;
   if (original_image_state_count != 0u)
      memcpy(cmd_buffer->image_states, original_image_states,
             (size_t)original_image_state_count *
                sizeof(*original_image_states));
   free(original_image_states);
   return result;
}

VkResult
r3v_native_record_zmask_initialize(VkCommandBuffer command_buffer,
                                   VkImage image)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, command_buffer);
   if (cmd_buffer == NULL || cmd_buffer->vk.base.device == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);
   const struct r3v_native_zmask_owner_state current_owner =
      cmd_buffer->current_zmask_owner_set
         ? cmd_buffer->current_zmask_owner
         : cmd_buffer->required_zmask_owner_set
              ? cmd_buffer->required_zmask_owner
              : device->zmask_owner;
   VK_FROM_HANDLE(r3v_native_image, destination, image);
   if (destination == NULL || current_owner.image == NULL ||
       current_owner.image == destination ||
       current_owner.metadata.status ==
          R3V_NATIVE_ZMASK_METADATA_INITIALIZED)
      return r3v_native_record_zmask_initialize_state(
         command_buffer, image, NULL, NULL, NULL);
   if (current_owner.metadata.status == R3V_NATIVE_ZMASK_METADATA_COMPRESSED)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   if (current_owner.metadata.status != R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR)
      return VK_ERROR_INITIALIZATION_FAILED;

   const uint32_t original_ib_size = cmd_buffer->ib_size_dwords;
   const uint32_t original_reference_count = cmd_buffer->reference_count;
   const uint32_t original_operation_count =
      cmd_buffer->ordered_operation_count;
   const uint32_t original_image_state_count = cmd_buffer->image_state_count;
   const enum r3v_native_cell_kind original_cell_kind = cmd_buffer->cell_kind;
   const bool original_required_owner_set =
      cmd_buffer->required_zmask_owner_set;
   const bool original_current_owner_set = cmd_buffer->current_zmask_owner_set;
   const struct r3v_native_zmask_owner_state original_required_owner =
      cmd_buffer->required_zmask_owner;
   const struct r3v_native_zmask_owner_state original_current_owner =
      cmd_buffer->current_zmask_owner;
   struct r3v_native_cmd_image_state *original_image_states = NULL;
   if (original_image_state_count != 0u) {
      original_image_states =
         malloc((size_t)original_image_state_count *
                sizeof(*original_image_states));
      if (original_image_states == NULL)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      memcpy(original_image_states, cmd_buffer->image_states,
             (size_t)original_image_state_count *
                sizeof(*original_image_states));
   }

   VkResult result = r3v_native_record_zmask_materialize(
      command_buffer, r3v_native_image_to_handle(current_owner.image),
      R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR,
      &current_owner.metadata);
   if (result == VK_SUCCESS)
      result = r3v_native_record_zmask_initialize_state(
         command_buffer, image, NULL, NULL, NULL);
   if (result != VK_SUCCESS) {
      cmd_buffer->ib_size_dwords = original_ib_size;
      cmd_buffer->reference_count = original_reference_count;
      cmd_buffer->ordered_operation_count = original_operation_count;
      cmd_buffer->image_state_count = original_image_state_count;
      cmd_buffer->cell_kind = original_cell_kind;
      cmd_buffer->required_zmask_owner_set = original_required_owner_set;
      cmd_buffer->current_zmask_owner_set = original_current_owner_set;
      cmd_buffer->required_zmask_owner = original_required_owner;
      cmd_buffer->current_zmask_owner = original_current_owner;
      if (original_image_state_count != 0u)
         memcpy(cmd_buffer->image_states, original_image_states,
                (size_t)original_image_state_count *
                   sizeof(*original_image_states));
   }
   free(original_image_states);
   return result;
}

VkResult
r3v_native_replay_zmask_initialize(
   VkCommandBuffer command_buffer,
   const struct r3v_native_ordered_operation *source_operation)
{
   if (source_operation == NULL ||
       source_operation->kind !=
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_ZMASK_INITIALIZE ||
       source_operation->payload.image_zmask_initialize.image == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   return r3v_native_record_zmask_initialize_state(
      command_buffer,
      r3v_native_image_to_handle(
         source_operation->payload.image_zmask_initialize.image),
      &source_operation->payload.image_zmask_initialize.source_owner,
      &source_operation->payload.image_zmask_initialize.source_metadata,
      &source_operation->payload.image_zmask_initialize.resulting_metadata);
}

static bool
r3v_native_zmask_fast_clear_source_valid(
   enum r3v_native_image_representation representation,
   const struct r3v_native_zmask_metadata_state *metadata)
{
   if (metadata == NULL)
      return false;
   switch (representation) {
   case R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED:
      return metadata->status == R3V_NATIVE_ZMASK_METADATA_RETIRED ||
             metadata->status == R3V_NATIVE_ZMASK_METADATA_INITIALIZED;
   case R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR:
      return metadata->status == R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR;
   case R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_COMPRESSED:
      return metadata->status == R3V_NATIVE_ZMASK_METADATA_COMPRESSED;
   default:
      return false;
   }
}

enum r3v_native_zmask_fast_clear_authority
r3v_native_zmask_fast_clear_select_facts(
   const struct r3v_native_zmask_fast_clear_facts *facts)
{
   if (facts == NULL)
      return R3V_NATIVE_ZMASK_FAST_CLEAR_ORDINARY;
   const bool common = facts->platform_qualified &&
                       facts->image_contract_qualified &&
                       facts->combined_aspects && facts->transfer_destination &&
                       facts->binding_valid && facts->layout_qualified &&
                       facts->materialization_scratch_valid &&
                       facts->fast_clear_plan_valid &&
                       facts->command_scope_valid;
   if (common && facts->automatic_qualified &&
       facts->automatic_source_valid)
      return R3V_NATIVE_ZMASK_FAST_CLEAR_AUTOMATIC;
   if (common && facts->experimental_gate &&
       facts->experimental_source_valid)
      return R3V_NATIVE_ZMASK_FAST_CLEAR_EXPERIMENTAL;
   return R3V_NATIVE_ZMASK_FAST_CLEAR_ORDINARY;
}

static const struct r3v_native_cmd_image_state *
r3v_native_zmask_find_image_state(
   const struct r3v_native_cmd_buffer *cmd_buffer,
   const struct r3v_native_image *image)
{
   if (cmd_buffer == NULL)
      return NULL;
   for (uint32_t index = 0u; index < cmd_buffer->image_state_count; index++) {
      if (cmd_buffer->image_states[index].image == image)
         return &cmd_buffer->image_states[index];
   }
   return NULL;
}

static bool
r3v_native_zmask_binding_valid(const struct r3v_native_image *image)
{
   return image != NULL && image->memory != NULL &&
          image->memory->bo.handle != 0u &&
          image->depth_bound.contract == &image->depth_contract &&
          image->depth_bound.binding_offset_bytes == image->memory_offset &&
          image->depth_bound.bo_bytes == image->memory->bo.size &&
          image->memory_offset <= image->memory->bo.size &&
          image->depth_contract.binding_bytes <=
             image->memory->bo.size - image->memory_offset &&
          image->depth_bound.surface_base_bytes >= image->memory_offset &&
          image->depth_bound.surface_base_bytes <= image->memory->bo.size &&
          image->depth_contract.layout.storage_bytes <=
             image->memory->bo.size - image->depth_bound.surface_base_bytes;
}

static bool
r3v_native_zmask_exact_image_contract(const struct r3v_native_image *image)
{
   return image != NULL && image->depth_family && image->optimal_tiling &&
          image->format == VK_FORMAT_D24_UNORM_S8_UINT &&
          image->image_type == VK_IMAGE_TYPE_2D && image->width == 64u &&
          image->height == 64u && image->depth == 1u &&
          image->array_layers == 1u && image->slice_count == 1u &&
          image->depth_contract.logical_extent.width == 64u &&
          image->depth_contract.logical_extent.height == 64u &&
          image->depth_contract.logical_extent.depth == 1u &&
          image->depth_contract.surface.pitch_pixels == 64u &&
          image->depth_contract.layout.bytes_per_pixel == 4u;
}

static bool
r3v_native_zmask_exact_layout(const struct r3v_native_image *image)
{
   return image != NULL && image->zmask_layout_admitted &&
          image->zmask_layout.fits_zmask_ram &&
          image->zmask_layout.stride_in_pixels == 64u &&
          image->zmask_layout.dwords == 16u &&
          image->zmask_layout.zmask_ram_dwords == 5120u &&
          !image->zmask_layout.zcomp8x8;
}

static bool
r3v_native_zmask_owner_materializable(
   const struct r3v_native_zmask_owner_state *owner, bool automatic)
{
   if (owner == NULL || owner->image == NULL || owner->metadata.generation == 0u)
      return false;
   const bool metadata_status_valid =
      owner->metadata.status == R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR ||
      (!automatic &&
       owner->metadata.status == R3V_NATIVE_ZMASK_METADATA_COMPRESSED);
   if (!metadata_status_valid)
      return false;
   if (!r3v_native_zmask_binding_valid(owner->image) ||
       !r3v_native_zmask_exact_layout(owner->image))
      return false;
   struct r300_zmask_materialize_plan plan;
   return r300_zmask_materialize_prefix(
             &owner->image->depth_contract.surface,
             &owner->image->zmask_layout, owner->metadata.clear_depth_code,
             owner->metadata.clear_stencil, &plan) == 0 &&
          r300_zmask_materialize_suffix(&plan) == 0;
}

enum r3v_native_zmask_fast_clear_authority
r3v_native_zmask_fast_clear_select(
   const struct r3v_native_device *device,
   const struct r3v_native_cmd_buffer *cmd_buffer,
   const struct r3v_native_image *image, uint32_t aspect_mask,
   uint32_t depth_code, uint32_t stencil)
{
   struct r3v_native_zmask_fast_clear_facts facts = {0};
   if (device == NULL || cmd_buffer == NULL || image == NULL)
      return R3V_NATIVE_ZMASK_FAST_CLEAR_ORDINARY;
   const struct r3v_native_cmd_image_state *state =
      r3v_native_zmask_find_image_state(cmd_buffer, image);
   const enum r3v_native_image_representation representation =
      state != NULL && state->current_representation_set
         ? state->current_representation
         : state != NULL ? state->required_representation
                         : image->committed_submission.representation;
   const struct r3v_native_zmask_metadata_state metadata =
      state != NULL && state->current_zmask_metadata_set
         ? state->current_zmask_metadata
         : state != NULL ? state->required_zmask_metadata
                         : image->committed_submission.zmask_metadata;
   const struct r3v_native_zmask_owner_state owner =
      cmd_buffer->current_zmask_owner_set
         ? cmd_buffer->current_zmask_owner
         : cmd_buffer->required_zmask_owner_set
              ? cmd_buffer->required_zmask_owner
              : device->zmask_owner;
   const struct r3v_native_zmask_owner_state no_owner = {0};
   const bool owner_absent = r3v_native_zmask_owner_equal(&owner, &no_owner);
   const bool owner_matches =
      owner.image == image &&
      r3v_native_zmask_metadata_equal(&owner.metadata, &metadata);
   const bool retired_target =
      representation == R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED &&
      metadata.status == R3V_NATIVE_ZMASK_METADATA_RETIRED;
   const bool fast_clear_target =
      representation == R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR &&
      metadata.status == R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR;
   const bool experimental_target =
      r3v_native_zmask_fast_clear_source_valid(representation, &metadata);
   struct r300_zmask_clear_plan plan;
   facts.automatic_qualified = device->zmask_automatic_qualified;
   facts.platform_qualified =
      device->pdevice != NULL &&
      device->pdevice->platform_id == R3V_NATIVE_ARMING_PLATFORM &&
      device->pdevice->pci_vendor_id == R3V_NATIVE_ARMING_PCI_VENDOR &&
      device->pdevice->pci_device_id == R3V_NATIVE_ARMING_PCI_DEVICE;
   facts.image_contract_qualified = r3v_native_zmask_exact_image_contract(image);
   facts.combined_aspects =
      aspect_mask == (R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH |
                      R300_ZB_COMBINED_CLEAR_ASPECT_STENCIL);
   facts.transfer_destination =
      (image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0u;
   facts.binding_valid = r3v_native_zmask_binding_valid(image);
   facts.layout_qualified = r3v_native_zmask_exact_layout(image);
   facts.materialization_scratch_valid =
      device->zmask_materialize_scratch_initialized &&
      device->zmask_materialize_vertex.bo.handle != 0u &&
      device->zmask_materialize_vertex.bo.size >=
         R3V_NATIVE_MEMORY_ALIGNMENT &&
      device->zmask_materialize_color.bo.handle != 0u &&
      device->zmask_materialize_color.bo.size >=
         R300_ZB_DEPTH_CONTROL_COLOR_BYTES;
   facts.fast_clear_plan_valid =
      stencil <= UINT8_MAX &&
      r300_zmask_fast_clear_plan_build(
         &image->depth_contract.surface, &image->zmask_layout, depth_code,
         stencil, &plan) == 0;
   facts.command_scope_valid =
      cmd_buffer->pass_target == NULL && !cmd_buffer->deferred_dispatch.pending;
   facts.automatic_source_valid =
      (retired_target && owner_absent) || (fast_clear_target && owner_matches) ||
      (retired_target && owner.image != image &&
       r3v_native_zmask_owner_materializable(&owner, true));
   facts.experimental_gate = device->zmask_fast_clear_gate != NULL;
   facts.experimental_source_valid =
      (retired_target && owner_absent) ||
      (experimental_target && owner_matches) ||
      (retired_target && owner.image != image &&
       r3v_native_zmask_owner_materializable(&owner, false));
   return r3v_native_zmask_fast_clear_select_facts(&facts);
}

static VkResult
r3v_native_record_zmask_fast_clear_state(
   VkCommandBuffer command_buffer, VkImage image_handle, uint32_t depth_code,
   uint32_t stencil,
   const enum r3v_native_image_representation *expected_representation,
   const struct r3v_native_zmask_metadata_state *expected_metadata,
   const struct r3v_native_zmask_metadata_state *fixed_resulting_metadata,
   enum r3v_native_zmask_fast_clear_authority authority)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, command_buffer);
   VK_FROM_HANDLE(r3v_native_image, image, image_handle);
   if (cmd_buffer == NULL || image == NULL || !image->depth_family ||
       image->memory == NULL || !image->zmask_layout_admitted ||
       image->depth_bound.contract != &image->depth_contract ||
       cmd_buffer->vk.base.device == NULL || stencil > UINT8_MAX ||
       cmd_buffer->pass_target != NULL || cmd_buffer->deferred_dispatch.pending)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);
   if ((authority == R3V_NATIVE_ZMASK_FAST_CLEAR_AUTOMATIC &&
        !device->zmask_automatic_qualified) ||
       (authority == R3V_NATIVE_ZMASK_FAST_CLEAR_EXPERIMENTAL &&
        device->zmask_fast_clear_gate == NULL))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   if (authority != R3V_NATIVE_ZMASK_FAST_CLEAR_AUTOMATIC &&
       authority != R3V_NATIVE_ZMASK_FAST_CLEAR_EXPERIMENTAL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_cmd_image_state *state =
      r3v_native_cmd_buffer_find_image_state(cmd_buffer, image);
   const enum r3v_native_image_representation representation =
      state != NULL && state->current_representation_set
         ? state->current_representation
         : state != NULL ? state->required_representation
                         : image->committed_submission.representation;
   const struct r3v_native_zmask_metadata_state metadata =
      state != NULL && state->current_zmask_metadata_set
         ? state->current_zmask_metadata
         : state != NULL ? state->required_zmask_metadata
                         : image->committed_submission.zmask_metadata;
   if (!r3v_native_zmask_fast_clear_source_valid(representation, &metadata) ||
       (expected_representation != NULL &&
        representation != *expected_representation) ||
       (expected_metadata != NULL &&
        !r3v_native_zmask_metadata_equal(&metadata, expected_metadata)))
      return VK_ERROR_INITIALIZATION_FAILED;

   const struct r3v_native_zmask_owner_state current_owner =
      cmd_buffer->current_zmask_owner_set
         ? cmd_buffer->current_zmask_owner
         : cmd_buffer->required_zmask_owner_set
              ? cmd_buffer->required_zmask_owner
              : device->zmask_owner;
   const struct r3v_native_zmask_owner_state no_owner = {0};
   if (metadata.status == R3V_NATIVE_ZMASK_METADATA_RETIRED) {
      if (!r3v_native_zmask_owner_equal(&current_owner, &no_owner))
         return VK_ERROR_INITIALIZATION_FAILED;
   } else if (current_owner.image != image ||
              !r3v_native_zmask_metadata_equal(&current_owner.metadata,
                                                &metadata)) {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   struct r3v_native_zmask_metadata_state resulting_metadata = {
      .status = R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR,
      .clear_depth_code = depth_code,
      .clear_stencil = stencil,
      .generation = fixed_resulting_metadata != NULL
                       ? fixed_resulting_metadata->generation
                       : p_atomic_inc_return(
                            &device->zmask_metadata_generation_counter),
   };
   if (fixed_resulting_metadata != NULL)
      resulting_metadata = *fixed_resulting_metadata;
   if (resulting_metadata.status != R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR ||
       resulting_metadata.clear_depth_code != depth_code ||
       resulting_metadata.clear_stencil != stencil ||
       resulting_metadata.generation == 0u)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r300_zmask_clear_plan plan;
   if (r300_zmask_fast_clear_plan_build(
          &image->depth_contract.surface, &image->zmask_layout, depth_code,
          stencil, &plan) != 0)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (r3v_native_cmd_buffer_reserve_ordered_operations(cmd_buffer, 1u) !=
       VK_SUCCESS)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   const uint32_t original_image_state_count = cmd_buffer->image_state_count;
   const uint32_t original_operation_count =
      cmd_buffer->ordered_operation_count;
   const bool original_required_owner_set =
      cmd_buffer->required_zmask_owner_set;
   const bool original_current_owner_set = cmd_buffer->current_zmask_owner_set;
   const struct r3v_native_zmask_owner_state original_required_owner =
      cmd_buffer->required_zmask_owner;
   const struct r3v_native_zmask_owner_state original_current_owner =
      cmd_buffer->current_zmask_owner;
   const struct r3v_native_cmd_image_state original_state =
      state != NULL ? *state : (struct r3v_native_cmd_image_state){0};

   struct r3v_native_zmask_owner_state resulting_owner;
   VkResult result = r3v_native_zmask_owner_from_image(
      image, &resulting_metadata, &resulting_owner);
   if (result == VK_SUCCESS)
      result = r3v_native_cmd_buffer_transition_image_representation(
         cmd_buffer, image, representation,
         R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR);
   if (result == VK_SUCCESS)
      result = r3v_native_cmd_buffer_transition_zmask_metadata(
         cmd_buffer, image, &metadata, &resulting_metadata);
   if (result == VK_SUCCESS)
      result = r3v_native_cmd_buffer_transition_zmask_owner(
         cmd_buffer, &current_owner, &resulting_owner);
   const struct r3v_native_bo_reference reference = {
      .handle = image->memory->bo.handle,
      .read_domains = RADEON_GEM_DOMAIN_GTT,
      .write_domain = RADEON_GEM_DOMAIN_GTT,
      .memory = image->memory,
   };
   if (result == VK_SUCCESS)
      result = r3v_native_cmd_buffer_append_raw_ib(
         device, cmd_buffer, plan.words, plan.dword_count, &reference, 1u,
         NULL, NULL, 0u);
   if (result == VK_SUCCESS) {
      cmd_buffer->ordered_operations[cmd_buffer->ordered_operation_count++] =
         (struct r3v_native_ordered_operation){
            .kind = R3V_NATIVE_ORDERED_OPERATION_IMAGE_FAST_CLEAR,
            .ib_position_dwords = cmd_buffer->ib_size_dwords,
            .payload.image_fast_clear = {
               .image = image,
               .authority = authority,
               .source_representation = representation,
               .source_metadata = metadata,
               .resulting_metadata = resulting_metadata,
            },
         };
      cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_ORDERED_IMAGE_COMPOSITION;
      return VK_SUCCESS;
   }

   cmd_buffer->ordered_operation_count = original_operation_count;
   cmd_buffer->required_zmask_owner_set = original_required_owner_set;
   cmd_buffer->current_zmask_owner_set = original_current_owner_set;
   cmd_buffer->required_zmask_owner = original_required_owner;
   cmd_buffer->current_zmask_owner = original_current_owner;
   if (state != NULL)
      *state = original_state;
   cmd_buffer->image_state_count = original_image_state_count;
   return result;
}

VkResult
r3v_native_record_zmask_fast_clear_with_authority(
   VkCommandBuffer command_buffer, VkImage image, uint32_t depth_code,
   uint32_t stencil, enum r3v_native_zmask_fast_clear_authority authority)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, command_buffer);
   VK_FROM_HANDLE(r3v_native_image, destination, image);
   if (cmd_buffer == NULL || destination == NULL ||
       cmd_buffer->vk.base.device == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);
   const struct r3v_native_zmask_owner_state current_owner =
      cmd_buffer->current_zmask_owner_set
         ? cmd_buffer->current_zmask_owner
         : cmd_buffer->required_zmask_owner_set
              ? cmd_buffer->required_zmask_owner
              : device->zmask_owner;
   if (current_owner.image == NULL || current_owner.image == destination)
      return r3v_native_record_zmask_fast_clear_state(
         command_buffer, image, depth_code, stencil, NULL, NULL, NULL,
         authority);

   enum r3v_native_image_representation owner_representation;
   switch (current_owner.metadata.status) {
   case R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR:
      owner_representation =
         R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR;
      break;
   case R3V_NATIVE_ZMASK_METADATA_COMPRESSED:
      owner_representation = R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_COMPRESSED;
      break;
   default:
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   const uint32_t original_ib_size = cmd_buffer->ib_size_dwords;
   const uint32_t original_reference_count = cmd_buffer->reference_count;
   const uint32_t original_operation_count =
      cmd_buffer->ordered_operation_count;
   const uint32_t original_image_state_count = cmd_buffer->image_state_count;
   const enum r3v_native_cell_kind original_cell_kind = cmd_buffer->cell_kind;
   const bool original_required_owner_set =
      cmd_buffer->required_zmask_owner_set;
   const bool original_current_owner_set = cmd_buffer->current_zmask_owner_set;
   const struct r3v_native_zmask_owner_state original_required_owner =
      cmd_buffer->required_zmask_owner;
   const struct r3v_native_zmask_owner_state original_current_owner =
      cmd_buffer->current_zmask_owner;
   struct r3v_native_cmd_image_state *original_image_states = NULL;
   if (original_image_state_count != 0u) {
      original_image_states =
         malloc((size_t)original_image_state_count *
                sizeof(*original_image_states));
      if (original_image_states == NULL)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      memcpy(original_image_states, cmd_buffer->image_states,
             (size_t)original_image_state_count *
                sizeof(*original_image_states));
   }

   VkResult result = r3v_native_record_zmask_materialize(
      command_buffer, r3v_native_image_to_handle(current_owner.image),
      owner_representation, &current_owner.metadata);
   if (result == VK_SUCCESS)
      result = r3v_native_record_zmask_fast_clear_state(
         command_buffer, image, depth_code, stencil, NULL, NULL, NULL,
         authority);
   if (result != VK_SUCCESS) {
      cmd_buffer->ib_size_dwords = original_ib_size;
      cmd_buffer->reference_count = original_reference_count;
      cmd_buffer->ordered_operation_count = original_operation_count;
      cmd_buffer->image_state_count = original_image_state_count;
      cmd_buffer->cell_kind = original_cell_kind;
      cmd_buffer->required_zmask_owner_set = original_required_owner_set;
      cmd_buffer->current_zmask_owner_set = original_current_owner_set;
      cmd_buffer->required_zmask_owner = original_required_owner;
      cmd_buffer->current_zmask_owner = original_current_owner;
      if (original_image_state_count != 0u)
         memcpy(cmd_buffer->image_states, original_image_states,
                (size_t)original_image_state_count *
                   sizeof(*original_image_states));
   }
   free(original_image_states);
   return result;
}

VkResult
r3v_native_record_zmask_fast_clear(VkCommandBuffer command_buffer,
                                   VkImage image, uint32_t depth_code,
                                   uint32_t stencil)
{
   return r3v_native_record_zmask_fast_clear_with_authority(
      command_buffer, image, depth_code, stencil,
      R3V_NATIVE_ZMASK_FAST_CLEAR_EXPERIMENTAL);
}

VkResult
r3v_native_replay_zmask_fast_clear(
   VkCommandBuffer command_buffer,
   const struct r3v_native_ordered_operation *source_operation)
{
   if (source_operation == NULL ||
       source_operation->kind !=
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_FAST_CLEAR ||
       source_operation->payload.image_fast_clear.image == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   const struct r3v_native_zmask_metadata_state *resulting =
      &source_operation->payload.image_fast_clear.resulting_metadata;
   const enum r3v_native_image_representation source_representation =
      source_operation->payload.image_fast_clear.source_representation;
   return r3v_native_record_zmask_fast_clear_state(
      command_buffer,
      r3v_native_image_to_handle(
         source_operation->payload.image_fast_clear.image),
      resulting->clear_depth_code, resulting->clear_stencil,
      &source_representation,
      &source_operation->payload.image_fast_clear.source_metadata, resulting,
      source_operation->payload.image_fast_clear.authority);
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
