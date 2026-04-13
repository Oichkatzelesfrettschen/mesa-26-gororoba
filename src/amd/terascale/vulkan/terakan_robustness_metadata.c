/*
 * Copyright © 2026 Eirikr Hinngart
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
 *   dword 28..63 : reserved (zero)
 */

#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_hw_state.h"

#include <string.h>

void
terakan_robustness_metadata_apply(
   struct terakan_gfx_command_writer * const command_writer,
   bool const is_compute)
{
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
      /* Dwords 0..11: SSBO byte sizes. */
      memcpy(mapping,
             command_writer->robustness_metadata.uav_byte_sizes,
             sizeof(command_writer->robustness_metadata.uav_byte_sizes));
      fprintf(stderr, "TERAKAN_METADATA: uav_byte_sizes[0..5] = %u %u %u %u %u %u\n",
              command_writer->robustness_metadata.uav_byte_sizes[0],
              command_writer->robustness_metadata.uav_byte_sizes[1],
              command_writer->robustness_metadata.uav_byte_sizes[2],
              command_writer->robustness_metadata.uav_byte_sizes[3],
              command_writer->robustness_metadata.uav_byte_sizes[4],
              command_writer->robustness_metadata.uav_byte_sizes[5]);
      /* Dwords 16..27: texel buffer element counts. */
      memcpy(mapping + 16,
             command_writer->robustness_metadata.texel_buffer_element_counts,
             sizeof(command_writer->robustness_metadata.texel_buffer_element_counts));
      /* dword 12: trash_page_addr — GPU VA >> 2 of the driver-owned trash page.
       * Used by math-predication write guards to redirect OOB writes to a safe
       * garbage sink instead of offset 0 of the target buffer. */
      {
         struct terakan_device const * const device =
            terakan_gfx_command_writer_device(command_writer);
         ((uint32_t *)mapping)[12] = device->robustness_trash_page_va_shr2;
      }

      command_writer->robustness_metadata.bo = bo;
      command_writer->robustness_metadata.va_kcache_lines = va_kcache_lines;
      command_writer->robustness_metadata.dirty = false;
      command_writer->robustness_metadata.bound_to_stages = 0;
   }

   /* Bind KCACHE bank 14 to all needed stages.
    *
    * For compute: skip the deferred hw_state binding.  The deferred path
    * emits to PS registers (ALU_CONST_CACHE_PS_14) but compute reads from
    * LS registers (ALU_CONST_CACHE_LS_14).  Additionally, the deferred
    * state is never flushed before DISPATCH_DIRECT — the compute path
    * uses manual PM4 emission exclusively.  The caller (CmdDispatch) is
    * responsible for emitting LS KCACHE bank 14 with COMPUTE_MODE_BIT
    * via terakan_emit_compute_kcache_bank(). */
   VkShaderStageFlags const bind_stages =
      (is_compute ? VK_SHADER_STAGE_COMPUTE_BIT
                  : (VK_SHADER_STAGE_ALL_GRAPHICS)) &
      ~command_writer->robustness_metadata.bound_to_stages;
   if (bind_stages) {
      if (!is_compute) {
         unsigned remaining = (unsigned)bind_stages;
         while (remaining) {
            mesa_shader_stage const stage_index = vk_to_mesa_shader_stage(
               (VkShaderStageFlagBits)((VkShaderStageFlags)1 << u_bit_scan(&remaining)));
            terakan_hw_state_sqc_set_kcache_for_stage[stage_index](
               &command_writer->hw_state_sqc,
               TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA,
               1,  /* size_lines: 1 KCACHE line (256 bytes) */
               command_writer->robustness_metadata.bo,
               command_writer->robustness_metadata.va_kcache_lines);
         }
      }
      command_writer->robustness_metadata.bound_to_stages |= bind_stages;
   }
}
