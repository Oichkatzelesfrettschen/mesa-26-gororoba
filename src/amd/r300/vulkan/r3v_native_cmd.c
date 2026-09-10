/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V command buffers: fixed-IB carriers for the device-internal
 * emitters.
 */

#include "r3v_native.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include "r3v_entrypoints.h"

#include "vk_alloc.h"
#include "vk_command_pool.h"

#include <stdlib.h>
#include <string.h>

static VkResult
r3v_native_cmd_buffer_reserve_array(struct vk_command_buffer *vk_cmd_buffer,
                                    void **storage, uint32_t count,
                                    uint32_t *capacity, uint32_t additional,
                                    size_t element_size, uint32_t initial)
{
   if (additional > UINT32_MAX - count)
      goto out_of_memory;
   const uint32_t required = count + additional;
   if (required <= *capacity)
      return VK_SUCCESS;

   uint32_t new_capacity = *capacity != 0 ? *capacity : initial;
   while (new_capacity < required) {
      if (new_capacity > UINT32_MAX / 2u)
         goto out_of_memory;
      new_capacity *= 2u;
   }
   if ((size_t)new_capacity > SIZE_MAX / element_size)
      goto out_of_memory;

   void *new_storage = vk_alloc(&vk_cmd_buffer->pool->alloc,
                                (size_t)new_capacity * element_size, 8,
                                VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (new_storage == NULL)
      goto out_of_memory;
   if (*storage != NULL && count != 0)
      memcpy(new_storage, *storage, (size_t)count * element_size);
   vk_free(&vk_cmd_buffer->pool->alloc, *storage);
   *storage = new_storage;
   *capacity = new_capacity;
   return VK_SUCCESS;

out_of_memory:
   vk_command_buffer_set_error(vk_cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY);
   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult
r3v_native_cmd_buffer_reserve_deferred_draws(
   struct r3v_native_cmd_buffer *cmd_buffer, uint32_t additional_count)
{
   if (additional_count > UINT32_MAX - cmd_buffer->deferred_draw_count)
      goto out_of_memory;

   const uint32_t required_count =
      cmd_buffer->deferred_draw_count + additional_count;
   if (required_count <= cmd_buffer->deferred_draw_capacity)
      return VK_SUCCESS;

   uint32_t new_capacity = cmd_buffer->deferred_draw_capacity != 0u
                              ? cmd_buffer->deferred_draw_capacity
                              : R3V_NATIVE_DEFERRED_DRAW_INITIAL_CAPACITY;
   while (new_capacity < required_count) {
      if (new_capacity > UINT32_MAX / 2u)
         goto out_of_memory;
      new_capacity *= 2u;
   }

   if ((size_t)new_capacity > SIZE_MAX / sizeof(*cmd_buffer->deferred_draws) ||
       (size_t)new_capacity > SIZE_MAX / sizeof(*cmd_buffer->owned_carriers) ||
       (size_t)new_capacity > SIZE_MAX / sizeof(*cmd_buffer->owned_color_sinks))
      goto out_of_memory;

   /* Reserve all three related arrays as one transaction.  A draw record
    * becomes visible only after every array has storage, so a failed growth
    * leaves the prior recording and its ownership vectors intact. */
   struct r3v_native_deferred_draw *new_draws = vk_alloc(
      &cmd_buffer->vk.pool->alloc,
      (size_t)new_capacity * sizeof(*new_draws), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   struct r3v_native_memory **new_carriers = vk_alloc(
      &cmd_buffer->vk.pool->alloc,
      (size_t)new_capacity * sizeof(*new_carriers), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   struct r3v_native_memory **new_color_sinks = vk_alloc(
      &cmd_buffer->vk.pool->alloc,
      (size_t)new_capacity * sizeof(*new_color_sinks), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (new_draws == NULL || new_carriers == NULL || new_color_sinks == NULL) {
      vk_free(&cmd_buffer->vk.pool->alloc, new_draws);
      vk_free(&cmd_buffer->vk.pool->alloc, new_carriers);
      vk_free(&cmd_buffer->vk.pool->alloc, new_color_sinks);
      goto out_of_memory;
   }

   memset(new_draws, 0, (size_t)new_capacity * sizeof(*new_draws));
   memset(new_carriers, 0, (size_t)new_capacity * sizeof(*new_carriers));
   memset(new_color_sinks, 0,
          (size_t)new_capacity * sizeof(*new_color_sinks));
   if (cmd_buffer->deferred_draw_count != 0u) {
      memcpy(new_draws, cmd_buffer->deferred_draws,
             (size_t)cmd_buffer->deferred_draw_count * sizeof(*new_draws));
      memcpy(new_carriers, cmd_buffer->owned_carriers,
             (size_t)cmd_buffer->deferred_draw_count *
                sizeof(*new_carriers));
      memcpy(new_color_sinks, cmd_buffer->owned_color_sinks,
             (size_t)cmd_buffer->deferred_draw_count *
                sizeof(*new_color_sinks));
   }

   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->deferred_draws);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->owned_carriers);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->owned_color_sinks);
   cmd_buffer->deferred_draws = new_draws;
   cmd_buffer->owned_carriers = new_carriers;
   cmd_buffer->owned_color_sinks = new_color_sinks;
   cmd_buffer->deferred_draw_capacity = new_capacity;
   return VK_SUCCESS;

out_of_memory:
   vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult
r3v_native_cmd_buffer_reserve_ordered_operations(
   struct r3v_native_cmd_buffer *cmd_buffer, uint32_t additional_count)
{
   return r3v_native_cmd_buffer_reserve_array(
      &cmd_buffer->vk, (void **)&cmd_buffer->ordered_operations,
      cmd_buffer->ordered_operation_count,
      &cmd_buffer->ordered_operation_capacity, additional_count,
      sizeof(*cmd_buffer->ordered_operations),
      R3V_NATIVE_ORDERED_OPERATION_INITIAL_CAPACITY);
}

VkResult
r3v_native_cmd_buffer_append_ordered_operation(
   struct r3v_native_cmd_buffer *cmd_buffer,
   const struct r3v_native_ordered_operation *operation)
{
   if (operation == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   VkResult result = r3v_native_cmd_buffer_reserve_ordered_operations(
      cmd_buffer, 1u);
   if (result != VK_SUCCESS)
      return result;
   cmd_buffer->ordered_operations[cmd_buffer->ordered_operation_count++] =
      *operation;
   return VK_SUCCESS;
}

VkResult
r3v_native_cmd_buffer_reserve_image_states(
   struct r3v_native_cmd_buffer *cmd_buffer, uint32_t additional_count)
{
   return r3v_native_cmd_buffer_reserve_array(
      &cmd_buffer->vk, (void **)&cmd_buffer->image_states,
      cmd_buffer->image_state_count, &cmd_buffer->image_state_capacity,
      additional_count, sizeof(*cmd_buffer->image_states),
      R3V_NATIVE_CMD_IMAGE_STATE_INITIAL_CAPACITY);
}

struct r3v_native_cmd_image_state *
r3v_native_cmd_buffer_find_image_state(
   struct r3v_native_cmd_buffer *cmd_buffer, struct r3v_native_image *image)
{
   if (image == NULL)
      return NULL;
   for (uint32_t index = 0; index < cmd_buffer->image_state_count; index++) {
      if (cmd_buffer->image_states[index].image == image)
         return &cmd_buffer->image_states[index];
   }
   return NULL;
}

VkResult
r3v_native_cmd_buffer_append_image_state(
   struct r3v_native_cmd_buffer *cmd_buffer, struct r3v_native_image *image,
   struct r3v_native_cmd_image_state **state_out)
{
   if (image == NULL || state_out == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct r3v_native_cmd_image_state *existing =
      r3v_native_cmd_buffer_find_image_state(cmd_buffer, image);
   if (existing != NULL) {
      *state_out = existing;
      return VK_SUCCESS;
   }

   VkResult result = r3v_native_cmd_buffer_reserve_image_states(cmd_buffer, 1u);
   if (result != VK_SUCCESS)
      return result;
   struct r3v_native_cmd_image_state *state =
      &cmd_buffer->image_states[cmd_buffer->image_state_count++];
   *state = (struct r3v_native_cmd_image_state){
      .image = image,
      .required_representation = image->committed_submission.representation,
      .required_representation_set = true,
      .required_zmask_metadata = image->committed_submission.zmask_metadata,
      .required_zmask_metadata_set = true,
   };
   *state_out = state;
   return VK_SUCCESS;
}

static bool
r3v_native_zmask_metadata_valid(
   const struct r3v_native_zmask_metadata_state *metadata)
{
   if (metadata == NULL ||
       metadata->status > R3V_NATIVE_ZMASK_METADATA_COMPRESSED)
      return false;
   if (metadata->status == R3V_NATIVE_ZMASK_METADATA_RETIRED)
      return metadata->clear_depth_code == 0u && metadata->clear_stencil == 0u &&
             metadata->generation == 0u;
   if (metadata->status == R3V_NATIVE_ZMASK_METADATA_INITIALIZED)
      return metadata->clear_depth_code == 0u && metadata->clear_stencil == 0u &&
             metadata->generation != 0u;
   return metadata->clear_depth_code <= 0xffffffu &&
          metadata->clear_stencil <= 0xffu && metadata->generation != 0u;
}

VkResult
r3v_native_cmd_buffer_transition_zmask_metadata(
   struct r3v_native_cmd_buffer *cmd_buffer, struct r3v_native_image *image,
   const struct r3v_native_zmask_metadata_state *required_metadata,
   const struct r3v_native_zmask_metadata_state *resulting_metadata)
{
   if (cmd_buffer == NULL || image == NULL ||
       !r3v_native_zmask_metadata_valid(required_metadata) ||
       !r3v_native_zmask_metadata_valid(resulting_metadata))
      return VK_ERROR_INITIALIZATION_FAILED;

   const uint32_t original_count = cmd_buffer->image_state_count;
   struct r3v_native_cmd_image_state *state = NULL;
   VkResult result = r3v_native_cmd_buffer_append_image_state(
      cmd_buffer, image, &state);
   if (result != VK_SUCCESS)
      return result;
   const struct r3v_native_cmd_image_state original = *state;
   const struct r3v_native_zmask_metadata_state *current_metadata =
      state->current_zmask_metadata_set ? &state->current_zmask_metadata
                                        : &state->required_zmask_metadata;
   if (!r3v_native_zmask_metadata_equal(current_metadata,
                                         required_metadata)) {
      *state = original;
      cmd_buffer->image_state_count = original_count;
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   state->current_zmask_metadata = *resulting_metadata;
   state->current_zmask_metadata_set = true;
   return VK_SUCCESS;
}

static bool
r3v_native_zmask_owner_valid(
   const struct r3v_native_zmask_owner_state *owner)
{
   if (owner == NULL)
      return false;
   if (owner->image == NULL) {
      const struct r3v_native_zmask_owner_state empty = {0};
      return r3v_native_zmask_owner_equal(owner, &empty);
   }
   return owner->image->zmask_layout_admitted &&
          owner->offset_dwords == 0u &&
          owner->dword_count == owner->image->zmask_layout.dwords &&
          owner->stride_in_pixels ==
             owner->image->zmask_layout.stride_in_pixels &&
          owner->zcomp8x8 == owner->image->zmask_layout.zcomp8x8 &&
          r3v_native_zmask_metadata_valid(&owner->metadata) &&
          owner->metadata.status != R3V_NATIVE_ZMASK_METADATA_RETIRED;
}

VkResult
r3v_native_zmask_owner_from_image(
   struct r3v_native_image *image,
   const struct r3v_native_zmask_metadata_state *metadata,
   struct r3v_native_zmask_owner_state *owner)
{
   if (image == NULL || metadata == NULL || owner == NULL ||
       !image->zmask_layout_admitted ||
       !r3v_native_zmask_metadata_valid(metadata) ||
       metadata->status == R3V_NATIVE_ZMASK_METADATA_RETIRED)
      return VK_ERROR_INITIALIZATION_FAILED;

   const struct r3v_native_zmask_owner_state candidate = {
      .image = image,
      .offset_dwords = 0u,
      .dword_count = image->zmask_layout.dwords,
      .stride_in_pixels = image->zmask_layout.stride_in_pixels,
      .zcomp8x8 = image->zmask_layout.zcomp8x8,
      .metadata = *metadata,
   };
   if (!r3v_native_zmask_owner_valid(&candidate))
      return VK_ERROR_INITIALIZATION_FAILED;
   *owner = candidate;
   return VK_SUCCESS;
}

VkResult
r3v_native_cmd_buffer_transition_zmask_owner(
   struct r3v_native_cmd_buffer *cmd_buffer,
   const struct r3v_native_zmask_owner_state *required_owner,
   const struct r3v_native_zmask_owner_state *resulting_owner)
{
   if (cmd_buffer == NULL || !r3v_native_zmask_owner_valid(required_owner) ||
       !r3v_native_zmask_owner_valid(resulting_owner))
      return VK_ERROR_INITIALIZATION_FAILED;
   if (required_owner->image != NULL && resulting_owner->image != NULL &&
       required_owner->image != resulting_owner->image)
      return VK_ERROR_INITIALIZATION_FAILED;

   const struct r3v_native_zmask_owner_state current_owner =
      cmd_buffer->current_zmask_owner_set ? cmd_buffer->current_zmask_owner
      : cmd_buffer->required_zmask_owner_set
         ? cmd_buffer->required_zmask_owner
         : *required_owner;
   if (cmd_buffer->required_zmask_owner_set &&
       !r3v_native_zmask_owner_equal(&current_owner, required_owner))
      return VK_ERROR_INITIALIZATION_FAILED;

   if (!cmd_buffer->required_zmask_owner_set) {
      cmd_buffer->required_zmask_owner = *required_owner;
      cmd_buffer->required_zmask_owner_set = true;
   }
   cmd_buffer->current_zmask_owner = *resulting_owner;
   cmd_buffer->current_zmask_owner_set = true;
   return VK_SUCCESS;
}

static bool
r3v_native_image_representation_valid(
   enum r3v_native_image_representation representation)
{
   return representation >= R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_LINEAR &&
          representation <= R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_COMPRESSED;
}

VkResult
r3v_native_cmd_buffer_transition_image_representation(
   struct r3v_native_cmd_buffer *cmd_buffer, struct r3v_native_image *image,
   enum r3v_native_image_representation required_representation,
   enum r3v_native_image_representation resulting_representation)
{
   if (cmd_buffer == NULL || image == NULL ||
       !r3v_native_image_representation_valid(required_representation) ||
       !r3v_native_image_representation_valid(resulting_representation))
      return VK_ERROR_INITIALIZATION_FAILED;

   const uint32_t original_count = cmd_buffer->image_state_count;
   struct r3v_native_cmd_image_state *state = NULL;
   VkResult result = r3v_native_cmd_buffer_append_image_state(
      cmd_buffer, image, &state);
   if (result != VK_SUCCESS)
      return result;
   const struct r3v_native_cmd_image_state original = *state;
   const enum r3v_native_image_representation current_representation =
      state->current_representation_set ? state->current_representation
                                        : state->required_representation;
   if (current_representation != required_representation) {
      *state = original;
      cmd_buffer->image_state_count = original_count;
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   state->current_representation = resulting_representation;
   state->current_representation_set = true;
   return VK_SUCCESS;
}

VkResult
r3v_native_cmd_buffer_require_image_layout(
   struct r3v_native_cmd_buffer *cmd_buffer, struct r3v_native_image *image,
   VkImageLayout layout, enum r3v_native_image_producer producer,
   bool writes_content)
{
   if (image != NULL && image->depth_family)
      layout = r3v_native_packed_depth_stencil_layout(layout);
   /* DEPTH_STENCIL_READ_ONLY_OPTIMAL permits depth and stencil reads while
    * forbidding every producer that changes packed attachment contents. */
   if (writes_content &&
       layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_cmd_image_state *state = NULL;
   VkResult result = r3v_native_cmd_buffer_append_image_state(
      cmd_buffer, image, &state);
   if (result != VK_SUCCESS)
      return result;

   const enum r3v_native_image_api_layout required_layout =
      (enum r3v_native_image_api_layout)layout;
   if (state->current_layout_set) {
      if (state->current_layout != required_layout)
         return VK_ERROR_INITIALIZATION_FAILED;
   } else {
      state->required_layout = required_layout;
      state->required_layout_set = true;
      state->current_layout = required_layout;
      state->current_layout_set = true;
   }
   if (writes_content) {
      state->producer = producer;
      state->visible_to =
         producer == R3V_NATIVE_IMAGE_PRODUCER_HOST
            ? R3V_NATIVE_IMAGE_VISIBLE_HOST
         : producer == R3V_NATIVE_IMAGE_PRODUCER_RB2D
            ? R3V_NATIVE_IMAGE_VISIBLE_RB2D
         : producer == R3V_NATIVE_IMAGE_PRODUCER_RB3D
            ? R3V_NATIVE_IMAGE_VISIBLE_RB3D
            : R3V_NATIVE_IMAGE_VISIBLE_ZB;
      state->content = R3V_NATIVE_IMAGE_CONTENT_INITIALIZED;
      state->producer_set = true;
      state->visibility_set = true;
      state->content_set = true;
   }
   return VK_SUCCESS;
}

VkResult
r3v_native_cmd_buffer_transition_image_layout(
   struct r3v_native_cmd_buffer *cmd_buffer, struct r3v_native_image *image,
   VkImageLayout old_layout, VkImageLayout new_layout)
{
   if (image != NULL && image->depth_family) {
      old_layout = r3v_native_packed_depth_stencil_layout(old_layout);
      new_layout = r3v_native_packed_depth_stencil_layout(new_layout);
   }
   struct r3v_native_cmd_image_state *state = NULL;
   VkResult result = r3v_native_cmd_buffer_append_image_state(
      cmd_buffer, image, &state);
   if (result != VK_SUCCESS)
      return result;

   const enum r3v_native_image_api_layout old_api_layout =
      (enum r3v_native_image_api_layout)old_layout;
   if (state->current_layout_set) {
      if (old_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
          state->current_layout != old_api_layout)
         return VK_ERROR_INITIALIZATION_FAILED;
   } else {
      state->required_layout = old_api_layout;
      state->required_layout_set = true;
   }
   state->current_layout = (enum r3v_native_image_api_layout)new_layout;
   state->current_layout_set = true;
   if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED)
      state->content = R3V_NATIVE_IMAGE_CONTENT_DISCARDED;
   if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED)
      state->content_set = true;
   return VK_SUCCESS;
}

VkResult
r3v_native_cmd_buffer_append_raw_ib(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer,
   const uint32_t *appended_dwords, uint32_t appended_dword_count,
   const struct r3v_native_bo_reference *new_references,
   uint32_t new_reference_count,
   const uint32_t *relocation_dword_indices,
   const uint32_t *relocation_reference_ordinals, uint32_t relocation_count)
{
   if ((appended_dword_count != 0 && appended_dwords == NULL) ||
       (new_reference_count != 0 && new_references == NULL) ||
       (relocation_count != 0 &&
        (relocation_dword_indices == NULL ||
         relocation_reference_ordinals == NULL)) ||
       (appended_dword_count == 0 && new_reference_count != 0) ||
       (cmd_buffer->ib_size_dwords != 0 && cmd_buffer->ib == NULL) ||
       (cmd_buffer->reference_count != 0 && cmd_buffer->references == NULL))
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   if (appended_dword_count > UINT32_MAX - cmd_buffer->ib_size_dwords ||
       new_reference_count > UINT32_MAX - cmd_buffer->reference_count)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   const uint32_t combined_dword_count =
      cmd_buffer->ib_size_dwords + appended_dword_count;
   const uint32_t combined_reference_capacity =
      cmd_buffer->reference_count + new_reference_count;
   if ((size_t)combined_dword_count > SIZE_MAX / sizeof(uint32_t) ||
       (size_t)combined_reference_capacity >
          SIZE_MAX / sizeof(*cmd_buffer->references) ||
       (size_t)new_reference_count > SIZE_MAX / sizeof(uint32_t))
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   uint32_t *appended_copy = NULL;
   uint32_t *combined_ib = NULL;
   struct r3v_native_bo_reference *combined_references = NULL;
   uint32_t *reference_indices = NULL;
   if (appended_dword_count != 0) {
      appended_copy = malloc((size_t)appended_dword_count * sizeof(*appended_copy));
      if (appended_copy == NULL)
         goto out_of_memory;
      memcpy(appended_copy, appended_dwords,
             (size_t)appended_dword_count * sizeof(*appended_copy));
   }
   if (combined_dword_count != 0) {
      combined_ib = malloc((size_t)combined_dword_count * sizeof(*combined_ib));
      if (combined_ib == NULL)
         goto out_of_memory;
      if (cmd_buffer->ib_size_dwords != 0)
         memcpy(combined_ib, cmd_buffer->ib,
                (size_t)cmd_buffer->ib_size_dwords * sizeof(*combined_ib));
      if (appended_dword_count != 0)
         memcpy(combined_ib + cmd_buffer->ib_size_dwords, appended_copy,
                (size_t)appended_dword_count * sizeof(*combined_ib));
   }
   if (combined_reference_capacity != 0) {
      combined_references = malloc((size_t)combined_reference_capacity *
                                   sizeof(*combined_references));
      if (combined_references == NULL)
         goto out_of_memory;
      if (cmd_buffer->reference_count != 0)
         memcpy(combined_references, cmd_buffer->references,
                (size_t)cmd_buffer->reference_count *
                   sizeof(*combined_references));
   }
   if (new_reference_count != 0) {
      reference_indices = malloc((size_t)new_reference_count *
                                 sizeof(*reference_indices));
      if (reference_indices == NULL)
         goto out_of_memory;
   }

   uint32_t merged_count = cmd_buffer->reference_count;
   for (uint32_t input_index = 0; input_index < new_reference_count;
        input_index++) {
      const struct r3v_native_bo_reference *input =
         &new_references[input_index];
      uint32_t merged_index = merged_count;
      for (uint32_t existing_index = 0; existing_index < merged_count;
           existing_index++) {
         struct r3v_native_bo_reference *existing =
            &combined_references[existing_index];
         if (existing->handle != input->handle)
            continue;
         if (existing->memory != input->memory) {
            free(reference_indices);
            free(combined_references);
            free(combined_ib);
            free(appended_copy);
            return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
         }
         existing->read_domains |= input->read_domains;
         existing->write_domain |= input->write_domain;
         merged_index = existing_index;
         break;
      }
      if (merged_index == merged_count)
         combined_references[merged_count++] = *input;
      reference_indices[input_index] = merged_index;
   }

   for (uint32_t relocation_index = 0; relocation_index < relocation_count;
        relocation_index++) {
      const uint32_t dword_index = relocation_dword_indices[relocation_index];
      const uint32_t reference_ordinal =
         relocation_reference_ordinals[relocation_index];
      if (dword_index >= appended_dword_count ||
          reference_ordinal >= new_reference_count) {
         free(reference_indices);
         free(combined_references);
         free(combined_ib);
         free(appended_copy);
         return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      }
      for (uint32_t previous = 0; previous < relocation_index; previous++) {
         if (relocation_dword_indices[previous] == dword_index) {
            free(reference_indices);
            free(combined_references);
            free(combined_ib);
            free(appended_copy);
            return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
         }
      }
      if (reference_indices[reference_ordinal] > UINT32_MAX / 4u) {
         free(reference_indices);
         free(combined_references);
         free(combined_ib);
         free(appended_copy);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      appended_copy[dword_index] =
         reference_indices[reference_ordinal] * 4u;
   }
   if (appended_dword_count != 0)
      memcpy(combined_ib + cmd_buffer->ib_size_dwords, appended_copy,
             (size_t)appended_dword_count * sizeof(*combined_ib));

   free(cmd_buffer->ib);
   free(cmd_buffer->references);
   free(reference_indices);
   free(appended_copy);
   cmd_buffer->ib = combined_ib;
   cmd_buffer->ib_size_dwords = combined_dword_count;
   cmd_buffer->references = combined_references;
   cmd_buffer->reference_count = merged_count;
   return VK_SUCCESS;

out_of_memory:
   free(reference_indices);
   free(combined_references);
   free(combined_ib);
   free(appended_copy);
   return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
}

void
r3v_native_cmd_buffer_release_ib(struct r3v_native_cmd_buffer *cmd_buffer)
{
   /* radeon_drm_vk_cs_build binds the IB chunk to this pointer rather than
    * copying the dwords (rg --fixed-strings RADEON_CHUNK_ID_IB
    * src/amd/radeon/drm_vk/radeon_drm_vk_cs.c), so a prepared submission
    * names storage this free returns to the allocator.  The prepared state
    * admits a commit on command-buffer pointer equality alone, and both a
    * reset and a destroy-then-reallocate keep that pointer equal, so the IB
    * release is the point that retires the transport holding it.  A
    * host-model carrier installs an IB with no device attached, so the
    * base device decides whether a prepared submission can exist at all.
    */
   if (cmd_buffer->vk.base.device != NULL) {
      struct r3v_native_device *device = container_of(
         cmd_buffer->vk.base.device, struct r3v_native_device, vk);
      if (device->prepared.valid && device->prepared.cmd_buffer == cmd_buffer)
         r3v_native_prepared_release(device);
   }

   free(cmd_buffer->ib);
   free(cmd_buffer->window_space_ib);
   free(cmd_buffer->references);
   cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_UNDECLARED;
   cmd_buffer->rb2d_tiled_copy_configured = false;
   cmd_buffer->rb2d_tiled_copy_write_mask = UINT32_MAX;
   cmd_buffer->zb_depth_clear_configured = false;
   cmd_buffer->zb_depth_clear_image = NULL;
   cmd_buffer->zb_depth_clear_aspect_mask = 0u;
   cmd_buffer->zb_depth_clear_depth_code = 0u;
   cmd_buffer->zb_depth_clear_stencil = 0u;
   cmd_buffer->rb2d_copy_geometry = R3V_NATIVE_RB2D_COPY_GEOMETRY_TILE;
   cmd_buffer->rb2d_copy_segment_count = 0u;
   cmd_buffer->rb2d_copy_source_buffer_bytes = 0u;
   cmd_buffer->rb2d_copy_destination_buffer_bytes = 0u;
   cmd_buffer->rb2d_copy_byte_carrier = false;
   free(cmd_buffer->rb2d_copy_operations);
   cmd_buffer->rb2d_copy_operations = NULL;
   cmd_buffer->rb2d_copy_operation_count = 0u;
   cmd_buffer->rb2d_copy_operation_capacity = 0u;
   cmd_buffer->zb_persistence_configured = false;
   cmd_buffer->zb_persistence_vertex = NULL;
   cmd_buffer->zb_persistence_vertex_generation = 0u;
   cmd_buffer->zb_persistence_vertex_handle = 0u;
   cmd_buffer->zb_persistence_depth_a = NULL;
   cmd_buffer->zb_persistence_depth_b = NULL;
   cmd_buffer->ib = NULL;
   cmd_buffer->ib_size_dwords = 0;
   cmd_buffer->window_space_ib = NULL;
   cmd_buffer->window_space_ib_size_dwords = 0;
   cmd_buffer->references = NULL;
   cmd_buffer->reference_count = 0;
   cmd_buffer->burst_draws = 0;
}

void
r3v_native_cmd_buffer_release_recording(
   struct r3v_native_cmd_buffer *cmd_buffer)
{
   for (uint32_t i = 0; i < cmd_buffer->deferred_draw_capacity; i++) {
      if (cmd_buffer->owned_carriers[i] == NULL)
         continue;
      struct r3v_native_device *device = container_of(
         cmd_buffer->vk.base.device, struct r3v_native_device, vk);
      radeon_drm_vk_bo_free(&device->drm, &cmd_buffer->owned_carriers[i]->bo);
      vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->owned_carriers[i]);
      cmd_buffer->owned_carriers[i] = NULL;
   }
   for (uint32_t i = 0; i < cmd_buffer->deferred_draw_capacity; i++) {
      if (cmd_buffer->owned_color_sinks[i] == NULL)
         continue;
      struct r3v_native_device *device = container_of(
         cmd_buffer->vk.base.device, struct r3v_native_device, vk);
      radeon_drm_vk_bo_free(&device->drm,
                            &cmd_buffer->owned_color_sinks[i]->bo);
      vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->owned_color_sinks[i]);
      cmd_buffer->owned_color_sinks[i] = NULL;
   }
   if (cmd_buffer->owned_slot != NULL) {
      struct r3v_native_device *device = container_of(
         cmd_buffer->vk.base.device, struct r3v_native_device, vk);
      radeon_drm_vk_bo_free(&device->drm, &cmd_buffer->owned_slot->bo);
      vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->owned_slot);
      cmd_buffer->owned_slot = NULL;
   }
   if (cmd_buffer->owned_multisample != NULL) {
      struct r3v_native_device *device = container_of(
         cmd_buffer->vk.base.device, struct r3v_native_device, vk);
      radeon_drm_vk_bo_free(&device->drm,
                            &cmd_buffer->owned_multisample->bo);
      vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->owned_multisample);
      cmd_buffer->owned_multisample = NULL;
   }
   for (uint32_t i = 0; i < cmd_buffer->deferred_copy_count; i++)
      vk_free(&cmd_buffer->vk.pool->alloc,
              cmd_buffer->deferred_copies[i].update_data);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->deferred_copies);
   cmd_buffer->deferred_copies = NULL;
   cmd_buffer->deferred_copy_capacity = 0;
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->ordered_operations);
   cmd_buffer->ordered_operations = NULL;
   cmd_buffer->ordered_operation_count = 0;
   cmd_buffer->ordered_operation_capacity = 0;
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->image_states);
   cmd_buffer->image_states = NULL;
   cmd_buffer->image_state_count = 0;
   cmd_buffer->image_state_capacity = 0;
   cmd_buffer->required_zmask_owner =
      (struct r3v_native_zmask_owner_state){0};
   cmd_buffer->current_zmask_owner =
      (struct r3v_native_zmask_owner_state){0};
   cmd_buffer->required_zmask_owner_set = false;
   cmd_buffer->current_zmask_owner_set = false;
   cmd_buffer->pass_target = NULL;
   cmd_buffer->pass_depth_target = NULL;
   cmd_buffer->active_render_pass = NULL;
   cmd_buffer->pass_color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
   cmd_buffer->pass_depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
   cmd_buffer->pass_color_final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
   cmd_buffer->pass_depth_final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
   cmd_buffer->pass_target_layer_offset = 0;
   cmd_buffer->bound_pipeline = NULL;
   cmd_buffer->bound_graphics_set = NULL;
   cmd_buffer->viewport_set = false;
   cmd_buffer->scissor_set = false;
   cmd_buffer->query_op_count = 0;
   cmd_buffer->active_query_pool = NULL;
   cmd_buffer->event_op_count = 0;
   memset(cmd_buffer->bound_vertex_buffers, 0,
          sizeof(cmd_buffer->bound_vertex_buffers));
   memset(cmd_buffer->bound_vertex_offsets, 0,
          sizeof(cmd_buffer->bound_vertex_offsets));
   cmd_buffer->vertex_bound_mask = 0;
   cmd_buffer->bound_index_buffer = NULL;
   cmd_buffer->bound_index_offset = 0;
   cmd_buffer->bound_index_bytes = 0;
   cmd_buffer->draw_recorded = false;
   for (uint32_t i = 0; i < cmd_buffer->deferred_draw_capacity; i++)
      free(cmd_buffer->deferred_draws[i].alternate_ib);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->deferred_draws);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->owned_carriers);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->owned_color_sinks);
   cmd_buffer->deferred_draws = NULL;
   cmd_buffer->owned_carriers = NULL;
   cmd_buffer->owned_color_sinks = NULL;
   cmd_buffer->deferred_draw_capacity = 0;
   cmd_buffer->deferred_draw_count = 0;
   cmd_buffer->render_pass_count = 0;
   cmd_buffer->active_pass_draw_index = 0;
   cmd_buffer->deferred_copy_count = 0;
   /* The routed record describes the copies this reset just dropped, so it
    * goes with them.  A record surviving the reset would report the next
    * recording as one a GPU route performs, and the submission boundary's
    * policy accounting reads exactly that flag. */
   cmd_buffer->fill_route_active = false;
   cmd_buffer->fill_route_provenance = (struct r3v_execution_provenance){0};
   cmd_buffer->bound_compute_pipeline = NULL;
   cmd_buffer->bound_compute_set = NULL;
   vk_free(&cmd_buffer->vk.pool->alloc,
           cmd_buffer->deferred_dispatch.gpu_expected);
   cmd_buffer->deferred_dispatch =
      (struct r3v_native_deferred_dispatch){0};
}

VkResult
r3v_native_cmd_buffer_append_ib(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r300_tcl_bypass_triangle_ib *cell,
   const struct r3v_native_bo_reference *references,
   const uint32_t *reference_slots, uint32_t reference_count,
   struct r300_tcl_bypass_triangle_ib *alternate_cell)
{
   /* slot_index below holds one entry per relocation slot the triangle
    * cells declare, and the appended cell's payloads are bound through
    * it, so a reference list longer than that has no slot to name and
    * refuses before the array is written.
    */
   if (cmd_buffer->ib == NULL || cmd_buffer->ib_size_dwords == 0 ||
       reference_count == 0 ||
       reference_count > R300_TRIANGLE_SLOT_COUNT)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   /* The merged array holds the installed entries plus at most one new
    * entry per appended reference, so one allocation covers the union
    * before any of it is committed.
    */
   const uint32_t merged_capacity =
      cmd_buffer->reference_count + reference_count;
   struct r3v_native_bo_reference *merged =
      calloc(merged_capacity, sizeof(*merged));
   if (merged == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   memcpy(merged, cmd_buffer->references,
          (size_t)cmd_buffer->reference_count * sizeof(*merged));
   uint32_t merged_count = cmd_buffer->reference_count;

   /* The winsys rule, applied here so the queue's own merge over the
    * result is idempotent: first-add order, one entry per handle,
    * domains ORed.  slot_index[slot] is the position the appended
    * cell's payload for that slot must name.
    */
   uint32_t slot_index[R300_TRIANGLE_SLOT_COUNT] = { 0 };
   uint32_t populated_slots = 0;
   for (uint32_t reference = 0; reference < reference_count; reference++) {
      const uint32_t slot =
         reference_slots != NULL ? reference_slots[reference] : reference;
      if (slot >= R300_TRIANGLE_SLOT_COUNT ||
          (populated_slots & (1u << slot)) != 0) {
         free(merged);
         return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      }
      populated_slots |= 1u << slot;
      uint32_t found = merged_count;
      for (uint32_t i = 0; i < merged_count; i++) {
         if (merged[i].handle == references[reference].handle) {
            found = i;
            break;
         }
      }
      if (found == merged_count)
         merged[merged_count++] = references[reference];
      merged[found].read_domains |= references[reference].read_domains;
      merged[found].write_domain |= references[reference].write_domain;
      slot_index[slot] = found;
   }

   const struct r300_tcl_bypass_triangle_ib *cells[] = {cell,
                                                        alternate_cell};
   for (uint32_t candidate = 0; candidate < ARRAY_SIZE(cells); candidate++) {
      const struct r300_tcl_bypass_triangle_ib *candidate_cell =
         cells[candidate];
      if (candidate_cell == NULL)
         continue;
      for (uint32_t site = 0; site < candidate_cell->reloc_site_count; site++) {
         const uint32_t slot = candidate_cell->reloc_sites[site].slot;
         if (slot >= R300_TRIANGLE_SLOT_COUNT ||
             (populated_slots & (1u << slot)) == 0) {
            free(merged);
            return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
         }
      }
   }

   /* An alternate cell shares the appended cell's references, so its
    * relocations bind to the same merged indices and the cell can
    * replace the appended span in place. */
   int bound = r300_tcl_bypass_triangle_bind_reloc_indices(
      cell, slot_index, R300_TRIANGLE_SLOT_COUNT);
   if (bound == 0 && alternate_cell != NULL)
      bound = r300_tcl_bypass_triangle_bind_reloc_indices(
         alternate_cell, slot_index, R300_TRIANGLE_SLOT_COUNT);
   if (bound != 0) {
      free(merged);
      return vk_error(device, r3v_native_cell_vk_result_from_errno(bound));
   }

   const uint32_t base = cmd_buffer->ib_size_dwords;
   const uint32_t total = base + cell->ib_size_dwords;
   uint32_t *ib = realloc(cmd_buffer->ib, (size_t)total * sizeof(uint32_t));
   if (ib == NULL) {
      free(merged);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   memcpy(ib + base, cell->ib,
          (size_t)cell->ib_size_dwords * sizeof(uint32_t));

   /* Every fallible step has completed: commit the concatenation. */
   free(cmd_buffer->references);
   cmd_buffer->ib = ib;
   cmd_buffer->ib_size_dwords = total;
   cmd_buffer->references = merged;
   cmd_buffer->reference_count = merged_count;
   if (cmd_buffer->cell_kind == R3V_NATIVE_CELL_KIND_ZB_DEPTH_CLEAR ||
       cmd_buffer->cell_kind ==
          R3V_NATIVE_CELL_KIND_RB2D_TILED_COPY_QUALIFICATION ||
       cmd_buffer->cell_kind ==
          R3V_NATIVE_CELL_KIND_ORDERED_IMAGE_COMPOSITION)
      cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_ORDERED_IMAGE_COMPOSITION;
   else
      cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_TRIANGLE_MULTI_PASS;
   return VK_SUCCESS;
}

void
r3v_native_cmd_buffer_install_ib(struct r3v_native_cmd_buffer *cmd_buffer,
                                 enum r3v_native_cell_kind kind,
                                 uint32_t *ib, uint32_t ib_size_dwords,
                                 struct r3v_native_bo_reference *references,
                                 uint32_t reference_count)
{
   r3v_native_cmd_buffer_release_ib(cmd_buffer);
   cmd_buffer->cell_kind = kind;
   cmd_buffer->ib = ib;
   cmd_buffer->ib_size_dwords = ib_size_dwords;
   cmd_buffer->references = references;
   cmd_buffer->reference_count = reference_count;
}

static VkResult
r3v_native_cmd_buffer_create(struct vk_command_pool *pool,
                             VkCommandBufferLevel level,
                             struct vk_command_buffer **cmd_buffer_out)
{
   struct r3v_native_cmd_buffer *cmd_buffer =
      vk_zalloc(&pool->alloc, sizeof(*cmd_buffer), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkResult result = vk_command_buffer_init(pool, &cmd_buffer->vk,
                                            &r3v_native_cmd_buffer_ops,
                                            level);
   if (result != VK_SUCCESS) {
      vk_free(&pool->alloc, cmd_buffer);
      return result;
   }

   *cmd_buffer_out = &cmd_buffer->vk;
   return VK_SUCCESS;
}

static void
r3v_native_cmd_buffer_reset(struct vk_command_buffer *cmd_buffer_base,
                            UNUSED VkCommandBufferResetFlags flags)
{
   struct r3v_native_cmd_buffer *cmd_buffer =
      container_of(cmd_buffer_base, struct r3v_native_cmd_buffer, vk);
   vk_command_buffer_reset(&cmd_buffer->vk);
   r3v_native_cmd_buffer_release_ib(cmd_buffer);
   r3v_native_cmd_buffer_release_recording(cmd_buffer);
}

static void
r3v_native_cmd_buffer_destroy(struct vk_command_buffer *cmd_buffer_base)
{
   struct r3v_native_cmd_buffer *cmd_buffer =
      container_of(cmd_buffer_base, struct r3v_native_cmd_buffer, vk);
   r3v_native_cmd_buffer_release_ib(cmd_buffer);
   r3v_native_cmd_buffer_release_recording(cmd_buffer);
   vk_command_buffer_finish(&cmd_buffer->vk);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer);
}

const struct vk_command_buffer_ops r3v_native_cmd_buffer_ops = {
   .create = r3v_native_cmd_buffer_create,
   .reset = r3v_native_cmd_buffer_reset,
   .destroy = r3v_native_cmd_buffer_destroy,
};

/* The runtime lifecycle carries the fail-closed recording contract: begin
 * enters RECORDING (an implicit re-begin resets, releasing any installed
 * IB), a poisoned recording ends INVALID with its recorded error returned,
 * and the queue admits only EXECUTABLE buffers.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                       const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   vk_command_buffer_begin(cmd_buffer, pBeginInfo);
   if (vk_command_buffer_get_record_result(cmd_buffer) == VK_SUCCESS) {
      const VkResult draw_storage_result =
         r3v_native_cmd_buffer_reserve_deferred_draws(
            container_of(cmd_buffer, struct r3v_native_cmd_buffer, vk), 1u);
      if (draw_storage_result != VK_SUCCESS)
         return draw_storage_result;
   }
   return vk_command_buffer_get_record_result(cmd_buffer);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   /* A render pass left open has no closing lowering, and a query
    * left active has no end publishing its availability, so the buffer
    * poisons instead of becoming executable with either incomplete.
    */
   if (cmd_buffer->pass_target != NULL ||
       cmd_buffer->active_query_pool != NULL) {
      vk_command_buffer_set_error(&cmd_buffer->vk,
                                  R3V_NATIVE_REFUSAL_RESULT);
   }
   return vk_command_buffer_end(&cmd_buffer->vk);
}
