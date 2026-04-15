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

#include "terakan_barrier.h"
#include "terakan_buffer.h"
#include "terakan_command_buffer.h"
#include "terakan_entrypoints.h"
#include "terakan_pipeline_graphics.h"

#include "amd/terascale/common/terascale_evergreend.h"
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
      /* HOST_WRITE barriers accumulate INV_TC | INV_SH so the *application* draw
       * fetches freshly-written host data from RAM instead of a stale cache line.
       * Internal meta draws (render pass clear, blit, query) execute between the
       * barrier record and the application draw.  If we let the meta draw consume
       * and discard those invalidation bits, the application draw runs with no
       * SURFACE_SYNC and still sees stale cached data.
       *
       * Fix: save the read-cache-invalidation bits before emitting the pending
       * barrier for the meta draw, then restore them so they will be re-emitted
       * before the next application draw.  Emitting an extra SURFACE_SYNC(TC|SH)
       * before the application draw is always safe; the overhead is negligible.
       */
      enum terakan_barrier_action_flags const saved_inv =
         command_writer->pending_barrier_actions &
         (TERAKAN_BARRIER_ACTION_INV_TC | TERAKAN_BARRIER_ACTION_INV_SH |
          TERAKAN_BARRIER_ACTION_INV_VC);
      terakan_barrier_emit_pending_actions(command_writer);
      command_writer->pending_barrier_actions |= saved_inv;
   } else {
      terakan_barrier_emit_pending_actions(command_writer);
   }
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
    * robustness reads).  Evergreen ISA §4.6.4: banks >= 14 are NOT
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
/*
 * GPU-driven indirect draw/dispatch for Terakan (TeraScale-2/Evergreen).
 *
 * Emits EG_PKT3_DRAW_INDIRECT / EG_PKT3_DRAW_INDEX_INDIRECT / PKT3_DISPATCH_INDIRECT
 * packets that instruct the Command Processor (CP) to fetch draw/dispatch parameters
 * directly from a GPU-resident buffer, eliminating CPU intervention from the inner loop.
 *
 * CRITICAL: A SURFACE_SYNC barrier must be emitted between any compute shader that
 * populates the indirect buffer and the indirect draw/dispatch that consumes it.
 * Without this, the CP may read stale cache data → GPU hang with garbage parameters.
 *
 * Barrier flags for compute → indirect handover:
 *   TC_ACTION_ENA (flush texture cache — compute UAV writes go through TC)
 *   SH_ACTION_ENA (flush shader export cache)
 *   VC_ACTION_ENA (invalidate vertex cache — CP reads indirect params via VC)
 */

/* --- vkCmdDrawIndirect --- */

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

   uint64_t const buffer_va = buffer->va + offset;
   struct terakan_bo * const bo = (struct terakan_bo *)buffer->bo;

   terakan_before_draw(command_writer);

   /* For each draw in the multi-draw, emit:
    * 1. Set VGT_DMA_BASE / VGT_DMA_BASE_HI to the indirect buffer address
    * 2. EG_PKT3_DRAW_INDIRECT with the byte offset for this draw's parameters
    *
    * VkDrawIndirectCommand layout (16 bytes):
    *   uint32_t vertexCount, instanceCount, firstVertex, firstInstance
    */
   for (uint32_t i = 0; i < drawCount; i++) {
      uint64_t const draw_va = buffer_va + (uint64_t)i * stride;

      /* Set VGT_DMA_BASE_HI + VGT_DMA_BASE (config regs at 0x287E4, 0x287E8) */
      uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 4 + 3, 1, 0, 1);
      if (unlikely(packet == NULL)) {
         return;
      }

      /* SET_CONFIG_REG for VGT_DMA_BASE_HI and VGT_DMA_BASE */
      *packet++ = PKT3(PKT3_SET_CONFIG_REG, 3, 0);
      *packet++ = (R_0287E4_VGT_DMA_BASE_HI - 0x8000) >> 2;
      uint32_t const * const packet_base_hi = packet;
      *packet++ = (uint32_t)(draw_va >> 32) & 0xFF;  /* VGT_DMA_BASE_HI */
      uint32_t const * const packet_base_lo = packet;
      *packet++ = (uint32_t)draw_va;                   /* VGT_DMA_BASE */

      /* DRAW_INDIRECT packet */
      *packet++ = PKT3(EG_PKT3_DRAW_INDIRECT, 2, 0);
      *packet++ = 0; /* data_offset (0 since address is already set) */
      *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX);

      /* Register BO relocation for the indirect buffer address */
      terakan_gfx_command_writer_add_relocation_for_40_bits(
         command_writer, &packet, packet_base_lo, packet_base_hi,
         0, 0, /* WDDM patch IDs — 0 for DRM path */
         terakan_bo_reference_writer_add_reference(
            &command_writer->base.bo_reference_writer,
            bo, true, false, TERAKAN_BO_PRIORITY_DRAW_INDIRECT));

      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }
}

/* --- vkCmdDrawIndexedIndirect --- */

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

   uint64_t const buffer_va = buffer->va + offset;
   struct terakan_bo * const bo = (struct terakan_bo *)buffer->bo;

   terakan_before_draw(command_writer);

   /* VkDrawIndexedIndirectCommand layout (20 bytes):
    *   uint32_t indexCount, instanceCount, firstIndex, vertexOffset, firstInstance
    */
   for (uint32_t i = 0; i < drawCount; i++) {
      uint64_t const draw_va = buffer_va + (uint64_t)i * stride;

      uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 4 + 3, 1, 0, 1);
      if (unlikely(packet == NULL)) {
         return;
      }

      /* SET_CONFIG_REG for VGT_DMA_BASE_HI and VGT_DMA_BASE */
      *packet++ = PKT3(PKT3_SET_CONFIG_REG, 3, 0);
      *packet++ = (R_0287E4_VGT_DMA_BASE_HI - 0x8000) >> 2;
      uint32_t const * const packet_base_hi = packet;
      *packet++ = (uint32_t)(draw_va >> 32) & 0xFF;
      uint32_t const * const packet_base_lo = packet;
      *packet++ = (uint32_t)draw_va;

      /* DRAW_INDEX_INDIRECT packet */
      *packet++ = PKT3(EG_PKT3_DRAW_INDEX_INDIRECT, 2, 0);
      *packet++ = 0; /* data_offset */
      *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_DMA);

      terakan_gfx_command_writer_add_relocation_for_40_bits(
         command_writer, &packet, packet_base_lo, packet_base_hi,
         0, 0,
         terakan_bo_reference_writer_add_reference(
            &command_writer->base.bo_reference_writer,
            bo, true, false, TERAKAN_BO_PRIORITY_DRAW_INDIRECT));

      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }
}
