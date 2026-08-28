/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terakan_draw.h"
#include "terakan_profile.h"
#include "terakan_instance.h"
#include "terakan_device.h"
#include "terakan_draw_indirect.h"

#include "terakan_barrier.h"
#include "terakan_buffer.h"
#include "terakan_command_buffer.h"
#include "terakan_entrypoints.h"
#include "terakan_pipeline_graphics.h"

#include "amd/terascale/common/terascale_evergreend.h"
#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "util/u_debug.h"
#include "util/u_endian.h"
#include "util/u_math.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

static inline bool
terakan_draw_debug_cs_dump_enabled(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);
   struct terakan_instance const * const inst =
      container_of(terakan_device_physical_device(device)->vk.base.instance,
                   struct terakan_instance const, vk);
   return (inst->debug_flags & TERAKAN_DEBUG_CS_DUMP) != 0;
}

static inline bool
terakan_draw_pre_draw_surface_sync_enabled(void)
{
   return debug_get_bool_option("TERAKAN_DRAW_PRE_DRAW_SURFACE_SYNC", false);
}

static void
terakan_draw_emit_pre_draw_surface_sync(struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t const surface_sync_cp_coher_cntl =
      S_0085F0_TC_ACTION_ENA(1) |
      S_0085F0_VC_ACTION_ENA(1) |
      S_0085F0_SH_ACTION_ENA(1) |
      S_0085F0_SMX_ACTION_ENA(1) |
      TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME;

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 5);
   if (unlikely(packet == NULL)) {
      if (unlikely(terakan_draw_debug_cs_dump_enabled(command_writer))) {
         fprintf(stderr, "terakan: pm4: PKT3_SURFACE_SYNC packet=NULL (skipped)\n");
      }
      return;
   }

   *packet++ = PKT3(PKT3_SURFACE_SYNC, 4 - 1, 0);
   *packet++ = surface_sync_cp_coher_cntl;
   *packet++ = UINT32_MAX;
   *packet++ = 0;
   *packet++ = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;

   if (unlikely(terakan_draw_debug_cs_dump_enabled(command_writer))) {
      fprintf(stderr,
              "terakan: pm4: PKT3_SURFACE_SYNC cp_coher_cntl=0x%08X coher_size=0x%08X coher_base=0x%08X poll=%u\n",
              surface_sync_cp_coher_cntl, UINT32_MAX, 0u,
              TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL);
   }

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBindIndexBuffer(VkCommandBuffer const commandBuffer, VkBuffer const bufferHandle,
                           VkDeviceSize const offset, VkIndexType const indexType)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_buffer const * const buffer = terakan_buffer_from_handle(bufferHandle);

   uint32_t vgt_index_type;
   VkDeviceSize vgt_index_buffer_size = buffer->vk.size - offset;
   if (indexType == VK_INDEX_TYPE_UINT32) {
#if UTIL_ARCH_BIG_ENDIAN
      vgt_index_type = VGT_INDEX_32 | VGT_DMA_SWAP_32_BIT;
#else
      vgt_index_type = VGT_INDEX_32;
#endif
      vgt_index_buffer_size /= sizeof(uint32_t);
   } else {
      assert(indexType == VK_INDEX_TYPE_UINT16);
#if UTIL_ARCH_BIG_ENDIAN
      vgt_index_type = VGT_INDEX_16 | VGT_DMA_SWAP_16_BIT;
#else
      vgt_index_type = VGT_INDEX_16;
#endif
      vgt_index_buffer_size /= sizeof(uint16_t);
   }
   vgt_index_buffer_size = MIN2(vgt_index_buffer_size, UINT32_MAX);

   command_writer->state_draw.vgt_index_type = vgt_index_type;
   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_VGT_INDEX_TYPE);

   /* The index buffer is not needed by internal draws, modify hw_state_draw directly. */
   uint64_t const vgt_index_buffer_va = buffer->va + offset;
   bool const vgt_index_buffer_modified =
      command_writer->hw_state_draw.vgt_index_buffer.bo != buffer->bo ||
      command_writer->hw_state_draw.vgt_index_buffer.va != vgt_index_buffer_va ||
      command_writer->hw_state_draw.vgt_index_buffer.size != vgt_index_buffer_size;
   command_writer->hw_state_draw.vgt_index_buffer.bo = buffer->bo;
   command_writer->hw_state_draw.vgt_index_buffer.va = vgt_index_buffer_va;
   command_writer->hw_state_draw.vgt_index_buffer.size = vgt_index_buffer_size;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_VGT_INDEX_BUFFER,
                                 vgt_index_buffer_modified);
}

static void
terakan_set_vertex_instance_offsets(struct terakan_gfx_command_writer * const command_writer,
                                    uint32_t const vertex_offset, uint32_t const instance_offset)
{
   command_writer->state_draw.vgt_index_offset = vertex_offset;
   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_VGT_INDEX_OFFSET);

   /* The instance offset is not needed by internal draws, modify hw_state_draw directly. */
   bool const sq_vtx_start_inst_loc_modified =
      command_writer->hw_state_draw.sq_vtx_start_inst_loc != instance_offset;
   command_writer->hw_state_draw.sq_vtx_start_inst_loc = instance_offset;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_SQ_VTX_START_INST_LOC,
                                 sq_vtx_start_inst_loc_modified);
}

void
terakan_before_hw_draw(struct terakan_gfx_command_writer * const command_writer,
                       bool const is_meta_draw)
{
   /* TODO(Triang3l): Maybe insert barriers after emitting the state changes in command emission,
    * not before, so state changes are not blocked by the barriers in the CP, and new work can begin
    * as soon as possible.
    */

   if (is_meta_draw) {
      /* Preserve INV bits across a meta draw only for pure read-cache
       * invalidation barriers. If producer-side flush actions are present
       * (for example, COMPUTE_SHADER_WRITE -> INDEX_READ), re-emitting INV
       * alone creates an extra same-CMDB SURFACE_SYNC(0x89800000) between
       * the meta draw and the application draw; consume that barrier once.
       */
      enum terakan_barrier_action_flags const meta_preserve_mask =
         TERAKAN_BARRIER_ACTION_SYNC_PFP_TO_ME | TERAKAN_BARRIER_ACTION_INV_TC |
         TERAKAN_BARRIER_ACTION_INV_SH | TERAKAN_BARRIER_ACTION_INV_VC;
      enum terakan_barrier_action_flags const pending_actions =
         command_writer->pending_barrier_actions;
      enum terakan_barrier_action_flags const saved_inv =
         (pending_actions & ~meta_preserve_mask) == 0
            ? (pending_actions & (TERAKAN_BARRIER_ACTION_INV_TC |
                                  TERAKAN_BARRIER_ACTION_INV_SH |
                                  TERAKAN_BARRIER_ACTION_INV_VC))
            : 0;
      terakan_barrier_emit_pending_actions(command_writer);
      command_writer->pending_barrier_actions |= saved_inv;
   } else {
      terakan_barrier_emit_pending_actions(command_writer);
   }
}

/* SFN's load_base_instance / load_base_vertex / load_draw_id read from a constant
 * buffer at slot R600_LDS_INFO_CONST_BUFFER (= 16) via VTX_FETCH.  In r600g (Gallium)
 * this buffer is populated per-draw by the state tracker.  Terakan must do the same.
 *
 * The buffer layout matches struct r600_lds_constant_buffer from r600_pipe.h.
 * SFN reads at byte offsets 32 (vertexid_base), 36 (instance_base), 40 (vertex_base),
 * 44 (draw_id).  We allocate the full 48-byte struct but only the draw-parameter
 * fields at offsets 32-47 are meaningful for non-tessellation draws.
 */
#define TERAKAN_DRAW_PARAMS_CONST_BUFFER 16
#define TERAKAN_DRAW_PARAMS_BUFFER_SIZE  48

static void
terakan_bind_draw_params(struct terakan_gfx_command_writer * const command_writer,
                         uint32_t const first_vertex, uint32_t const first_instance)
{
   struct terakan_bo const *bo;
   uint64_t va;
   uint32_t * const mapping = terakan_push_buffer_allocate(
      command_writer->base.command_buffer, TERAKAN_DRAW_PARAMS_BUFFER_SIZE,
      sizeof(uint32_t), &bo, &va);
   if (unlikely(mapping == NULL))
      return;

   memset(mapping, 0, TERAKAN_DRAW_PARAMS_BUFFER_SIZE);
   mapping[8]  = 0;               /* vertexid_base (offset 32) */
   mapping[9]  = first_instance;   /* instance_base (offset 36) */
   mapping[10] = first_vertex;     /* vertex_base   (offset 40) */
   mapping[11] = 0;               /* draw_id       (offset 44) */

   uint32_t resource[8] = {
      [0] = (uint32_t)va,
      [1] = TERAKAN_DRAW_PARAMS_BUFFER_SIZE - 1,
      [2] = S_030008_BASE_ADDRESS_HI(va >> 32) | S_030008_STRIDE(16) |
            S_030008_DATA_FORMAT(0x23 /* FMT_32_32_32_32_FLOAT */),
      [3] = S_03000C_DST_SEL_X(TERASCALE_SWIZZLE_X) | S_03000C_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
            S_03000C_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_03000C_DST_SEL_W(TERASCALE_SWIZZLE_W),
      [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
   };

   BITSET_SET(command_writer->hw_state_sqc.needed.resources.vs,
              TERAKAN_DRAW_PARAMS_CONST_BUFFER);
   terakan_hw_state_sqc_set_resource_vs(&command_writer->hw_state_sqc,
                                        TERAKAN_DRAW_PARAMS_CONST_BUFFER,
                                        bo, resource);
}

static void
terakan_before_draw(struct terakan_gfx_command_writer * const command_writer)
{
   uint64_t t0 = 0;
   struct terakan_device * const device = terakan_gfx_command_writer_device(command_writer);
   struct terakan_instance const * const inst =
      container_of(terakan_device_physical_device(device)->vk.base.instance,
                   struct terakan_instance const, vk);
   bool const profiling = inst->debug_flags & TERAKAN_DEBUG_PROFILE;
   if (profiling)
      t0 = terakan_profile_now_ns();

   /* If compute dispatch switched SQ into compute mode and the app issues a
    * draw without rebinding graphics, force one graphics bind so SQ_CONFIG and
    * SQ thread management are restored before state application. */
   if (command_writer->sq_config_is_compute_mode && command_writer->bound_graphics_pipeline) {
      terakan_pipeline_graphics_bind(command_writer, command_writer->bound_graphics_pipeline);
   }

   terakan_state_draw_apply_pending(command_writer);

   terakan_push_constants_apply(command_writer, false);

   /* Bind robustness metadata to KCACHE bank 14 if the current graphics
    * pipeline emitted any shader stages that need it (write guards or
    * robustness reads).  Evergreen ISA section 4.6.4: banks >= 14 are NOT
    * dynamically indexed, so a single binding serves all stages. */
   if (command_writer->graphics_kcache_needed &
       ((uint16_t)1 << TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA)) {
      terakan_robustness_metadata_apply(command_writer, false);
   }

   terakan_before_hw_draw(command_writer, false);
   if (unlikely(terakan_draw_pre_draw_surface_sync_enabled())) {
      terakan_draw_emit_pre_draw_surface_sync(command_writer);
   }

   if (profiling) {
      device->profile.draw_ns += terakan_profile_now_ns() - t0;
      device->profile.draw_count++;
   }
}

/* For VK_KHR_multiview view-loop expansion: write the per-iteration view
 * index to the dedicated driver push-constant slot and mark its modified
 * bit when the value changes so the KCACHE buffer is re-uploaded before
 * the next draw.  The graphics/compute shader reads this slot via the
 * load_view_index NIR lowering in terakan_nir_lower_abi.c, which
 * materializes gl_ViewIndex for the application. */
static inline void
terakan_set_view_index_push_constant(struct terakan_gfx_command_writer * const command_writer,
                                     uint32_t const view_index)
{
   uint32_t * const view_index_constant =
      &command_writer->push_constants_state.driver_constants.view_index;
   if (*view_index_constant == view_index) {
      return;
   }

   *view_index_constant = view_index;
   command_writer->push_constants_state.driver_constants_modified |=
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_VIEW_INDEX);
}

/* For VK_KHR_multiview view-loop expansion: redirect every bound CB_COLOR
 * attachment and the DB depth/stencil attachment to render only to the
 * single array slice corresponding to the current view. Per the
 * VK_KHR_multiview specification, view N of viewMask writes to framebuffer
 * layer baseArrayLayer + N; on Evergreen / TeraScale-2 the per-slice
 * routing is encoded in CB_COLOR{M}_VIEW.SLICE_START (bits 0..10) and
 * SLICE_MAX (bits 13..23), with SLICE_START == SLICE_MAX restricting the
 * write to one layer. DB_DEPTH_VIEW uses the same SLICE_START / SLICE_MAX
 * field layout (see AMD 3D Engine Programming Guide for AMD Evergreen,
 * DB_DEPTH_VIEW register). Marks CB_COLOR_RTV and DB_DEPTH_STENCIL_BUFFER
 * state pending so the apply re-emits the descriptors. */
static void
terakan_set_view_attachment_layer(struct terakan_gfx_command_writer * const command_writer,
                                  uint32_t const view_index)
{
   struct terakan_state_draw * const state = &command_writer->state_draw;
   uint32_t const view_field =
      S_028C6C_SLICE_START(view_index) | S_028C6C_SLICE_MAX(view_index);

   for (uint32_t i = 0; i < TERAKAN_COLOR_HW_RTV_COUNT; ++i) {
      struct terakan_state_draw_cb_color * const cb = &state->cb_color_rtv.attachments[i];
      if (cb->bo != NULL) {
         cb->color.view = view_field;
      }
   }

   if (state->db_depth_stencil_buffer.bo != NULL) {
      state->db_depth_stencil_buffer.descriptor.view = view_field;
      terakan_state_draw_set_pending(state, TERAKAN_STATE_DRAW_INDEX_DB_DEPTH_STENCIL_BUFFER);
   }

   terakan_state_draw_set_pending(state, TERAKAN_STATE_DRAW_INDEX_CB_COLOR_RTV);
}

/* Emit the actual PKT3_DRAW_INDEX_AUTO for one (sub-)draw.  Called either
 * directly (single-view) or once per set bit in view_mask (multiview). */
static void
terakan_emit_draw_index_auto(struct terakan_gfx_command_writer * const command_writer,
                             uint32_t const vertexCount,
                             uint32_t const firstVertex, uint32_t const firstInstance)
{
   terakan_before_draw(command_writer);

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 3);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_DRAW_INDEX_AUTO, 3 - 2, 0);
   *packet++ = vertexCount;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX);
   if (unlikely(terakan_draw_debug_cs_dump_enabled(command_writer))) {
      fprintf(stderr,
              "terakan: pm4: PKT3_DRAW_INDEX_AUTO vertex_count=%u first_vertex=%u first_instance=%u\n",
              vertexCount, firstVertex, firstInstance);
   }
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDraw(VkCommandBuffer const commandBuffer, uint32_t const vertexCount,
                uint32_t const instanceCount, uint32_t const firstVertex,
                uint32_t const firstInstance)
{
   if (unlikely(instanceCount == 0)) {
      /* VGT_NUM_INSTANCES 0 is interpreted as 1. */
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   terakan_hw_state_draw_set_vgt_num_instances(&command_writer->hw_state_draw, instanceCount);
   terakan_set_vertex_instance_offsets(command_writer, firstVertex, firstInstance);

   if (command_writer->bound_graphics_pipeline != NULL &&
       command_writer->bound_graphics_pipeline->shaders[MESA_SHADER_VERTEX]
          .shader.vs_draw_parameters_enabled) {
      terakan_bind_draw_params(command_writer, firstVertex, firstInstance);
   }

   /* VK_KHR_multiview view-loop expansion.  When state->view_mask is
    * non-zero (set by terakan_CmdBeginRendering from
    * VkRenderingInfo.viewMask), the single draw expands into one draw
    * per set bit.  For each iteration, write the per-view index into
    * the driver push-constant slot consumed by the load_view_index NIR
    * lowering (which materializes gl_ViewIndex for the application),
    * and redirect every bound attachment to render only to its
    * corresponding array slice.  view_mask == 0 writes view_index 0 before
    * the single-view draw. */
   uint32_t view_mask = command_writer->state_draw.view_mask;
   if (view_mask != 0) {
      while (view_mask != 0) {
         uint32_t const view_idx = (uint32_t)__builtin_ctz(view_mask);
         view_mask &= view_mask - 1;
         terakan_set_view_index_push_constant(command_writer, view_idx);
         terakan_set_view_attachment_layer(command_writer, view_idx);
         terakan_emit_draw_index_auto(command_writer, vertexCount,
                                      firstVertex, firstInstance);
      }
   } else {
      terakan_set_view_index_push_constant(command_writer, 0);
      terakan_emit_draw_index_auto(command_writer, vertexCount,
                                   firstVertex, firstInstance);
   }
}

/* Emit one PKT3_DRAW_INDEX_OFFSET sub-draw.  Called either directly
 * (single-view) or once per set bit in view_mask (multiview). */
static void
terakan_emit_draw_index_offset(struct terakan_gfx_command_writer * const command_writer,
                               uint32_t const indexCount,
                               uint32_t const firstIndex)
{
   terakan_before_draw(command_writer);

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 4);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(EG_PKT3_DRAW_INDEX_OFFSET, 4 - 2, 0);
   *packet++ = firstIndex;
   *packet++ = indexCount;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_DMA);
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDrawIndexed(VkCommandBuffer const commandBuffer, uint32_t const indexCount,
                       uint32_t const instanceCount, uint32_t const firstIndex,
                       int32_t const vertexOffset, uint32_t const firstInstance)
{
   if (unlikely(instanceCount == 0)) {
      /* VGT_NUM_INSTANCES 0 is interpreted as 1. */
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   terakan_hw_state_draw_set_vgt_num_instances(&command_writer->hw_state_draw, instanceCount);
   terakan_set_vertex_instance_offsets(command_writer, (uint32_t)vertexOffset, firstInstance);

   if (command_writer->bound_graphics_pipeline != NULL &&
       command_writer->bound_graphics_pipeline->shaders[MESA_SHADER_VERTEX]
          .shader.vs_draw_parameters_enabled) {
      terakan_bind_draw_params(command_writer, (uint32_t)vertexOffset, firstInstance);
   }

   /* VK_KHR_multiview view-loop expansion for indexed draws.  When
    * view_mask != 0 the indexed draw expands to one
    * PKT3_DRAW_INDEX_OFFSET per set bit, updating the view_index push
    * constant and the per-attachment array slice before each.  view_mask == 0
    * writes view_index 0 before the single-view draw. */
   uint32_t view_mask = command_writer->state_draw.view_mask;
   if (view_mask != 0) {
      while (view_mask != 0) {
         uint32_t const view_idx = (uint32_t)__builtin_ctz(view_mask);
         view_mask &= view_mask - 1;
         terakan_set_view_index_push_constant(command_writer, view_idx);
         terakan_set_view_attachment_layer(command_writer, view_idx);
         terakan_emit_draw_index_offset(command_writer, indexCount, firstIndex);
      }
   } else {
      terakan_set_view_index_push_constant(command_writer, 0);
      terakan_emit_draw_index_offset(command_writer, indexCount, firstIndex);
   }
}
/*
 * GPU-driven indirect draw/dispatch for Terakan (TeraScale-2/Evergreen).
 *
 * Emits EG_PKT3_DRAW_INDIRECT / EG_PKT3_DRAW_INDEX_INDIRECT / PKT3_DISPATCH_INDIRECT
 * packets that instruct the Command Processor (CP) to fetch draw/dispatch parameters
 * directly from a GPU-resident buffer, eliminating CPU intervention from the inner loop.
 *
 * CRITICAL: A SURFACE_SYNC barrier must be emitted between any compute shader that
 * populates the indirect buffer and the indirect draw/dispatch that consumes it.
 * Without this, the CP may read stale cache data, causing a GPU hang with garbage parameters.
 *
 * Barrier flags for compute to indirect handover:
 *   TC_ACTION_ENA (flush texture cache - compute UAV writes go through TC)
 *   SH_ACTION_ENA (flush shader export cache)
 *   VC_ACTION_ENA (invalidate vertex cache - CP reads indirect params via VC)
 */

/* Emit drawCount PKT3_DRAW_{,INDEX_}INDIRECT packets against an indirect
 * buffer, optionally indexed.  Issues terakan_before_draw once at the
 * start so the helper can be invoked once per view-mask iteration with
 * the per-view CB/DB attachment-layer state freshly re-applied; the
 * outer multi-draw loop within the helper keeps the per-iteration state
 * model symmetric to the non-indirect path (terakan_emit_draw_index_*).
 *
 * VkDrawIndirectCommand layout (16 bytes):
 *   uint32_t vertexCount, instanceCount, firstVertex, firstInstance
 * VkDrawIndexedIndirectCommand layout (20 bytes):
 *   uint32_t indexCount, instanceCount, firstIndex, vertexOffset, firstInstance
 */
static uint32_t *
terakan_emit_copy_dw_mem_to_register(struct terakan_gfx_command_writer * const command_writer,
                                     uint32_t * packet, struct terakan_bo * const bo,
                                     uint64_t const source_va, uint32_t const dst_register)
{
   *packet++ = PKT3(PKT3_COPY_DW, 4, 0);
   *packet++ = COPY_DW_SRC_IS_MEM | COPY_DW_DST_IS_REG;
   uint32_t const * const packet_addr_lo = packet;
   *packet++ = (uint32_t)source_va;
   uint32_t const * const packet_addr_hi = packet;
   *packet++ = (uint32_t)((source_va >> 32) & 0xFF);
   *packet++ = dst_register >> 2;
   *packet++ = 0;

   /* COPY_DW uses the same 40-bit memory source operand shape as CP DMA. */
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_addr_lo, packet_addr_hi,
      TERASCALE_WDDM_PATCH_IDS_CP_DMA_SRC_LO, TERASCALE_WDDM_PATCH_IDS_CP_DMA_SRC_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                bo, true, false,
                                                TERAKAN_BO_PRIORITY_DRAW_INDIRECT));

   return packet;
}

static void
terakan_emit_draw_indirect_batch(struct terakan_gfx_command_writer * const command_writer,
                                 uint64_t const buffer_bo_offset,
                                 uint64_t const command_buffer_offset,
                                 uint64_t const buffer_size,
                                 uint32_t const drawCount, uint32_t const stride,
                                 struct terakan_bo * const bo, bool const indexed)
{
   terakan_before_draw(command_writer);

   uint32_t const draw_opcode =
      indexed ? EG_PKT3_DRAW_INDEX_INDIRECT : EG_PKT3_DRAW_INDIRECT;
   uint32_t const source_select =
      indexed ? V_0287F0_DI_SRC_SEL_DMA : V_0287F0_DI_SRC_SEL_AUTO_INDEX;
   uint64_t const draw_struct_size = indexed ? 20u : 16u;

   /* Evergreen CP reads only vertexCount/instanceCount from the indirect
    * buffer (Evergreen/Northern Islands 3D register reference
    * R_008970_VGT_INDEX_TYPE, PACKET3_DRAW_INDIRECT DW layout).  It does
    * NOT auto-load firstVertex into VGT_INDX_OFFSET (R_028408) or
    * firstInstance into SQ_VTX_START_INST_LOC (R_03CFF4).  COPY_DW loads
    * the two register values from the indirect BO at execution time so
    * compute, transfer, or host writes after command recording are visible
    * to the draw when the Vulkan synchronization chain makes them visible
    * to the command processor. */

   for (uint32_t draw_index = 0; draw_index < drawCount; ++draw_index) {
      struct terakan_draw_indirect_command_offsets draw_offsets;
      /* Vulkan bounds cover the buffer-relative command range.  Radeon
       * SET_BASE relocates the BO base, so the packet's 32-bit data_offset
       * also includes the buffer's VkBindBufferMemoryInfo::memoryOffset. */
      if (unlikely(!terakan_draw_indirect_command_offsets(
             buffer_bo_offset, command_buffer_offset, buffer_size,
             draw_index, stride, draw_struct_size, &draw_offsets))) {
         continue;
      }

      uint64_t const vertex_offset_field_offset =
         draw_offsets.buffer + sizeof(uint32_t) * (indexed ? 3u : 2u);
      uint64_t const instance_offset_field_offset =
         draw_offsets.buffer + sizeof(uint32_t) * (indexed ? 4u : 3u);

      /* PM4 payload per draw before relocation records (19 dwords):
       *   COPY_DW indirect vertex offset -> VGT_INDX_OFFSET     : 6 dwords
       *   COPY_DW indirect instance offset -> SQ_VTX_START_INST : 6 dwords
       *   SET_BASE                                              : 4 dwords
       *   DRAW_*_INDIRECT                                       : 3 dwords
       *
       * PKT3 count field = (total packet DWs) - 2.
       * Reloc dwords are allocated by emit_with_bo via
       * relocation_for_40_bits_count; they are NOT included in
       * packet_dwords. */
      uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 6 + 6 + 4 + 3, 1, 0, 3);
      if (unlikely(packet == NULL)) {
         return;
      }

      packet = terakan_emit_copy_dw_mem_to_register(
         command_writer, packet, bo, bo->va + buffer_bo_offset + vertex_offset_field_offset,
         R_028408_VGT_INDX_OFFSET);
      packet = terakan_emit_copy_dw_mem_to_register(
         command_writer, packet, bo, bo->va + buffer_bo_offset + instance_offset_field_offset,
         R_03CFF4_SQ_VTX_START_INST_LOC);

      *packet++ = PKT3(EG_PKT3_SET_BASE, 2, 0);
      *packet++ = 1; /* BASE_INDEX = DX11 Draw_Index_Indirect Patch Table */
      uint32_t const * const packet_addr_lo = packet;
      *packet++ = 0; /* ADDR_LO -- kernel patches from reloc */
      uint32_t const * const packet_addr_hi = packet;
      *packet++ = 0; /* ADDR_HI -- kernel patches from reloc */

      /* The DRM CS validator (evergreen_cs.c PACKET3_SET_BASE handler)
       * calls radeon_cs_packet_next_reloc() after advancing p->idx past
       * SET_BASE, expecting PKT3_NOP at that position before any DRAW
       * packet.  Emit the reloc NOP here, between SET_BASE and DRAW. */
      terakan_gfx_command_writer_add_relocation_for_40_bits(
         command_writer, &packet, packet_addr_lo, packet_addr_hi,
         0, 0,
         terakan_bo_reference_writer_add_reference(
            &command_writer->base.bo_reference_writer,
            bo, true, false, TERAKAN_BO_PRIORITY_DRAW_INDIRECT));

      *packet++ = PKT3(draw_opcode, 1, 0);
      *packet++ = draw_offsets.bo;
      *packet++ = S_0287F0_SOURCE_SELECT(source_select);

      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }

   /* COPY_DW makes the CP write these registers from the indirect BO.
    * Re-emit the tracked CPU-side values before the next draw that relies
    * on terakan_hw_state_draw rather than this indirect-load sequence. */
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_VGT_INDEX_OFFSET,
                                 true);
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_SQ_VTX_START_INST_LOC,
                                 true);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDrawIndirect(VkCommandBuffer const commandBuffer,
                        VkBuffer const bufferHandle,
                        VkDeviceSize const offset,
                        uint32_t const drawCount,
                        uint32_t const stride)
{
   if (unlikely(drawCount == 0)) {
      return;
   }

   struct terakan_command_buffer * const cmd =
      terakan_command_buffer_from_handle(commandBuffer);
   struct terakan_gfx_command_writer * const command_writer = cmd->command_writer.gfx;
   struct terakan_buffer const * const buffer = terakan_buffer_from_handle(bufferHandle);

   struct terakan_bo * const bo = (struct terakan_bo *)buffer->bo;
   assert(buffer->va >= bo->va);
   uint64_t const buffer_bo_offset = buffer->va - bo->va;

   /* VK_KHR_multiview view-loop expansion for indirect draws.  When
    * view_mask != 0 the full multi-draw batch (drawCount packets)
    * emits once per set bit, refreshing the view_index push constant
    * and the per-attachment array slice between iterations. */
   uint32_t view_mask = command_writer->state_draw.view_mask;
   if (view_mask != 0) {
      while (view_mask != 0) {
         uint32_t const view_idx = (uint32_t)__builtin_ctz(view_mask);
         view_mask &= view_mask - 1;
         terakan_set_view_index_push_constant(command_writer, view_idx);
         terakan_set_view_attachment_layer(command_writer, view_idx);
         terakan_emit_draw_indirect_batch(command_writer, buffer_bo_offset, offset,
                                          buffer->vk.size,
                                          drawCount, stride, bo, false);
      }
   } else {
      terakan_set_view_index_push_constant(command_writer, 0);
      terakan_emit_draw_indirect_batch(command_writer, buffer_bo_offset, offset,
                                       buffer->vk.size,
                                       drawCount, stride, bo, false);
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDrawIndexedIndirect(VkCommandBuffer const commandBuffer,
                               VkBuffer const bufferHandle,
                               VkDeviceSize const offset,
                               uint32_t const drawCount,
                               uint32_t const stride)
{
   if (unlikely(drawCount == 0)) {
      return;
   }

   struct terakan_command_buffer * const cmd =
      terakan_command_buffer_from_handle(commandBuffer);
   struct terakan_gfx_command_writer * const command_writer = cmd->command_writer.gfx;
   struct terakan_buffer const * const buffer = terakan_buffer_from_handle(bufferHandle);

   struct terakan_bo * const bo = (struct terakan_bo *)buffer->bo;
   assert(buffer->va >= bo->va);
   uint64_t const buffer_bo_offset = buffer->va - bo->va;

   /* VK_KHR_multiview view-loop expansion for indexed indirect draws.
    * Symmetric to terakan_CmdDrawIndirect; the drawCount-loop is
    * repeated per set bit in view_mask. */
   uint32_t view_mask = command_writer->state_draw.view_mask;
   if (view_mask != 0) {
      while (view_mask != 0) {
         uint32_t const view_idx = (uint32_t)__builtin_ctz(view_mask);
         view_mask &= view_mask - 1;
         terakan_set_view_index_push_constant(command_writer, view_idx);
         terakan_set_view_attachment_layer(command_writer, view_idx);
         terakan_emit_draw_indirect_batch(command_writer, buffer_bo_offset, offset,
                                          buffer->vk.size,
                                          drawCount, stride, bo, true);
      }
   } else {
      terakan_set_view_index_push_constant(command_writer, 0);
      terakan_emit_draw_indirect_batch(command_writer, buffer_bo_offset, offset,
                                       buffer->vk.size,
                                       drawCount, stride, bo, true);
   }
}
