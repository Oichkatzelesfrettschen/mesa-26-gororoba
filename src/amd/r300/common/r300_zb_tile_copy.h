/*
 * SPDX-License-Identifier: MIT
 *
 * A plan describes mathematical byte spans inside complete 2 KiB macrotiles.
 * The offsets are not RB2D register payloads: callers must separately encode
 * and validate any base field required by an RB2D command.
 */

#ifndef R300_ZB_TILE_COPY_H
#define R300_ZB_TILE_COPY_H

#include <stdbool.h>
#include <stdint.h>

#define R300_ZB_TILE_COPY_BYTES 2048u
#define R300_ZB_TILE_COPY_MAX_SEGMENTS 4u

struct r300_zb_tile_copy_mapping {
   uint64_t tile_offset_bytes;
   uint64_t buffer_bytes;
   uint32_t macro_x;
   uint32_t macro_y;
   uint32_t macro_width;
   uint32_t macro_height;
   uint32_t macro_pitch;
};

struct r300_zb_tile_copy_request {
   struct r300_zb_tile_copy_mapping source;
   struct r300_zb_tile_copy_mapping destination;
   bool same_buffer;
   bool same_format;
   bool compressed;
   bool multisample;
};

struct r300_zb_tile_copy_segment {
   uint64_t source_offset_bytes;
   uint64_t destination_offset_bytes;
   uint32_t byte_count;
};

struct r300_zb_tile_copy_plan {
   uint32_t segment_count;
   struct r300_zb_tile_copy_segment segments[R300_ZB_TILE_COPY_MAX_SEGMENTS];
};

enum r300_zb_tile_copy_refusal {
   R300_ZB_TILE_COPY_OK = 0,
   R300_ZB_TILE_COPY_REFUSE_PLAN_NULL,
   R300_ZB_TILE_COPY_REFUSE_FORMAT,
   R300_ZB_TILE_COPY_REFUSE_COMPRESSED,
   R300_ZB_TILE_COPY_REFUSE_MULTISAMPLE,
   R300_ZB_TILE_COPY_REFUSE_OFFSET_ALIGNMENT,
   R300_ZB_TILE_COPY_REFUSE_MAPPING_BOUNDS,
   R300_ZB_TILE_COPY_REFUSE_MAPPING_PITCH,
   R300_ZB_TILE_COPY_REFUSE_BUFFER_BOUNDS,
   R300_ZB_TILE_COPY_REFUSE_OVERLAP,
};

enum r300_zb_tile_copy_refusal
r300_zb_tile_copy_plan_build(const struct r300_zb_tile_copy_request *request,
                             struct r300_zb_tile_copy_plan *plan);

#endif
