/* SPDX-License-Identifier: MIT */

#ifndef R300_ZB_ASPECT_COPY_H
#define R300_ZB_ASPECT_COPY_H

#include "r300_rb2d_copy.h"
#include "r300_zb_depth_layout.h"

#include <stdbool.h>
#include <stdint.h>

enum r300_zb_aspect_copy_aspect {
   R300_ZB_ASPECT_COPY_DEPTH,
   R300_ZB_ASPECT_COPY_STENCIL,
};

enum r300_zb_aspect_copy_direction {
   R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE,
   R300_ZB_ASPECT_COPY_SURFACE_TO_BUFFER,
};

enum r300_zb_aspect_copy_refusal {
   R300_ZB_ASPECT_COPY_OK = 0,
   R300_ZB_ASPECT_COPY_REFUSE_INPUT,
   R300_ZB_ASPECT_COPY_REFUSE_FORMAT,
   R300_ZB_ASPECT_COPY_REFUSE_ASPECT,
   R300_ZB_ASPECT_COPY_REFUSE_SURFACE_BOUNDS,
   R300_ZB_ASPECT_COPY_REFUSE_BUFFER_BOUNDS,
   R300_ZB_ASPECT_COPY_REFUSE_OVERFLOW,
};

/* buffer_offset_bytes identifies the first byte of the aspect buffer:
 * X8_D24_UNORM_PACK32 for depth (the X byte is unused), or S8_UINT for
 * stencil. buffer_pitch_bytes is the corresponding row pitch. */
enum r300_zb_aspect_copy_refusal r300_zb_aspect_copy_plan(
   const struct r300_zb_depth_surface *surface,
   uint64_t surface_base_bytes, uint64_t mapped_surface_bytes,
   uint32_t x, uint32_t y, uint64_t buffer_offset_bytes,
   uint32_t buffer_pitch_bytes, uint64_t buffer_bytes,
   enum r300_zb_aspect_copy_aspect aspect,
   enum r300_zb_aspect_copy_direction direction,
   struct r300_rb2d_copy_segment *out);

#endif
