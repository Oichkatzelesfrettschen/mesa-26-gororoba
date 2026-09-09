/* SPDX-License-Identifier: MIT */

#include "r3v_native.h"

#include "amd/r300/common/r300_rb2d_copy.h"
#include "amd/r300/common/r300_zb_tile_copy.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define R3V_RB2D_TILED_COPY_REFERENCE_SOURCE 0u
#define R3V_RB2D_TILED_COPY_REFERENCE_DESTINATION 1u
#define R3V_RB2D_TILED_COPY_REFERENCE_COUNT 2u

static int
build_stream(const struct r300_rb2d_copy_plan *copy_plan, uint32_t mask,
             uint32_t **words_out, uint32_t *dword_count_out)
{
   if (r300_rb2d_copy_plan_check(copy_plan) != R300_RB2D_COPY_OK)
      return -EINVAL;

   const uint32_t capacity = R300_RB2D_COPY_DWORDS(copy_plan->segment_count);
   uint32_t *words = calloc(capacity, sizeof(*words));
   if (words == NULL)
      return -ENOMEM;
   struct r300_rb2d_copy_ib emitted;
   int result = r300_rb2d_copy_emit_masked_into(copy_plan, mask, words,
                                                capacity, &emitted);
   if (result != 0 || emitted.ib_size_dwords != capacity ||
       r300_rb2d_copy_validate_reloc_sites(&emitted) != 0) {
      free(words);
      return result != 0 ? result : -EINVAL;
   }

   /* The common emitter numbers relocation sites independently.  Native
    * submission owns two references, so every source site names relocation
    * chunk zero and every destination site names chunk one.  The site list
    * was validated before any payload changes, and every index is therefore
    * inside the complete stream.
    */
   for (uint32_t site_index = 0; site_index < emitted.reloc_site_count;
        site_index++) {
      const struct r300_rb2d_copy_reloc_site *site =
         &emitted.reloc_sites[site_index];
      words[site->ib_index] =
         site->role == R300_RB2D_COPY_SLOT_SOURCE
            ? R3V_RB2D_TILED_COPY_REFERENCE_SOURCE * 4u
            : R3V_RB2D_TILED_COPY_REFERENCE_DESTINATION * 4u;
   }
   *words_out = words;
   *dword_count_out = capacity;
   return 0;
}

VkResult
r3v_native_record_rb2d_tiled_copy(
   VkCommandBuffer commandBuffer, VkDeviceMemory sourceMemory,
   VkDeviceMemory destinationMemory,
   const struct r300_zb_tile_copy_request *request)
{
   return r3v_native_record_rb2d_tiled_copy_masked(
      commandBuffer, sourceMemory, destinationMemory, request, UINT32_MAX);
}

VkResult
r3v_native_record_rb2d_tiled_copy_masked(
   VkCommandBuffer commandBuffer, VkDeviceMemory sourceMemory,
   VkDeviceMemory destinationMemory,
   const struct r300_zb_tile_copy_request *request, uint32_t mask)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, source_memory, sourceMemory);
   VK_FROM_HANDLE(r3v_native_memory, destination_memory, destinationMemory);
   if (cmd_buffer == NULL || source_memory == NULL || destination_memory == NULL ||
       request == NULL || source_memory == destination_memory ||
       source_memory->bo.handle == destination_memory->bo.handle ||
       request->same_buffer ||
       request->source.buffer_bytes != source_memory->bo.size ||
       request->destination.buffer_bytes != destination_memory->bo.size)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r300_zb_tile_copy_plan tile_plan;
   if (r300_zb_tile_copy_plan_build(request, &tile_plan) !=
       R300_ZB_TILE_COPY_OK)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct r300_rb2d_copy_segment
      segment_storage[R300_RB2D_COPY_MAX_SEGMENTS];
   struct r300_rb2d_copy_plan copy_plan;
   if (r300_rb2d_copy_plan_from_zb_tile(
          &tile_plan, source_memory->bo.size, destination_memory->bo.size,
          false, segment_storage, &copy_plan) != R300_RB2D_COPY_OK)
      return VK_ERROR_INITIALIZATION_FAILED;
   VkResult result = r3v_native_record_rb2d_copy(
      commandBuffer, sourceMemory, destinationMemory, &copy_plan, mask);
   if (result != VK_SUCCESS)
      return result;
   cmd_buffer->rb2d_tiled_copy_request = *request;
   cmd_buffer->rb2d_tiled_copy_write_mask = mask;
   cmd_buffer->rb2d_tiled_copy_configured = true;
   cmd_buffer->rb2d_copy_geometry = R3V_NATIVE_RB2D_COPY_GEOMETRY_TILE;
   return VK_SUCCESS;
}

VkResult
r3v_native_record_rb2d_copy(
   VkCommandBuffer commandBuffer, VkDeviceMemory sourceMemory,
   VkDeviceMemory destinationMemory, const struct r300_rb2d_copy_plan *plan,
   uint32_t mask)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, source_memory, sourceMemory);
   VK_FROM_HANDLE(r3v_native_memory, destination_memory, destinationMemory);
   if (cmd_buffer == NULL || source_memory == NULL || destination_memory == NULL ||
       plan == NULL || plan->segments == NULL || plan->same_buffer ||
       source_memory == destination_memory ||
       source_memory->bo.handle == destination_memory->bo.handle ||
       plan->source_buffer_bytes != source_memory->bo.size ||
       plan->destination_buffer_bytes != destination_memory->bo.size ||
       r300_rb2d_copy_plan_check(plan) != R300_RB2D_COPY_OK)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);
   uint32_t *ib = NULL;
   uint32_t ib_dwords = 0u;
   const int emit_result = build_stream(plan, mask, &ib, &ib_dwords);
   if (emit_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(emit_result));

   struct r3v_native_bo_reference *references =
      calloc(R3V_RB2D_TILED_COPY_REFERENCE_COUNT, sizeof(*references));
   if (references == NULL) {
      free(ib);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   references[R3V_RB2D_TILED_COPY_REFERENCE_SOURCE] =
      (struct r3v_native_bo_reference){
         .handle = source_memory->bo.handle,
         .read_domains = RADEON_GEM_DOMAIN_GTT,
         .write_domain = 0u,
         .memory = source_memory,
      };
   references[R3V_RB2D_TILED_COPY_REFERENCE_DESTINATION] =
      (struct r3v_native_bo_reference){
         .handle = destination_memory->bo.handle,
         .read_domains = 0u,
         .write_domain = RADEON_GEM_DOMAIN_GTT,
         .memory = destination_memory,
      };

   r3v_native_cmd_buffer_install_ib(
      cmd_buffer, R3V_NATIVE_CELL_KIND_RB2D_TILED_COPY_QUALIFICATION, ib,
      ib_dwords, references, R3V_RB2D_TILED_COPY_REFERENCE_COUNT);
   memcpy(cmd_buffer->rb2d_copy_segments, plan->segments,
          plan->segment_count * sizeof(cmd_buffer->rb2d_copy_segments[0]));
   cmd_buffer->rb2d_copy_segment_count = plan->segment_count;
   cmd_buffer->rb2d_copy_geometry = R3V_NATIVE_RB2D_COPY_GEOMETRY_SEGMENTS;
   cmd_buffer->rb2d_copy_source_buffer_bytes = plan->source_buffer_bytes;
   cmd_buffer->rb2d_copy_destination_buffer_bytes = plan->destination_buffer_bytes;
   cmd_buffer->rb2d_copy_byte_carrier = plan->byte_carrier;
   cmd_buffer->rb2d_tiled_copy_write_mask = mask;
   cmd_buffer->rb2d_tiled_copy_configured = true;
   return VK_SUCCESS;
}

bool
r3v_native_rb2d_tiled_copy_geometry_valid(
   const struct r3v_native_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer == NULL ||
       !cmd_buffer->rb2d_tiled_copy_configured ||
       cmd_buffer->cell_kind !=
          R3V_NATIVE_CELL_KIND_RB2D_TILED_COPY_QUALIFICATION ||
       cmd_buffer->reference_count != R3V_RB2D_TILED_COPY_REFERENCE_COUNT ||
       cmd_buffer->references == NULL)
      return false;
   const struct r3v_native_bo_reference *source =
      &cmd_buffer->references[R3V_RB2D_TILED_COPY_REFERENCE_SOURCE];
   const struct r3v_native_bo_reference *destination =
      &cmd_buffer->references[R3V_RB2D_TILED_COPY_REFERENCE_DESTINATION];
   if (source->memory == NULL || destination->memory == NULL ||
       source->memory == destination->memory || source->handle == destination->handle ||
       source->handle != source->memory->bo.handle ||
       destination->handle != destination->memory->bo.handle ||
       source->read_domains != RADEON_GEM_DOMAIN_GTT ||
       source->write_domain != 0u || destination->read_domains != 0u ||
       destination->write_domain != RADEON_GEM_DOMAIN_GTT ||
       (cmd_buffer->rb2d_copy_geometry ==
           R3V_NATIVE_RB2D_COPY_GEOMETRY_SEGMENTS &&
        (cmd_buffer->rb2d_copy_source_buffer_bytes != source->memory->bo.size ||
         cmd_buffer->rb2d_copy_destination_buffer_bytes !=
            destination->memory->bo.size)) ||
       cmd_buffer->rb2d_tiled_copy_request.same_buffer ||
       cmd_buffer->rb2d_tiled_copy_request.source.buffer_bytes !=
          source->memory->bo.size ||
       cmd_buffer->rb2d_tiled_copy_request.destination.buffer_bytes !=
          destination->memory->bo.size)
      return false;

   struct r300_rb2d_copy_segment segment_storage[R300_RB2D_COPY_MAX_SEGMENTS];
   struct r300_rb2d_copy_plan copy_plan;
   if (cmd_buffer->rb2d_copy_geometry ==
          R3V_NATIVE_RB2D_COPY_GEOMETRY_SEGMENTS) {
      if (cmd_buffer->rb2d_copy_segment_count == 0u ||
          cmd_buffer->rb2d_copy_segment_count > R300_RB2D_COPY_MAX_SEGMENTS)
         return false;
      memcpy(segment_storage, cmd_buffer->rb2d_copy_segments,
             cmd_buffer->rb2d_copy_segment_count * sizeof(segment_storage[0]));
      copy_plan = (struct r300_rb2d_copy_plan){
         .source_buffer_bytes = cmd_buffer->rb2d_copy_source_buffer_bytes,
         .destination_buffer_bytes = cmd_buffer->rb2d_copy_destination_buffer_bytes,
         .same_buffer = false,
         .segments = segment_storage,
         .segment_count = cmd_buffer->rb2d_copy_segment_count,
         .byte_carrier = cmd_buffer->rb2d_copy_byte_carrier,
      };
   } else {
      struct r300_zb_tile_copy_plan tile_plan;
      if (r300_zb_tile_copy_plan_build(&cmd_buffer->rb2d_tiled_copy_request,
                                        &tile_plan) != R300_ZB_TILE_COPY_OK ||
          r300_rb2d_copy_plan_from_zb_tile(
             &tile_plan, source->memory->bo.size, destination->memory->bo.size,
             false, segment_storage, &copy_plan) != R300_RB2D_COPY_OK)
         return false;
   }
   uint32_t *expected = NULL;
   uint32_t expected_dwords = 0u;
   const uint32_t mask = cmd_buffer->rb2d_tiled_copy_write_mask;
   const int result = build_stream(&copy_plan, mask, &expected,
                                   &expected_dwords);
   const bool valid = result == 0 && cmd_buffer->ib != NULL &&
                      cmd_buffer->ib_size_dwords == expected_dwords &&
                      memcmp(cmd_buffer->ib, expected,
                             (size_t)expected_dwords * sizeof(*expected)) == 0;
   free(expected);
   return valid;
}
