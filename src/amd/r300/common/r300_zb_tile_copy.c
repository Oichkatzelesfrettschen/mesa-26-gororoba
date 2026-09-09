/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_zb_tile_copy.h"

#include <string.h>

static bool
mapping_valid(const struct r300_zb_tile_copy_mapping *mapping)
{
   if (mapping->tile_offset_bytes % R300_ZB_TILE_COPY_BYTES != 0u)
      return false;
   if (mapping->macro_width == 0u || mapping->macro_height == 0u ||
       mapping->macro_pitch < mapping->macro_width ||
       mapping->macro_x >= mapping->macro_width ||
       mapping->macro_y >= mapping->macro_height)
      return false;
   if (mapping->tile_offset_bytes > mapping->buffer_bytes ||
       R300_ZB_TILE_COPY_BYTES >
          mapping->buffer_bytes - mapping->tile_offset_bytes)
      return false;
   return true;
}

enum r300_zb_tile_copy_refusal
r300_zb_tile_copy_plan_build(const struct r300_zb_tile_copy_request *request,
                             struct r300_zb_tile_copy_plan *plan)
{
   if (request == NULL || plan == NULL)
      return R300_ZB_TILE_COPY_REFUSE_PLAN_NULL;
   if (!request->same_format)
      return R300_ZB_TILE_COPY_REFUSE_FORMAT;
   if (request->compressed)
      return R300_ZB_TILE_COPY_REFUSE_COMPRESSED;
   if (request->multisample)
      return R300_ZB_TILE_COPY_REFUSE_MULTISAMPLE;
   if (!mapping_valid(&request->source) ||
       !mapping_valid(&request->destination)) {
      if (request->source.tile_offset_bytes % R300_ZB_TILE_COPY_BYTES != 0u ||
          request->destination.tile_offset_bytes % R300_ZB_TILE_COPY_BYTES != 0u)
         return R300_ZB_TILE_COPY_REFUSE_OFFSET_ALIGNMENT;
      if (request->source.macro_pitch < request->source.macro_width ||
          request->destination.macro_pitch < request->destination.macro_width)
         return R300_ZB_TILE_COPY_REFUSE_MAPPING_PITCH;
      if (request->source.macro_width == 0u ||
          request->source.macro_height == 0u ||
          request->destination.macro_width == 0u ||
          request->destination.macro_height == 0u ||
          request->source.macro_x >= request->source.macro_width ||
          request->source.macro_y >= request->source.macro_height ||
          request->destination.macro_x >= request->destination.macro_width ||
          request->destination.macro_y >= request->destination.macro_height)
         return R300_ZB_TILE_COPY_REFUSE_MAPPING_BOUNDS;
      return R300_ZB_TILE_COPY_REFUSE_BUFFER_BOUNDS;
   }

   if (request->same_buffer) {
      const uint64_t source_end = request->source.tile_offset_bytes +
                                  R300_ZB_TILE_COPY_BYTES;
      const uint64_t destination_end = request->destination.tile_offset_bytes +
                                       R300_ZB_TILE_COPY_BYTES;
      if (request->source.tile_offset_bytes < destination_end &&
          request->destination.tile_offset_bytes < source_end)
         return R300_ZB_TILE_COPY_REFUSE_OVERLAP;
   }

   const uint32_t x_parity = request->source.macro_x ^
                             request->destination.macro_x;
   const uint32_t y_parity = request->source.macro_y ^
                             request->destination.macro_y;
   const uint32_t destination_xor = (x_parity & 1u) * 1024u |
                                    (y_parity & 1u) * 512u;
   const uint32_t segment_count = y_parity & 1u ? 4u :
                                  (x_parity & 1u ? 2u : 1u);
   const uint32_t segment_bytes = R300_ZB_TILE_COPY_BYTES / segment_count;

   struct r300_zb_tile_copy_plan candidate;
   memset(&candidate, 0, sizeof(candidate));
   candidate.segment_count = segment_count;
   for (uint32_t i = 0; i < segment_count; i++) {
      const uint32_t source_within = i * segment_bytes;
      candidate.segments[i].source_offset_bytes =
         request->source.tile_offset_bytes + source_within;
      candidate.segments[i].destination_offset_bytes =
         request->destination.tile_offset_bytes +
         (source_within ^ destination_xor);
      candidate.segments[i].byte_count = segment_bytes;
   }
   *plan = candidate;
   return R300_ZB_TILE_COPY_OK;
}
