/* SPDX-License-Identifier: MIT */

#include "r3v_native.h"

#include "amd/r300/common/r300_rb2d_fill.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

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
                 .aspect_mask = aspect_mask,
                 .depth_code = depth_code,
                 .stencil = stencil,
              },
           });
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
