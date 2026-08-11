/*
 * SPDX-License-Identifier: MIT
 *
 * Deferred transfer-copy execution: recorded regions move through host
 * mappings of the bound memory at queue submission.
 */

#include "r3v_native.h"

#include "vk_log.h"

#include <string.h>

/* Resolves one memory object's mapping for the copy, reusing a live
 * application mapping and owning a fresh one otherwise; the owned flag
 * tells the release which case it holds.
 */
static VkResult
map_memory(struct r3v_native_device *device,
           struct r3v_native_memory *memory, uint8_t **map_out,
           bool *owned_out)
{
   *owned_out = memory->map == NULL;
   if (*owned_out &&
       radeon_drm_vk_bo_map(&device->drm, &memory->bo, &memory->map) != 0) {
      *owned_out = false;
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: transfer memory is not CPU-mappable "
                       "at submission");
   }
   *map_out = memory->map;
   return VK_SUCCESS;
}

static void
release_memory(struct r3v_native_device *device,
               struct r3v_native_memory *memory, bool owned)
{
   if (!owned)
      return;
   /* Host writes publish while the address is live, the same
    * unsnooped-GART contract vkUnmapMemory holds.
    */
   radeon_drm_vk_bo_cache_sync(&device->drm, memory->map, memory->bo.size);
   radeon_drm_vk_bo_unmap(&device->drm, &memory->bo, memory->map);
   memory->map = NULL;
}

/* One recorded copy: the record-time admission proved every byte
 * bound, including the bind ranges, so execution's own failure surface
 * is the mapping ioctl and a resource unbound between record and
 * submit.  memmove carries the image-to-image case whose source and
 * destination share memory; Vulkan gives overlapping copy regions
 * undefined results, and the row-wise move keeps the outcome defined
 * bytes rather than undefined reads.
 */
static VkResult
execute_copy(struct r3v_native_device *device,
             const struct r3v_native_deferred_copy *op)
{
   struct r3v_native_memory *src_memory;
   struct r3v_native_memory *dst_memory;
   uint64_t src_base_offset, dst_base_offset;
   uint64_t src_pitch, dst_pitch;

   switch (op->kind) {
   case R3V_NATIVE_COPY_BUFFER_TO_BUFFER: {
      src_memory = op->src_buffer->memory;
      dst_memory = op->dst_buffer->memory;
      if (src_memory == NULL || dst_memory == NULL)
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: buffer copy source or destination "
                          "is unbound at submission");

      uint8_t *src_map, *dst_map;
      bool src_owned, dst_owned;
      VkResult result = map_memory(device, src_memory, &src_map, &src_owned);
      if (result != VK_SUCCESS)
         return result;
      result = map_memory(device, dst_memory, &dst_map, &dst_owned);
      if (result != VK_SUCCESS) {
         release_memory(device, src_memory, src_owned);
         return result;
      }
      memmove(dst_map + op->dst_buffer->offset + op->dst_offset,
              src_map + op->src_buffer->offset + op->src_offset, op->size);
      if (!dst_owned)
         radeon_drm_vk_bo_cache_sync(&device->drm, dst_map,
                                     dst_memory->bo.size);
      release_memory(device, dst_memory, dst_owned);
      release_memory(device, src_memory, src_owned);
      return VK_SUCCESS;
   }
   case R3V_NATIVE_COPY_BUFFER_TO_IMAGE:
      src_memory = op->buffer->memory;
      src_base_offset = op->buffer->offset + op->buffer_offset;
      src_pitch = op->buffer_row_length * 4;
      dst_memory = op->dst_image->memory;
      dst_base_offset = (uint64_t)op->dst_y * op->dst_image->row_pitch_bytes +
                        (uint64_t)op->dst_x * 4;
      dst_pitch = op->dst_image->row_pitch_bytes;
      break;
   case R3V_NATIVE_COPY_IMAGE_TO_BUFFER:
      src_memory = op->src_image->memory;
      src_base_offset = (uint64_t)op->src_y * op->src_image->row_pitch_bytes +
                        (uint64_t)op->src_x * 4;
      src_pitch = op->src_image->row_pitch_bytes;
      dst_memory = op->buffer->memory;
      dst_base_offset = op->buffer->offset + op->buffer_offset;
      dst_pitch = op->buffer_row_length * 4;
      break;
   case R3V_NATIVE_COPY_CLEAR_IMAGE: {
      /* One mapping, one fill: the packed texel lands across the full
       * extent, the row walk skipping the pitch padding the family
       * leaves untouched.
       */
      dst_memory = op->dst_image->memory;
      if (dst_memory == NULL)
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: clear destination is unbound at "
                          "submission");
      uint8_t *clear_map;
      bool clear_owned;
      VkResult clear_result =
         map_memory(device, dst_memory, &clear_map, &clear_owned);
      if (clear_result != VK_SUCCESS)
         return clear_result;
      for (uint32_t row = 0; row < op->height; row++) {
         uint32_t *texels =
            (uint32_t *)(clear_map +
                         (uint64_t)row * op->dst_image->row_pitch_bytes);
         for (uint32_t x = 0; x < op->width; x++)
            texels[x] = op->clear_dword;
      }
      if (!clear_owned)
         radeon_drm_vk_bo_cache_sync(&device->drm, clear_map,
                                     dst_memory->bo.size);
      release_memory(device, dst_memory, clear_owned);
      return VK_SUCCESS;
   }
   case R3V_NATIVE_COPY_IMAGE_TO_IMAGE:
      src_memory = op->src_image->memory;
      src_base_offset = (uint64_t)op->src_y * op->src_image->row_pitch_bytes +
                        (uint64_t)op->src_x * 4;
      src_pitch = op->src_image->row_pitch_bytes;
      dst_memory = op->dst_image->memory;
      dst_base_offset = (uint64_t)op->dst_y * op->dst_image->row_pitch_bytes +
                        (uint64_t)op->dst_x * 4;
      dst_pitch = op->dst_image->row_pitch_bytes;
      break;
   default:
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: unknown transfer kind %d", op->kind);
   }

   /* Unbound memory at execution is a resource the application dropped
    * out from under the recorded copy.
    */
   if (src_memory == NULL || dst_memory == NULL)
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: transfer source or destination is "
                       "unbound at submission");

   uint8_t *src_map, *dst_map;
   bool src_owned, dst_owned;
   VkResult result = map_memory(device, src_memory, &src_map, &src_owned);
   if (result != VK_SUCCESS)
      return result;
   result = map_memory(device, dst_memory, &dst_map, &dst_owned);
   if (result != VK_SUCCESS) {
      release_memory(device, src_memory, src_owned);
      return result;
   }

   const uint64_t row_bytes = (uint64_t)op->width * 4;
   for (uint32_t row = 0; row < op->height; row++) {
      memmove(dst_map + dst_base_offset + row * dst_pitch,
              src_map + src_base_offset + row * src_pitch,
              row_bytes);
   }

   /* The destination publishes even through a borrowed application
    * mapping: the copy's bytes must reach memory before the submission
    * retires, the same contract the deferred draw holds.
    */
   if (!dst_owned)
      radeon_drm_vk_bo_cache_sync(&device->drm, dst_map,
                                  dst_memory->bo.size);

   release_memory(device, dst_memory, dst_owned);
   release_memory(device, src_memory, src_owned);
   return VK_SUCCESS;
}

VkResult
r3v_native_cmd_buffer_execute_deferred_copies(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer)
{
   for (uint32_t i = 0; i < cmd_buffer->deferred_copy_count; i++) {
      VkResult result =
         execute_copy(device, &cmd_buffer->deferred_copies[i]);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}
