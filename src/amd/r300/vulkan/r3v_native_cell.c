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
#include "amd/r300/common/r300_delivery_route.h"
#include "amd/r300/common/r300_r2vb_carrier_delivery.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_vertex_format.h"
#include "amd/r300/cpu/r300_cpu_vertex.h"

#include "util/macros.h"
#include "vk_command_pool.h"
#include "vk_log.h"

#include <errno.h>
#include <inttypes.h>
#include <radeon_drm.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The cell renders a 64x64 ARGB8888 target at a 64-pixel pitch; the color
 * allocation carries one extra row past the render extent as the output
 * oracle's canary, and the whole allocation is sentinel-filled before
 * submission so any device write is detectable.
 */
#define R3V_TRIANGLE_VERTEX_BYTES \
   (R300_TRIANGLE_VERTEX_DWORDS * sizeof(float))

static VkResult
record_triangle_cell_tail(struct r3v_native_device *device,
                          struct r3v_native_cmd_buffer *cmd_buffer,
                          struct r3v_native_memory *vertex_memory,
                          struct r3v_native_memory *color_memory);

/* The gather writes the first three logical vertices as a packed 48-byte
 * carrier.  A caller may point records into the same mapped BO, so compute
 * the exact physical source range before the first destination write.  The
 * range arithmetic fails closed on overflow; the CPU gather remains the
 * authority for semantic bounds and format validation.
 */
static bool
stream_source_overlaps_carrier(
   const struct r3v_native_vertex_stream_desc *stream,
   const void *carrier, uint64_t carrier_bytes)
{
   const struct r300_vertex_format_semantics *format =
      r300_vertex_format_semantics((enum r300_vertex_format_id)
                                      stream->format_id);
   if (format == NULL || stream->records == NULL)
      return false;

   const uint64_t vertex_count = R300_TRIANGLE_VERTEX_DWORDS / 4;
   /* Zero stride is the Vulkan constant-binding form: all requested
    * vertices read the one record at the stream base, so the source range
    * below remains one record wide and still rejects an aliased carrier.
    */
   if (stream->stride != 0 &&
       stream->stride < format->semantic_record_bytes)
      return false;

   const uint64_t first_offset =
      (uint64_t)stream->first_vertex * stream->stride;
   const uint64_t tail_offset = (vertex_count - 1) * stream->stride;
   if (first_offset > UINT64_MAX - tail_offset)
      return true;

   const uint64_t source_offset = first_offset;
   if (tail_offset > UINT64_MAX - format->semantic_record_bytes)
      return true;
   const uint64_t source_bytes =
      tail_offset + format->semantic_record_bytes;

   const uintptr_t source_base = (uintptr_t)stream->records;
   const uintptr_t carrier_base = (uintptr_t)carrier;
   if (source_offset > UINTPTR_MAX - source_base ||
       source_bytes > UINTPTR_MAX - (source_base + source_offset) ||
       carrier_bytes > UINTPTR_MAX - carrier_base)
      return true;

   const uintptr_t source_start = source_base + source_offset;
   const uintptr_t source_end = source_start + source_bytes;
   const uintptr_t carrier_end = carrier_base + carrier_bytes;
   return source_start < carrier_end && carrier_base < source_end;
}

VkResult
r3v_native_cell_vk_result_from_errno(int emit_result)
{
   return emit_result == -ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY
                                 : VK_ERROR_INITIALIZATION_FAILED;
}

/* The triangle cell binds both roles at byte offset zero.  A shared GEM BO
 * makes the vertex fetch overlap the color target, and
 * radeon_drm_vk_reloc_list_add (rg --fixed-strings
 * radeon_drm_vk_reloc_list_add src/) folds duplicate handles into one
 * relocation slot.  The recorder rejects aliased roles before either mapping
 * or sentinel publication begins.
 */
static VkResult
validate_triangle_memory_roles(struct r3v_native_device *device,
                               const struct r3v_native_memory *vertex_memory,
                               const struct r3v_native_memory *color_memory)
{
   if (vertex_memory->bo.handle == color_memory->bo.handle)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: triangle vertex and color roles "
                       "require distinct GEM objects");
   return VK_SUCCESS;
}

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
       color_memory->bo.size < R300_TRIANGLE_COLOR_BYTES) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: triangle cell needs %zu vertex bytes "
                       "and %u color bytes",
                       (size_t)R3V_TRIANGLE_VERTEX_BYTES,
                       R300_TRIANGLE_COLOR_BYTES);
   }

   VkResult role_result = validate_triangle_memory_roles(
      device, vertex_memory, color_memory);
   if (role_result != VK_SUCCESS)
      return role_result;

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
   VkResult role_result = validate_triangle_memory_roles(
      device, vertex_memory, color_memory);
   if (role_result != VK_SUCCESS)
      return role_result;

   /* The recorded cell is self-contained: the emission opens with the
    * first-draw contract prefix resolved at the target extent, so the
    * result does not ride whatever state the previous client left in
    * the pipeline; at the maximum extent the construction is the
    * byte-identical reference cell backing the arming digest and the
    * manifest.
    */
   struct r300_tcl_bypass_triangle_ib cell;
   int emit_result =
      r300_tcl_bypass_triangle_extent_emit(width, height, &cell);
   if (emit_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(emit_result));

   /* Reference order is relocation-slot order: the queue records the index
    * returned by radeon_drm_vk_reloc_list_add for each reference, and the
    * distinct-role check keeps this cell's slot payloads equal to the final
    * relocation indices.  The fixed cell rejects shared handles before this
    * list reaches the queue, so deduplication cannot leave a slot payload
    * outside the relocation chunk.
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
   VkResult role_result = validate_triangle_memory_roles(
      device, vertex_memory, color_memory);
   if (role_result != VK_SUCCESS)
      return role_result;

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
   VkResult role_result = validate_triangle_memory_roles(
      device, carrier_memory, color_memory);
   if (role_result != VK_SUCCESS)
      return role_result;

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

/* Submission-time execution of the public render pass: an empty pass applies
 * its load-op clear, while a draw also reads the bound stream.  Both paths
 * execute here, so a vertex write between record and submit is honored and
 * an unsubmitted command buffer leaves application memory untouched.
 */
VkResult
r3v_native_cmd_buffer_execute_deferred_draw(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer)
{
   struct r3v_native_deferred_draw *draw = &cmd_buffer->deferred_draw;
   if (!draw->pending)
      return VK_SUCCESS;

   /* CmdBeginRenderPass records the load-op clear before a draw exists.  An
    * empty subpass has no vertex stream or carrier to execute, but its clear
    * still realizes at queue submission.
    */
   if (draw->buffer == NULL)
      return sentinel_fill_color(device, draw->target_memory,
                                 draw->target_fill_bytes);

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
      /* Delivery route selection lives in r300_delivery_route_resolve:
       * the CPU gather is the default and the semantic oracle, and the
       * R2VB identity delivery engages only on the exact opt-in value
       * and the formats it models.  Under the R2VB route the delivery
       * holds the FP24 fixed-point domain and refuses outside it, and
       * the CPU gather then re-derives the same carrier -- a byte
       * divergence falsifies the identity control and refuses the draw
       * rather than submitting bytes the two routes disagree on.
       */
      struct r300_delivery_route_decision route_decision;
      r300_delivery_route_resolve(
         getenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL"),
         stream.format_id, &route_decision);
      const bool r2vb_route =
         route_decision.route == R300_DELIVERY_ROUTE_R2VB_HOST_MODEL;
      int gathered;
      if (r2vb_route) {
         gathered = r300_r2vb_identity_deliver(
            stream.format_id, &source, stream.first_vertex,
            R300_TRIANGLE_VERTEX_DWORDS / 4, carrier->map,
            R300_TRIANGLE_VERTEX_DWORDS);
         if (gathered == 0) {
            uint32_t oracle[R300_TRIANGLE_VERTEX_DWORDS];
            gathered = r300_cpu_vertex_gather(
               stream.format_id, &source, stream.first_vertex,
               R300_TRIANGLE_VERTEX_DWORDS / 4, oracle,
               R300_TRIANGLE_VERTEX_DWORDS);
            if (gathered == 0 &&
                memcmp(oracle, carrier->map,
                       R300_TRIANGLE_VERTEX_DWORDS * 4) != 0) {
               result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                                  "r3v-native: R2VB delivery diverged "
                                  "from the CPU gather oracle");
            }
         }
      } else {
         gathered = r300_cpu_vertex_gather(
            stream.format_id, &source, stream.first_vertex,
            R300_TRIANGLE_VERTEX_DWORDS / 4, carrier->map,
            R300_TRIANGLE_VERTEX_DWORDS);
      }
      if (result == VK_SUCCESS && gathered != 0) {
         result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                            "r3v-native: vertex %s refused (%d): %s",
                            r2vb_route ? "delivery" : "gather", gathered,
                            route_decision.reason);
      } else if (result == VK_SUCCESS) {
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
            /* Negated-conjunction bounds: an unordered comparison
             * fails its conjunct, so a NaN component refuses instead
             * of passing every ordered test.
             */
            if (!(pos[3] == 1.0f) ||
                !(pos[0] >= -1.0f && pos[0] <= 1.0f) ||
                !(pos[1] >= -1.0f && pos[1] <= 1.0f) ||
                !(pos[2] >= 0.0f && pos[2] <= 1.0f)) {
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
 * shared tail.  The shared tail is established by
 * (rg --fixed-strings "record_triangle_cell_tail"
 * src/amd/r300/vulkan/r3v_native_cell.c).  A refused gather -- unknown
 * format, unproven bound, undersized carrier, or overlapping source and
 * carrier ranges -- reports before any BO write.
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
       color_memory->bo.size < R300_TRIANGLE_COLOR_BYTES) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: triangle cell needs %zu vertex bytes "
                       "and %u color bytes",
                       (size_t)R3V_TRIANGLE_VERTEX_BYTES,
                       R300_TRIANGLE_COLOR_BYTES);
   }

   if (stream == NULL || stream->records == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   VkResult role_result = validate_triangle_memory_roles(
      device, vertex_memory, color_memory);
   if (role_result != VK_SUCCESS)
      return role_result;

   bool owns_map = vertex_memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &vertex_memory->bo,
                            &vertex_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: triangle vertex memory is not "
                       "CPU-mappable");
   }
   if (stream_source_overlaps_carrier(stream, vertex_memory->map,
                                      R3V_TRIANGLE_VERTEX_BYTES)) {
      if (owns_map) {
         radeon_drm_vk_bo_unmap(&device->drm, &vertex_memory->bo,
                                vertex_memory->map);
         vertex_memory->map = NULL;
      }
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: vertex source overlaps carrier");
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

   if (color_memory->bo.size < R300_TRIANGLE_COLOR_BYTES) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: direct-write cell needs %u color bytes",
                       R300_TRIANGLE_COLOR_BYTES);
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
   int emit_result = r300_direct_write_emit(&cell);
   if (emit_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(emit_result));
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
