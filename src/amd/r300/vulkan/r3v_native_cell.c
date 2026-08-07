/*
 * SPDX-License-Identifier: MIT
 *
 * Fixed-cell recorder: lowers the TCL-bypass triangle into a native
 * command buffer from live GEM buffer objects.
 */

#include "r3v_native.h"

#include "amd/r300/common/r300_first_draw_state.h"
#include "amd/r300/common/r300_fragment_binary.h"
#include "amd/r300/common/r300_direct_write.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include "util/macros.h"
#include "vk_command_pool.h"
#include "vk_log.h"

#include <inttypes.h>
#include <radeon_drm.h>
#include <stdlib.h>
#include <string.h>

/* The cell renders a 64x64 ARGB8888 target at a 64-pixel pitch; the color
 * allocation carries one extra row past the render extent as the output
 * oracle's canary, and the whole allocation is sentinel-filled before
 * submission so any device write is detectable.
 */
#define R3V_TRIANGLE_COLOR_BYTES (64 * 65 * 4)
#define R3V_TRIANGLE_VERTEX_BYTES \
   (R300_TRIANGLE_VERTEX_DWORDS * sizeof(float))

/* Records the fixed TCL-bypass triangle cell into a native command buffer:
 * writes the pretransformed vertices through the vertex memory's mapping,
 * builds the reference fragment binary, emits the cell IB, and installs it
 * with the two BO references in relocation-slot order.  The pre-hardware
 * harness and the attended-cell runner link this entry directly; the
 * recording is submit-free, and the queue's hazard gate guards execution.
 */
VkResult
r3v_native_record_tcl_bypass_triangle(VkCommandBuffer commandBuffer,
                                      VkDeviceMemory vertexMemory,
                                      VkDeviceMemory colorMemory)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, vertex_memory, vertexMemory);
   VK_FROM_HANDLE(r3v_native_memory, color_memory, colorMemory);

   if (cmd_buffer == NULL || vertex_memory == NULL || color_memory == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);

   if (vertex_memory->bo.size < R3V_TRIANGLE_VERTEX_BYTES ||
       color_memory->bo.size < R3V_TRIANGLE_COLOR_BYTES) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: triangle cell needs %zu vertex bytes "
                       "and %u color bytes",
                       (size_t)R3V_TRIANGLE_VERTEX_BYTES,
                       R3V_TRIANGLE_COLOR_BYTES);
   }

   /* The vertex payload lands through the memory's CPU mapping; a
    * NO_CPU_ACCESS placement fails here and the recorder reports it
    * instead of submitting an unwritten stream.
    */
   bool owns_map = vertex_memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &vertex_memory->bo,
                            &vertex_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: triangle vertex memory is not "
                       "CPU-mappable");
   }
   memcpy(vertex_memory->map, r300_tcl_bypass_triangle_vertices,
          R3V_TRIANGLE_VERTEX_BYTES);
   /* The GART aperture reads unsnooped, so the vertex bytes publish by
    * cache writeback while the mapping's address is still live; munmap
    * leaves dirty lines in place and is not a publication mechanism.
    */
   radeon_drm_vk_bo_cache_sync(&device->drm, vertex_memory->map,
                               R3V_TRIANGLE_VERTEX_BYTES);
   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &vertex_memory->bo,
                             vertex_memory->map);
      vertex_memory->map = NULL;
   }

   /* Sentinel-fill the whole color allocation and publish it, so the
    * output oracle reads a deterministic pre-draw state and any device
    * write -- inside or past the render extent -- is detectable.
    */
   bool owns_color_map = color_memory->map == NULL;
   if (owns_color_map &&
       radeon_drm_vk_bo_map(&device->drm, &color_memory->bo,
                            &color_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: triangle color memory is not "
                       "CPU-mappable");
   }
   uint32_t *color_pixels = color_memory->map;
   for (uint64_t i = 0; i < color_memory->bo.size / 4; i++)
      color_pixels[i] = R300_TRIANGLE_COLOR_SENTINEL;
   radeon_drm_vk_bo_cache_sync(&device->drm, color_memory->map,
                               color_memory->bo.size);
   if (owns_color_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &color_memory->bo,
                             color_memory->map);
      color_memory->map = NULL;
   }

   /* The recorded cell is self-contained: the reference emission opens
    * with the first-draw contract prefix, so the result does not ride
    * whatever state the previous client left in the pipeline, and the
    * same construction backs the arming digest and the manifest.
    */
   struct r300_tcl_bypass_triangle_ib cell;
   if (r300_tcl_bypass_triangle_reference_emit(&cell) != 0)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   /* Reference order is relocation-slot order: the queue folds the array
    * in index order and the dedupe keeps first-add order, so the IB's
    * slot payloads name the final relocation indices.
    */
   struct r3v_native_bo_reference *references =
      calloc(R300_TRIANGLE_SLOT_COUNT, sizeof(*references));
   if (references == NULL) {
      r300_tcl_bypass_triangle_release(&cell);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   references[R300_TRIANGLE_SLOT_VERTEX] = (struct r3v_native_bo_reference){
      .handle = vertex_memory->bo.handle,
      .read_domains = RADEON_GEM_DOMAIN_GTT,
      .write_domain = 0,
      .memory = vertex_memory,
   };
   references[R300_TRIANGLE_SLOT_COLOR] = (struct r3v_native_bo_reference){
      .handle = color_memory->bo.handle,
      .read_domains = 0,
      .write_domain = RADEON_GEM_DOMAIN_GTT,
      .memory = color_memory,
   };

   r3v_native_cmd_buffer_install_ib(cmd_buffer, cell.ib, cell.ib_size_dwords,
                                    references, R300_TRIANGLE_SLOT_COUNT);
   /* install_ib took ownership of cell.ib; only the descriptor resets. */
   cell.ib = NULL;
   r300_tcl_bypass_triangle_release(&cell);

   return VK_SUCCESS;
}

/* Records the direct-write control cell: sentinel-fills and publishes the
 * color allocation, emits the fixed 2D solid-fill stream, and installs it
 * with the color target as the one BO reference.  The stream fetches no
 * source, so the reference list carries a single write-domain entry.
 */
VkResult
r3v_native_record_direct_write(VkCommandBuffer commandBuffer,
                               VkDeviceMemory colorMemory)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, color_memory, colorMemory);

   if (cmd_buffer == NULL || color_memory == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);

   if (color_memory->bo.size < R3V_TRIANGLE_COLOR_BYTES) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: direct-write cell needs %u color bytes",
                       R3V_TRIANGLE_COLOR_BYTES);
   }

   /* Sentinel-fill the whole color allocation and publish it, so the
    * output oracle reads a deterministic pre-write state and any device
    * write -- inside or past the oracle-covered range -- is detectable.
    */
   /* The recorder rejects a sub-dword tail before mapping: a 32-bit
    * oracle read observes no partial dword, so a trailing remainder is
    * unpublishable content behind the sentinel claim.
    */
   if (color_memory->bo.size % 4 != 0) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: direct-write color size %" PRIu64
                       " is not whole dwords",
                       (uint64_t)color_memory->bo.size);
   }
   bool owns_map = color_memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &color_memory->bo,
                            &color_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: direct-write color memory is not "
                       "CPU-mappable");
   }
   if (color_memory->map == NULL) {
      if (owns_map)
         color_memory->map = NULL;
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: direct-write color mapping is absent "
                       "after a successful map");
   }
   uint32_t *color_pixels = color_memory->map;
   for (uint64_t i = 0; i < color_memory->bo.size / 4; i++)
      color_pixels[i] = R300_TRIANGLE_COLOR_SENTINEL;
   radeon_drm_vk_bo_cache_sync(&device->drm, color_memory->map,
                               color_memory->bo.size);
   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &color_memory->bo,
                             color_memory->map);
      color_memory->map = NULL;
   }

   struct r300_direct_write_ib cell;
   if (r300_direct_write_emit(&cell) != 0)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   if (r300_direct_write_validate_reloc_sites(&cell) != 0) {
      r300_direct_write_release(&cell);
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   struct r3v_native_bo_reference *references =
      calloc(R300_DIRECT_WRITE_SLOT_COUNT, sizeof(*references));
   if (references == NULL) {
      r300_direct_write_release(&cell);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   references[R300_DIRECT_WRITE_SLOT_COLOR] =
      (struct r3v_native_bo_reference){
         .handle = color_memory->bo.handle,
         .read_domains = 0,
         .write_domain = RADEON_GEM_DOMAIN_GTT,
         .memory = color_memory,
      };

   r3v_native_cmd_buffer_install_ib(cmd_buffer, cell.ib, cell.ib_size_dwords,
                                    references,
                                    R300_DIRECT_WRITE_SLOT_COUNT);
   /* install_ib took ownership of cell.ib; only the descriptor resets. */
   cell.ib = NULL;
   r300_direct_write_release(&cell);

   return VK_SUCCESS;
}
