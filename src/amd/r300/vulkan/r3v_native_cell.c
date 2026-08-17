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
#include "amd/r300/common/r300_r2vb_float2_tuple_pass.h"
#include "amd/r300/common/r300_r2vb_producer_pass.h"
#include "amd/r300/common/r300_r2vb_reingest_pass.h"
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

   r3v_native_cmd_buffer_install_ib(cmd_buffer,
                                    R3V_NATIVE_CELL_KIND_TRIANGLE, cell.ib,
                                    cell.ib_size_dwords, references,
                                    R300_TRIANGLE_SLOT_COUNT);
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
         getenv("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL"),
         stream.format_id, &route_decision);
      /* The GPU producer route names a device-side delivery this
       * deferred draw cannot execute: live producer submission routes
       * only through the operator-armed attended surface.  The exact
       * double opt-in therefore refuses the draw by name instead of
       * downgrading to a host copy the caller did not select.
       */
      if (route_decision.route == R300_DELIVERY_ROUTE_R2VB_GPU_PRODUCER) {
         result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                            "r3v-native: %s; live producer submission "
                            "routes through the attended cell surface",
                            route_decision.reason);
      }
      const bool r2vb_route =
         route_decision.route == R300_DELIVERY_ROUTE_R2VB_HOST_MODEL;
      int gathered = 0;
      if (result != VK_SUCCESS) {
         /* The refused route delivers nothing; the shared unmap and
          * error paths below still run.
          */
      } else if (r2vb_route) {
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
         const char *operation = r2vb_route ? "R2VB delivery" : "CPU gather";
         const char *errno_text = gathered < 0 ? strerror(-gathered)
                                               : "non-errno refusal";
         result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                            "r3v-native: %s refused (%d): %s; route "
                            "context: %s",
                            operation, gathered, errno_text,
                            route_decision.reason);
      } else if (result == VK_SUCCESS &&
                 route_decision.position_space ==
                    R300_CARRIER_POSITION_CLIP) {
         /* The route declares a clip-space carrier, so the one
          * viewport transform happens here; a WINDOW declaration
          * means the producer already transformed on the device and
          * the carrier binds untransformed.  The admitted vertex
          * program passes its input to
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
         if (result == VK_SUCCESS)
            memcpy(carrier->map, positions, sizeof(positions));
      }
      /* Publication is delivery-unconditional: a WINDOW-space carrier
       * skips the transform branch above yet its bytes still cross to
       * the device through this one sync.
       */
      if (result == VK_SUCCESS)
         radeon_drm_vk_bo_cache_sync(&device->drm, carrier->map,
                                     R3V_TRIANGLE_VERTEX_BYTES);
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

   r3v_native_cmd_buffer_install_ib(cmd_buffer,
                                    R3V_NATIVE_CELL_KIND_DIRECT_WRITE,
                                    cell.ib, cell.ib_size_dwords, references,
                                    R300_DIRECT_WRITE_SLOT_COUNT);
   /* install_ib took ownership of cell.ib; only the descriptor resets. */
   cell.ib = NULL;
   r300_direct_write_release(&cell);

   return VK_SUCCESS;
}

int
r3v_native_producer_carrier_bytes(uint32_t *out)
{
   struct r300_r2vb_producer_layout layout;
   int rc = r300_r2vb_producer_layout_single_row(
      R300_R2VB_PRODUCER_REFERENCE_COUNT, &layout);
   if (rc != 0)
      return rc;
   *out = layout.pitch_pixels * layout.height * R300_R2VB_PRODUCER_CPP_BYTES;
   return 0;
}

/* Installs one fixed producer stream against the carrier BO.  The
 * reference and sweep emissions share the layout and slot contract, so
 * the install differs only in the emitter that supplies the IB.
 */
static int
producer_cell_install_stream(struct r3v_native_cmd_buffer *cmd_buffer,
                             struct r3v_native_memory *carrier_memory,
                             int (*emit)(struct r300_r2vb_producer_ib *))
{
   struct r300_r2vb_producer_ib cell;
   int emit_result = emit(&cell);
   if (emit_result != 0)
      return emit_result;
   emit_result = r300_r2vb_producer_pass_validate_reloc_sites(&cell);
   if (emit_result != 0) {
      r300_r2vb_producer_pass_release(&cell);
      return emit_result;
   }

   struct r3v_native_bo_reference *references =
      calloc(R300_R2VB_PRODUCER_SLOT_COUNT, sizeof(*references));
   if (references == NULL) {
      r300_r2vb_producer_pass_release(&cell);
      return -ENOMEM;
   }
   /* The carrier crosses both directions of the pass: the color backend
    * writes the slot row and a later vertex fetch reads it, so the one
    * relocation carries the GTT domain on both sides.
    */
   references[R300_R2VB_PRODUCER_SLOT_CARRIER] =
      (struct r3v_native_bo_reference){
         .handle = carrier_memory->bo.handle,
         .read_domains = RADEON_GEM_DOMAIN_GTT,
         .write_domain = RADEON_GEM_DOMAIN_GTT,
         .memory = carrier_memory,
      };

   r3v_native_cmd_buffer_install_ib(cmd_buffer,
                                    R3V_NATIVE_CELL_KIND_R2VB_PRODUCER,
                                    cell.ib, cell.ib_size_dwords, references,
                                    R300_R2VB_PRODUCER_SLOT_COUNT);
   /* install_ib took ownership of cell.ib; only the descriptor resets. */
   cell.ib = NULL;
   r300_r2vb_producer_pass_release(&cell);
   return 0;
}

int
r3v_native_producer_cell_install(struct r3v_native_cmd_buffer *cmd_buffer,
                                 struct r3v_native_memory *carrier_memory)
{
   return producer_cell_install_stream(cmd_buffer, carrier_memory,
                                       r300_r2vb_producer_reference_emit);
}

/* The arming gate freezes one carrier footprint per producer-kind cell,
 * so the sweep rides the same geometry contract only while its count
 * equals the reference count.
 */
_Static_assert(R300_R2VB_PRODUCER_FP24_SWEEP_COUNT ==
                  R300_R2VB_PRODUCER_REFERENCE_COUNT,
               "the sweep stream must keep the producer cell's frozen "
               "carrier geometry");

int
r3v_native_producer_fp24_sweep_cell_install(
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory)
{
   return producer_cell_install_stream(cmd_buffer, carrier_memory,
                                       r300_r2vb_producer_fp24_sweep_emit);
}

_Static_assert(R300_R2VB_PRODUCER_FP24_BISECT_COUNT ==
                  R300_R2VB_PRODUCER_REFERENCE_COUNT,
               "the bisection stream must keep the producer cell's frozen "
               "carrier geometry");

int
r3v_native_producer_fp24_bisect_cell_install(
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory)
{
   return producer_cell_install_stream(cmd_buffer, carrier_memory,
                                       r300_r2vb_producer_fp24_bisect_emit);
}

/* Records the producer-only cell: fills the carrier allocation with the
 * manifest poison, publishes it for the unsnooped GART, and installs the
 * reference producer pass against that one BO.  The poison is what makes
 * the read-back decidable -- a slot still holding it is a slot the pass
 * left unwritten -- so the prefill covers the whole allocation while the
 * expected extent covers the written slots alone.
 */
static VkResult
record_r2vb_producer_stream(VkCommandBuffer commandBuffer,
                            VkDeviceMemory carrierMemory,
                            int (*install)(struct r3v_native_cmd_buffer *,
                                           struct r3v_native_memory *))
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, carrier_memory, carrierMemory);

   if (cmd_buffer == NULL || carrier_memory == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);

   uint32_t carrier_bytes;
   if (r3v_native_producer_carrier_bytes(&carrier_bytes) != 0)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   /* The carrier is the render target, so its allocation is the reference
    * layout's slot row exactly; the arming gate freezes the same value,
    * and one contract keeps the recorded cell and the armed cell equal.
    */
   if (carrier_memory->bo.size != carrier_bytes) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: producer cell takes a %u-byte carrier",
                       carrier_bytes);
   }
   /* A 32-bit read-back observes no partial dword, so a sub-dword tail is
    * unpublishable content behind the poison claim.
    */
   if (carrier_memory->bo.size % 4 != 0) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: producer carrier size %" PRIu64
                       " is not whole dwords",
                       (uint64_t)carrier_memory->bo.size);
   }

   bool owns_map = carrier_memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &carrier_memory->bo,
                            &carrier_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: producer carrier memory is not "
                       "CPU-mappable");
   }
   uint32_t *carrier_dwords = carrier_memory->map;
   for (uint64_t i = 0; i < carrier_memory->bo.size / 4; i++)
      carrier_dwords[i] = R300_R2VB_PRODUCER_POISON_DWORD;
   radeon_drm_vk_bo_cache_sync(&device->drm, carrier_memory->map,
                               carrier_memory->bo.size);
   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &carrier_memory->bo,
                             carrier_memory->map);
      carrier_memory->map = NULL;
   }

   int install_result = install(cmd_buffer, carrier_memory);
   if (install_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(install_result));
   return VK_SUCCESS;
}

VkResult
r3v_native_record_r2vb_producer(VkCommandBuffer commandBuffer,
                                VkDeviceMemory carrierMemory)
{
   return record_r2vb_producer_stream(commandBuffer, carrierMemory,
                                      r3v_native_producer_cell_install);
}

VkResult
r3v_native_record_r2vb_producer_fp24_sweep(VkCommandBuffer commandBuffer,
                                           VkDeviceMemory carrierMemory)
{
   return record_r2vb_producer_stream(
      commandBuffer, carrierMemory,
      r3v_native_producer_fp24_sweep_cell_install);
}

VkResult
r3v_native_record_r2vb_producer_fp24_bisect(VkCommandBuffer commandBuffer,
                                            VkDeviceMemory carrierMemory)
{
   return record_r2vb_producer_stream(
      commandBuffer, carrierMemory,
      r3v_native_producer_fp24_bisect_cell_install);
}

int
r3v_native_reingest_cell_install(struct r3v_native_cmd_buffer *cmd_buffer,
                                 struct r3v_native_memory *carrier_memory,
                                 struct r3v_native_memory *color_memory)
{
   struct r300_r2vb_reingest_ib cell;
   int emit_result = r300_r2vb_reingest_reference_emit(&cell);
   if (emit_result != 0)
      return emit_result;
   emit_result = r300_r2vb_reingest_validate_reloc_sites(&cell);
   if (emit_result != 0) {
      r300_r2vb_reingest_pass_release(&cell);
      return emit_result;
   }

   struct r3v_native_bo_reference *references =
      calloc(R300_R2VB_REINGEST_SLOT_COUNT, sizeof(*references));
   if (references == NULL) {
      r300_r2vb_reingest_pass_release(&cell);
      return -ENOMEM;
   }
   /* The carrier crosses both engines inside one submission -- the
    * producer's color backend writes the slot row and the consumer's
    * vertex fetch reads it back -- so its one relocation entry carries
    * the GTT domain in both directions; the color target holds the
    * consuming draw alone.
    */
   references[R300_R2VB_REINGEST_SLOT_CARRIER] =
      (struct r3v_native_bo_reference){
         .handle = carrier_memory->bo.handle,
         .read_domains = RADEON_GEM_DOMAIN_GTT,
         .write_domain = RADEON_GEM_DOMAIN_GTT,
         .memory = carrier_memory,
      };
   references[R300_R2VB_REINGEST_SLOT_COLOR] =
      (struct r3v_native_bo_reference){
         .handle = color_memory->bo.handle,
         .read_domains = 0,
         .write_domain = RADEON_GEM_DOMAIN_GTT,
         .memory = color_memory,
      };

   r3v_native_cmd_buffer_install_ib(cmd_buffer,
                                    R3V_NATIVE_CELL_KIND_R2VB_REINGEST,
                                    cell.ib, cell.ib_size_dwords, references,
                                    R300_R2VB_REINGEST_SLOT_COUNT);
   /* install_ib took ownership of cell.ib; only the descriptor resets. */
   cell.ib = NULL;
   r300_r2vb_reingest_pass_release(&cell);
   return 0;
}

/* Records the re-ingest cell: poisons the carrier allocation, sentinel-
 * fills the color target's declared footprint, publishes both for the
 * unsnooped GART, and installs the concatenated reference stream against
 * the two BOs.  The carrier poison keeps the producer stage decidable
 * and the target sentinel keeps the consuming draw decidable, so a
 * failure classifies to its stage from the retained bytes alone.
 */
VkResult
r3v_native_record_r2vb_reingest(VkCommandBuffer commandBuffer,
                                VkDeviceMemory carrierMemory,
                                VkDeviceMemory colorMemory)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, carrier_memory, carrierMemory);
   VK_FROM_HANDLE(r3v_native_memory, color_memory, colorMemory);

   if (cmd_buffer == NULL || carrier_memory == NULL || color_memory == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);

   if (carrier_memory == color_memory) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: the carrier and the color target are "
                       "distinct allocations");
   }

   uint32_t carrier_bytes;
   if (r3v_native_producer_carrier_bytes(&carrier_bytes) != 0)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   if (carrier_memory->bo.size != carrier_bytes) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: re-ingest cell takes a %u-byte carrier",
                       carrier_bytes);
   }
   if (color_memory->bo.size != R3V_NATIVE_TARGET_MEMORY_BYTES) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: re-ingest cell takes a %u-byte color "
                       "target",
                       (unsigned)R3V_NATIVE_TARGET_MEMORY_BYTES);
   }

   bool owns_map = carrier_memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &carrier_memory->bo,
                            &carrier_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: re-ingest carrier memory is not "
                       "CPU-mappable");
   }
   uint32_t *carrier_dwords = carrier_memory->map;
   for (uint64_t i = 0; i < carrier_memory->bo.size / 4; i++)
      carrier_dwords[i] = R300_R2VB_PRODUCER_POISON_DWORD;
   radeon_drm_vk_bo_cache_sync(&device->drm, carrier_memory->map,
                               carrier_memory->bo.size);
   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &carrier_memory->bo,
                             carrier_memory->map);
      carrier_memory->map = NULL;
   }

   VkResult fill_result = sentinel_fill_color(device, color_memory,
                                              color_memory->bo.size);
   if (fill_result != VK_SUCCESS)
      return fill_result;

   int install_result = r3v_native_reingest_cell_install(
      cmd_buffer, carrier_memory, color_memory);
   if (install_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(install_result));
   return VK_SUCCESS;
}

/* The vertex allocation the tuple cell fetches: the FLOAT_4 slot-position
 * array followed by the FLOAT_2 model records over the reference count.
 */
#define R3V_NATIVE_FLOAT2_TUPLE_VERTEX_BYTES               \
   (R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT *               \
    (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES +            \
     R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES))

int
r3v_native_float2_tuple_cell_install(
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory,
   struct r3v_native_memory *vertex_memory)
{
   struct r300_r2vb_float2_tuple_ib cell;
   int emit_result = r300_r2vb_float2_tuple_reference_emit(&cell);
   if (emit_result != 0)
      return emit_result;
   emit_result = r300_r2vb_float2_tuple_pass_validate_reloc_sites(&cell);
   if (emit_result != 0) {
      r300_r2vb_float2_tuple_pass_release(&cell);
      return emit_result;
   }

   struct r3v_native_bo_reference *references =
      calloc(R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT, sizeof(*references));
   if (references == NULL) {
      r300_r2vb_float2_tuple_pass_release(&cell);
      return -ENOMEM;
   }
   /* The carrier's relocation carries the GTT domain in both directions:
    * the color backend writes the slot row here and a consuming vertex
    * fetch reads it in the delivery contract, the same pairing the
    * retained no-submit manifest's BO table declares.  The vertex BO
    * feeds the two LOAD_VBPNTR arrays and is device-read alone.
    */
   references[R300_R2VB_FLOAT2_TUPLE_SLOT_CARRIER] =
      (struct r3v_native_bo_reference){
         .handle = carrier_memory->bo.handle,
         .read_domains = RADEON_GEM_DOMAIN_GTT,
         .write_domain = RADEON_GEM_DOMAIN_GTT,
         .memory = carrier_memory,
      };
   references[R300_R2VB_FLOAT2_TUPLE_SLOT_VERTEX] =
      (struct r3v_native_bo_reference){
         .handle = vertex_memory->bo.handle,
         .read_domains = RADEON_GEM_DOMAIN_GTT,
         .write_domain = 0,
         .memory = vertex_memory,
      };

   r3v_native_cmd_buffer_install_ib(cmd_buffer,
                                    R3V_NATIVE_CELL_KIND_R2VB_FLOAT2_TUPLE,
                                    cell.ib, cell.ib_size_dwords, references,
                                    R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT);
   /* install_ib took ownership of cell.ib; only the descriptor resets. */
   cell.ib = NULL;
   r300_r2vb_float2_tuple_pass_release(&cell);
   return 0;
}

/* Records the fetched tuple cell: poisons the carrier allocation, writes
 * the reference vertex stream into the vertex allocation, publishes both
 * for the unsnooped GART, and installs the reference tuple pass against
 * the two BOs.  The carrier poison keeps the delivery decidable, and the
 * host-written vertex bytes are the fetch's ground truth: a post-run
 * comparison against the same serialization detects any device write
 * into the fetch source.
 */
VkResult
r3v_native_record_r2vb_float2_tuple(VkCommandBuffer commandBuffer,
                                    VkDeviceMemory carrierMemory,
                                    VkDeviceMemory vertexMemory)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, carrier_memory, carrierMemory);
   VK_FROM_HANDLE(r3v_native_memory, vertex_memory, vertexMemory);

   if (cmd_buffer == NULL || carrier_memory == NULL || vertex_memory == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);

   if (carrier_memory == vertex_memory) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: the carrier and the vertex stream are "
                       "distinct allocations");
   }

   uint32_t carrier_bytes;
   if (r3v_native_producer_carrier_bytes(&carrier_bytes) != 0)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   if (carrier_memory->bo.size != carrier_bytes) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: tuple cell takes a %u-byte carrier",
                       carrier_bytes);
   }
   if (vertex_memory->bo.size != R3V_NATIVE_FLOAT2_TUPLE_VERTEX_BYTES) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: tuple cell takes a %u-byte vertex "
                       "stream",
                       (unsigned)R3V_NATIVE_FLOAT2_TUPLE_VERTEX_BYTES);
   }

   bool owns_map = carrier_memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &carrier_memory->bo,
                            &carrier_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: tuple carrier memory is not "
                       "CPU-mappable");
   }
   uint32_t *carrier_dwords = carrier_memory->map;
   for (uint64_t i = 0; i < carrier_memory->bo.size / 4; i++)
      carrier_dwords[i] = R300_R2VB_PRODUCER_POISON_DWORD;
   radeon_drm_vk_bo_cache_sync(&device->drm, carrier_memory->map,
                               carrier_memory->bo.size);
   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &carrier_memory->bo,
                             carrier_memory->map);
      carrier_memory->map = NULL;
   }

   bool owns_vertex_map = vertex_memory->map == NULL;
   if (owns_vertex_map &&
       radeon_drm_vk_bo_map(&device->drm, &vertex_memory->bo,
                            &vertex_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: tuple vertex memory is not "
                       "CPU-mappable");
   }
   int stream_result = r300_r2vb_float2_tuple_vertex_stream(
      r300_r2vb_float2_tuple_reference_records,
      R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, vertex_memory->map,
      R3V_NATIVE_FLOAT2_TUPLE_VERTEX_BYTES);
   if (stream_result == 0) {
      radeon_drm_vk_bo_cache_sync(&device->drm, vertex_memory->map,
                                  vertex_memory->bo.size);
   }
   if (owns_vertex_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &vertex_memory->bo,
                             vertex_memory->map);
      vertex_memory->map = NULL;
   }
   if (stream_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(stream_result));

   int install_result = r3v_native_float2_tuple_cell_install(
      cmd_buffer, carrier_memory, vertex_memory);
   if (install_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(install_result));
   return VK_SUCCESS;
}

/* The serial status-load cell is the tuple cell's frozen stream under
 * its own kind: recording reuses the tuple path byte for byte, then
 * stamps the serial kind, so the arming gate applies the serial bound
 * to the identical PM4 the one-shot tuple cell qualified on silicon.
 */
VkResult
r3v_native_record_r2vb_status_load_serial(VkCommandBuffer commandBuffer,
                                          VkDeviceMemory carrierMemory,
                                          VkDeviceMemory vertexMemory)
{
   VkResult result = r3v_native_record_r2vb_float2_tuple(
      commandBuffer, carrierMemory, vertexMemory);
   if (result != VK_SUCCESS)
      return result;
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL;
   return VK_SUCCESS;
}
