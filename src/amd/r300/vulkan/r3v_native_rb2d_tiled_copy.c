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
             uint32_t **words_out, uint32_t *dword_count_out,
             struct r300_rb2d_copy_ib *emitted_out)
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
   if (emitted_out != NULL)
      *emitted_out = emitted;
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
   if (cmd_buffer->rb2d_tiled_copy_configured)
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
   if ((!cmd_buffer->rb2d_tiled_copy_configured &&
        (cmd_buffer->ib_size_dwords != 0u || cmd_buffer->reference_count != 0u ||
         cmd_buffer->cell_kind != R3V_NATIVE_CELL_KIND_UNDECLARED)) ||
       (cmd_buffer->rb2d_tiled_copy_configured &&
        cmd_buffer->cell_kind !=
           R3V_NATIVE_CELL_KIND_RB2D_TILED_COPY_QUALIFICATION))
      return VK_ERROR_INITIALIZATION_FAILED;
   if (cmd_buffer->rb2d_tiled_copy_configured &&
       cmd_buffer->rb2d_copy_geometry ==
          R3V_NATIVE_RB2D_COPY_GEOMETRY_TILE)
      return VK_ERROR_INITIALIZATION_FAILED;
   uint32_t *ib = NULL;
   uint32_t ib_dwords = 0u;
   struct r300_rb2d_copy_ib emitted;
   const int emit_result = build_stream(plan, mask, &ib, &ib_dwords,
                                        &emitted);
   if (emit_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(emit_result));

   const uint32_t old_reference_count = cmd_buffer->reference_count;
   if (old_reference_count > UINT32_MAX -
          R3V_RB2D_TILED_COPY_REFERENCE_COUNT ||
       cmd_buffer->rb2d_copy_operation_count == UINT32_MAX ||
       cmd_buffer->ib_size_dwords > UINT32_MAX - ib_dwords) {
      free(ib);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   const uint32_t merged_capacity = old_reference_count +
                                     R3V_RB2D_TILED_COPY_REFERENCE_COUNT;
   struct r3v_native_bo_reference *merged =
      calloc(merged_capacity, sizeof(*merged));
   if (merged == NULL) {
      free(ib);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   memcpy(merged, cmd_buffer->references,
          (size_t)old_reference_count * sizeof(*merged));
   uint32_t merged_count = old_reference_count;
   const struct r3v_native_bo_reference new_references[] = {
      {
         .handle = source_memory->bo.handle,
         .read_domains = RADEON_GEM_DOMAIN_GTT,
         .write_domain = 0u,
         .memory = source_memory,
      },
      {
         .handle = destination_memory->bo.handle,
         .read_domains = 0u,
         .write_domain = RADEON_GEM_DOMAIN_GTT,
         .memory = destination_memory,
      },
   };
   uint32_t reference_index[R3V_RB2D_TILED_COPY_REFERENCE_COUNT];
   for (uint32_t new_index = 0; new_index <
        R3V_RB2D_TILED_COPY_REFERENCE_COUNT; new_index++) {
      uint32_t found = merged_count;
      for (uint32_t old_index = 0; old_index < merged_count; old_index++) {
         if (merged[old_index].handle != new_references[new_index].handle)
            continue;
         if (merged[old_index].memory != new_references[new_index].memory) {
            free(merged);
            free(ib);
            return VK_ERROR_INITIALIZATION_FAILED;
         }
         found = old_index;
         break;
      }
      if (found == merged_count)
         merged[merged_count++] = new_references[new_index];
      else {
         merged[found].read_domains |= new_references[new_index].read_domains;
         merged[found].write_domain |= new_references[new_index].write_domain;
      }
      reference_index[new_index] = found;
   }

   const uint32_t old_operation_count = cmd_buffer->rb2d_copy_operation_count;
   struct r3v_native_rb2d_copy_operation *operations = malloc(
      (size_t)(old_operation_count + 1u) * sizeof(*operations));
   if (operations == NULL) {
      free(merged);
      free(ib);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   const uint32_t old_ib_dwords = cmd_buffer->ib_size_dwords;
   uint32_t *combined = malloc((size_t)(old_ib_dwords + ib_dwords) *
                               sizeof(*combined));
   if (combined == NULL) {
      free(operations);
      free(merged);
      free(ib);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   if (old_operation_count)
      memcpy(operations, cmd_buffer->rb2d_copy_operations,
             (size_t)old_operation_count * sizeof(*operations));
   if (old_ib_dwords)
      memcpy(combined, cmd_buffer->ib,
             (size_t)old_ib_dwords * sizeof(*combined));
   for (uint32_t site_index = 0; site_index < emitted.reloc_site_count;
        site_index++) {
      const struct r300_rb2d_copy_reloc_site *site =
         &emitted.reloc_sites[site_index];
      ib[site->ib_index] = reference_index[
         site->role == R300_RB2D_COPY_SLOT_SOURCE
            ? R3V_RB2D_TILED_COPY_REFERENCE_SOURCE
            : R3V_RB2D_TILED_COPY_REFERENCE_DESTINATION] * 4u;
   }
   memcpy(combined + old_ib_dwords, ib,
          (size_t)ib_dwords * sizeof(*combined));
   free(ib);
   operations[old_operation_count] = (struct r3v_native_rb2d_copy_operation){
      .source_memory = source_memory,
      .destination_memory = destination_memory,
      .segment_count = plan->segment_count,
      .source_buffer_bytes = plan->source_buffer_bytes,
      .destination_buffer_bytes = plan->destination_buffer_bytes,
      .write_mask = mask,
      .byte_carrier = plan->byte_carrier,
   };
   memcpy(operations[old_operation_count].segments, plan->segments,
          plan->segment_count * sizeof(plan->segments[0]));

   free(cmd_buffer->references);
   free(cmd_buffer->ib);
   free(cmd_buffer->rb2d_copy_operations);
   cmd_buffer->ib = combined;
   cmd_buffer->ib_size_dwords = old_ib_dwords + ib_dwords;
   cmd_buffer->references = merged;
   cmd_buffer->reference_count = merged_count;
   cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_RB2D_TILED_COPY_QUALIFICATION;
   cmd_buffer->rb2d_copy_operations = operations;
   cmd_buffer->rb2d_copy_operation_count = old_operation_count + 1u;
   cmd_buffer->rb2d_copy_operation_capacity = old_operation_count + 1u;
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
       cmd_buffer->reference_count == 0u || cmd_buffer->references == NULL ||
       cmd_buffer->rb2d_copy_operation_count == 0u ||
       cmd_buffer->rb2d_copy_operations == NULL)
      return false;

   for (uint32_t reference_index = 0u;
        reference_index < cmd_buffer->reference_count; reference_index++) {
      const struct r3v_native_bo_reference *reference =
         &cmd_buffer->references[reference_index];
      if (reference->memory == NULL ||
         reference->handle != reference->memory->bo.handle)
         return false;
   }

   if (cmd_buffer->rb2d_copy_geometry == R3V_NATIVE_RB2D_COPY_GEOMETRY_TILE) {
      struct r300_zb_tile_copy_plan tile_plan;
      struct r300_rb2d_copy_segment tile_segments[R300_RB2D_COPY_MAX_SEGMENTS];
      struct r300_rb2d_copy_plan tile_copy_plan;
      const struct r3v_native_rb2d_copy_operation *operation =
         &cmd_buffer->rb2d_copy_operations[0];
      if (cmd_buffer->reference_count != R3V_RB2D_TILED_COPY_REFERENCE_COUNT ||
          r300_zb_tile_copy_plan_build(&cmd_buffer->rb2d_tiled_copy_request,
                                       &tile_plan) != R300_ZB_TILE_COPY_OK ||
          r300_rb2d_copy_plan_from_zb_tile(
             &tile_plan, cmd_buffer->references[0].memory->bo.size,
             cmd_buffer->references[1].memory->bo.size, false, tile_segments,
             &tile_copy_plan) != R300_RB2D_COPY_OK ||
          operation->source_buffer_bytes != tile_copy_plan.source_buffer_bytes ||
          operation->destination_buffer_bytes !=
             tile_copy_plan.destination_buffer_bytes ||
          operation->segment_count != tile_copy_plan.segment_count ||
          operation->byte_carrier != tile_copy_plan.byte_carrier ||
          memcmp(operation->segments, tile_copy_plan.segments,
                 operation->segment_count * sizeof(operation->segments[0])) != 0)
         return false;
   }

   uint32_t *expected = NULL;
   uint32_t expected_dwords = 0u;
   for (uint32_t operation_index = 0u;
        operation_index < cmd_buffer->rb2d_copy_operation_count;
        operation_index++) {
      const struct r3v_native_rb2d_copy_operation *operation =
         &cmd_buffer->rb2d_copy_operations[operation_index];
      if (operation->source_memory == NULL ||
          operation->destination_memory == NULL ||
          operation->source_memory == operation->destination_memory ||
          operation->source_memory->bo.handle ==
             operation->destination_memory->bo.handle ||
          operation->source_buffer_bytes != operation->source_memory->bo.size ||
          operation->destination_buffer_bytes !=
             operation->destination_memory->bo.size ||
          operation->segment_count == 0u ||
          operation->segment_count > R300_RB2D_COPY_MAX_SEGMENTS)
         goto invalid;

      uint32_t source_index = cmd_buffer->reference_count;
      uint32_t destination_index = cmd_buffer->reference_count;
      for (uint32_t reference_index = 0u;
           reference_index < cmd_buffer->reference_count; reference_index++) {
         const struct r3v_native_bo_reference *reference =
            &cmd_buffer->references[reference_index];
         if (reference->memory == operation->source_memory)
            source_index = reference_index;
         if (reference->memory == operation->destination_memory)
            destination_index = reference_index;
      }
      if (source_index == cmd_buffer->reference_count ||
          destination_index == cmd_buffer->reference_count ||
          (cmd_buffer->references[source_index].read_domains &
           RADEON_GEM_DOMAIN_GTT) == 0u ||
          (cmd_buffer->references[destination_index].write_domain &
           RADEON_GEM_DOMAIN_GTT) == 0u)
         goto invalid;

      const struct r300_rb2d_copy_plan copy_plan = {
         .source_buffer_bytes = operation->source_buffer_bytes,
         .destination_buffer_bytes = operation->destination_buffer_bytes,
         .same_buffer = false,
         .segments = operation->segments,
         .segment_count = operation->segment_count,
         .byte_carrier = operation->byte_carrier,
      };
      uint32_t *operation_words = NULL;
      uint32_t operation_dwords = 0u;
      struct r300_rb2d_copy_ib emitted;
      if (build_stream(&copy_plan, operation->write_mask, &operation_words,
                       &operation_dwords, &emitted) != 0)
         goto invalid;
      for (uint32_t site_index = 0u; site_index < emitted.reloc_site_count;
           site_index++) {
         const struct r300_rb2d_copy_reloc_site *site =
            &emitted.reloc_sites[site_index];
         operation_words[site->ib_index] =
            (site->role == R300_RB2D_COPY_SLOT_SOURCE ? source_index :
             destination_index) * 4u;
      }
      if (expected_dwords > UINT32_MAX - operation_dwords) {
         free(operation_words);
         goto invalid;
      }
      uint32_t *grown = realloc(expected,
                                (size_t)(expected_dwords + operation_dwords) *
                                   sizeof(*expected));
      if (grown == NULL) {
         free(operation_words);
         goto invalid;
      }
      expected = grown;
      memcpy(expected + expected_dwords, operation_words,
             (size_t)operation_dwords * sizeof(*expected));
      expected_dwords += operation_dwords;
      free(operation_words);
   }
   if (cmd_buffer->rb2d_copy_geometry == R3V_NATIVE_RB2D_COPY_GEOMETRY_TILE &&
       cmd_buffer->rb2d_copy_operation_count != 1u)
      goto invalid;
   const bool valid = cmd_buffer->ib != NULL &&
                      cmd_buffer->ib_size_dwords == expected_dwords &&
                      memcmp(cmd_buffer->ib, expected,
                             (size_t)expected_dwords * sizeof(*expected)) == 0;
   free(expected);
   return valid;

invalid:
   free(expected);
   return false;
}
