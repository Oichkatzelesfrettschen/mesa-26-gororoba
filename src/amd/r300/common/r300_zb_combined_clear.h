/* SPDX-License-Identifier: MIT */

#ifndef R300_ZB_COMBINED_CLEAR_H
#define R300_ZB_COMBINED_CLEAR_H

#include "r300_rb2d_fill.h"
#include "r300_zb_depth_layout.h"

#include <stdint.h>

#define R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH 1u
#define R300_ZB_COMBINED_CLEAR_ASPECT_STENCIL 2u
#define R300_ZB_COMBINED_CLEAR_ASPECTS \
   (R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH | R300_ZB_COMBINED_CLEAR_ASPECT_STENCIL)

struct r300_zb_combined_clear_request {
   const struct r300_zb_depth_surface *surface;
   uint64_t surface_base_bytes;
   uint64_t mapped_surface_bytes;
   uint32_t pitch_bytes;
   enum r300_rb2d_format format;
   uint32_t aspect_mask;
   uint32_t depth_code;
   uint32_t stencil;
};

struct r300_zb_combined_clear_plan {
   struct r300_rb2d_fill_rect rect;
   struct r300_rb2d_fill_plan fill;
   uint32_t packed_word;
};

enum r300_zb_combined_clear_refusal {
   R300_ZB_COMBINED_CLEAR_OK = 0,
   R300_ZB_COMBINED_CLEAR_REFUSE_INPUT,
   R300_ZB_COMBINED_CLEAR_REFUSE_SURFACE,
   R300_ZB_COMBINED_CLEAR_REFUSE_BASE,
   R300_ZB_COMBINED_CLEAR_REFUSE_PITCH,
   R300_ZB_COMBINED_CLEAR_REFUSE_FORMAT,
   R300_ZB_COMBINED_CLEAR_REFUSE_ASPECT,
   R300_ZB_COMBINED_CLEAR_REFUSE_DEPTH,
   R300_ZB_COMBINED_CLEAR_REFUSE_STENCIL,
   R300_ZB_COMBINED_CLEAR_REFUSE_ENVELOPE,
   R300_ZB_COMBINED_CLEAR_REFUSE_OVERFLOW,
};

enum r300_zb_combined_clear_refusal
r300_zb_combined_clear_plan(const struct r300_zb_combined_clear_request *request,
                            struct r300_zb_combined_clear_plan *out);

#endif
