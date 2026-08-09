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
#include "amd/r300/cpu/r300_cpu_vertex.h"

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

static VkResult
record_triangle_cell_tail(struct r3v_native_device *device,
                          struct r3v_native_cmd_buffer *cmd_buffer,
                          struct r3v_native_memory *vertex_memory,
                          struct r3v_native_memory *color_memory);

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

   return record_triangle_cell_tail(device, cmd_buffer, vertex_memory,
                                    color_memory);
}

/* Sentinel-fills the leading fill_bytes of the color memory and
 * publishes them for the unsnooped GART, so the output oracle reads a
 * deterministic pre-draw state and any device write inside the filled
 * range is detectable.  fill_bytes bounds the write to the caller's
 * declared footprint; content past it in the same allocation stays
 * untouched.
 */
static VkResult
sentinel_fill_color(struct r3v_native_device *device,
                    struct r3v_native_memory *color_memory,
                    uint64_t fill_bytes)
{
   bool owns_color_map = color_memory->map == NULL;
   if (owns_color_map &&
       radeon_drm_vk_bo_map(&device->drm, &color_memory->bo,
                            &color_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: triangle color memory is not "
                       "CPU-mappable");
   }
   uint32_t *color_pixels = color_memory->map;
   for (uint64_t i = 0; i < fill_bytes / 4; i++)
      color_pixels[i] = R300_TRIANGLE_COLOR_SENTINEL;
   radeon_drm_vk_bo_cache_sync(&device->drm, color_memory->map,
                               fill_bytes);
   if (owns_color_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &color_memory->bo,
                             color_memory->map);
      color_memory->map = NULL;
   }
   return VK_SUCCESS;
}

/* Cell emission and installation with no memory writes: the emitted IB
 * and its references depend only on the two BO handles, so the recorded
 * digest is independent of when the carrier and target bytes land.
 */
static VkResult
emit_and_install_triangle_cell(struct r3v_native_device *device,
                               struct r3v_native_cmd_buffer *cmd_buffer,
                               struct r3v_native_memory *vertex_memory,
                               struct r3v_native_memory *color_memory,
                               uint32_t width, uint32_t height)
{
   /* The recorded cell is self-contained: the emission opens with the
    * first-draw contract prefix resolved at the target extent, so the
    * result does not ride whatever state the previous client left in
    * the pipeline; at the maximum extent the construction is the
    * byte-identical reference cell backing the arming digest and the
    * manifest.
    */
   struct r300_tcl_bypass_triangle_ib cell;
   if (r300_tcl_bypass_triangle_extent_emit(width, height, &cell) != 0)
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

/* The delivery-independent remainder of triangle recording for the
 * record-time delivery routes: sentinel publication over the whole
 * allocation -- their oracle contract covers every byte -- then cell
 * emission and installation.
 */
static VkResult
record_triangle_cell_tail(struct r3v_native_device *device,
                          struct r3v_native_cmd_buffer *cmd_buffer,
                          struct r3v_native_memory *vertex_memory,
                          struct r3v_native_memory *color_memory)
{
   VkResult result =
      sentinel_fill_color(device, color_memory, color_memory->bo.size);
   if (result != VK_SUCCESS)
      return result;
   return emit_and_install_triangle_cell(device, cmd_buffer, vertex_memory,
                                         color_memory,
                                         R3V_NATIVE_TARGET_WIDTH,
                                         R3V_NATIVE_TARGET_HEIGHT);
}

VkResult
r3v_native_record_tcl_bypass_triangle_carrier(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory,
   struct r3v_native_image *target_image)
{
   struct r3v_native_memory *color_memory = target_image->memory;
   if (carrier_memory->bo.size < R3V_TRIANGLE_VERTEX_BYTES ||
       color_memory->bo.size <
          r3v_native_image_footprint_bytes(target_image->height)) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: triangle cell needs %zu vertex bytes "
                       "and the target image's declared footprint",
                       (size_t)R3V_TRIANGLE_VERTEX_BYTES);
   }
   return emit_and_install_triangle_cell(device, cmd_buffer, carrier_memory,
                                         color_memory, target_image->width,
                                         target_image->height);
}

/* Submission-time execution of the public draw: the bound stream reads
 * and the load-op clear happen here, so a vertex write between record
 * and submit is honored and an unsubmitted command buffer leaves
 * application memory untouched.
 */
VkResult
r3v_native_cmd_buffer_execute_deferred_draw(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer)
{
   struct r3v_native_deferred_draw *draw = &cmd_buffer->deferred_draw;
   if (!draw->pending)
      return VK_SUCCESS;

   struct r3v_native_buffer *buffer = draw->buffer;
   struct r3v_native_memory *memory = buffer->memory;
   struct r3v_native_memory *carrier = cmd_buffer->owned_carrier;

   bool owns_map = memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &memory->bo, &memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: bound vertex memory is not "
                       "CPU-mappable at submission");
   }
   const struct r3v_native_vertex_stream_desc stream = {
      .records =
         (const uint8_t *)memory->map + buffer->offset + draw->stream_base,
      .size_bytes = buffer->vk.size - draw->stream_base,
      .stride = draw->stride,
      .first_vertex = draw->first_vertex,
      .format_id = draw->format_id,
   };

   bool owns_carrier_map = carrier->map == NULL;
   VkResult result = VK_SUCCESS;
   if (owns_carrier_map &&
       radeon_drm_vk_bo_map(&device->drm, &carrier->bo,
                            &carrier->map) != 0) {
      result = vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                         "r3v-native: carrier memory is not CPU-mappable "
                         "at submission");
   } else {
      const struct r300_cpu_vertex_stream source = {
         .data = stream.records,
         .stride = stream.stride,
         .size_bytes = stream.size_bytes,
      };
      int gathered = r300_cpu_vertex_gather(
         stream.format_id, &source, stream.first_vertex,
         R300_TRIANGLE_VERTEX_DWORDS / 4, carrier->map,
         R300_TRIANGLE_VERTEX_DWORDS);
      if (gathered != 0) {
         result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                            "r3v-native: vertex gather refused (%d)",
                            gathered);
      } else {
         /* The admitted vertex program passes its input to
          * gl_Position, so the CPU vertex node realizes the Vulkan
          * viewport transform here: x and y map from NDC to window
          * coordinates over the pass target's extent, z passes
          * through the identity depth range, and w carries the exact
          * value 1 -- the perspective divide is the identity there.
          * The admitted domain is the clip volume, so scissor and
          * clip coincide and the raster needs no clipper; a record
          * outside it refuses, and the submit reports device loss.
          */
         float positions[R300_TRIANGLE_VERTEX_DWORDS];
         memcpy(positions, carrier->map, sizeof(positions));
         for (unsigned v = 0;
              result == VK_SUCCESS && v < R300_TRIANGLE_VERTEX_DWORDS / 4;
              v++) {
            float *pos = &positions[v * 4];
            if (pos[3] != 1.0f || pos[0] < -1.0f || pos[0] > 1.0f ||
                pos[1] < -1.0f || pos[1] > 1.0f || pos[2] < 0.0f ||
                pos[2] > 1.0f) {
               result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                                  "r3v-native: vertex %u outside the "
                                  "admitted clip volume or w != 1", v);
               break;
            }
            pos[0] = (pos[0] + 1.0f) * ((float)draw->target_width / 2.0f);
            pos[1] = (pos[1] + 1.0f) * ((float)draw->target_height / 2.0f);
         }
         if (result == VK_SUCCESS) {
            memcpy(carrier->map, positions, sizeof(positions));
            radeon_drm_vk_bo_cache_sync(&device->drm, carrier->map,
                                        R3V_TRIANGLE_VERTEX_BYTES);
         }
      }
      if (owns_carrier_map) {
         radeon_drm_vk_bo_unmap(&device->drm, &carrier->bo, carrier->map);
         carrier->map = NULL;
      }
   }

   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &memory->bo, memory->map);
      memory->map = NULL;
   }
   if (result != VK_SUCCESS)
      return result;

   /* The load-op clear realizes as the sentinel fill over the image's
    * declared memory footprint alone; a larger allocation keeps its
    * remaining bytes, so a resource bound past the image survives the
    * draw.
    */
   /* pending stays set: every submission re-reads the stream and
    * re-clears, the execution-time semantics each submit carries.
    */
   return sentinel_fill_color(device, draw->target_memory,
                              draw->target_fill_bytes);
}

/* Carrier delivery: gathers the cell's three vertices from the caller's
 * stream through the CPU vertex executor, byte-defined end to end, into
 * the mapped GTT carrier, then records the same fixed cell through the
 * shared tail.  A refused gather -- unknown format, unproven bound,
 * undersized carrier -- reports before any BO write.
 */
VkResult
r3v_native_record_tcl_bypass_triangle_from_stream(
   VkCommandBuffer commandBuffer, VkDeviceMemory vertexMemory,
   VkDeviceMemory colorMemory,
   const struct r3v_native_vertex_stream_desc *stream)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, vertex_memory, vertexMemory);
   VK_FROM_HANDLE(r3v_native_memory, color_memory, colorMemory);

   if (cmd_buffer == NULL || vertex_memory == NULL || color_memory == NULL ||
       stream == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);

   return r3v_native_record_tcl_bypass_triangle_gathered(
      device, cmd_buffer, vertex_memory, color_memory, stream);
}

VkResult
r3v_native_record_tcl_bypass_triangle_gathered(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *vertex_memory,
   struct r3v_native_memory *color_memory,
   const struct r3v_native_vertex_stream_desc *stream)
{
   if (vertex_memory->bo.size < R3V_TRIANGLE_VERTEX_BYTES ||
       color_memory->bo.size < R3V_TRIANGLE_COLOR_BYTES) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: triangle cell needs %zu vertex bytes "
                       "and %u color bytes",
                       (size_t)R3V_TRIANGLE_VERTEX_BYTES,
                       R3V_TRIANGLE_COLOR_BYTES);
   }

   bool owns_map = vertex_memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &vertex_memory->bo,
                            &vertex_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: triangle vertex memory is not "
                       "CPU-mappable");
   }
   const struct r300_cpu_vertex_stream source = {
      .data = stream->records,
      .stride = stream->stride,
      .size_bytes = stream->size_bytes,
   };
   int gathered = r300_cpu_vertex_gather(
      stream->format_id, &source, stream->first_vertex,
      R300_TRIANGLE_VERTEX_DWORDS / 4, vertex_memory->map,
      R300_TRIANGLE_VERTEX_DWORDS);
   if (gathered != 0) {
      if (owns_map) {
         radeon_drm_vk_bo_unmap(&device->drm, &vertex_memory->bo,
                                vertex_memory->map);
         vertex_memory->map = NULL;
      }
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: vertex gather refused (%d)", gathered);
   }
   radeon_drm_vk_bo_cache_sync(&device->drm, vertex_memory->map,
                               R3V_TRIANGLE_VERTEX_BYTES);
   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &vertex_memory->bo,
                             vertex_memory->map);
      vertex_memory->map = NULL;
   }

   return record_triangle_cell_tail(device, cmd_buffer, vertex_memory,
                                    color_memory);
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
