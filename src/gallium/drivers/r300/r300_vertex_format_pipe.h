/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_VERTEX_FORMAT_PIPE_H
#define R300_VERTEX_FORMAT_PIPE_H

#include <stdbool.h>

#include "amd/r300/common/r300_r2vb_source_contract.h"
#include "util/format/u_formats.h"

/* Gallium adapter only.  The neutral format record carries no pipe_format
 * dependency and no route-admission decision. */
static inline bool
r300_vertex_format_from_pipe(enum pipe_format format,
                              enum r300_vertex_format_id *out)
{
   if (!out)
      return false;

   switch (format) {
   case PIPE_FORMAT_R32_FLOAT:
      *out = R300_VERTEX_FORMAT_F32_1;
      return true;
   case PIPE_FORMAT_R32G32_FLOAT:
      *out = R300_VERTEX_FORMAT_F32_2;
      return true;
   case PIPE_FORMAT_R32G32B32_FLOAT:
      *out = R300_VERTEX_FORMAT_F32_3;
      return true;
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
      *out = R300_VERTEX_FORMAT_F32_4;
      return true;
   default:
      *out = R300_VERTEX_FORMAT_INVALID;
      return false;
   }
}

static inline enum pipe_format
r300_vertex_format_to_pipe(enum r300_vertex_format_id format)
{
   switch (format) {
   case R300_VERTEX_FORMAT_F32_1:
      return PIPE_FORMAT_R32_FLOAT;
   case R300_VERTEX_FORMAT_F32_2:
      return PIPE_FORMAT_R32G32_FLOAT;
   case R300_VERTEX_FORMAT_F32_3:
      return PIPE_FORMAT_R32G32B32_FLOAT;
   case R300_VERTEX_FORMAT_F32_4:
      return PIPE_FORMAT_R32G32B32A32_FLOAT;
   default:
      return PIPE_FORMAT_NONE;
   }
}

/* Gallium bridge for the complete source contract.  Route policy remains in
 * the neutral contract; this adapter contributes only pipe_format identity. */
static inline bool
r300_r2vb_source_contract_from_pipe(
   enum pipe_format format,
   bool float2_enabled,
   uint64_t allocation_bytes,
   uint64_t buffer_offset,
   uint64_t attribute_offset,
   uint32_t stride_bytes,
   uint32_t start,
   uint32_t count,
   struct r300_r2vb_source_contract *out)
{
   enum r300_vertex_format_id neutral = R300_VERTEX_FORMAT_INVALID;
   return r300_vertex_format_from_pipe(format, &neutral) &&
          r300_r2vb_source_contract_init(
             neutral, float2_enabled, allocation_bytes, buffer_offset,
             attribute_offset, stride_bytes, start, count, out);
}

#endif /* R300_VERTEX_FORMAT_PIPE_H */
