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
#include "amd/r300/vulkan/r3v_delivery_route.h"
#include "amd/r300/common/r300_r2vb_carrier_delivery.h"
#include "amd/r300/common/r300_r2vb_fetched_producer.h"
#include "amd/r300/common/r300_r2vb_float2_tuple_pass.h"
#include "amd/r300/common/r300_r2vb_producer_pass.h"
#include "amd/r300/common/r300_r2vb_reingest_pass.h"
#include "amd/r300/common/r300_flat_color0_plan.h"
#include "amd/r300/common/r300_rs_tex_adj_probe.h"
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
#include <float.h>
#include <inttypes.h>
#include <math.h>
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
fill_color(struct r3v_native_device *device,
           struct r3v_native_memory *color_memory, uint64_t fill_offset,
           uint64_t fill_bytes, uint32_t dword);

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
      fill_color(device, color_memory, 0, color_memory->bo.size,
                 R300_TRIANGLE_COLOR_SENTINEL);
   if (result != VK_SUCCESS)
      return result;

   struct r300_tcl_bypass_triangle_ib cell;
   int emit_result = r300_tcl_bypass_triangle_render_shape_emit(shape, &cell);
   if (emit_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(emit_result));
   struct r3v_native_bo_reference *references =
      calloc(R300_TRIANGLE_RENDER_SLOT_COUNT, sizeof(*references));
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
                                    R300_TRIANGLE_RENDER_SLOT_COUNT);
   cell.ib = NULL;
   r300_tcl_bypass_triangle_release(&cell);
   return VK_SUCCESS;
}

/* Writes dword over fill_bytes of the color memory from fill_offset and
 * publishes them for the unsnooped GART.  The offset is the image's
 * bind offset, where render row 0 starts, and fill_bytes bounds the
 * write to the caller's declared footprint; content outside that window
 * in the same allocation stays untouched.  The load-op clear passes the
 * pass's own packed color; the record-time delivery routes pass
 * R300_TRIANGLE_COLOR_SENTINEL, the pre-draw state their output oracle
 * reads, which differs from every draw color in each byte lane so any
 * device write inside the range is detectable.
 */
static VkResult
fill_color(struct r3v_native_device *device,
           struct r3v_native_memory *color_memory, uint64_t fill_offset,
           uint64_t fill_bytes, uint32_t dword)
{
   bool owns_color_map = color_memory->map == NULL;
   if (owns_color_map &&
       radeon_drm_vk_bo_map(&device->drm, &color_memory->bo,
                            &color_memory->map) != 0) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r3v-native: triangle color memory is not "
                       "CPU-mappable");
   }
   uint32_t *color_pixels =
      (uint32_t *)((uint8_t *)color_memory->map + fill_offset);
   for (uint64_t i = 0; i < fill_bytes / 4; i++)
      color_pixels[i] = dword;
   radeon_drm_vk_bo_cache_sync(&device->drm, color_pixels, fill_bytes);
   if (owns_color_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &color_memory->bo,
                             color_memory->map);
      color_memory->map = NULL;
   }
   return VK_SUCCESS;
}

static int
emit_triangle_cell_for_position_space(
   const struct r300_triangle_render_shape *shape, bool varying,
   bool flat_color0, uint8_t rs_probe_candidate,
   uint32_t source_triangle_count,
   const struct r3v_native_sampled_texture *sampled, bool clip_space,
   struct r300_tcl_bypass_triangle_ib *cell)
{
   if (sampled == NULL && varying && !flat_color0 &&
       rs_probe_candidate != R3V_RS_PROBE_NONE) {
      /* The rasterizer probe candidate: the control varying cell's
       * bytes with the candidate's one word, so the pass records the
       * stream the census classifies. */
      struct r300_rs_tex_adj_probe_plan plan;
      if (rs_probe_candidate == R3V_RS_PROBE_W_SELECT_ONE)
         r300_rs_tex_adj_probe_plan_w_select_one(&plan);
      else
         r300_rs_tex_adj_probe_plan_tex_adj(&plan);
      return r300_tcl_bypass_triangle_rs_tex_adj_family_emit(
         shape->width, shape->height, clip_space, source_triangle_count,
         &plan, cell);
   }
   if (sampled == NULL && varying && flat_color0) {
      /* The direct GA Flat route: the canonical plan alone, carried
       * by each draw's own contract prefix. */
      struct r300_flat_color0_plan plan;
      r300_flat_color0_plan_direct_first(&plan);
      return r300_tcl_bypass_triangle_flat_color0_family_emit(
         shape->width, shape->height, clip_space, source_triangle_count,
         &plan, cell);
   }
   if (sampled != NULL) {
      if (clip_space) {
         return r300_tcl_bypass_triangle_clip_space_sampled_emit(
            shape->width, shape->height, source_triangle_count,
            sampled->texture_offset, sampled->texture_width,
            sampled->texture_height, sampled->texture_pitch_texels,
            sampled->texture_lanes, cell);
      }
      return r300_tcl_bypass_triangle_sampled_emit(
         shape->width, shape->height, source_triangle_count,
         sampled->texture_offset, sampled->texture_width,
         sampled->texture_height, sampled->texture_pitch_texels,
         sampled->texture_lanes, cell);
   }

   if (clip_space) {
      if (varying) {
         return r300_tcl_bypass_triangle_clip_space_family_emit(
            shape->width, shape->height, true, source_triangle_count, cell);
      }
      return r300_tcl_bypass_triangle_clip_space_render_shape_emit(
         shape, source_triangle_count, cell);
   }

   if (varying || source_triangle_count != 1) {
      return r300_tcl_bypass_triangle_family_emit(
         shape->width, shape->height, varying, source_triangle_count, cell);
   }
   return r300_tcl_bypass_triangle_render_shape_emit(shape, cell);
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
                               const struct r300_triangle_render_shape *shape,
                               bool clip_space, bool varying, bool flat_color0,
                               uint8_t rs_probe_candidate,
                               uint32_t triangle_count,
                               const struct r3v_native_sampled_texture
                                  *sampled)
{
   VkResult role_result = validate_triangle_memory_roles(
      device, vertex_memory, color_memory);
   if (role_result != VK_SUCCESS)
      return role_result;

   /* The recorded cell is self-contained: the emission opens with the
    * first-draw contract prefix resolved at the target extent, so the
    * result does not ride whatever state the previous client left in
    * the pipeline; at the reference shape the construction is the
    * byte-identical reference cell backing the arming digest and the
    * manifest.
    *
    * Two emitters cover the recorded draws.  Every constant-color
    * single-triangle draw takes the render-shape emitter, which places
    * extent, pitch, lane order, target offset, and the fragment
    * constant the admitted module wrote, each through its one register
    * class, so the constant that executes is the pipeline's own at
    * every target including the reference one.  A varying record shape
    * and a host-expanded instance count have their emitter in the
    * parameterized cell family alone, which carries the reference
    * pitch, lane order, and base offset, so those two shapes execute
    * at the reference target and refuse elsewhere.
    */
   struct r300_triangle_render_shape reference;
   r300_tcl_bypass_triangle_render_shape_reference(&reference);
   const bool reference_target =
      shape->pitch_pixels == reference.pitch_pixels &&
      shape->lanes == reference.lanes && shape->target_offset == 0 &&
      shape->width <= reference.width && shape->height <= reference.height;
   /* Varying and sampled cells retain the reference target family.  The
    * constant-color render-shape emitter carries arbitrary admitted target
    * geometry directly.
    */
   if ((sampled != NULL || varying || triangle_count != 1) &&
       !reference_target)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   if (sampled != NULL && !varying)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   /* CPU and host-model delivery need a fixed seven-triangle capacity per
    * source triangle because the stream bytes arrive after recording.  The
    * GPU producer instead owns an already transformed three-record stream;
    * retain its ordinary consumer for the one source shape that route admits.
    */
   struct r300_tcl_bypass_triangle_ib window_cell = {0};
   const bool retain_window_cell =
      clip_space && cmd_buffer->ib == NULL && sampled == NULL && !varying &&
      triangle_count == 1;
   int emit_result = 0;
   if (retain_window_cell) {
      emit_result = emit_triangle_cell_for_position_space(
         shape, varying, flat_color0, rs_probe_candidate, triangle_count,
         sampled, false, &window_cell);
      if (emit_result != 0)
         return vk_error(device,
                         r3v_native_cell_vk_result_from_errno(emit_result));
   }

   struct r300_tcl_bypass_triangle_ib cell = {0};
   emit_result = emit_triangle_cell_for_position_space(
      shape, varying, flat_color0, rs_probe_candidate, triangle_count,
      sampled, clip_space, &cell);
   if (emit_result != 0)
      r300_tcl_bypass_triangle_release(&window_cell);
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
   const uint32_t slot_count = sampled != NULL
                                  ? R300_TRIANGLE_SAMPLED_SLOT_COUNT
                                  : R300_TRIANGLE_RENDER_SLOT_COUNT;
   struct r3v_native_bo_reference *references =
      calloc(slot_count, sizeof(*references));
   if (references == NULL) {
      r300_tcl_bypass_triangle_release(&cell);
      r300_tcl_bypass_triangle_release(&window_cell);
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
   if (sampled != NULL) {
      references[R300_TRIANGLE_SLOT_TEXTURE] =
         (struct r3v_native_bo_reference){
            .handle = sampled->memory->bo.handle,
            .read_domains = RADEON_GEM_DOMAIN_GTT,
            .write_domain = 0,
            .memory = sampled->memory,
         };
   }

   /* A second recorded pass appends its cell to the installed stream:
    * each half opens with its own first-draw contract, so the
    * concatenation carries no state across the boundary, and every cell
    * closes with the destination-cache flush that publishes its writes
    * before the next one runs -- the coherency edge the composed cell
    * holds on silicon.  A first pass installs.
    */
   if (cmd_buffer->ib != NULL && cmd_buffer->deferred_draw_count > 1) {
      const VkResult appended = r3v_native_cmd_buffer_append_ib(
         device, cmd_buffer, &cell, references, slot_count);
      free(references);
      if (appended != VK_SUCCESS) {
         r300_tcl_bypass_triangle_release(&cell);
         r300_tcl_bypass_triangle_release(&window_cell);
         return appended;
      }
      /* append_ib copied the appended dwords; the descriptor still owns
       * its allocation.
       */
      r300_tcl_bypass_triangle_release(&cell);
      r300_tcl_bypass_triangle_release(&window_cell);
      return VK_SUCCESS;
   }

   r3v_native_cmd_buffer_install_ib(cmd_buffer,
                                    sampled != NULL
                                       ? R3V_NATIVE_CELL_KIND_TRIANGLE_SAMPLED
                                       : R3V_NATIVE_CELL_KIND_TRIANGLE,
                                    cell.ib, cell.ib_size_dwords, references,
                                    slot_count);
   /* install_ib took ownership of cell.ib; only the descriptor resets. */
   cell.ib = NULL;
   r300_tcl_bypass_triangle_release(&cell);
   if (retain_window_cell) {
      cmd_buffer->window_space_ib = window_cell.ib;
      cmd_buffer->window_space_ib_size_dwords = window_cell.ib_size_dwords;
      window_cell.ib = NULL;
   }
   r300_tcl_bypass_triangle_release(&window_cell);

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
      fill_color(device, color_memory, 0, color_memory->bo.size,
                 R300_TRIANGLE_COLOR_SENTINEL);
   if (result != VK_SUCCESS)
      return result;
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   return emit_and_install_triangle_cell(device, cmd_buffer, vertex_memory,
                                         color_memory, &shape, false, false,
                                         false, 0, 1, NULL);
}

VkResult
r3v_native_record_tcl_bypass_triangle_carrier(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory,
   struct r3v_native_image *target_image, uint32_t target_layer_offset,
   bool varying, bool flat_color0, uint8_t rs_probe_candidate,
   uint32_t triangle_count, const uint32_t color_bits[4],
   const struct r3v_native_sampled_texture *sampled)
{
   struct r3v_native_memory *color_memory = target_image->memory;
   VkResult role_result = validate_triangle_memory_roles(
      device, carrier_memory, color_memory);
   if (role_result != VK_SUCCESS)
      return role_result;

   if (triangle_count < 1 || triangle_count > R300_TRIANGLE_MAX_TRIANGLES)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   const uint64_t carrier_bytes =
      (uint64_t)triangle_count *
      R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT *
      (varying ? R3V_TRIANGLE_VARYING_VERTEX_BYTES
               : R3V_TRIANGLE_VERTEX_BYTES);
   const uint64_t target_base =
      target_image->memory_offset + target_layer_offset;
   if (carrier_memory->bo.size < carrier_bytes ||
       color_memory->bo.size - target_base < target_image->layer_pitch_bytes) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: triangle cell needs %" PRIu64
                       " clipping-capacity vertex bytes and the target "
                       "image's declared "
                       "footprint",
                       carrier_bytes);
   }

   /* The target image and the bound pipeline carry the shape between
    * them: the image holds the extent, the row pitch, and the lane
    * order, and the pipeline holds the fragment constant.  A varying
    * program writes no constant, so its shape takes the reference
    * one the family emitter uses.
    */
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   shape.width = target_image->width;
   shape.height = target_image->height;
   shape.pitch_pixels = target_image->row_pitch_bytes / 4;
   shape.lanes = target_image->lanes;
   /* The bind offset plus the attached layer's stride is where render
    * row 0 starts, so it travels as the cell's RB3D_COLOROFFSET0
    * payload; r3v_BindImageMemory admitted the bind offset against the
    * register's base granularity, and each family's row pitch keeps the
    * layer stride a multiple of the same 32 bytes.
    */
   shape.target_offset = (uint32_t)target_base;
   if (!varying)
      memcpy(shape.color_bits, color_bits, sizeof(shape.color_bits));
   return emit_and_install_triangle_cell(device, cmd_buffer, carrier_memory,
                                         color_memory, &shape, true, varying,
                                         flat_color0, rs_probe_candidate,
                                         triangle_count, sampled);
}

/* Vulkan 1.0 Fixed-Function Vertex Post-Processing defines the view volume
 * as six homogeneous half-spaces and clips shader outputs with the same edge
 * parameter before perspective division.  Clipping a convex triangle against
 * six planes can add at most one vertex per plane: nine polygon vertices and
 * seven fan triangles.
 */
#define R3V_NATIVE_CLIP_PLANE_COUNT 6u
#define R3V_NATIVE_CLIP_MAX_POLYGON_VERTICES \
   (3u + R3V_NATIVE_CLIP_PLANE_COUNT)
#define R3V_NATIVE_CLIP_MAX_RECORD_DWORDS \
   (R300_VERTEX_JOB_POSITION_DWORDS + R300_VERTEX_JOB_VARYING_DWORDS)

struct r3v_native_clip_vertex {
   double values[R3V_NATIVE_CLIP_MAX_RECORD_DWORDS];
};

static double
clip_plane_distance(const struct r3v_native_clip_vertex *vertex,
                    uint32_t plane)
{
   const double *position = vertex->values;
   switch (plane) {
   case 0:
      return (double)position[0] + position[3];
   case 1:
      return (double)position[3] - position[0];
   case 2:
      return (double)position[1] + position[3];
   case 3:
      return (double)position[3] - position[1];
   case 4:
      return position[2];
   case 5:
      return (double)position[3] - position[2];
   default:
      assert(!"clip plane outside the fixed view volume");
      return -1.0;
   }
}

static int
append_clip_vertex(struct r3v_native_clip_vertex vertices[],
                   uint32_t *vertex_count,
                   const struct r3v_native_clip_vertex *vertex)
{
   if (*vertex_count >= R3V_NATIVE_CLIP_MAX_POLYGON_VERTICES)
      return -EOVERFLOW;
   vertices[(*vertex_count)++] = *vertex;
   return 0;
}

static int
intersect_clip_edge(const struct r3v_native_clip_vertex *previous,
                    const struct r3v_native_clip_vertex *current,
                    double previous_distance, double current_distance,
                    uint32_t plane, uint32_t record_dwords,
                    struct r3v_native_clip_vertex *intersection)
{
   const double previous_weight = fabs(current_distance);
   const double current_weight = fabs(previous_distance);
   const double weight_sum = previous_weight + current_weight;
   if (weight_sum == 0.0 || !isfinite(weight_sum))
      return -EDOM;

   memset(intersection, 0, sizeof(*intersection));
   for (uint32_t component = 0; component < record_dwords; component++) {
      intersection->values[component] =
         (previous->values[component] * previous_weight +
          current->values[component] * current_weight) /
         weight_sum;
   }

   /* Put the generated position exactly on the active plane.  The weighted
    * intersection stays in binary64 through all six planes, so later plane
    * intersections retain the containment established by earlier planes.
    */
   switch (plane) {
   case 0:
      intersection->values[0] = -intersection->values[3];
      break;
   case 1:
      intersection->values[0] = intersection->values[3];
      break;
   case 2:
      intersection->values[1] = -intersection->values[3];
      break;
   case 3:
      intersection->values[1] = intersection->values[3];
      break;
   case 4:
      intersection->values[2] = 0.0;
      break;
   case 5:
      intersection->values[2] = intersection->values[3];
      break;
   default:
      return -EINVAL;
   }
   return 0;
}

static bool
clip_vertex_inside_all_planes(const struct r3v_native_clip_vertex *vertex)
{
   for (uint32_t plane = 0; plane < R3V_NATIVE_CLIP_PLANE_COUNT; plane++) {
      if (!(clip_plane_distance(vertex, plane) >= 0.0))
         return false;
   }
   return true;
}

static int
clip_one_triangle(const uint32_t *records, uint32_t record_dwords,
                  struct r3v_native_clip_vertex clipped[],
                  uint32_t *clipped_count)
{
   struct r3v_native_clip_vertex current[R3V_NATIVE_CLIP_MAX_POLYGON_VERTICES] =
      {0};
   struct r3v_native_clip_vertex next[R3V_NATIVE_CLIP_MAX_POLYGON_VERTICES] =
      {0};
   uint32_t current_count = 3;
   for (uint32_t vertex = 0; vertex < current_count; vertex++) {
      for (uint32_t component = 0; component < record_dwords; component++) {
         float value;
         memcpy(&value, &records[vertex * record_dwords + component],
                sizeof(value));
         if (component < R300_VERTEX_JOB_POSITION_DWORDS && !isfinite(value))
            return -EDOM;
         current[vertex].values[component] = value;
      }
   }

   for (uint32_t plane = 0;
        plane < R3V_NATIVE_CLIP_PLANE_COUNT && current_count != 0; plane++) {
      uint32_t next_count = 0;
      const struct r3v_native_clip_vertex *previous =
         &current[current_count - 1];
      double previous_distance = clip_plane_distance(previous, plane);
      bool previous_inside = previous_distance >= 0.0;
      for (uint32_t vertex = 0; vertex < current_count; vertex++) {
         const struct r3v_native_clip_vertex *candidate = &current[vertex];
         const double candidate_distance =
            clip_plane_distance(candidate, plane);
         const bool candidate_inside = candidate_distance >= 0.0;
         if (previous_inside != candidate_inside) {
            struct r3v_native_clip_vertex intersection;
            int result = intersect_clip_edge(
               previous, candidate, previous_distance, candidate_distance,
               plane, record_dwords, &intersection);
            if (result != 0)
               return result;
            result = append_clip_vertex(next, &next_count, &intersection);
            if (result != 0)
               return result;
         }
         if (candidate_inside) {
            const int result =
               append_clip_vertex(next, &next_count, candidate);
            if (result != 0)
               return result;
         }
         previous = candidate;
         previous_distance = candidate_distance;
         previous_inside = candidate_inside;
      }
      memcpy(current, next, (size_t)next_count * sizeof(next[0]));
      current_count = next_count;
   }

   for (uint32_t vertex = 0; vertex < current_count; vertex++) {
      if (!clip_vertex_inside_all_planes(&current[vertex]))
         return -ERANGE;
   }

   memcpy(clipped, current, (size_t)current_count * sizeof(current[0]));
   *clipped_count = current_count;
   return 0;
}

static void
write_degenerate_record(float record[R3V_NATIVE_CLIP_MAX_RECORD_DWORDS],
                        uint32_t record_dwords)
{
   memset(record, 0, (size_t)record_dwords * sizeof(float));
   record[3] = 1.0f;
}

static bool
project_clip_vertex(const struct r3v_native_clip_vertex *vertex,
                    uint32_t record_dwords, uint32_t target_width,
                    uint32_t target_height, double reciprocal_scale,
                    float output[R3V_NATIVE_CLIP_MAX_RECORD_DWORDS])
{
   const double clip_w = vertex->values[3];
   if (clip_w <= 0.0 || reciprocal_scale <= 0.0)
      return false;
   const double normalized_x = vertex->values[0] / clip_w;
   const double normalized_y = vertex->values[1] / clip_w;
   const double normalized_z = vertex->values[2] / clip_w;
   const double window_x =
      (normalized_x + 1.0) * ((double)target_width / 2.0);
   const double window_y =
      (normalized_y + 1.0) * ((double)target_height / 2.0);
   if (!(normalized_x >= -1.0 && normalized_x <= 1.0) ||
       !(normalized_y >= -1.0 && normalized_y <= 1.0) ||
       !(normalized_z >= 0.0 && normalized_z <= 1.0) ||
       !isfinite(window_x) || !isfinite(window_y))
      return false;

   const double reciprocal_w = reciprocal_scale / clip_w;
   if (!(reciprocal_w > 0.0) || !isfinite(reciprocal_w))
      return false;

   memset(output, 0, (size_t)record_dwords * sizeof(float));
   output[0] = (float)window_x;
   output[1] = (float)window_y;
   output[2] = (float)normalized_z;
   /* R300's software-transformed vertex convention carries reciprocal clip
    * W in the pretransformed position for perspective-correct raster
    * interpolation.
    */
   output[3] = (float)MIN2(reciprocal_w, (double)FLT_MAX);
   if (output[3] == 0.0f)
      output[3] = FLT_TRUE_MIN;
   for (uint32_t component = R300_VERTEX_JOB_POSITION_DWORDS;
        component < record_dwords; component++)
      output[component] = (float)vertex->values[component];
   return true;
}

static int
expand_clip_space_triangles(
   const uint32_t *source_records, uint32_t source_triangle_count,
   uint32_t record_dwords, uint32_t target_width, uint32_t target_height,
   VkCullModeFlags cull_mode, VkFrontFace front_face, bool sample_mask_zero,
   uint32_t *output_records, uint32_t output_capacity_dwords)
{
   if ((record_dwords != R300_VERTEX_JOB_POSITION_DWORDS &&
        record_dwords != R3V_NATIVE_CLIP_MAX_RECORD_DWORDS) ||
       source_triangle_count == 0 || source_records == NULL ||
       output_records == NULL)
      return -EINVAL;

   const uint64_t output_vertex_count =
      (uint64_t)source_triangle_count *
      R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT * 3u;
   const uint64_t required_dwords = output_vertex_count * record_dwords;
   if (required_dwords > output_capacity_dwords)
      return -ENOSPC;

   const uint32_t vertices_per_source =
      R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT * 3u;
   for (uint32_t triangle = 0; triangle < source_triangle_count; triangle++) {
      uint32_t *triangle_output =
         &output_records[(uint64_t)triangle * vertices_per_source *
                         record_dwords];
      float degenerate[R3V_NATIVE_CLIP_MAX_RECORD_DWORDS];
      write_degenerate_record(degenerate, record_dwords);
      for (uint32_t vertex = 0; vertex < vertices_per_source; vertex++) {
         memcpy(&triangle_output[vertex * record_dwords], degenerate,
                (size_t)record_dwords * sizeof(float));
      }

      struct r3v_native_clip_vertex
         polygon[R3V_NATIVE_CLIP_MAX_POLYGON_VERTICES];
      uint32_t polygon_count = 0;
      const int clipped = clip_one_triangle(
         &source_records[(uint64_t)triangle * 3u * record_dwords],
         record_dwords, polygon, &polygon_count);
      if (clipped != 0)
         return clipped;
      if (polygon_count < 3)
         continue;

      double minimum_positive_w = DBL_MAX;
      double maximum_positive_w = 0.0;
      for (uint32_t vertex = 0; vertex < polygon_count; vertex++) {
         const double clip_w = polygon[vertex].values[3];
         if (clip_w > 0.0 && clip_w < minimum_positive_w)
            minimum_positive_w = clip_w;
         if (clip_w > maximum_positive_w)
            maximum_positive_w = clip_w;
      }
      if (minimum_positive_w == DBL_MAX)
         continue;

      /* Multiplying every reciprocal W in one source triangle by the same
       * positive scale leaves perspective interpolation unchanged.  Clamp
       * that scale to the interval whose smallest and largest reciprocals
       * fit binary32.  A generated clip vertex can lie closer to W = 0 than
       * FLT_TRUE_MIN even though every input was binary32; when that makes
       * the interval empty, keep the largest reciprocal finite and round
       * smaller positive weights up to FLT_TRUE_MIN instead of publishing
       * the singular value zero.
       */
      const double minimum_reciprocal_scale =
         (double)FLT_TRUE_MIN * maximum_positive_w;
      const double maximum_reciprocal_scale =
         (double)FLT_MAX * minimum_positive_w;
      double reciprocal_scale = 1.0;
      if (reciprocal_scale < minimum_reciprocal_scale)
         reciprocal_scale = minimum_reciprocal_scale;
      if (reciprocal_scale > maximum_reciprocal_scale)
         reciprocal_scale = maximum_reciprocal_scale;

      const uint32_t fan_triangle_count = polygon_count - 2u;
      if (fan_triangle_count >
          R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT)
         return -EOVERFLOW;
      for (uint32_t fan = 0; fan < fan_triangle_count; fan++) {
         const struct r3v_native_clip_vertex *fan_vertices[3] = {
            &polygon[0], &polygon[fan + 1], &polygon[fan + 2],
         };
         float window[3][R3V_NATIVE_CLIP_MAX_RECORD_DWORDS];
         bool projected = true;
         for (uint32_t vertex = 0; vertex < 3; vertex++) {
            projected &= project_clip_vertex(
               fan_vertices[vertex], record_dwords, target_width,
               target_height, reciprocal_scale, window[vertex]);
         }
         /* The only fixed-volume point with W zero is the homogeneous
          * origin.  Perspective division is singular there; containing its
          * fan triangle as a degenerate produces no invalid carrier values.
          */
         if (!projected)
            continue;

         const double area2 =
            ((double)window[1][0] - window[0][0]) *
               ((double)window[2][1] - window[0][1]) -
            ((double)window[2][0] - window[0][0]) *
               ((double)window[1][1] - window[0][1]);
         bool collapse = sample_mask_zero;
         if (!collapse && area2 != 0.0 && cull_mode != VK_CULL_MODE_NONE) {
            const bool counter_clockwise = area2 > 0.0;
            const bool front_facing =
               counter_clockwise ==
               (front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE);
            const VkCullModeFlags facing_bit =
               front_facing ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
            collapse = (cull_mode & facing_bit) != 0;
         }
         if (collapse) {
            memcpy(window[1], window[0],
                   (size_t)record_dwords * sizeof(float));
            memcpy(window[2], window[0],
                   (size_t)record_dwords * sizeof(float));
         }
         uint32_t *fan_output =
            &triangle_output[fan * 3u * record_dwords];
         for (uint32_t vertex = 0; vertex < 3; vertex++) {
            memcpy(&fan_output[vertex * record_dwords], window[vertex],
                   (size_t)record_dwords * sizeof(float));
         }
      }
   }
   return 0;
}

/* Submission-time execution of one recorded render pass: an empty pass
 * applies its load-op clear, while a draw also reads the bound stream.
 * Both paths execute here, so a vertex write between record and submit
 * is honored and an unsubmitted command buffer leaves application
 * memory untouched.  The pass carries its own carrier, so a command
 * buffer holding several passes calls this once per record.
 */
static VkResult
execute_one_deferred_draw(struct r3v_native_device *device,
                          struct r3v_native_deferred_draw *draw,
                          struct r3v_native_memory *carrier)
{
   if (!draw->pending)
      return VK_SUCCESS;

   /* CmdBeginRenderPass records the load-op clear before a draw exists.  An
    * empty subpass has no vertex stream or carrier to execute, but its clear
    * still realizes at queue submission.
    */
   if (draw->stream_mask == 0) {
      VkResult clear_result =
         fill_color(device, draw->target_memory, draw->target_fill_offset,
                    draw->target_fill_bytes, draw->clear_dword);
      if (clear_result != VK_SUCCESS || draw->clear_rect_count == 0)
         return clear_result;
      /* The recorded attachment clears land after the load-op clear,
       * in API order, over the target's own row pitch from its bind
       * offset.
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
                            draw->target_fill_offset +
                            (uint64_t)(rect->y + row) *
                               draw->target_row_bytes) +
               rect->x;
            for (uint32_t x = 0; x < rect->width; x++)
               texels[x] = rect->dword;
         }
      }
      radeon_drm_vk_bo_cache_sync(&device->drm,
                                  (uint8_t *)target->map +
                                     draw->target_fill_offset,
                                  draw->target_fill_bytes);
      if (owns_map) {
         radeon_drm_vk_bo_unmap(&device->drm, &target->bo, target->map);
         target->map = NULL;
      }
      return VK_SUCCESS;
   }

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
      return fill_color(device, draw->target_memory,
                        draw->target_fill_offset, draw->target_fill_bytes,
                        draw->clear_dword);
   }

   /* The CPU route stages the vertex-job outputs and the fixed-capacity
    * clip result separately.  The carrier receives one copy only after every
    * input triangle clips successfully, so a refused later record cannot
    * leave a partially expanded stream behind.  The reference input and its
    * seven-triangle output ride the stack; larger draws use command-scope
    * host allocations released before return.
    */
   const uint32_t record_dwords =
      r300_vertex_job_record_dwords(&draw->vertex_job);
   const uint32_t record_count = draw->vertex_count * draw->instance_count;
   const uint32_t source_triangle_count = record_count / 3u;
   const uint32_t staged_dwords = record_count * record_dwords;
   const uint64_t expanded_dwords_u64 =
      (uint64_t)source_triangle_count *
      R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT * 3u * record_dwords;
   if (expanded_dwords_u64 > UINT32_MAX) {
      vk_free(&device->vk.alloc, vertex_ids_heap);
      for (uint32_t i = 0; i < owned_map_count; i++) {
         radeon_drm_vk_bo_unmap(&device->drm, &owned_maps[i]->bo,
                                owned_maps[i]->map);
         owned_maps[i]->map = NULL;
      }
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   const uint32_t expanded_dwords = (uint32_t)expanded_dwords_u64;
   uint32_t staged_stack[R300_TRIANGLE_VARYING_VERTEX_DWORDS];
   uint32_t expanded_stack
      [R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT *
       R300_TRIANGLE_VARYING_VERTEX_DWORDS];
   uint32_t *staged_heap = NULL;
   uint32_t *expanded_heap = NULL;
   uint32_t *staged = staged_stack;
   uint32_t *expanded = expanded_stack;
   if (staged_dwords > ARRAY_SIZE(staged_stack)) {
      staged_heap = vk_alloc(&device->vk.alloc,
                             (size_t)staged_dwords * sizeof(uint32_t), 8,
                             VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (staged_heap == NULL)
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      staged = staged_heap;
   }
   if (result == VK_SUCCESS &&
       expanded_dwords > ARRAY_SIZE(expanded_stack)) {
      expanded_heap = vk_alloc(&device->vk.alloc,
                               (size_t)expanded_dwords * sizeof(uint32_t), 8,
                               VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (expanded_heap == NULL)
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      expanded = expanded_heap;
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
      /* Delivery route selection lives in r3v_delivery_route_resolve:
       * the CPU gather is the default and the semantic oracle, and the
       * R2VB identity delivery engages only on the exact opt-in value
       * and the formats it models.  Under the R2VB route the delivery
       * holds the FP24 fixed-point domain and refuses outside it, and
       * the CPU gather then re-derives the same carrier -- a byte
       * divergence falsifies the identity control and refuses the draw
       * rather than submitting bytes the two routes disagree on.
       */
      struct r3v_delivery_route_decision route_decision;
      r3v_delivery_route_resolve(device->r2vb_delivery_gate,
                                  device->r2vb_gpu_delivery_gate,
                                  device->r2vb_fetched_gate,
                                  stream.format_id, &route_decision);
      /* The GPU producer route names a device-side delivery this
       * deferred draw cannot execute: live producer submission routes
       * only through the operator-armed attended surface.  The exact
       * double opt-in therefore refuses the draw by name instead of
       * downgrading to a host copy the caller did not select.
       */
      if (route_decision.route == R3V_DELIVERY_ROUTE_R2VB_GPU_PRODUCER ||
          route_decision.route ==
             R3V_DELIVERY_ROUTE_R2VB_GPU_PRODUCER_FETCHED) {
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
      /* flat_mask == 0 also keeps every Flat draw, the direct GA route
       * included, on the CPU route below, where the clipping-class
       * check runs (r3v_interpolation_lowering.h). */
      const bool r2vb_route =
         route_decision.route == R3V_DELIVERY_ROUTE_R2VB_HOST_MODEL &&
         draw->vertex_job_identity && draw->post_vs.flat_mask == 0 &&
         !draw->indexed &&
         draw->cull_mode == VK_CULL_MODE_NONE &&
         !draw->sample_mask_zero &&
         draw->instance_count == 1 && draw->vertex_count == 3 &&
         r300_cpu_vertex_range_in_bounds(stream.format_id, &source,
                                         stream.first_vertex,
                                         R300_TRIANGLE_VERTEX_DWORDS / 4);
      int gathered = 0;
      uint32_t delivered[R300_TRIANGLE_VERTEX_DWORDS];
      if (result != VK_SUCCESS) {
         /* The refused route delivers nothing; the shared unmap and
          * error paths below still run.
          */
      } else if (r2vb_route) {
         gathered = r300_r2vb_identity_deliver(
            stream.format_id, &source, stream.first_vertex,
            R300_TRIANGLE_VERTEX_DWORDS / 4, delivered,
            R300_TRIANGLE_VERTEX_DWORDS);
         if (gathered == 0) {
            uint32_t oracle[R300_TRIANGLE_VERTEX_DWORDS];
            gathered = r300_cpu_vertex_gather(
               stream.format_id, &source, stream.first_vertex,
               R300_TRIANGLE_VERTEX_DWORDS / 4, oracle,
               R300_TRIANGLE_VERTEX_DWORDS);
            if (gathered == 0 &&
                memcmp(oracle, delivered,
                       R300_TRIANGLE_VERTEX_DWORDS * 4) != 0) {
               result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                                  "r3v-native: R2VB delivery diverged "
                                  "from the CPU gather oracle");
            }
         }
         if (gathered == 0 && result == VK_SUCCESS)
            memcpy(staged, delivered, sizeof(delivered));
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
         /* The records now hold every vertex's own outputs; the
          * post-vertex lowering replicates each Flat varying from the
          * provoking vertex across its triangle before the clipper
          * reads the list, so a clipped edge interpolates equal
          * values and the rasterizer's interpolation of equal
          * endpoints is the flat value. */
         if (gathered == 0) {
            /* The direct GA route keeps each triangle's distinct
             * values -- the GA selects the provoking vertex's color 0
             * per primitive -- only while every source triangle's
             * clipping class is ACCEPT; a fan the clipper emits from a
             * partially clipped triangle carries vertices the source
             * never had, so the list is replicated ahead of the
             * clipper, and the GA's selection over equal endpoints is
             * the identity. */
            bool direct_flat =
               draw->direct_flat &&
               route_decision.position_space == R300_CARRIER_POSITION_CLIP;
            for (uint32_t t = 0; direct_flat && t < source_triangle_count;
                 t++) {
               direct_flat =
                  r3v_interpolation_clip_class_of_triangle(
                     &staged[(size_t)t * 3u * record_dwords],
                     record_dwords) == R3V_INTERPOLATION_CLIP_ACCEPT;
            }
            if (!direct_flat) {
               gathered = r3v_post_vs_lower_triangles(
                  &draw->post_vs, staged, source_triangle_count,
                  record_dwords);
            }
            /* The direct GB W_SELECT route interpolates every
             * varying linearly in window space; a vertex the clipper
             * generates carries the clip-space linear value, which
             * differs in general from the value the Vulkan
             * specification assigns a clipped NoPerspective output
             * (Clipping Shader Outputs), so the partial class refuses
             * ahead of carrier publication. */
            if (gathered == 0 && draw->direct_noperspective) {
               for (uint32_t t = 0; t < source_triangle_count; t++) {
                  if (r3v_interpolation_clip_class_of_triangle(
                         &staged[(size_t)t * 3u * record_dwords],
                         record_dwords) != R3V_INTERPOLATION_CLIP_ACCEPT) {
                     result = vk_errorf(
                        device, VK_ERROR_INITIALIZATION_FAILED,
                        "r3v-native: the direct GB W_SELECT NoPerspective "
                        "route admits the clipping class ACCEPT alone; "
                        "source triangle %u is partially clipped", t);
                     break;
                  }
               }
            }
         }
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
         const int clipped = expand_clip_space_triangles(
            staged, source_triangle_count, record_dwords, draw->target_width,
            draw->target_height, draw->cull_mode, draw->front_face,
            draw->sample_mask_zero, expanded, expanded_dwords);
         if (clipped != 0) {
            result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                               "r3v-native: homogeneous triangle clipping "
                               "refused (%d): %s",
                               clipped, strerror(-clipped));
         } else {
            memcpy(carrier->map, expanded,
                   (size_t)expanded_dwords * sizeof(uint32_t));
         }
      } else if (result == VK_SUCCESS) {
         memcpy(carrier->map, staged,
                (size_t)staged_dwords * sizeof(uint32_t));
      }
      if (result == VK_SUCCESS) {
         const size_t publication_bytes =
            route_decision.position_space == R300_CARRIER_POSITION_CLIP
               ? (size_t)expanded_dwords * sizeof(uint32_t)
               : (size_t)staged_dwords * sizeof(uint32_t);
         radeon_drm_vk_bo_cache_sync(&device->drm, carrier->map,
                                     publication_bytes);
      }
   }
   if (owns_carrier_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &carrier->bo, carrier->map);
      carrier->map = NULL;
   }
   vk_free(&device->vk.alloc, expanded_heap);
   vk_free(&device->vk.alloc, staged_heap);
   vk_free(&device->vk.alloc, vertex_ids_heap);

   for (uint32_t i = 0; i < owned_map_count; i++) {
      radeon_drm_vk_bo_unmap(&device->drm, &owned_maps[i]->bo,
                             owned_maps[i]->map);
      owned_maps[i]->map = NULL;
   }
   if (result != VK_SUCCESS)
      return result;

   /* The load-op clear realizes as the pass's packed color over the
    * image's declared memory footprint alone; a larger allocation keeps its
    * remaining bytes, so a resource bound past the image survives the
    * draw.
    */
   /* pending stays set: every submission re-reads the stream and
    * re-clears, the execution-time semantics each submit carries.
    */
   return fill_color(device, draw->target_memory, draw->target_fill_offset,
                     draw->target_fill_bytes, draw->clear_dword);
}
/* Submission-time execution of every recorded render pass, in record
 * order: each carries its own load-op clear, its own carrier, and its
 * own vertex execution, so a second pass executes exactly what a first
 * one does over its own state.
 */
VkResult
r3v_native_cmd_buffer_execute_deferred_draws(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer)
{
   const uint32_t count = MAX2(cmd_buffer->deferred_draw_count, 1u);
   for (uint32_t i = 0; i < count; i++) {
      const VkResult result = execute_one_deferred_draw(
         device, &cmd_buffer->deferred_draws[i],
         cmd_buffer->owned_carriers[i]);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
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
   struct r3v_native_deferred_draw *draw = &cmd_buffer->deferred_draws[0];

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
       cmd_buffer->deferred_draw_count != 1 ||
       (cmd_buffer->cell_kind != R3V_NATIVE_CELL_KIND_TRIANGLE &&
        cmd_buffer->cell_kind !=
           R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED)) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: fetched GPU producer route admits the "
                       "identity vertex job over one source stream on the "
                       "recorded single-draw triangle consumer alone; the "
                       "route binds one source relocation role");
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
   struct r3v_native_memory *carrier = cmd_buffer->owned_carriers[0];
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
   if (!composed_before &&
       (cmd_buffer->window_space_ib == NULL ||
        cmd_buffer->window_space_ib_size_dwords == 0)) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: fetched GPU producer route requires the "
                       "recorded window-space triangle consumer");
   }
   const uint32_t *consumer_words =
      composed_before ? cmd_buffer->ib + draw->gpu_producer_dwords
                      : cmd_buffer->window_space_ib;
   const uint32_t consumer_dwords =
      composed_before ? cmd_buffer->ib_size_dwords - draw->gpu_producer_dwords
                      : cmd_buffer->window_space_ib_size_dwords;
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
   free(cmd_buffer->window_space_ib);
   free(cmd_buffer->references);
   cmd_buffer->ib = route.ib;
   cmd_buffer->ib_size_dwords = route.ib_size_dwords;
   cmd_buffer->window_space_ib = NULL;
   cmd_buffer->window_space_ib_size_dwords = 0;
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
   struct r3v_native_deferred_draw *draw = &cmd_buffer->deferred_draws[0];
   if (!draw->pending || draw->stream_mask == 0)
      return VK_SUCCESS;

   /* The routes deliver the slot-0 position stream, so the route
    * resolves on its format and the admissions below refuse a job
    * reading any other slot by name; a job that leaves slot 0 unread
    * resolves through the INVALID format to the CPU route. */
   struct r3v_delivery_route_decision route;
   r3v_delivery_route_resolve(device->r2vb_delivery_gate,
                               device->r2vb_gpu_delivery_gate,
                               device->r2vb_fetched_gate,
                               (draw->stream_mask & 1u)
                                  ? draw->streams[0].format_id
                                  : R300_VERTEX_FORMAT_INVALID,
                               &route);
   if (route.route != R3V_DELIVERY_ROUTE_R2VB_GPU_PRODUCER_FETCHED &&
       route.route != R3V_DELIVERY_ROUTE_R2VB_GPU_PRODUCER)
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
   if (route.route == R3V_DELIVERY_ROUTE_R2VB_GPU_PRODUCER_FETCHED)
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
       cmd_buffer->deferred_draw_count != 1 ||
       (cmd_buffer->cell_kind != R3V_NATIVE_CELL_KIND_TRIANGLE &&
        cmd_buffer->cell_kind !=
           R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC)) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: GPU producer route admits the "
                       "identity vertex job over one source stream on the "
                       "recorded single-draw triangle consumer alone");
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

   if (!draw->gpu_producer_delivery &&
       (cmd_buffer->window_space_ib == NULL ||
        cmd_buffer->window_space_ib_size_dwords == 0)) {
      r300_r2vb_producer_pass_release(&producer);
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: GPU producer route requires the recorded "
                       "window-space triangle consumer");
   }

   /* Composition: producer ++ consumer in one IB.  The producer's
    * carrier relocation payload and the consumer's vertex slot payload
    * both name relocation entry zero, and the consumer's reference list
    * already binds the carrier there, so the list only gains the write
    * domain the color backend needs.
    */
   if (!draw->gpu_producer_delivery) {
      const uint32_t combined_dwords =
         producer.ib_size_dwords + cmd_buffer->window_space_ib_size_dwords;
      uint32_t *combined = malloc(combined_dwords * sizeof(uint32_t));
      if (combined == NULL) {
         r300_r2vb_producer_pass_release(&producer);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      memcpy(combined, producer.ib,
             producer.ib_size_dwords * sizeof(uint32_t));
      memcpy(combined + producer.ib_size_dwords, cmd_buffer->window_space_ib,
             cmd_buffer->window_space_ib_size_dwords * sizeof(uint32_t));
      free(cmd_buffer->ib);
      free(cmd_buffer->window_space_ib);
      cmd_buffer->ib = combined;
      cmd_buffer->ib_size_dwords = combined_dwords;
      cmd_buffer->window_space_ib = NULL;
      cmd_buffer->window_space_ib_size_dwords = 0;
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
   struct r3v_native_memory *carrier = cmd_buffer->owned_carriers[0];
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
   struct r3v_native_deferred_draw *draw = &cmd_buffer->deferred_draws[0];
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

   struct r3v_native_memory *carrier = cmd_buffer->owned_carriers[0];
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

   VkResult fill_result = fill_color(device, color_memory, 0,
                                     color_memory->bo.size,
                                     R300_TRIANGLE_COLOR_SENTINEL);
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

/* Records the composed render-then-sample cell: one stream renders the
 * first shape into the render target, publishes it through the
 * destination-cache flush the render half closes with, then invalidates
 * the texture tags and samples that target into the second target.  The
 * first target fills two slots, so the reference array is built merged
 * -- one entry per buffer object, in slot order, the shared entry
 * carrying both the write domain the render half needs and the read
 * domain the sample half's texture check needs -- and the cell's
 * relocation payloads are bound to that array's own positions.  The
 * queue merges by handle again over the same list, which is then
 * idempotent, so the indices the digest covers are the indices the
 * kernel reads.
 */
VkResult
r3v_native_record_composed_render_sample(
   VkCommandBuffer commandBuffer, VkDeviceMemory renderVertexMemory,
   VkDeviceMemory renderColorMemory, VkDeviceMemory sampleVertexMemory,
   VkDeviceMemory sampleColorMemory,
   const struct r300_triangle_composed_render_sample *composed)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, render_vertex, renderVertexMemory);
   VK_FROM_HANDLE(r3v_native_memory, render_color, renderColorMemory);
   VK_FROM_HANDLE(r3v_native_memory, sample_vertex, sampleVertexMemory);
   VK_FROM_HANDLE(r3v_native_memory, sample_color, sampleColorMemory);

   if (cmd_buffer == NULL || render_vertex == NULL || render_color == NULL ||
       sample_vertex == NULL || sample_color == NULL || composed == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);

   /* The four roles the cell fills: the two vertex arrays, the shared
    * first target, and the second target.  A handle repeated across two
    * of them would merge entries the payload binding cannot separate,
    * since a merged entry carries one domain pair for both roles.
    */
   struct r3v_native_memory *const role[4] = { render_vertex, render_color,
                                               sample_vertex, sample_color };
   for (unsigned a = 0; a < 4; a++) {
      for (unsigned b = a + 1; b < 4; b++) {
         if (role[a]->bo.handle == role[b]->bo.handle)
            return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                             "r3v-native: composed cell roles require "
                             "distinct GEM objects");
      }
   }

   struct r3v_tcl_bypass_composed_slot {
      struct r3v_native_memory *memory;
      uint32_t read_domains;
      uint32_t write_domain;
   };
   const struct r3v_tcl_bypass_composed_slot slots[R300_TRIANGLE_SLOT_COUNT] = {
      [R300_TRIANGLE_SLOT_VERTEX] = { render_vertex, RADEON_GEM_DOMAIN_GTT, 0 },
      [R300_TRIANGLE_SLOT_COLOR] = { render_color, 0, RADEON_GEM_DOMAIN_GTT },
      [R300_TRIANGLE_SLOT_TEXTURE] = { render_color, RADEON_GEM_DOMAIN_GTT, 0 },
      [R300_TRIANGLE_SLOT_COMPOSED_VERTEX] = { sample_vertex,
                                               RADEON_GEM_DOMAIN_GTT, 0 },
      [R300_TRIANGLE_SLOT_COMPOSED_COLOR] = { sample_color, 0,
                                              RADEON_GEM_DOMAIN_GTT },
   };

   struct r3v_native_bo_reference *references =
      calloc(R3V_NATIVE_COMPOSED_REFERENCE_COUNT, sizeof(*references));
   if (references == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* One merge, by the winsys rule: first-add order, one entry per
    * handle, domains ORed.  The map reads back the array's own
    * positions, so the payloads and the entries cannot drift apart.
    */
   uint32_t slot_index[R300_TRIANGLE_SLOT_COUNT];
   uint32_t reference_count = 0;
   for (uint32_t slot = 0; slot < R300_TRIANGLE_SLOT_COUNT; slot++) {
      uint32_t found = reference_count;
      for (uint32_t i = 0; i < reference_count; i++) {
         if (references[i].handle == slots[slot].memory->bo.handle) {
            found = i;
            break;
         }
      }
      if (found == reference_count) {
         references[reference_count++] = (struct r3v_native_bo_reference){
            .handle = slots[slot].memory->bo.handle,
            .memory = slots[slot].memory,
         };
      }
      references[found].read_domains |= slots[slot].read_domains;
      references[found].write_domain |= slots[slot].write_domain;
      slot_index[slot] = found;
   }

   struct r300_tcl_bypass_triangle_ib cell;
   int emit_result =
      r300_tcl_bypass_triangle_composed_render_sample_emit(composed, &cell);
   if (emit_result == 0) {
      emit_result = r300_tcl_bypass_triangle_bind_reloc_indices(
         &cell, slot_index, R300_TRIANGLE_SLOT_COUNT);
      if (emit_result != 0)
         r300_tcl_bypass_triangle_release(&cell);
   }
   if (emit_result != 0) {
      free(references);
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(emit_result));
   }

   r3v_native_cmd_buffer_install_ib(
      cmd_buffer, R3V_NATIVE_CELL_KIND_TRIANGLE_COMPOSED_RENDER_SAMPLE,
      cell.ib, cell.ib_size_dwords, references, reference_count);
   cell.ib = NULL;
   r300_tcl_bypass_triangle_release(&cell);
   return VK_SUCCESS;
}

/* Records the two-pass cell: the first pass installs its render-shape
 * cell over its own vertex page and color target, and the second pass
 * appends its cell through r3v_native_cmd_buffer_append_ib, the route
 * the public two-draw command buffer takes.  The caller's declared
 * binding must equal the one the handles produce under the winsys
 * first-add rule, which is what makes the offline emitter's stream the
 * recorded one.
 */
VkResult
r3v_native_record_multi_pass(VkCommandBuffer commandBuffer,
                             VkDeviceMemory firstVertexMemory,
                             VkDeviceMemory firstColorMemory,
                             VkDeviceMemory secondVertexMemory,
                             VkDeviceMemory secondColorMemory,
                             const struct r300_triangle_multi_pass *mp)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, first_vertex, firstVertexMemory);
   VK_FROM_HANDLE(r3v_native_memory, first_color, firstColorMemory);
   VK_FROM_HANDLE(r3v_native_memory, second_vertex, secondVertexMemory);
   VK_FROM_HANDLE(r3v_native_memory, second_color, secondColorMemory);

   if (cmd_buffer == NULL || first_vertex == NULL || first_color == NULL ||
       second_vertex == NULL || second_color == NULL || mp == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);

   if (r300_tcl_bypass_triangle_multi_pass_binding_validate(mp) != 0)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   /* The binding the handles produce: a second vertex page equal to the
    * first takes index 0, else the next unused index; the second color
    * likewise from index 1.  The roles within a pass stay distinct, and
    * a role crossing (a vertex page that is a color target) has no
    * admitted binding, so it refuses here as it does at the emitter.
    */
   if (first_vertex->bo.handle == first_color->bo.handle ||
       second_vertex->bo.handle == second_color->bo.handle ||
       second_vertex->bo.handle == first_color->bo.handle ||
       second_color->bo.handle == first_vertex->bo.handle)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: two-pass roles alias one GEM object");
   const uint32_t vertex_index =
      second_vertex->bo.handle == first_vertex->bo.handle ? 0u : 2u;
   const uint32_t color_index =
      second_color->bo.handle == first_color->bo.handle
         ? 1u
         : (vertex_index == 0u ? 2u : 3u);
   if (vertex_index != mp->second_vertex_index ||
       color_index != mp->second_color_index)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: the declared binding (%u, %u) is not "
                       "the one the handles produce (%u, %u)",
                       mp->second_vertex_index, mp->second_color_index,
                       vertex_index, color_index);

   struct r3v_native_memory *const vertex[2] = { first_vertex,
                                                 second_vertex };
   struct r3v_native_memory *const color[2] = { first_color, second_color };
   for (unsigned pass = 0; pass < 2; pass++) {
      struct r300_tcl_bypass_triangle_ib cell;
      const int emitted =
         r300_tcl_bypass_triangle_render_shape_emit(&mp->pass[pass], &cell);
      if (emitted != 0)
         return vk_error(device,
                         r3v_native_cell_vk_result_from_errno(emitted));
      struct r3v_native_bo_reference *references =
         calloc(R300_TRIANGLE_RENDER_SLOT_COUNT, sizeof(*references));
      if (references == NULL) {
         r300_tcl_bypass_triangle_release(&cell);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      references[R300_TRIANGLE_SLOT_VERTEX] =
         (struct r3v_native_bo_reference){
            .handle = vertex[pass]->bo.handle,
            .read_domains = RADEON_GEM_DOMAIN_GTT,
            .write_domain = 0,
            .memory = vertex[pass],
         };
      references[R300_TRIANGLE_SLOT_COLOR] = (struct r3v_native_bo_reference){
         .handle = color[pass]->bo.handle,
         .read_domains = 0,
         .write_domain = RADEON_GEM_DOMAIN_GTT,
         .memory = color[pass],
      };
      if (pass == 0) {
         r3v_native_cmd_buffer_install_ib(
            cmd_buffer, R3V_NATIVE_CELL_KIND_TRIANGLE_RENDER_SHAPE, cell.ib,
            cell.ib_size_dwords, references, R300_TRIANGLE_RENDER_SLOT_COUNT);
         cell.ib = NULL;
         r300_tcl_bypass_triangle_release(&cell);
         continue;
      }
      const VkResult appended = r3v_native_cmd_buffer_append_ib(
         device, cmd_buffer, &cell, references,
         R300_TRIANGLE_RENDER_SLOT_COUNT);
      free(references);
      r300_tcl_bypass_triangle_release(&cell);
      if (appended != VK_SUCCESS)
         return appended;
   }
   return VK_SUCCESS;
}

/* Records the multisample resolve cell: the render half draws the
 * reference triangle into a sample-expanded surface with the subsample
 * set live, then the resolve half covers the extent under
 * RB3D_AARESOLVE_CTL.AARESOLVE_MODE_RESOLVE so the downsampled samples
 * reach RB3D_AARESOLVE_OFFSET.  Both halves render into the one
 * multisample surface, so its two slots merge into one write entry, and
 * the reference array is built merged with the payloads bound to its own
 * positions.
 *
 * The multisample surface is the recording's own allocation in
 * RADEON_GEM_DOMAIN_VRAM with no fallback domain and no CPU access, so
 * the create itself is the placement -- the memory-type policy's type 1
 * gives VRAM | GTT, under which a host-unmapped allocation proves
 * nothing about residency.  Its size carries the sample expansion
 * (r300_texture_desc.c multiplies the layer size and leaves the stride
 * alone), which the kernel checks no term of: r100_cs_track_check sizes
 * the color buffer as pitch * cpp * maxy.  The resolve destination is
 * bounded there -- aa.pitch * cb[0].cpp * maxy + aa.offset against the
 * buffer size -- so an undersized destination refuses at the validator.
 */
VkResult
r3v_native_record_msaa_resolve(VkCommandBuffer commandBuffer,
                               VkDeviceMemory renderVertexMemory,
                               VkDeviceMemory coverVertexMemory,
                               VkDeviceMemory destinationMemory,
                               const struct r300_triangle_msaa_resolve *msaa)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_memory, render_vertex, renderVertexMemory);
   VK_FROM_HANDLE(r3v_native_memory, cover_vertex, coverVertexMemory);
   VK_FROM_HANDLE(r3v_native_memory, destination, destinationMemory);

   if (cmd_buffer == NULL || render_vertex == NULL || cover_vertex == NULL ||
       destination == NULL || msaa == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);

   struct r3v_native_memory *const role[3] = { render_vertex, cover_vertex,
                                               destination };
   for (unsigned a = 0; a < 3; a++) {
      for (unsigned b = a + 1; b < 3; b++) {
         if (role[a]->bo.handle == role[b]->bo.handle)
            return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                             "r3v-native: resolve cell roles require "
                             "distinct GEM objects");
      }
   }

   struct r300_tcl_bypass_triangle_ib cell;
   int emit_result = r300_tcl_bypass_triangle_msaa_resolve_emit(msaa, &cell);
   if (emit_result != 0)
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(emit_result));

   /* The emission admitted the shape, so the footprint arithmetic below
    * runs over bounded extents.
    */
   const uint64_t layer_bytes = (uint64_t)msaa->render.pitch_pixels *
                                msaa->render.height * sizeof(uint32_t);
   const uint64_t surface_bytes = (uint64_t)msaa->render.target_offset +
                                  layer_bytes * msaa->sample_count;

   struct r3v_native_memory *surface = cmd_buffer->owned_multisample;
   if (surface != NULL && surface->bo.size < surface_bytes) {
      radeon_drm_vk_bo_free(&device->drm, &surface->bo);
      vk_free(&cmd_buffer->vk.pool->alloc, surface);
      cmd_buffer->owned_multisample = NULL;
      surface = NULL;
   }
   const bool surface_is_new = surface == NULL;
   if (surface == NULL) {
      surface = vk_zalloc(&cmd_buffer->vk.pool->alloc, sizeof(*surface), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (surface == NULL) {
         r300_tcl_bypass_triangle_release(&cell);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      if (radeon_drm_vk_bo_create(&device->drm, surface_bytes,
                                  R3V_NATIVE_MEMORY_ALIGNMENT,
                                  RADEON_GEM_DOMAIN_VRAM,
                                  RADEON_GEM_NO_CPU_ACCESS, false,
                                  &surface->bo) != 0) {
         vk_free(&cmd_buffer->vk.pool->alloc, surface);
         r300_tcl_bypass_triangle_release(&cell);
         return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "r3v-native: multisample surface requires a "
                          "device-local VRAM allocation");
      }
   }

   struct r3v_tcl_bypass_msaa_slot {
      struct r3v_native_memory *memory;
      uint32_t read_domains;
      uint32_t write_domain;
   };
   /* Both halves write the multisample surface: the render half through
    * the color backend, the resolve half through the same bound target
    * while AARESOLVE_MODE redirects the downsampled output.  The
    * destination carries its own write; the two vertex arrays are read.
    */
   const struct r3v_tcl_bypass_msaa_slot slots[R300_TRIANGLE_SLOT_COUNT] = {
      [R300_TRIANGLE_SLOT_VERTEX] = { render_vertex, RADEON_GEM_DOMAIN_GTT,
                                      0 },
      [R300_TRIANGLE_SLOT_COLOR] = { surface, 0, RADEON_GEM_DOMAIN_VRAM },
      [R300_TRIANGLE_SLOT_TEXTURE] = { surface, 0, RADEON_GEM_DOMAIN_VRAM },
      [R300_TRIANGLE_SLOT_COMPOSED_VERTEX] = { cover_vertex,
                                               RADEON_GEM_DOMAIN_GTT, 0 },
      [R300_TRIANGLE_SLOT_COMPOSED_COLOR] = { destination, 0,
                                              RADEON_GEM_DOMAIN_GTT },
   };

   struct r3v_native_bo_reference *references =
      calloc(R3V_NATIVE_MSAA_REFERENCE_COUNT, sizeof(*references));
   if (references == NULL) {
      if (surface_is_new) {
         radeon_drm_vk_bo_free(&device->drm, &surface->bo);
         vk_free(&cmd_buffer->vk.pool->alloc, surface);
      }
      r300_tcl_bypass_triangle_release(&cell);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   uint32_t slot_index[R300_TRIANGLE_SLOT_COUNT];
   uint32_t reference_count = 0;
   for (uint32_t slot = 0; slot < R300_TRIANGLE_SLOT_COUNT; slot++) {
      uint32_t found = reference_count;
      for (uint32_t i = 0; i < reference_count; i++) {
         if (references[i].handle == slots[slot].memory->bo.handle) {
            found = i;
            break;
         }
      }
      if (found == reference_count) {
         references[reference_count++] = (struct r3v_native_bo_reference){
            .handle = slots[slot].memory->bo.handle,
            .memory = slots[slot].memory,
         };
      }
      references[found].read_domains |= slots[slot].read_domains;
      references[found].write_domain |= slots[slot].write_domain;
      slot_index[slot] = found;
   }

   emit_result = r300_tcl_bypass_triangle_bind_reloc_indices(
      &cell, slot_index, R300_TRIANGLE_SLOT_COUNT);
   if (emit_result != 0) {
      free(references);
      if (surface_is_new) {
         radeon_drm_vk_bo_free(&device->drm, &surface->bo);
         vk_free(&cmd_buffer->vk.pool->alloc, surface);
      }
      r300_tcl_bypass_triangle_release(&cell);
      return vk_error(device,
                      r3v_native_cell_vk_result_from_errno(emit_result));
   }

   r3v_native_cmd_buffer_install_ib(
      cmd_buffer, R3V_NATIVE_CELL_KIND_TRIANGLE_MSAA_RESOLVE, cell.ib,
      cell.ib_size_dwords, references, reference_count);
   cmd_buffer->owned_multisample = surface;
   cell.ib = NULL;
   r300_tcl_bypass_triangle_release(&cell);
   return VK_SUCCESS;
}
