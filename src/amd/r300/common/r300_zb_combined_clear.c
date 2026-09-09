/* SPDX-License-Identifier: MIT */

#include "r300_zb_combined_clear.h"

#include <limits.h>
#include <stddef.h>

enum r300_zb_combined_clear_refusal
r300_zb_combined_clear_plan(const struct r300_zb_combined_clear_request *request,
                            struct r300_zb_combined_clear_plan *out)
{
   if (request == NULL || out == NULL)
      return R300_ZB_COMBINED_CLEAR_REFUSE_INPUT;
   if (request->surface != &r300_zb_depth_surface_rs485m_z24_macrotiled_logical)
      return R300_ZB_COMBINED_CLEAR_REFUSE_SURFACE;

   struct r300_zb_depth_layout layout;
   if (r300_zb_depth_layout_compute(request->surface,
                                    R300_ZB_DEPTH_GUARD_BYTES, &layout) != 0)
      return R300_ZB_COMBINED_CLEAR_REFUSE_SURFACE;
   if (request->surface_base_bytes != layout.base_offset_bytes)
      return R300_ZB_COMBINED_CLEAR_REFUSE_BASE;
   if (request->pitch_bytes != 256u ||
       request->pitch_bytes % R300_RB2D_PITCH_GRANULARITY != 0)
      return R300_ZB_COMBINED_CLEAR_REFUSE_PITCH;
   if (request->format != R300_RB2D_FORMAT_ARGB8888)
      return R300_ZB_COMBINED_CLEAR_REFUSE_FORMAT;
   if (request->aspect_mask != R300_ZB_COMBINED_CLEAR_ASPECTS)
      return R300_ZB_COMBINED_CLEAR_REFUSE_ASPECT;
   if (request->depth_code > 0x00ffffffu)
      return R300_ZB_COMBINED_CLEAR_REFUSE_DEPTH;
   if (request->stencil > 0xffu)
      return R300_ZB_COMBINED_CLEAR_REFUSE_STENCIL;
   if (layout.total_bytes > UINT64_MAX - request->surface_base_bytes ||
       request->mapped_surface_bytes <
          request->surface_base_bytes + layout.total_bytes)
      return R300_ZB_COMBINED_CLEAR_REFUSE_ENVELOPE;

   uint32_t word;
   if (r300_zb_depth_pack(request->surface, request->depth_code,
                          request->stencil, &word) != 0)
      return R300_ZB_COMBINED_CLEAR_REFUSE_DEPTH;

   const struct r300_rb2d_fill_rect rect = {
      .x = 0u, .y = 0u, .width = layout.width, .height = layout.height,
      .value = word,
   };
   const struct r300_rb2d_fill_plan fill = {
      .surface = {
         .base_offset_bytes = (uint32_t)request->surface_base_bytes,
         .pitch_bytes = request->pitch_bytes,
         .width_pixels = layout.width,
         .height_pixels = layout.height,
         .format = request->format,
      },
      .write_mask = 0xfu,
      .rects = &rect,
      .rect_count = 1u,
   };
   if (r300_rb2d_fill_plan_check(&fill) != R300_RB2D_FILL_OK)
      return R300_ZB_COMBINED_CLEAR_REFUSE_OVERFLOW;
   *out = (struct r300_zb_combined_clear_plan){
      .rect = rect, .fill = fill, .packed_word = word,
   };
   out->fill.rects = &out->rect;
   return R300_ZB_COMBINED_CLEAR_OK;
}
