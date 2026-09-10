/* SPDX-License-Identifier: MIT */

#include "r300_zb_combined_clear.h"

#include <limits.h>
#include <stddef.h>

static bool
is_rs485m_z24_macrotiled_logical(
   const struct r300_zb_depth_surface *surface)
{
   const struct r300_zb_depth_surface *qualified =
      &r300_zb_depth_surface_rs485m_z24_macrotiled_logical;
   return surface != NULL &&
          surface->address_resolver == qualified->address_resolver &&
          surface->depth_format == qualified->depth_format &&
          surface->bytes_per_pixel == qualified->bytes_per_pixel &&
          surface->microtile == qualified->microtile &&
          surface->macrotile == qualified->macrotile &&
          surface->width == qualified->width &&
          surface->height == qualified->height &&
          surface->pitch_pixels == qualified->pitch_pixels &&
          surface->allocation_rows == qualified->allocation_rows &&
          surface->depth_sentinel_code == qualified->depth_sentinel_code &&
          surface->raw_allocation_mapping ==
             qualified->raw_allocation_mapping &&
          surface->uniform_packed_initialization ==
             qualified->uniform_packed_initialization &&
          surface->logical_pixel_addressing ==
             qualified->logical_pixel_addressing &&
          surface->logical_image_readback ==
             qualified->logical_image_readback;
}

enum r300_zb_combined_clear_refusal
r300_zb_combined_clear_plan(const struct r300_zb_combined_clear_request *request,
                            struct r300_zb_combined_clear_plan *out)
{
   if (request == NULL || out == NULL)
      return R300_ZB_COMBINED_CLEAR_REFUSE_INPUT;
   if (!is_rs485m_z24_macrotiled_logical(request->surface))
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
   if (request->aspect_mask == 0u ||
       (request->aspect_mask & ~R300_ZB_COMBINED_CLEAR_ASPECTS) != 0u)
      return R300_ZB_COMBINED_CLEAR_REFUSE_ASPECT;
   if (request->depth_code > 0x00ffffffu)
      return R300_ZB_COMBINED_CLEAR_REFUSE_DEPTH;
   if (request->stencil > 0xffu)
      return R300_ZB_COMBINED_CLEAR_REFUSE_STENCIL;
   if (request->surface_base_bytes >
          UINT64_MAX - request->binding_offset_bytes ||
       layout.total_bytes > UINT64_MAX - request->surface_base_bytes ||
       request->binding_offset_bytes >
          UINT64_MAX - request->surface_base_bytes - layout.total_bytes ||
       request->mapped_surface_bytes < request->binding_offset_bytes +
                                          request->surface_base_bytes +
                                          layout.total_bytes)
      return R300_ZB_COMBINED_CLEAR_REFUSE_ENVELOPE;

   const uint64_t bo_surface_base =
      request->binding_offset_bytes + request->surface_base_bytes;
   if (bo_surface_base > UINT32_MAX)
      return R300_ZB_COMBINED_CLEAR_REFUSE_ENVELOPE;

   uint32_t word;
   if (r300_zb_depth_pack(request->surface, request->depth_code,
                          request->stencil, &word) != 0)
      return R300_ZB_COMBINED_CLEAR_REFUSE_DEPTH;

   if (layout.storage_bytes == 0u ||
       layout.storage_bytes % R300_ZB_MACROTILE_BYTES != 0u ||
       layout.macrotile_width == 0u || layout.macrotile_height == 0u ||
       request->surface->width % layout.macrotile_width != 0u ||
       request->surface->height % layout.macrotile_height != 0u)
      return R300_ZB_COMBINED_CLEAR_REFUSE_OVERFLOW;
   /* A uniform packed clear is invariant under the tiled address
    * permutation.  Reinterpret each complete logical macrotile as one
    * linear RB2D row.  The row count follows the logical extent rather than
    * the rounded storage height, so padding rows remain untouched. */
   const uint32_t fill_width_pixels =
      R300_ZB_MACROTILE_BYTES / request->surface->bytes_per_pixel;
   const uint32_t logical_macrotile_columns =
      request->surface->width / layout.macrotile_width;
   const uint32_t logical_macrotile_rows =
      request->surface->height / layout.macrotile_height;
   const uint64_t logical_macrotile_count =
      (uint64_t)logical_macrotile_columns * logical_macrotile_rows;
   if (logical_macrotile_count == 0u ||
       logical_macrotile_count > UINT32_MAX)
      return R300_ZB_COMBINED_CLEAR_REFUSE_OVERFLOW;
   const uint32_t fill_height = (uint32_t)logical_macrotile_count;

   const struct r300_rb2d_fill_rect rect = {
      .x = 0u,
      .y = 0u,
      .width = fill_width_pixels,
      .height = fill_height,
      .value = word,
   };
   const struct r300_rb2d_fill_plan fill = {
      .surface = {
         .base_offset_bytes = (uint32_t)bo_surface_base,
         .pitch_bytes = R300_ZB_MACROTILE_BYTES,
         .width_pixels = fill_width_pixels,
         .height_pixels = fill_height,
         .format = request->format,
      },
      .write_mask = request->aspect_mask == R300_ZB_COMBINED_CLEAR_ASPECTS
                       ? UINT32_MAX
                       : request->aspect_mask == R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH
                            ? 0xffffff00u
                            : 0x000000ffu,
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
