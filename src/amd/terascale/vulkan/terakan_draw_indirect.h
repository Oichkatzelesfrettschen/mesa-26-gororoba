/* SPDX-License-Identifier: MIT */

#ifndef TERAKAN_DRAW_INDIRECT_H
#define TERAKAN_DRAW_INDIRECT_H

#include <stdbool.h>
#include <stdint.h>

struct terakan_draw_indirect_command_offsets {
   /* Vulkan buffer-relative byte offset used for buffer-size validation. */
   uint64_t buffer;
   /* BO-relative byte offset encoded in DRAW_*_INDIRECT.data_offset. */
   uint32_t bo;
};

static inline bool
terakan_draw_indirect_command_offsets(
   uint64_t const buffer_bo_offset, uint64_t const command_buffer_offset,
   uint64_t const buffer_size, uint32_t const draw_index, uint32_t const stride,
   uint64_t const command_size, struct terakan_draw_indirect_command_offsets * const offsets_out)
{
   uint64_t const draw_stride_offset = (uint64_t)draw_index * stride;
   if (command_buffer_offset > UINT64_MAX - draw_stride_offset) {
      return false;
   }

   uint64_t const buffer_offset = command_buffer_offset + draw_stride_offset;
   if (buffer_offset > buffer_size || command_size > buffer_size - buffer_offset) {
      return false;
   }

   if (buffer_bo_offset > UINT32_MAX || buffer_offset > UINT32_MAX - buffer_bo_offset) {
      return false;
   }

   offsets_out->buffer = buffer_offset;
   offsets_out->bo = (uint32_t)(buffer_bo_offset + buffer_offset);
   return true;
}

#endif
