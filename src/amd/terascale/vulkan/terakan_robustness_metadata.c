/*
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

/* Runtime population of KCACHE bank 14 robustness metadata.
 *
 * The write guard compiler code (in terakan_nir_lower_abi.c) emits KCACHE
 * reads from bank 14 to load per-UAV bounds for MEM_RAT write checks.
 * This module provides the runtime half: allocating a push-buffer BO,
 * populating it with the current per-slot UAV bounds, and binding it to
 * KCACHE bank 14 at draw/dispatch time.
 *
 * Layout (KCACHE bank 14, 1 line = 256 bytes):
 *   dword  0..11 : uint32_t ssbo_byte_sizes[12]
 *                   For STORAGE_BUFFER: exact byte count from Vulkan range.
 *   dword 12     : uint32_t trash_page_addr (GPU VA >> 2)
 *   dword 13..15 : reserved (zero)
 *   dword 16..27 : uint32_t texel_buffer_element_counts[12]
 *                   For STORAGE_TEXEL_BUFFER: element count from VkBufferView.
 *   dword 28..39 : uint32_t uav_base_array_layers[12]
 *                   baseArrayLayer of each
 *                   STORAGE_IMAGE UAV's VkImageView.  Read by NIR
 *                   lowering to inject the slice index into MEM_RAT
 *                   STORE_TYPED coord.z, compensating for Evergreen
 *                   hardware treating R3.z as the absolute slice index
 *                   and ignoring CB_COLOR_VIEW.SLICE_START on writes.
 *   dword 40..51 : uint32_t view_swizzles[12]  (2 bindings packed per dword)
 *                   Per-sampler-binding VkComponentMapping for
 *                   `terakan_nir_lower_tg4_view_swizzle`.  Each binding
 *                   packs as 16 bits = 4 channels x 4-bit targets, with
 *                   the channel-target encoding matching
 *                   VK_COMPONENT_SWIZZLE_R/G/B/A/ZERO/ONE (0..5).  AMD
 *                   Evergreen-Family ISA Chapter 6: SQ_TEX_RESOURCE_WORD4
 *                   DST_SEL permutes the four FETCH result lanes -- for
 *                   FETCH4 those lanes are spatial samples, so the
 *                   descriptor-side bake is wrong; the NIR pass uses
 *                   this metadata to pre-map the gather component
 *                   argument before emitting the FETCH4.
 *   dword 52..63 : reserved (zero)
 */

#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_hw_state.h"
#include "terakan_pipeline_compute.h"
#include "terakan_pipeline_graphics.h"

#include "util/u_debug.h"

#include <string.h>

void
terakan_robustness_metadata_apply(
   struct terakan_gfx_command_writer * const command_writer,
   bool const is_compute)
{
   BITSET_DECLARE(zero_needed,
                  TERAKAN_ROBUSTNESS_METADATA_MUTABLE_RESOURCE_COUNT) = {0};
   BITSET_WORD const *needed = zero_needed;
   unsigned mutable_resource_count =
      TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL;
   if (is_compute) {
      if (command_writer->bound_compute_pipeline != NULL) {
         needed = command_writer->bound_compute_pipeline->shader
                     .robustness_metadata_for_mutable_resources_needed;
      }
   } else {
      mutable_resource_count =
         TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL;
      if (command_writer->bound_graphics_pipeline != NULL &&
          (command_writer->bound_graphics_pipeline->shader_stages &
           VK_SHADER_STAGE_FRAGMENT_BIT)) {
         needed = command_writer->bound_graphics_pipeline
                     ->shaders[MESA_SHADER_FRAGMENT]
                     .robustness_metadata_for_mutable_resources_needed;
      }
   }

   struct terakan_robustness_metadata_payload compacted_payload;
   bool const compacted = terakan_robustness_metadata_compact(
      needed, mutable_resource_count,
      command_writer->robustness_metadata
         .mutable_resources[is_compute ? 1 : 0],
      &compacted_payload);
   if (unlikely(!compacted))
      memset(&compacted_payload, 0, sizeof(compacted_payload));
   if (memcmp(&command_writer->robustness_metadata.payload,
              &compacted_payload, sizeof(compacted_payload)) != 0) {
      command_writer->robustness_metadata.payload = compacted_payload;
      command_writer->robustness_metadata.dirty = true;
   }

   if (!command_writer->robustness_metadata.dirty &&
       command_writer->robustness_metadata.bo != NULL) {
      /* Data unchanged since last upload — just ensure KCACHE is bound to
       * any stages that haven't been bound yet. */
      VkShaderStageFlags const need_stages =
         is_compute ? VK_SHADER_STAGE_COMPUTE_BIT
                    : (VK_SHADER_STAGE_ALL_GRAPHICS);
      VkShaderStageFlags const missing =
         need_stages & ~command_writer->robustness_metadata.bound_to_stages;
      if (!missing)
         return;
      /* Fall through to bind the existing BO to new stages. */
   } else {
      /* Allocate and populate a fresh metadata buffer. */
      struct terakan_bo const *bo;
      uint32_t va_kcache_lines;
      uint32_t * const mapping = (uint32_t *)terakan_push_buffer_allocate_kcache(
         command_writer->base.command_buffer, TERAKAN_KCACHE_HW_LINE_BYTES,
         &bo, &va_kcache_lines);
      if (unlikely(mapping == NULL))
         return;

      /* Zero the entire KCACHE line (256 bytes = 64 dwords).
       * Then fill the per-UAV byte sizes and texel buffer element counts. */
      memset(mapping, 0, TERAKAN_KCACHE_HW_LINE_BYTES);

      /* PROBE_FILL_LINE (): if set, overwrite the entire
       * 256-byte KCACHE line with 0xDEADBEEF AFTER the field writes
       * below.  If shader's KC14 read returns 0xDEADBEEF, the BO IS
       * being fetched (residual is a slot-offset bug); if shader still
       * returns 0, the BO is not being fetched at all. */
      bool const probe_fill_line =
         debug_get_bool_option("TERAKAN_PROBE_FILL_LINE", false);

      /* Dwords 0..11: SSBO byte sizes. */
      memcpy(mapping,
             command_writer->robustness_metadata.payload.uav_byte_sizes,
             sizeof(command_writer->robustness_metadata.payload.uav_byte_sizes));
      if (debug_get_bool_option("TERAKAN_DEBUG_ROBUSTNESS_METADATA", false)) {
         fprintf(stderr, "terakan/robustness_metadata: uav_byte_sizes[0..5] = %u %u %u %u %u %u\n",
                 command_writer->robustness_metadata.payload.uav_byte_sizes[0],
                 command_writer->robustness_metadata.payload.uav_byte_sizes[1],
                 command_writer->robustness_metadata.payload.uav_byte_sizes[2],
                 command_writer->robustness_metadata.payload.uav_byte_sizes[3],
                 command_writer->robustness_metadata.payload.uav_byte_sizes[4],
                 command_writer->robustness_metadata.payload.uav_byte_sizes[5]);
         fprintf(stderr,
                 "terakan/robustness_metadata: uav_base_array_layers[0..5] = %u %u %u %u %u %u\n",
                 command_writer->robustness_metadata.payload.uav_base_array_layers[0],
                 command_writer->robustness_metadata.payload.uav_base_array_layers[1],
                 command_writer->robustness_metadata.payload.uav_base_array_layers[2],
                 command_writer->robustness_metadata.payload.uav_base_array_layers[3],
                 command_writer->robustness_metadata.payload.uav_base_array_layers[4],
                 command_writer->robustness_metadata.payload.uav_base_array_layers[5]);
      }
      /* Dwords 16..27: texel buffer element counts. */
      memcpy(mapping + 16,
             command_writer->robustness_metadata.payload.texel_buffer_element_counts,
             sizeof(command_writer->robustness_metadata.payload.texel_buffer_element_counts));
      /* Dwords 28..39: per-UAV baseArrayLayer for slice-coordinate lowering. */
      memcpy(mapping + 28,
             command_writer->robustness_metadata.payload.uav_base_array_layers,
             sizeof(command_writer->robustness_metadata.payload.uav_base_array_layers));
      /* Dwords 40..51: per-sampler-binding VkComponentMapping pack
       * (two bindings per dword, 16 bits each).  Consumed by
       * terakan_nir_lower_tg4_view_swizzle to pre-map the gather
       * component argument before FETCH4 emission. */
      memcpy(mapping + 40,
             command_writer->robustness_metadata.view_swizzles,
             sizeof(command_writer->robustness_metadata.view_swizzles));
      /* dword 12: trash_page_addr — GPU VA >> 2 of the driver-owned trash page.
       * Used by math-predication write guards to redirect OOB writes to a safe
       * garbage sink instead of offset 0 of the target buffer. */
      {
         struct terakan_device const * const device =
            terakan_gfx_command_writer_device(command_writer);
         ((uint32_t *)mapping)[12] = device->robustness_trash_page_va_shr2;
      }

      /* Apply PROBE_FILL_LINE: blanket-fill 0xDEADBEEF across all 64
       * dwords AFTER the field-specific writes.  Overrides everything
       * so the shader's KC14 read should return 0xDEADBEEF if it
       * actually fetches from this BO. */
      if (probe_fill_line) {
         for (uint32_t i = 0; i < TERAKAN_KCACHE_HW_LINE_BYTES / 4u; ++i) {
            ((uint32_t *)mapping)[i] = 0xDEADBEEFu;
         }
         fprintf(stderr,
            "TERAKAN_PROBE_FILL_LINE: filled 256-byte line with 0xDEADBEEF\n");
      }

      /* flush CPU store buffers before the IB
       * consumer submits.  Push-buffer allocations are GTT-backed and
       * may be write-combine-mapped; CPU writes can sit in the WC
       * buffer indefinitely, invisible to the GPU fetch.  __builtin_
       * ia32_sfence() is an SSE-guaranteed baseline fence on x86_64
       * (Sumo is x86_64); it orders all prior store instructions
       * before any subsequent store, which is what the GPU-visibility
       * guarantee requires here.
       *
       * Post-reloc IB capture 2026-04-19 (steinmarder
       * findings/) falsified H2
       * (kernel reloc parser); this is H1 (WC race) under test.
       *
       * Gated behind TERAKAN_FIX_O_SFENCE=1 during validation.
       * Promote to default once the 78-test single_layer sweep
       * turns green. */
      static int fix_o_cached = -1;
      if (fix_o_cached < 0) {
         fix_o_cached = debug_get_bool_option("TERAKAN_FIX_O_SFENCE", false) ? 1 : 0;
      }
      if (fix_o_cached) {
         __builtin_ia32_sfence();
      }

      command_writer->robustness_metadata.bo = bo;
      command_writer->robustness_metadata.va_kcache_lines = va_kcache_lines;
      command_writer->robustness_metadata.dirty = false;
      command_writer->robustness_metadata.bound_to_stages = 0;
   }

   /* Bind KCACHE bank 14 to all needed stages. */
   VkShaderStageFlags const bind_stages =
      (is_compute ? VK_SHADER_STAGE_COMPUTE_BIT
                  : (VK_SHADER_STAGE_ALL_GRAPHICS)) &
      ~command_writer->robustness_metadata.bound_to_stages;
   if (bind_stages) {
      unsigned remaining = (unsigned)bind_stages;
      while (remaining) {
         mesa_shader_stage const stage_index = vk_to_mesa_shader_stage(
            (VkShaderStageFlagBits)((VkShaderStageFlags)1 << u_bit_scan(&remaining)));
         /* Compute uses the FS KCACHE slot (SQC convention). */
         mesa_shader_stage const sqc_stage =
            (stage_index == MESA_SHADER_COMPUTE) ? MESA_SHADER_FRAGMENT : stage_index;
         terakan_hw_state_sqc_set_kcache_for_stage[sqc_stage](
            &command_writer->hw_state_sqc,
            TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA,
            1,  /* size_lines: 1 KCACHE line (256 bytes) */
            command_writer->robustness_metadata.bo,
            command_writer->robustness_metadata.va_kcache_lines);
      }
      command_writer->robustness_metadata.bound_to_stages |= bind_stages;
   }
}
