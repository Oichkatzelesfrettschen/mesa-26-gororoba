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
#include "amd/r300/common/r300_r2vb_fetched_producer.h"
#include "amd/r300/common/r300_r2vb_float2_tuple_pass.h"
#include "amd/r300/common/r300_r2vb_producer_pass.h"
#include "amd/r300/common/r300_r2vb_reingest_pass.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_vertex_format.h"
#include "amd/r300/common/r300_zb_depth_control_cell.h"
#include "amd/r300/cpu/r300_cpu_vertex.h"
#include "amd/r300/cpu/r300_cpu_vertex_job.h"

#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_command_pool.h"
#include "vk_log.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <radeon_drm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The cell renders a 64x64 ARGB8888 target at a 64-pixel pitch; the color
 * allocation carries one extra row past the render extent as the output
 * oracle's canary, and the whole allocation is sentinel-filled before
 * submission so any device write is detectable.
 */
#define R3V_TRIANGLE_VERTEX_BYTES \
   (R300_TRIANGLE_VERTEX_DWORDS * sizeof(float))
#define R3V_TRIANGLE_VARYING_VERTEX_BYTES \
   (R300_TRIANGLE_VARYING_VERTEX_DWORDS * sizeof(float))

static VkResult
sentinel_fill_color(struct r3v_native_device *device,
                    struct r3v_native_memory *color_memory,
                    uint64_t fill_bytes);

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

VkResult
r3v_native_record_tcl_bypass_triangle_render_shape(
   VkCommandBuffer commandBuffer, VkDeviceMemory vertexMemory,
   VkDeviceMemory colorMemory,
   const struct r300_triangle_render_shape *shape)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, vertex_memory, vertexMemory);
   VK_FROM_HANDLE(r3v_native_memory, color_memory, colorMemory);
   if (cmd_buffer == NULL || vertex_memory == NULL || color_memory == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);
   if (r300_tcl_bypass_triangle_render_shape_validate(shape) != 0)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: render shape outside the family");
   const uint32_t color_bytes =
      r300_tcl_bypass_triangle_render_shape_color_bytes(shape);
   if (vertex_memory->bo.size < R3V_TRIANGLE_VERTEX_BYTES ||
       color_memory->bo.size < color_bytes) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: render shape needs %zu vertex bytes "
                       "and %u color bytes",
                       (size_t)R3V_TRIANGLE_VERTEX_BYTES, color_bytes);
   }
   VkResult role_result = validate_triangle_memory_roles(
      device, vertex_memory, color_memory);
   if (role_result != VK_SUCCESS)
      return role_result;

   float vertices[R300_TRIANGLE_VERTEX_DWORDS];
   r300_tcl_bypass_triangle_render_shape_vertices(shape, vertices);
   bool owns_map = vertex_memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &vertex_memory->bo,
                            &vertex_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: render shape vertex memory is not "
                       "CPU-mappable");
   }
   memcpy(vertex_memory->map, vertices, sizeof(vertices));
   radeon_drm_vk_bo_cache_sync(&device->drm, vertex_memory->map,
                               sizeof(vertices));
   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &vertex_memory->bo,
                             vertex_memory->map);
      vertex_memory->map = NULL;
   }

   VkResult result =
      sentinel_fill_color(device, color_memory, color_memory->bo.size);
   if (result != VK_SUCCESS)
      return result;

   struct r300_tcl_bypass_triangle_ib cell;
   int emit_result = r300_tcl_bypass_triangle_render_shape_emit(shape, &cell);
   if (emit_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(emit_result));
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
                                    R3V_NATIVE_CELL_KIND_TRIANGLE_RENDER_SHAPE,
                                    cell.ib, cell.ib_size_dwords, references,
                                    R300_TRIANGLE_SLOT_COUNT);
   cell.ib = NULL;
   r300_tcl_bypass_triangle_release(&cell);
   return VK_SUCCESS;
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
                               uint32_t width, uint32_t height,
                               bool varying, uint32_t triangle_count)
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
   int emit_result = r300_tcl_bypass_triangle_family_emit(
      width, height, varying, triangle_count, &cell);
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
                                         R3V_NATIVE_TARGET_HEIGHT, false, 1);
}

VkResult
r3v_native_record_tcl_bypass_triangle_carrier(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory,
   struct r3v_native_image *target_image, bool varying,
   uint32_t triangle_count)
{
   struct r3v_native_memory *color_memory = target_image->memory;
   VkResult role_result = validate_triangle_memory_roles(
      device, carrier_memory, color_memory);
   if (role_result != VK_SUCCESS)
      return role_result;

   if (triangle_count < 1 || triangle_count > R300_TRIANGLE_MAX_TRIANGLES)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   const uint64_t carrier_bytes =
      (uint64_t)triangle_count * (varying ? R3V_TRIANGLE_VARYING_VERTEX_BYTES
                                          : R3V_TRIANGLE_VERTEX_BYTES);
   if (carrier_memory->bo.size < carrier_bytes ||
       color_memory->bo.size <
          r3v_native_image_footprint_bytes(target_image->height)) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: triangle cell needs %" PRIu64
                       " vertex bytes and the target image's declared "
                       "footprint",
                       carrier_bytes);
   }
   return emit_and_install_triangle_cell(device, cmd_buffer, carrier_memory,
                                         color_memory, target_image->width,
                                         target_image->height, varying,
                                         triangle_count);
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
   if (draw->stream_mask == 0) {
      VkResult clear_result = sentinel_fill_color(
         device, draw->target_memory, draw->target_fill_bytes);
      if (clear_result != VK_SUCCESS || draw->clear_rect_count == 0)
         return clear_result;
      /* The recorded attachment clears land after the load-op clear,
       * in API order, over the render family's fixed row pitch at
       * bind offset zero.
       */
      struct r3v_native_memory *target = draw->target_memory;
      bool owns_map = target->map == NULL;
      if (owns_map &&
          radeon_drm_vk_bo_map(&device->drm, &target->bo,
                               &target->map) != 0) {
         return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                          "r3v-native: pass target memory is not "
                          "CPU-mappable for the attachment clear");
      }
      for (uint32_t r = 0; r < draw->clear_rect_count; r++) {
         const struct r3v_native_pass_clear_rect *rect =
            &draw->clear_rects[r];
         for (uint32_t row = 0; row < rect->height; row++) {
            uint32_t *texels =
               (uint32_t *)((uint8_t *)target->map +
                            (uint64_t)(rect->y + row) *
                               R3V_NATIVE_TARGET_ROW_BYTES) +
               rect->x;
            for (uint32_t x = 0; x < rect->width; x++)
               texels[x] = rect->dword;
         }
      }
      radeon_drm_vk_bo_cache_sync(&device->drm, target->map,
                                  draw->target_fill_bytes);
      if (owns_map) {
         radeon_drm_vk_bo_unmap(&device->drm, &target->bo, target->map);
         target->map = NULL;
      }
      return VK_SUCCESS;
   }

   struct r3v_native_memory *carrier = cmd_buffer->owned_carrier;

   /* Every stream the job reads maps for the execution; two slots may
    * share one memory (one buffer bound to two bindings, or two buffers
    * in one allocation), so the first slot to map a memory owns the
    * unmap and the unmap runs after the last use.  robustBufferAccess
    * enabled at device creation makes an out-of-bounds vertex record
    * read zeros; with the feature off the same record refuses the draw
    * before any write. */
   struct r300_vertex_stream sources[R300_VERTEX_JOB_MAX_INPUTS] = { 0 };
   /* One owned map per read slot plus the index buffer's. */
   struct r3v_native_memory *owned_maps[R300_VERTEX_JOB_MAX_INPUTS + 1];
   uint32_t owned_map_count = 0;
   VkResult result = VK_SUCCESS;
   for (uint32_t slot = 0; slot < R300_VERTEX_JOB_MAX_INPUTS; slot++) {
      if (!(draw->stream_mask & (1u << slot)))
         continue;
      const struct r3v_native_deferred_stream *stream_desc =
         &draw->streams[slot];
      struct r3v_native_memory *memory = stream_desc->buffer->memory;
      if (memory->map == NULL) {
         if (radeon_drm_vk_bo_map(&device->drm, &memory->bo,
                                  &memory->map) != 0) {
            result = vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                               "r3v-native: bound vertex memory is not "
                               "CPU-mappable at submission");
            break;
         }
         owned_maps[owned_map_count++] = memory;
      }
      sources[slot] = (struct r300_vertex_stream){
         .data = (const uint8_t *)memory->map + stream_desc->buffer->offset +
                 stream_desc->stream_base,
         .stride = stream_desc->stride,
         .size_bytes = stream_desc->buffer->vk.size - stream_desc->stream_base,
         .oob_reads_zero = device->vk.enabled_features.robustBufferAccess,
         .instance_rate = stream_desc->instance_rate,
         .instance_divisor = stream_desc->instance_divisor,
      };
   }
   /* The indexed draw reads its three indices now, at execution, so an
    * index written between record and submit is honored like a vertex
    * write: each index sums with the base vertex in 32-bit wrapping
    * arithmetic (a negative sum wraps past any bound and the robust
    * rule judges it as an out-of-bounds record), and the vertex
    * numbers drive the gather in place of the linear range. */
   uint32_t vertex_ids_stack[R300_TRIANGLE_VERTEX_DWORDS / 4] = { 0 };
   uint32_t *vertex_ids = vertex_ids_stack;
   uint32_t *vertex_ids_heap = NULL;
   if (result == VK_SUCCESS && draw->indexed &&
       draw->vertex_count > ARRAY_SIZE(vertex_ids_stack)) {
      vertex_ids_heap =
         vk_alloc(&device->vk.alloc,
                  (size_t)draw->vertex_count * sizeof(uint32_t), 8,
                  VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (vertex_ids_heap == NULL)
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      vertex_ids = vertex_ids_heap;
   }
   if (result == VK_SUCCESS && draw->indexed) {
      struct r3v_native_memory *memory = draw->index_buffer->memory;
      if (memory->map == NULL) {
         if (radeon_drm_vk_bo_map(&device->drm, &memory->bo,
                                  &memory->map) != 0) {
            result = vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                               "r3v-native: bound index memory is not "
                               "CPU-mappable at submission");
         } else {
            owned_maps[owned_map_count++] = memory;
         }
      }
      if (result == VK_SUCCESS) {
         const uint8_t *indices =
            (const uint8_t *)memory->map + draw->index_buffer->offset +
            draw->index_base +
            (uint64_t)draw->first_index * draw->index_bytes;
         for (uint32_t v = 0; v < draw->vertex_count; v++) {
            uint32_t index;
            if (draw->index_bytes == 2) {
               uint16_t index16;
               memcpy(&index16, indices + v * 2, sizeof(index16));
               index = index16;
            } else {
               memcpy(&index, indices + v * 4, sizeof(index));
            }
            vertex_ids[v] = index + (uint32_t)draw->vertex_offset;
         }
      }
   }
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, vertex_ids_heap);
      for (uint32_t i = 0; i < owned_map_count; i++) {
         radeon_drm_vk_bo_unmap(&device->drm, &owned_maps[i]->bo,
                                owned_maps[i]->map);
         owned_maps[i]->map = NULL;
      }
      return result;
   }
   /* Slot 0 is the position stream the delivery routes model: the
    * route resolves on its format, and the GPU and R2VB host-model
    * routes then admit the slot-0 identity job alone, so a job reading
    * other slots under a producer gate refuses by name at admission
    * rather than taking a route the gate did not select.  A job that
    * leaves slot 0 unread resolves through the INVALID format to the
    * CPU route, as a format outside the producer family does. */
   const struct r3v_native_vertex_stream_desc stream = {
      .records = sources[0].data,
      .size_bytes = sources[0].size_bytes,
      .stride = sources[0].stride,
      .first_vertex = draw->first_vertex,
      .format_id = (draw->stream_mask & 1u) ? draw->streams[0].format_id
                                            : R300_VERTEX_FORMAT_INVALID,
   };
   const struct r300_vertex_stream source = sources[0];

   /* An admitted GPU-producer delivery already poisoned and published
    * the carrier at admission; the device writes it, so the host fill,
    * route resolution, and viewport transform below stay out and only
    * the load-op clear executes here.
    */
   if (draw->gpu_producer_delivery) {
      vk_free(&device->vk.alloc, vertex_ids_heap);
      for (uint32_t i = 0; i < owned_map_count; i++) {
         radeon_drm_vk_bo_unmap(&device->drm, &owned_maps[i]->bo,
                                owned_maps[i]->map);
         owned_maps[i]->map = NULL;
      }
      return sentinel_fill_color(device, draw->target_memory,
                                 draw->target_fill_bytes);
   }

   /* The CPU route stages its records on the host and the carrier
    * receives them only after the clip-volume check below, so a
    * refused record leaves the carrier untouched.  The staging covers
    * the draw's 3 * instance_count records of record_dwords each (a
    * job storing the varying writes eight-dword records; the positions
    * sit at the head of each record for the transform below): the
    * reference three ride the stack, a larger expansion rides a host
    * allocation released before return. */
   const uint32_t record_dwords =
      r300_vertex_job_record_dwords(&draw->vertex_job);
   const uint32_t record_count = draw->vertex_count * draw->instance_count;
   const uint32_t staged_dwords = record_count * record_dwords;
   uint32_t staged_stack[R300_TRIANGLE_VARYING_VERTEX_DWORDS];
   uint32_t *staged_heap = NULL;
   uint32_t *staged = staged_stack;
   if (staged_dwords > ARRAY_SIZE(staged_stack)) {
      staged_heap = vk_alloc(&device->vk.alloc,
                             (size_t)staged_dwords * sizeof(uint32_t), 8,
                             VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (staged_heap == NULL)
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      staged = staged_heap;
   }

   bool owns_carrier_map = result == VK_SUCCESS && carrier->map == NULL;
   if (result != VK_SUCCESS) {
      /* The staging refusal delivers nothing; the shared unmap and
       * error paths below still run. */
   } else if (owns_carrier_map &&
              radeon_drm_vk_bo_map(&device->drm, &carrier->bo,
                                   &carrier->map) != 0) {
      owns_carrier_map = false;
      result = vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                         "r3v-native: carrier memory is not CPU-mappable "
                         "at submission");
   } else {
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
      r300_delivery_route_resolve(device->r2vb_delivery_gate,
                                  device->r2vb_gpu_delivery_gate,
                                  device->r2vb_fetched_gate,
                                  stream.format_id, &route_decision);
      /* The GPU producer route names a device-side delivery this
       * deferred draw cannot execute: live producer submission routes
       * only through the operator-armed attended surface.  The exact
       * double opt-in therefore refuses the draw by name instead of
       * downgrading to a host copy the caller did not select.
       */
      if (route_decision.route == R300_DELIVERY_ROUTE_R2VB_GPU_PRODUCER ||
          route_decision.route ==
             R300_DELIVERY_ROUTE_R2VB_GPU_PRODUCER_FETCHED) {
         result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                            "r3v-native: %s; live producer submission "
                            "routes through the attended cell surface",
                            route_decision.reason);
      }
      /* The R2VB host model delivers one linear range of in-bounds
       * records for one instance, as the device fetch it models would,
       * and stands in for the identity vertex job alone; a robust range
       * with a record outside the bound, an indexed draw whose
       * dereference happens on the host, an instanced draw, and every
       * other admitted job execute on the CPU interpreter route. */
      const bool r2vb_route =
         route_decision.route == R300_DELIVERY_ROUTE_R2VB_HOST_MODEL &&
         draw->vertex_job_identity && !draw->indexed &&
         draw->cull_mode == VK_CULL_MODE_NONE &&
         !draw->sample_mask_zero &&
         draw->instance_count == 1 && draw->vertex_count == 3 &&
         r300_cpu_vertex_range_in_bounds(stream.format_id, &source,
                                         stream.first_vertex,
                                         R300_TRIANGLE_VERTEX_DWORDS / 4);
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
         /* The CPU route executes the pipeline's vertex job over every
          * stream it reads, once per instance from first_instance,
          * instance-major; the identity job over one instance reduces
          * to the gather, so the reference cell's carrier bytes are
          * unchanged. */
         const struct r300_cpu_vertex_draw cpu_draw = {
            .vertex_ids = draw->indexed ? vertex_ids : NULL,
            .first_vertex = stream.first_vertex,
            .vertex_count = draw->vertex_count,
            .first_instance = draw->first_instance,
            .instance_count = draw->instance_count,
         };
         gathered = r300_cpu_vertex_job_execute_draw(
            &draw->vertex_job, sources, &cpu_draw, staged, staged_dwords);
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
          * The R2VB delivery landed in the carrier, so it transforms
          * through a host copy; the CPU route transforms its staging.
          */
         uint32_t delivered[R300_TRIANGLE_VERTEX_DWORDS];
         uint32_t *records = staged;
         uint32_t position_count = record_count;
         uint32_t position_stride = record_dwords;
         uint32_t position_dwords = staged_dwords;
         if (r2vb_route) {
            memcpy(delivered, carrier->map, sizeof(delivered));
            records = delivered;
            position_count = R300_TRIANGLE_VERTEX_DWORDS / 4;
            position_stride = 4;
            position_dwords = R300_TRIANGLE_VERTEX_DWORDS;
         }
         for (uint32_t v = 0; result == VK_SUCCESS && v < position_count;
              v++) {
            float pos[4];
            memcpy(pos, &records[v * position_stride], sizeof(pos));
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
            memcpy(&records[v * position_stride], pos, sizeof(pos));
         }
         /* Facing cull in window coordinates: the signed area
          * 2A = (x1-x0)(y2-y0) - (x2-x0)(y1-y0), positive for a
          * counter-clockwise triangle; a culled triangle's three
          * records collapse to its first vertex, a degenerate
          * triangle the raster draws no fragment for, so the IB and
          * record count stand.  A zero-area triangle already draws
          * nothing and passes through.
          */
         if (result == VK_SUCCESS &&
             (draw->cull_mode != VK_CULL_MODE_NONE ||
              draw->sample_mask_zero)) {
            for (uint32_t t = 0; t + 3 <= position_count; t += 3) {
               float p[3][2];
               for (unsigned v = 0; v < 3; v++)
                  memcpy(p[v], &records[(t + v) * position_stride],
                         sizeof(p[v]));
               const double area2 =
                  ((double)p[1][0] - p[0][0]) *
                     ((double)p[2][1] - p[0][1]) -
                  ((double)p[2][0] - p[0][0]) *
                     ((double)p[1][1] - p[0][1]);
               /* Zero coverage collapses every triangle; culling
                * decides by facing. */
               if (!draw->sample_mask_zero) {
                  if (area2 == 0.0)
                     continue;
                  const bool counter_clockwise = area2 > 0.0;
                  const bool front_facing =
                     counter_clockwise ==
                     (draw->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE);
                  const VkCullModeFlags facing_bit =
                     front_facing ? VK_CULL_MODE_FRONT_BIT
                                  : VK_CULL_MODE_BACK_BIT;
                  if ((draw->cull_mode & facing_bit) == 0)
                     continue;
               }
               for (unsigned v = 1; v < 3; v++)
                  memcpy(&records[(t + v) * position_stride],
                         &records[t * position_stride],
                         (size_t)position_stride * sizeof(uint32_t));
            }
         }
         if (result == VK_SUCCESS)
            memcpy(carrier->map, records,
                   (size_t)position_dwords * sizeof(uint32_t));
      } else if (result == VK_SUCCESS && !r2vb_route) {
         memcpy(carrier->map, staged,
                (size_t)staged_dwords * sizeof(uint32_t));
      }
      /* Publication is delivery-unconditional: a WINDOW-space carrier
       * skips the transform branch above yet its bytes still cross to
       * the device through this one sync.
       */
      if (result == VK_SUCCESS)
         radeon_drm_vk_bo_cache_sync(&device->drm, carrier->map,
                                     r2vb_route ? R3V_TRIANGLE_VERTEX_BYTES
                                                : (size_t)staged_dwords *
                                                     sizeof(uint32_t));
   }
   if (owns_carrier_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &carrier->bo, carrier->map);
      carrier->map = NULL;
   }
   vk_free(&device->vk.alloc, staged_heap);
   vk_free(&device->vk.alloc, vertex_ids_heap);

   for (uint32_t i = 0; i < owned_map_count; i++) {
      radeon_drm_vk_bo_unmap(&device->drm, &owned_maps[i]->bo,
                             owned_maps[i]->map);
      owned_maps[i]->map = NULL;
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

/* The producer footprint over the triangle's three records: the odd
 * count pads to the four-slot pitch, sixteen dwords of C4_32_FP row.
 */
#define R3V_GPU_PRODUCER_CARRIER_DWORDS 16u

/* The slot-position BO: one GTT page holds the three reference records
 * with the page-granular GEM allocation the memory contract uses.
 */
#define R3V_FETCHED_SLOT_BO_BYTES 4096u

/* The fetched route's reference-list order, which is the relocation-chunk
 * order the composed payloads name: the consumer's two slots keep their
 * indices and the producer's two arrays follow.
 */
enum r3v_fetched_reference {
   R3V_FETCHED_REFERENCE_CARRIER = R300_TRIANGLE_SLOT_VERTEX,
   R3V_FETCHED_REFERENCE_COLOR = R300_TRIANGLE_SLOT_COLOR,
   R3V_FETCHED_REFERENCE_SLOT = 2,
   R3V_FETCHED_REFERENCE_SOURCE = 3,
   R3V_FETCHED_REFERENCE_COUNT = 4,
};

/* Admits the fetched GPU-producer route for the pending deferred draw:
 * the producer fetches the bound vertex BO and a driver-owned slot BO,
 * composed ahead of the recorded consumer through the role composer.
 * Every fallible step -- oracle, geometry, composition, slot allocation,
 * reference list -- completes before the command buffer changes, so a
 * refusal leaves the recorded IB, references, carrier, and kind exactly
 * as recorded and the next submission sees no half-applied state.
 */
static VkResult
admit_fetched_producer(struct r3v_native_device *device,
                       struct r3v_native_cmd_buffer *cmd_buffer)
{
   struct r3v_native_deferred_draw *draw = &cmd_buffer->deferred_draw;

   if (device->gpu_producer_quarantined) {
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: GPU producer capability is "
                       "quarantined on this device; a completed delivery "
                       "diverged from the CPU oracle");
   }
   /* The composed route binds one source relocation role, so the
    * identity job over slot 0 alone is admissible: a job reading any
    * other slot would need a relocation the composition has no role
    * for. */
   if (!draw->vertex_job_identity || draw->stream_mask != 1u ||
       (cmd_buffer->cell_kind != R3V_NATIVE_CELL_KIND_TRIANGLE &&
        cmd_buffer->cell_kind !=
           R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED)) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: fetched GPU producer route admits the "
                       "identity vertex job over one source stream on the "
                       "recorded triangle consumer alone; the route binds "
                       "one source relocation role");
   }

   /* Source geometry: the first fetched record's byte offset inside the
    * bound memory's BO, dword-granular, with the stride the pipeline
    * declared.  The VBPNTR pointer is a 32-bit byte offset.
    */
   const struct r3v_native_deferred_stream *position = &draw->streams[0];
   struct r3v_native_buffer *buffer = position->buffer;
   struct r3v_native_memory *memory = buffer->memory;
   const uint64_t first_byte = buffer->offset + position->stream_base +
                               (uint64_t)draw->first_vertex * position->stride;
   if (first_byte > UINT32_MAX) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: fetched source offset %" PRIu64
                       " exceeds the 32-bit vertex pointer",
                       first_byte);
   }
   if (first_byte % 4 != 0 || position->stride % 4 != 0) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: fetched source offset %" PRIu64
                       " and stride %u must be dword-granular; the VBPNTR "
                       "pointer and stride fields carry dwords",
                       first_byte, position->stride);
   }
   const struct r300_r2vb_fetched_source source = {
      .format_id = position->format_id,
      .offset_bytes = (uint32_t)first_byte,
      .stride_bytes = position->stride,
      .bo_size_bytes = memory->bo.size,
   };

   /* The four relocations resolve one BO each: the reloc list folds
    * duplicate handles into one entry, which would shift the chunk index
    * the composed payloads name.  The carrier and slot BOs are
    * driver-owned and distinct by construction; the application's source
    * memory must not be the color target's.
    */
   struct r3v_native_memory *carrier = cmd_buffer->owned_carrier;
   if (memory->bo.handle == draw->target_memory->bo.handle ||
       memory->bo.handle == carrier->bo.handle) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: fetched source memory aliases another "
                       "bound buffer object; the route binds four distinct "
                       "relocations");
   }

   /* The oracle: the delivery identity over the source stream, the bytes
    * the device fetch and the US identity path reproduce, refusing an
    * out-of-domain component (-EDOM) or an out-of-bounds record; the CPU
    * gather re-derives the same bytes, the executor remaining the oracle
    * of record.  The fetched route delivers in-bounds records alone, so
    * robustBufferAccess's zero substitution has no device-side form here
    * and an out-of-bounds range refuses by name.
    */
   bool owns_map = memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &memory->bo, &memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: bound vertex memory is not "
                       "CPU-mappable at submission");
   }
   uint32_t expected[R300_TRIANGLE_VERTEX_DWORDS];
   uint32_t oracle[R300_TRIANGLE_VERTEX_DWORDS];
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)memory->map + buffer->offset +
              position->stream_base,
      .stride = position->stride,
      .size_bytes = buffer->vk.size - position->stream_base,
   };
   int delivered = r300_r2vb_identity_deliver(
      position->format_id, &stream, draw->first_vertex,
      R300_TRIANGLE_VERTEX_DWORDS / 4, expected, R300_TRIANGLE_VERTEX_DWORDS);
   int gathered = delivered == 0
                     ? r300_cpu_vertex_gather(position->format_id, &stream,
                                              draw->first_vertex,
                                              R300_TRIANGLE_VERTEX_DWORDS / 4,
                                              oracle,
                                              R300_TRIANGLE_VERTEX_DWORDS)
                     : 0;
   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &memory->bo, memory->map);
      memory->map = NULL;
   }
   if (delivered != 0) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: fetched delivery oracle refused (%d)%s",
                       delivered,
                       delivered == -EDOM
                          ? "; a record is outside the FP24 fixed-point "
                            "domain"
                          : "; the route fetches in-bounds records alone");
   }
   if (gathered != 0 || memcmp(oracle, expected, sizeof(oracle)) != 0) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: CPU gather (%d) diverged from the "
                       "delivery identity under the fetched route",
                       gathered);
   }

   /* The consumer half is the recorded cell: the whole IB on first
    * admission, the slice past the producer prefix on resubmission.  Its
    * two relocation sites carry the triangle slots as payloads.
    */
   const bool composed_before =
      cmd_buffer->cell_kind == R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED;
   const uint32_t *consumer_words =
      composed_before ? cmd_buffer->ib + draw->gpu_producer_dwords
                      : cmd_buffer->ib;
   const uint32_t consumer_dwords =
      composed_before ? cmd_buffer->ib_size_dwords - draw->gpu_producer_dwords
                      : cmd_buffer->ib_size_dwords;
   uint32_t site_index[2];
   uint32_t site_payload[2];
   const int sites = r300_pm4_scan_reloc_sites(
      consumer_words, consumer_dwords, site_index, site_payload, 2);
   uint32_t carrier_site = 0;
   uint32_t color_site = 0;
   if (sites == 2) {
      for (unsigned i = 0; i < 2; i++) {
         if (site_payload[i] == R300_TRIANGLE_SLOT_VERTEX * 4)
            carrier_site = site_index[i];
         else if (site_payload[i] == R300_TRIANGLE_SLOT_COLOR * 4)
            color_site = site_index[i];
      }
   }
   if (sites != 2 || carrier_site == 0 || color_site == 0) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: recorded consumer carries %d "
                       "relocation sites; the fetched route composes over "
                       "the triangle's carrier and color sites",
                       sites);
   }

   const struct r300_r2vb_fetched_route_params route_params = {
      .source = source,
      .slot_offset_bytes = 0,
      .slot_bo_size_bytes = R3V_FETCHED_SLOT_BO_BYTES,
      .consumer_words = consumer_words,
      .consumer_dwords = consumer_dwords,
      .consumer_carrier_site = carrier_site,
      .consumer_color_site = color_site,
      .roles = {
         .chunk_index = {
            [R300_R2VB_BO_CARRIER] = R3V_FETCHED_REFERENCE_CARRIER,
            [R300_R2VB_BO_COLOR] = R3V_FETCHED_REFERENCE_COLOR,
            [R300_R2VB_BO_SLOT] = R3V_FETCHED_REFERENCE_SLOT,
            [R300_R2VB_BO_MODEL] = R3V_FETCHED_REFERENCE_SOURCE,
         },
      },
   };
   struct r300_r2vb_fetched_route_ib route;
   int composed = r300_r2vb_fetched_route_compose(&route_params, &route);
   if (composed == 0 && device->gpu_producer_compose_inject_errno != 0) {
      r300_r2vb_fetched_route_release(&route);
      composed = device->gpu_producer_compose_inject_errno;
   }
   if (composed != 0) {
      return vk_errorf(device, r3v_native_cell_vk_result_from_errno(composed),
                       "r3v-native: fetched route composition refused "
                       "(%d)", composed);
   }
   /* The composition binds exactly the five sites over four roles, and
    * the consumer slice lands verbatim past the producer prefix.
    */
   if (route.composition.reloc_count != 5 ||
       route.ib_size_dwords != route.consumer_start_dwords + consumer_dwords) {
      r300_r2vb_fetched_route_release(&route);
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: fetched route composed %u sites over "
                       "%u dwords; five sites and a verbatim consumer "
                       "slice are the contract",
                       route.composition.reloc_count, route.ib_size_dwords);
   }

   /* The slot BO: allocated once per command buffer, its records
    * rewritten on every admission so the content matches the pass the
    * composition just emitted.
    */
   struct r3v_native_memory *slot = cmd_buffer->owned_slot;
   if (slot == NULL) {
      slot = vk_zalloc(&cmd_buffer->vk.pool->alloc, sizeof(*slot), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (slot == NULL) {
         r300_r2vb_fetched_route_release(&route);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      if (radeon_drm_vk_bo_create(&device->drm, R3V_FETCHED_SLOT_BO_BYTES,
                                  R3V_NATIVE_MEMORY_ALIGNMENT,
                                  RADEON_GEM_DOMAIN_GTT, 0, false,
                                  &slot->bo) != 0) {
         vk_free(&cmd_buffer->vk.pool->alloc, slot);
         r300_r2vb_fetched_route_release(&route);
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
   }
   void *slot_map = NULL;
   if (radeon_drm_vk_bo_map(&device->drm, &slot->bo, &slot_map) != 0) {
      if (cmd_buffer->owned_slot == NULL) {
         radeon_drm_vk_bo_free(&device->drm, &slot->bo);
         vk_free(&cmd_buffer->vk.pool->alloc, slot);
      }
      r300_r2vb_fetched_route_release(&route);
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: slot memory is not CPU-mappable at "
                       "admission");
   }
   uint32_t slot_words[R300_R2VB_PRODUCER_REFERENCE_COUNT * 4];
   ASSERTED const int positioned = r300_r2vb_fetched_producer_slot_positions(
      R300_R2VB_PRODUCER_REFERENCE_COUNT, slot_words, ARRAY_SIZE(slot_words));
   assert(positioned == 0);
   memcpy(slot_map, slot_words, sizeof(slot_words));
   radeon_drm_vk_bo_cache_sync(&device->drm, slot_map, sizeof(slot_words));
   radeon_drm_vk_bo_unmap(&device->drm, &slot->bo, slot_map);

   /* The reference list: the consumer's two slots in place, the carrier
    * gaining the color backend's write domain, then the slot and source
    * arrays device-read.
    */
   struct r3v_native_bo_reference *references =
      calloc(R3V_FETCHED_REFERENCE_COUNT, sizeof(*references));
   if (references == NULL) {
      if (cmd_buffer->owned_slot == NULL) {
         radeon_drm_vk_bo_free(&device->drm, &slot->bo);
         vk_free(&cmd_buffer->vk.pool->alloc, slot);
      }
      r300_r2vb_fetched_route_release(&route);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   references[R3V_FETCHED_REFERENCE_CARRIER] =
      cmd_buffer->references[R300_TRIANGLE_SLOT_VERTEX];
   references[R3V_FETCHED_REFERENCE_CARRIER].read_domains =
      RADEON_GEM_DOMAIN_GTT;
   references[R3V_FETCHED_REFERENCE_CARRIER].write_domain =
      RADEON_GEM_DOMAIN_GTT;
   references[R3V_FETCHED_REFERENCE_COLOR] =
      cmd_buffer->references[R300_TRIANGLE_SLOT_COLOR];
   references[R3V_FETCHED_REFERENCE_SLOT] = (struct r3v_native_bo_reference){
      .handle = slot->bo.handle,
      .read_domains = RADEON_GEM_DOMAIN_GTT,
      .write_domain = 0,
      .memory = slot,
   };
   references[R3V_FETCHED_REFERENCE_SOURCE] =
      (struct r3v_native_bo_reference){
         .handle = memory->bo.handle,
         .read_domains = RADEON_GEM_DOMAIN_GTT,
         .write_domain = 0,
         .memory = memory,
      };

   /* Every fallible step has completed: install the composed stream. */
   free(cmd_buffer->ib);
   free(cmd_buffer->references);
   cmd_buffer->ib = route.ib;
   cmd_buffer->ib_size_dwords = route.ib_size_dwords;
   cmd_buffer->references = references;
   cmd_buffer->reference_count = R3V_FETCHED_REFERENCE_COUNT;
   cmd_buffer->owned_slot = slot;
   cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED;
   draw->gpu_producer_dwords = route.consumer_start_dwords;
   draw->gpu_producer_delivery = true;
   route.ib = NULL;

   /* The poisoned carrier crosses to the device now; the read-back judges
    * every slot against the delivery identity plus the pad slot's poison.
    */
   bool owns_carrier_map = carrier->map == NULL;
   if (owns_carrier_map &&
       radeon_drm_vk_bo_map(&device->drm, &carrier->bo,
                            &carrier->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: carrier memory is not CPU-mappable "
                       "at admission");
   }
   uint32_t *carrier_words = carrier->map;
   for (uint32_t i = 0; i < R3V_GPU_PRODUCER_CARRIER_DWORDS; i++)
      carrier_words[i] = R300_R2VB_PRODUCER_POISON_DWORD;
   radeon_drm_vk_bo_cache_sync(&device->drm, carrier->map,
                               R3V_GPU_PRODUCER_CARRIER_DWORDS * 4);
   if (owns_carrier_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &carrier->bo, carrier->map);
      carrier->map = NULL;
   }
   memcpy(draw->gpu_expected_carrier, expected, sizeof(expected));
   for (uint32_t i = R300_TRIANGLE_VERTEX_DWORDS;
        i < R3V_GPU_PRODUCER_CARRIER_DWORDS; i++)
      draw->gpu_expected_carrier[i] = R300_R2VB_PRODUCER_POISON_DWORD;
   return VK_SUCCESS;
}

VkResult
r3v_native_deferred_draw_admit_gpu_producer(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer)
{
   struct r3v_native_deferred_draw *draw = &cmd_buffer->deferred_draw;
   if (!draw->pending || draw->stream_mask == 0)
      return VK_SUCCESS;

   /* The routes deliver the slot-0 position stream, so the route
    * resolves on its format and the admissions below refuse a job
    * reading any other slot by name; a job that leaves slot 0 unread
    * resolves through the INVALID format to the CPU route. */
   struct r300_delivery_route_decision route;
   r300_delivery_route_resolve(device->r2vb_delivery_gate,
                               device->r2vb_gpu_delivery_gate,
                               device->r2vb_fetched_gate,
                               (draw->stream_mask & 1u)
                                  ? draw->streams[0].format_id
                                  : R300_VERTEX_FORMAT_INVALID,
                               &route);
   if (route.route != R300_DELIVERY_ROUTE_R2VB_GPU_PRODUCER_FETCHED &&
       route.route != R300_DELIVERY_ROUTE_R2VB_GPU_PRODUCER)
      return VK_SUCCESS;
   /* Both producer routes fetch the source records as one linear range
    * (embedded immediates or the VBPNTR array); an indexed draw
    * dereferences its indices on the host, so it refuses by name rather
    * than taking a route whose source order the indices do not drive. */
   if (draw->indexed) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: GPU producer routes fetch one linear "
                       "source range; an indexed draw executes on the CPU "
                       "route");
   }
   /* The producers deliver one instance's three records; the host
    * expansion of further instances, and the per-instance streams and
    * InstanceIndex it feeds, belong to the CPU route. */
   /* The host applies culling by collapsing records after its own
    * transform, and the producer routes deliver records the device
    * transformed, so a culling pipeline executes on the CPU route.
    */
   if (draw->cull_mode != VK_CULL_MODE_NONE || draw->sample_mask_zero) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: GPU producer routes deliver "
                       "untouched records; a culling or zero-coverage "
                       "pipeline executes on the CPU route");
   }
   /* The producer cells embed exactly the reference three records, so
    * a multi-triangle list refuses the producer routes by name.
    */
   if (draw->vertex_count != 3) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: GPU producer routes deliver the "
                       "three-record cell; a multi-triangle list executes "
                       "on the CPU route");
   }
   if (draw->instance_count != 1) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: GPU producer routes fetch one linear "
                       "source range for one instance; an instanced draw "
                       "executes on the CPU route");
   }
   if (route.route == R300_DELIVERY_ROUTE_R2VB_GPU_PRODUCER_FETCHED)
      return admit_fetched_producer(device, cmd_buffer);

   if (device->gpu_producer_quarantined) {
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: GPU producer capability is "
                       "quarantined on this device; a completed delivery "
                       "diverged from the CPU oracle");
   }

   /* Structural predicate: the identity vertex job over the F32_4
    * position stream on the recorded triangle consumer.  The route
    * resolver already bound the format; the job identity and the
    * consumer kind are the remaining shape facts.
    */
   if (!draw->vertex_job_identity || draw->stream_mask != 1u ||
       (cmd_buffer->cell_kind != R3V_NATIVE_CELL_KIND_TRIANGLE &&
        cmd_buffer->cell_kind !=
           R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC)) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: GPU producer route admits the "
                       "identity vertex job over one source stream on the "
                       "recorded triangle consumer alone");
   }

   /* The oracle read: the same execution-time gather the CPU route
    * runs, retained as the read-back expectation.  Under the identity
    * job and F32_4 these dwords are the records the producer embeds.
    */
   const struct r3v_native_deferred_stream *position = &draw->streams[0];
   struct r3v_native_buffer *buffer = position->buffer;
   struct r3v_native_memory *memory = buffer->memory;
   bool owns_map = memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &memory->bo, &memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: bound vertex memory is not "
                       "CPU-mappable at submission");
   }
   uint32_t oracle[R300_TRIANGLE_VERTEX_DWORDS];
   const struct r300_vertex_stream source = {
      .data = (const uint8_t *)memory->map + buffer->offset +
              position->stream_base,
      .stride = position->stride,
      .size_bytes = buffer->vk.size - position->stream_base,
   };
   int gathered = r300_cpu_vertex_gather(
      position->format_id, &source, draw->first_vertex,
      R300_TRIANGLE_VERTEX_DWORDS / 4, oracle, R300_TRIANGLE_VERTEX_DWORDS);
   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &memory->bo, memory->map);
      memory->map = NULL;
   }
   if (gathered != 0) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: vertex gather refused (%d) under the "
                       "GPU producer route", gathered);
   }

   /* Numeric predicate: the emitter refuses a record outside the FP24
    * fixed-point domain with -EDOM, so an admitted emission is one the
    * delivery identity predicts byte-exact.
    */
   float records[R300_R2VB_PRODUCER_REFERENCE_COUNT][4];
   memcpy(records, oracle, sizeof(records));
   struct r300_r2vb_producer_ib producer;
   int emit_result = r300_r2vb_producer_records_emit(
      (const float(*)[4])records, &producer);
   if (emit_result != 0) {
      return vk_errorf(device,
                       r3v_native_cell_vk_result_from_errno(emit_result),
                       "r3v-native: GPU producer emission refused (%d)%s",
                       emit_result,
                       emit_result == -EDOM
                          ? "; a record is outside the FP24 "
                            "fixed-point domain"
                          : "");
   }

   /* Transport predicate: the composed pass carries the
    * silicon-qualified reference contract outside the record payloads,
    * proven against a fresh reference emission rather than assumed
    * from sharing the emitter.
    */
   struct r300_r2vb_producer_ib reference;
   emit_result = r300_r2vb_producer_reference_emit(&reference);
   if (emit_result == 0) {
      emit_result = r300_r2vb_producer_pass_semantic_equal(&producer,
                                                           &reference);
      if (emit_result == 0)
         emit_result =
            r300_r2vb_producer_pass_validate_reloc_sites(&producer);
      r300_r2vb_producer_pass_release(&reference);
   }
   if (emit_result != 0) {
      r300_r2vb_producer_pass_release(&producer);
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: composed producer diverged from the "
                       "qualified transport contract (%d)", emit_result);
   }

   /* Composition: producer ++ consumer in one IB.  The producer's
    * carrier relocation payload and the consumer's vertex slot payload
    * both name relocation entry zero, and the consumer's reference list
    * already binds the carrier there, so the list only gains the write
    * domain the color backend needs.
    */
   if (!draw->gpu_producer_delivery) {
      const uint32_t combined_dwords =
         producer.ib_size_dwords + cmd_buffer->ib_size_dwords;
      uint32_t *combined = malloc(combined_dwords * sizeof(uint32_t));
      if (combined == NULL) {
         r300_r2vb_producer_pass_release(&producer);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      memcpy(combined, producer.ib,
             producer.ib_size_dwords * sizeof(uint32_t));
      memcpy(combined + producer.ib_size_dwords, cmd_buffer->ib,
             cmd_buffer->ib_size_dwords * sizeof(uint32_t));
      free(cmd_buffer->ib);
      cmd_buffer->ib = combined;
      cmd_buffer->ib_size_dwords = combined_dwords;
      draw->gpu_producer_dwords = producer.ib_size_dwords;
      cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC;
      cmd_buffer->references[R300_TRIANGLE_SLOT_VERTEX].write_domain =
         RADEON_GEM_DOMAIN_GTT;
   } else {
      /* A resubmission re-reads the stream, so the producer prefix is
       * re-emitted in place; the reference-shaped emission is
       * fixed-length, so a size drift names a broken invariant.
       */
      if (producer.ib_size_dwords != draw->gpu_producer_dwords) {
         r300_r2vb_producer_pass_release(&producer);
         return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                          "r3v-native: recomposed producer size %u "
                          "differs from the admitted %u",
                          producer.ib_size_dwords,
                          draw->gpu_producer_dwords);
      }
      memcpy(cmd_buffer->ib, producer.ib,
             producer.ib_size_dwords * sizeof(uint32_t));
   }
   r300_r2vb_producer_pass_release(&producer);
   /* The flag names one fact: the recorded IB carries the producer prefix
    * ahead of the consumer.  The composition above establishes it, so it is
    * recorded here rather than at the successful tail; a failure in the
    * carrier steps that follow leaves a composed IB, and a resubmission of
    * this buffer re-emits the prefix in place instead of prepending a
    * second one.
    */
   draw->gpu_producer_delivery = true;

   /* The poisoned carrier crosses to the device now, so the read-back
    * decides every slot: a record dword still holding the poison names
    * a slot the pass left unwritten, and the pad slot must keep it.
    */
   struct r3v_native_memory *carrier = cmd_buffer->owned_carrier;
   bool owns_carrier_map = carrier->map == NULL;
   if (owns_carrier_map &&
       radeon_drm_vk_bo_map(&device->drm, &carrier->bo,
                            &carrier->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: carrier memory is not CPU-mappable "
                       "at admission");
   }
   uint32_t *carrier_words = carrier->map;
   for (uint32_t i = 0; i < R3V_GPU_PRODUCER_CARRIER_DWORDS; i++)
      carrier_words[i] = R300_R2VB_PRODUCER_POISON_DWORD;
   radeon_drm_vk_bo_cache_sync(&device->drm, carrier->map,
                               R3V_GPU_PRODUCER_CARRIER_DWORDS * 4);
   if (owns_carrier_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &carrier->bo, carrier->map);
      carrier->map = NULL;
   }

   memcpy(draw->gpu_expected_carrier, oracle, sizeof(oracle));
   for (uint32_t i = R300_TRIANGLE_VERTEX_DWORDS;
        i < R3V_GPU_PRODUCER_CARRIER_DWORDS; i++)
      draw->gpu_expected_carrier[i] = R300_R2VB_PRODUCER_POISON_DWORD;
   return VK_SUCCESS;
}

/* Writes one carrier artifact into the evidence directory.  Retention
 * is best-effort: the read-back verdict is already decided, and a full
 * or read-only evidence directory changes the run's outcome through the
 * runner's own retention check rather than here.
 */
static void
retain_carrier_bytes(const char *manifest_dir, const char *name,
                     const void *bytes, size_t size)
{
   char path[1024];
   int length = snprintf(path, sizeof(path), "%s/%s", manifest_dir, name);
   if (length <= 0 || (size_t)length >= sizeof(path))
      return;
   FILE *file = fopen(path, "wb");
   if (file == NULL)
      return;
   fwrite(bytes, 1, size, file);
   fclose(file);
}

VkResult
r3v_native_deferred_draw_verify_gpu_producer(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer)
{
   struct r3v_native_deferred_draw *draw = &cmd_buffer->deferred_draw;
   if (!draw->gpu_producer_delivery)
      return VK_SUCCESS;

   /* Admission records gpu_producer_delivery where the composition
    * establishes it, ahead of the oracle capture, so the flag alone does
    * not promise a populated expectation.  The submit path returns on an
    * admission error and never reaches here, which is the invariant this
    * read-back depends on: a verify after a failed admit would compare
    * against a previous execution's oracle.
    */
   assert(draw->gpu_producer_dwords != 0);

   struct r3v_native_memory *carrier = cmd_buffer->owned_carrier;
   bool owns_map = carrier->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &carrier->bo,
                            &carrier->map) != 0) {
      device->gpu_producer_quarantined = true;
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: carrier read-back mapping failed; "
                       "GPU producer capability quarantined");
   }
   /* The carrier is the one device output this verdict reads, and it
    * reaches this point with no live mapping: admission unmaps it after
    * publishing the poison, so the post-completion invalidate over the
    * command buffer's live mappings
    * (rg --fixed-strings "Device writes landed in memory past the cache"
    * src/amd/r300/vulkan/r3v_native_queue.c) passes it by.  The RS480 GART
    * runs with request snooping disabled and every GTT mapping is
    * ttm_cached, so the poison written at admission still covers these
    * lines and a read through a fresh mapping returns it.  Invalidating the
    * read extent applies the rule r3v_MapMemory states for the public path
    * to the one internal host read of device output; the lines are clean
    * since admission published them, so the flush carries nothing back.
    * The BO-aware form names the carrier handle in the host-model event
    * record, so the harness witnesses this invalidate by handle.
    */
   radeon_drm_vk_bo_cache_sync_for_bo(&device->drm, &carrier->bo,
                                      carrier->map,
                                      sizeof(draw->gpu_expected_carrier));
   const bool matches =
      memcmp(carrier->map, draw->gpu_expected_carrier,
             sizeof(draw->gpu_expected_carrier)) == 0;
   /* The read-back bytes are the delivery's whole result, so a match
    * retains them beside the expectation exactly as a divergence does:
    * an attended run whose evidence holds only its failures leaves a
    * success unauditable.
    */
   if (device->manifest_dir != NULL) {
      retain_carrier_bytes(device->manifest_dir, "gpu_carrier_observed.bin",
                           carrier->map,
                           sizeof(draw->gpu_expected_carrier));
      retain_carrier_bytes(device->manifest_dir, "gpu_carrier_expected.bin",
                           draw->gpu_expected_carrier,
                           sizeof(draw->gpu_expected_carrier));
   }
   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &carrier->bo, carrier->map);
      carrier->map = NULL;
   }
   if (!matches) {
      device->gpu_producer_quarantined = true;
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: GPU producer carrier diverged from "
                       "the CPU oracle; capability quarantined and the "
                       "observed bytes retained");
   }
   return VK_SUCCESS;
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
   const struct r300_vertex_stream source = {
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

/* Fills a live mapping of memory with a repeated 16-bit sentinel and
 * publishes it for the unsnooped GART.  The allocation is whole 16-bit
 * units by the recorder's size contract, so no partial unit trails the
 * fill.
 */
static VkResult
zb_depth_control_fill_u16(struct r3v_native_device *device,
                          struct r3v_native_memory *memory, uint16_t value,
                          const char *role)
{
   bool owns_map = memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &memory->bo, &memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: depth control %s memory is not "
                       "CPU-mappable", role);
   }
   uint16_t *units = memory->map;
   for (uint64_t i = 0; i < memory->bo.size / 2; i++)
      units[i] = value;
   radeon_drm_vk_bo_cache_sync(&device->drm, memory->map, memory->bo.size);
   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &memory->bo, memory->map);
      memory->map = NULL;
   }
   return VK_SUCCESS;
}

#define R3V_ZB_DEPTH_CONTROL_VERTEX_BYTES \
   (R300_ZB_DEPTH_CONTROL_VERTEX_DWORDS * sizeof(uint32_t))

/* Records the depth control cell: writes the six-vertex payload,
 * sentinel-fills the color target and the Z16 depth surface, publishes
 * each for the unsnooped GART, and installs the reference IB with the
 * three BO references in relocation-slot order.  Each memory is exactly
 * the cell's declared footprint, so the geometry the arming gate freezes
 * is the shape this recorder admitted.  Recording is submit-free; the
 * queue's hazard gate guards execution.
 */
VkResult
r3v_native_record_zb_depth_control(VkCommandBuffer commandBuffer,
                                   VkDeviceMemory vertexMemory,
                                   VkDeviceMemory colorMemory,
                                   VkDeviceMemory depthMemory)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, vertex_memory, vertexMemory);
   VK_FROM_HANDLE(r3v_native_memory, color_memory, colorMemory);
   VK_FROM_HANDLE(r3v_native_memory, depth_memory, depthMemory);

   if (cmd_buffer == NULL || vertex_memory == NULL || color_memory == NULL ||
       depth_memory == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);

   /* Exact declared footprints: the arming gate's frozen-geometry fact
    * compares these sizes, so a larger allocation would record a cell
    * the gate then refuses, and the recorder names that here instead.
    */
   if (vertex_memory->bo.size != R3V_ZB_DEPTH_CONTROL_VERTEX_ALLOCATION ||
       color_memory->bo.size != R300_ZB_DEPTH_CONTROL_COLOR_BYTES ||
       depth_memory->bo.size != R300_ZB_DEPTH_CONTROL_DEPTH_BYTES) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: depth control needs exactly %u vertex, "
                       "%u color, and %u depth bytes",
                       R3V_ZB_DEPTH_CONTROL_VERTEX_ALLOCATION,
                       R300_ZB_DEPTH_CONTROL_COLOR_BYTES,
                       R300_ZB_DEPTH_CONTROL_DEPTH_BYTES);
   }

   /* Three distinct GEM objects: radeon_drm_vk_reloc_list_add folds
    * duplicate handles into one relocation slot, which would leave a
    * slot payload outside the relocation chunk.
    */
   if (vertex_memory->bo.handle == color_memory->bo.handle ||
       vertex_memory->bo.handle == depth_memory->bo.handle ||
       color_memory->bo.handle == depth_memory->bo.handle) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: depth control roles require three "
                       "distinct GEM objects");
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
                       "r3v-native: depth control vertex memory is not "
                       "CPU-mappable");
   }
   memcpy(vertex_memory->map, r300_zb_depth_control_vertices,
          R3V_ZB_DEPTH_CONTROL_VERTEX_BYTES);
   radeon_drm_vk_bo_cache_sync(&device->drm, vertex_memory->map,
                               R3V_ZB_DEPTH_CONTROL_VERTEX_BYTES);
   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &vertex_memory->bo,
                             vertex_memory->map);
      vertex_memory->map = NULL;
   }

   /* Both surfaces are sentinel-filled whole and published, so each
    * oracle reads a deterministic pre-draw state and any device write is
    * detectable.  The color sentinel repeats per 16-bit half, so one
    * fill routine covers both fills without a second publication path.
    */
   VkResult fill_result = zb_depth_control_fill_u16(
      device, color_memory, (uint16_t)(R300_TRIANGLE_COLOR_SENTINEL & 0xffff),
      "color");
   if (fill_result != VK_SUCCESS)
      return fill_result;
   fill_result = zb_depth_control_fill_u16(
      device, depth_memory, R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL, "depth");
   if (fill_result != VK_SUCCESS)
      return fill_result;

   struct r300_zb_depth_control_ib cell;
   int emit_result = r300_zb_depth_control_reference_emit(&cell);
   if (emit_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(emit_result));
   if (r300_zb_depth_control_validate_reloc_sites(&cell) != 0) {
      r300_zb_depth_control_release(&cell);
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   struct r3v_native_bo_reference *references =
      calloc(R300_ZB_DEPTH_CONTROL_SLOT_COUNT, sizeof(*references));
   if (references == NULL) {
      r300_zb_depth_control_release(&cell);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   references[R300_ZB_DEPTH_CONTROL_SLOT_VERTEX] =
      (struct r3v_native_bo_reference){
         .handle = vertex_memory->bo.handle,
         .read_domains = RADEON_GEM_DOMAIN_GTT,
         .write_domain = 0,
         .memory = vertex_memory,
      };
   references[R300_ZB_DEPTH_CONTROL_SLOT_COLOR] =
      (struct r3v_native_bo_reference){
         .handle = color_memory->bo.handle,
         .read_domains = 0,
         .write_domain = RADEON_GEM_DOMAIN_GTT,
         .memory = color_memory,
      };
   /* The depth surface crosses both directions: the host fill is the
    * comparison's stored operand and the device writes passing
    * fragments, so the relocation carries the GTT domain on both sides.
    */
   references[R300_ZB_DEPTH_CONTROL_SLOT_DEPTH] =
      (struct r3v_native_bo_reference){
         .handle = depth_memory->bo.handle,
         .read_domains = RADEON_GEM_DOMAIN_GTT,
         .write_domain = RADEON_GEM_DOMAIN_GTT,
         .memory = depth_memory,
      };

   r3v_native_cmd_buffer_install_ib(cmd_buffer,
                                    R3V_NATIVE_CELL_KIND_ZB_DEPTH_CONTROL,
                                    cell.ib, cell.ib_size_dwords, references,
                                    R300_ZB_DEPTH_CONTROL_SLOT_COUNT);
   /* install_ib took ownership of cell.ib; only the descriptor resets. */
   cell.ib = NULL;
   r300_zb_depth_control_release(&cell);

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
   int emit_result = r300_r2vb_reingest_pass_emit(&cell);
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

/* The FLOAT_4 model variant widens the model array to full records. */
#define R3V_NATIVE_FLOAT4_MODEL_VERTEX_BYTES               \
   (R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT *               \
    (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES +            \
     R300_R2VB_FLOAT4_MODEL_STRIDE_BYTES))

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

int
r3v_native_burst_carrier_bytes(uint32_t draws, uint32_t *out)
{
   if (draws < 1 || draws > R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS)
      return -EINVAL;
   uint32_t stride;
   int rc = r300_r2vb_float2_tuple_burst_member_stride_bytes(&stride);
   if (rc != 0)
      return rc;
   *out = draws * stride;
   return 0;
}

/* Records the burst status-load cell: draws members of the tuple stream
 * in one IB, each member retargeted to its own carrier row.  The
 * carrier poison covers every member row plus the padding pixels, so
 * each member's delivery and the inter-member containment both stay
 * decidable; the vertex stream is the single shared fetch source all
 * members read.
 */
static VkResult
record_status_load_burst_width(VkCommandBuffer commandBuffer,
                               VkDeviceMemory carrierMemory,
                               VkDeviceMemory vertexMemory, uint32_t draws,
                               bool model_float4)
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
   if (r3v_native_burst_carrier_bytes(draws, &carrier_bytes) != 0) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: burst cell takes 1..%u members",
                       R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS);
   }
   if (carrier_memory->bo.size != carrier_bytes) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: %u-member burst takes a %u-byte "
                       "carrier",
                       draws, carrier_bytes);
   }
   const uint32_t vertex_bytes =
      model_float4 ? R3V_NATIVE_FLOAT4_MODEL_VERTEX_BYTES
                   : R3V_NATIVE_FLOAT2_TUPLE_VERTEX_BYTES;
   if (vertex_memory->bo.size != vertex_bytes) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: burst cell takes a %u-byte vertex "
                       "stream",
                       vertex_bytes);
   }

   bool owns_map = carrier_memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &carrier_memory->bo,
                            &carrier_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: burst carrier memory is not "
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
                       "r3v-native: burst vertex memory is not "
                       "CPU-mappable");
   }
   int stream_result =
      model_float4
         ? r300_r2vb_float4_model_vertex_stream(
              r300_r2vb_float2_tuple_reference_records,
              R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, vertex_memory->map,
              vertex_bytes)
         : r300_r2vb_float2_tuple_vertex_stream(
              r300_r2vb_float2_tuple_reference_records,
              R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, vertex_memory->map,
              vertex_bytes);
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

   struct r300_r2vb_float2_tuple_burst_ib cell;
   int emit_result =
      model_float4
         ? r300_r2vb_float4_model_burst_reference_emit(draws, &cell)
         : r300_r2vb_float2_tuple_burst_reference_emit(draws, &cell);
   if (emit_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(emit_result));
   emit_result = r300_r2vb_float2_tuple_burst_validate_reloc_sites(&cell);
   if (emit_result != 0) {
      r300_r2vb_float2_tuple_burst_release(&cell);
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(emit_result));
   }

   struct r3v_native_bo_reference *references =
      calloc(R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT, sizeof(*references));
   if (references == NULL) {
      r300_r2vb_float2_tuple_burst_release(&cell);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
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

   r3v_native_cmd_buffer_install_ib(
      cmd_buffer, R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_BURST, cell.ib,
      cell.ib_size_dwords, references, R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT);
   cmd_buffer->burst_draws = draws;
   cmd_buffer->burst_model_float4 = model_float4;
   /* install_ib took ownership of cell.ib; only the descriptor resets. */
   cell.ib = NULL;
   cell.owns_ib = false;
   r300_r2vb_float2_tuple_burst_release(&cell);
   return VK_SUCCESS;
}

VkResult
r3v_native_record_r2vb_status_load_burst(VkCommandBuffer commandBuffer,
                                         VkDeviceMemory carrierMemory,
                                         VkDeviceMemory vertexMemory,
                                         uint32_t draws)
{
   return record_status_load_burst_width(commandBuffer, carrierMemory,
                                         vertexMemory, draws, false);
}

/* The fetch-width contrast workload: the burst status-load cell over
 * the FLOAT_4 model emission -- the model records stored and fetched
 * at full width under the identity swizzle, VAP_VTX_SIZE 8 -- with the
 * tuple's carrier expectation unchanged.  The arming digest binds the
 * widened stream to its own declared evidence.
 */
VkResult
r3v_native_record_r2vb_status_load_burst_float4_model(
   VkCommandBuffer commandBuffer, VkDeviceMemory carrierMemory,
   VkDeviceMemory vertexMemory, uint32_t draws)
{
   return record_status_load_burst_width(commandBuffer, carrierMemory,
                                         vertexMemory, draws, true);
}
