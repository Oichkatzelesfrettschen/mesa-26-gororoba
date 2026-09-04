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

static VkResult execute_host_copy(struct r3v_native_device *device,
                                 const struct r3v_native_deferred_copy *op);

/* A routed record is the device's, and the host counter stands above every
 * kind and every mapping below it, because the map -- not the store loop
 * under it -- is the observable host side effect a hardware claim excludes.
 * One count lands per record that produced its bytes here, so a GPU route's
 * claim is that this counter does not move for the record it carries: a
 * number a test reads rather than a property a reader infers from the
 * source.  A record that fails before writing moves nothing, since the
 * counter is read as evidence that the host produced a result.
 */
static VkResult
execute_copy(struct r3v_native_device *device,
             const struct r3v_native_deferred_copy *op)
{
   if (op->gpu_routed)
      return VK_SUCCESS;

   const VkResult result = execute_host_copy(device, op);
   if (result == VK_SUCCESS)
      device->host_semantic_writes++;
   return result;
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
execute_host_copy(struct r3v_native_device *device,
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

      uint8_t *src_map = NULL, *dst_map = NULL;
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
      src_pitch = (uint64_t)op->buffer_row_length * op->dst_image->texel_bytes;
      dst_memory = op->dst_image->memory;
      dst_base_offset = op->dst_image->memory_offset +
                        (uint64_t)op->dst_y * op->dst_image->row_pitch_bytes +
                        (uint64_t)op->dst_x * op->dst_image->texel_bytes;
      dst_pitch = op->dst_image->row_pitch_bytes;
      break;
   case R3V_NATIVE_COPY_IMAGE_TO_BUFFER:
      src_memory = op->src_image->memory;
      src_base_offset = op->src_image->memory_offset +
                        (uint64_t)op->src_y * op->src_image->row_pitch_bytes +
                        (uint64_t)op->src_x * op->src_image->texel_bytes;
      src_pitch = op->src_image->row_pitch_bytes;
      dst_memory = op->buffer->memory;
      dst_base_offset = op->buffer->offset + op->buffer_offset;
      dst_pitch = (uint64_t)op->buffer_row_length * op->src_image->texel_bytes;
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
      uint8_t *clear_map = NULL;
      bool clear_owned;
      VkResult clear_result =
         map_memory(device, dst_memory, &clear_map, &clear_owned);
      if (clear_result != VK_SUCCESS)
         return clear_result;
      const uint32_t texel_bytes = op->dst_image->texel_bytes;
      for (uint32_t row = 0; row < op->height; row++) {
         uint8_t *texels = clear_map + op->dst_image->memory_offset +
                           (uint64_t)row * op->dst_image->row_pitch_bytes;
         for (uint32_t x = 0; x < op->width; x++)
            memcpy(texels + (uint64_t)x * texel_bytes, op->clear_texel,
                   texel_bytes);
      }
      if (!clear_owned)
         radeon_drm_vk_bo_cache_sync(&device->drm, clear_map,
                                     dst_memory->bo.size);
      release_memory(device, dst_memory, clear_owned);
      return VK_SUCCESS;
   }
   case R3V_NATIVE_COPY_FILL_BUFFER:
   case R3V_NATIVE_COPY_UPDATE_BUFFER: {
      dst_memory = op->dst_buffer->memory;
      if (dst_memory == NULL)
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: fill or update destination is "
                          "unbound at submission");
      uint8_t *dst_map = NULL;
      bool dst_owned;
      VkResult result = map_memory(device, dst_memory, &dst_map, &dst_owned);
      if (result != VK_SUCCESS)
         return result;
      uint8_t *dst = dst_map + op->dst_buffer->offset + op->dst_offset;
      if (op->kind == R3V_NATIVE_COPY_UPDATE_BUFFER) {
         memcpy(dst, op->update_data, op->size);
      } else {
         /* The pattern is already little-endian; a byte-wise store
          * needs no alignment from the destination offset's dword
          * multiple, which the record admission proved anyway. */
         for (uint64_t i = 0; i < op->size; i += 4)
            memcpy(dst + i, &op->clear_dword, 4);
      }
      if (!dst_owned)
         radeon_drm_vk_bo_cache_sync(&device->drm, dst_map,
                                     dst_memory->bo.size);
      release_memory(device, dst_memory, dst_owned);
      return VK_SUCCESS;
   }
   case R3V_NATIVE_COPY_BLIT_IMAGE: {
      /* Nearest resample: destination texel (dx, dy) reads the source
       * texel at ((d + 0.5) * src_extent / dst_extent), the spec's
       * nearest sample point, computed as (2d + 1) * src / (2 * dst).
       */
      src_memory = op->src_image->memory;
      dst_memory = op->dst_image->memory;
      if (src_memory == NULL || dst_memory == NULL)
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: blit source or destination is "
                          "unbound at submission");
      uint8_t *blit_src_map = NULL, *blit_dst_map = NULL;
      bool blit_src_owned, blit_dst_owned;
      VkResult blit_result =
         map_memory(device, src_memory, &blit_src_map, &blit_src_owned);
      if (blit_result != VK_SUCCESS)
         return blit_result;
      blit_result =
         map_memory(device, dst_memory, &blit_dst_map, &blit_dst_owned);
      if (blit_result != VK_SUCCESS) {
         release_memory(device, src_memory, blit_src_owned);
         return blit_result;
      }
      radeon_drm_vk_bo_cache_sync(&device->drm, blit_src_map,
                                  src_memory->bo.size);
      const uint32_t texel_bytes = op->dst_image->texel_bytes;
      const uint8_t *src_base =
         blit_src_map + op->src_image->memory_offset;
      uint8_t *dst_base = blit_dst_map + op->dst_image->memory_offset;
      for (uint32_t dy = 0; dy < op->dst_height; dy++) {
         const uint32_t sy =
            op->src_y + (uint32_t)(((uint64_t)(2 * dy + 1) * op->height) /
                                   (2 * (uint64_t)op->dst_height));
         const uint8_t *src_row =
            src_base + (uint64_t)sy * op->src_image->row_pitch_bytes;
         uint8_t *dst_row =
            dst_base +
            (uint64_t)(op->dst_y + dy) * op->dst_image->row_pitch_bytes +
            (uint64_t)op->dst_x * texel_bytes;
         for (uint32_t dx = 0; dx < op->dst_width; dx++) {
            const uint32_t sx =
               op->src_x +
               (uint32_t)(((uint64_t)(2 * dx + 1) * op->width) /
                          (2 * (uint64_t)op->dst_width));
            memcpy(dst_row + (uint64_t)dx * texel_bytes,
                   src_row + (uint64_t)sx * texel_bytes, texel_bytes);
         }
      }
      if (!blit_dst_owned)
         radeon_drm_vk_bo_cache_sync(&device->drm, blit_dst_map,
                                     dst_memory->bo.size);
      release_memory(device, dst_memory, blit_dst_owned);
      release_memory(device, src_memory, blit_src_owned);
      return VK_SUCCESS;
   }
   case R3V_NATIVE_COPY_IMAGE_TO_IMAGE:
      src_memory = op->src_image->memory;
      src_base_offset = op->src_image->memory_offset +
                        (uint64_t)op->src_y * op->src_image->row_pitch_bytes +
                        (uint64_t)op->src_x * op->src_image->texel_bytes;
      src_pitch = op->src_image->row_pitch_bytes;
      dst_memory = op->dst_image->memory;
      dst_base_offset = op->dst_image->memory_offset +
                        (uint64_t)op->dst_y * op->dst_image->row_pitch_bytes +
                        (uint64_t)op->dst_x * op->dst_image->texel_bytes;
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

   uint8_t *src_map = NULL, *dst_map = NULL;
   bool src_owned, dst_owned;
   VkResult result = map_memory(device, src_memory, &src_map, &src_owned);
   if (result != VK_SUCCESS)
      return result;
   result = map_memory(device, dst_memory, &dst_map, &dst_owned);
   if (result != VK_SUCCESS) {
      release_memory(device, src_memory, src_owned);
      return result;
   }

   /* A source image the render pass drew into holds device output in an
    * unsnooped GTT mapping, so the host read invalidates the cache
    * lines covering it first.  The invalidate runs here, at execution,
    * and the queue executes the post-draw group after the completion
    * wait, so the lines dropped are the ones the completed submission
    * left stale; the queue's own post-completion sync reaches only the
    * mappings the application already holds, and this one covers the
    * mapping the copy takes.  CLFLUSH writes back and invalidates, so
    * the same primitive that publishes host writes serves the read
    * direction, and a mapping the application already holds keeps its
    * pending writes.
    */
   if (op->src_image != NULL)
      radeon_drm_vk_bo_cache_sync(&device->drm, src_map,
                                  src_memory->bo.size);

   const uint64_t row_bytes =
      (uint64_t)op->width * (op->dst_image != NULL ? op->dst_image->texel_bytes
                                                   : op->src_image->texel_bytes);
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
   struct r3v_native_cmd_buffer *cmd_buffer,
   enum r3v_native_copy_group group)
{
   for (uint32_t i = 0; i < cmd_buffer->deferred_copy_count; i++) {
      const struct r3v_native_deferred_copy *op =
         &cmd_buffer->deferred_copies[i];
      if (op->group != group)
         continue;
      VkResult result = execute_copy(device, op);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}
