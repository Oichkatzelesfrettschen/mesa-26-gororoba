/* SPDX-License-Identifier: MIT */

#include "r300_zb_aspect_copy.h"

#include "r300_reg.h"

#include <errno.h>
#include <stddef.h>

enum r300_zb_aspect_copy_refusal
r300_zb_aspect_copy_plan(
   const struct r300_zb_depth_surface *surface,
   uint64_t surface_base_bytes, uint64_t mapped_surface_bytes,
   uint32_t x, uint32_t y, uint64_t buffer_offset_bytes,
   uint32_t buffer_pitch_bytes, uint64_t buffer_bytes,
   enum r300_zb_aspect_copy_aspect aspect,
   enum r300_zb_aspect_copy_direction direction,
   struct r300_rb2d_copy_segment *out)
{
   if (surface == NULL || out == NULL || buffer_pitch_bytes == 0u ||
       (direction != R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE &&
        direction != R300_ZB_ASPECT_COPY_SURFACE_TO_BUFFER))
      return R300_ZB_ASPECT_COPY_REFUSE_INPUT;
   if (surface->depth_format != R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL ||
       surface->bytes_per_pixel != 4u || !surface->logical_pixel_addressing ||
       buffer_pitch_bytes < (uint64_t)surface->width *
                              (aspect == R300_ZB_ASPECT_COPY_DEPTH ? 4u : 1u))
      return R300_ZB_ASPECT_COPY_REFUSE_FORMAT;
   if (aspect != R300_ZB_ASPECT_COPY_DEPTH &&
       aspect != R300_ZB_ASPECT_COPY_STENCIL)
      return R300_ZB_ASPECT_COPY_REFUSE_ASPECT;
   if (x >= surface->width || y >= surface->height)
      return R300_ZB_ASPECT_COPY_REFUSE_SURFACE_BOUNDS;

   uint64_t surface_pixel;
   if (r300_zb_depth_address_checked(surface, surface_base_bytes, mapped_surface_bytes,
                                     x, y, &surface_pixel) != 0)
      return R300_ZB_ASPECT_COPY_REFUSE_SURFACE_BOUNDS;
   const uint32_t aspect_bytes = aspect == R300_ZB_ASPECT_COPY_DEPTH ? 3u : 1u;
   /* R300 packs stencil in the low byte and depth in the upper three;
    * X8_D24_UNORM_PACK32 places depth in the low three buffer bytes;
    * S8_UINT transfers stencil through a separate one-byte buffer texel. */
   const uint32_t surface_aspect_offset =
      aspect == R300_ZB_ASPECT_COPY_DEPTH ? 1u : 0u;
   const uint32_t buffer_aspect_offset_in_texel = 0u;
   if (surface_pixel > UINT64_MAX - surface_aspect_offset)
      return R300_ZB_ASPECT_COPY_REFUSE_OVERFLOW;
   const uint64_t surface_offset = surface_pixel + surface_aspect_offset;
   if (surface_offset > mapped_surface_bytes ||
       aspect_bytes > mapped_surface_bytes - surface_offset)
      return R300_ZB_ASPECT_COPY_REFUSE_SURFACE_BOUNDS;

   const uint64_t row_offset = (uint64_t)y * buffer_pitch_bytes;
   const uint64_t texel_offset =
      (uint64_t)x * (aspect == R300_ZB_ASPECT_COPY_DEPTH ? 4u : 1u);
   if (row_offset > UINT64_MAX - texel_offset ||
       buffer_offset_bytes > UINT64_MAX - row_offset - texel_offset ||
       buffer_offset_bytes + row_offset + texel_offset > buffer_bytes ||
       aspect_bytes > buffer_bytes -
                          (buffer_offset_bytes + row_offset + texel_offset))
      return R300_ZB_ASPECT_COPY_REFUSE_BUFFER_BOUNDS;
   const uint64_t buffer_aspect_offset = buffer_offset_bytes + row_offset +
                                         texel_offset + buffer_aspect_offset_in_texel;
   if (buffer_aspect_offset > buffer_bytes ||
       aspect_bytes > buffer_bytes - buffer_aspect_offset)
      return R300_ZB_ASPECT_COPY_REFUSE_BUFFER_BOUNDS;

   const uint64_t source_end = direction == R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE
                                  ? buffer_aspect_offset + aspect_bytes
                                  : surface_offset + aspect_bytes;
   const uint64_t destination_end =
      direction == R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE
         ? surface_offset + aspect_bytes
         : buffer_aspect_offset + aspect_bytes;
   if (source_end < (direction == R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE
                        ? buffer_aspect_offset : surface_offset) ||
       destination_end < (direction == R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE
                             ? surface_offset : buffer_aspect_offset) ||
       (direction == R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE
           ? buffer_aspect_offset : surface_offset) % 256u + aspect_bytes >
          256u ||
       (direction == R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE
           ? surface_offset : buffer_aspect_offset) % 256u + aspect_bytes >
          256u)
      return R300_ZB_ASPECT_COPY_REFUSE_BUFFER_BOUNDS;

   struct r300_rb2d_copy_segment candidate = {
      .source_offset_bytes = direction == R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE
                                ? buffer_aspect_offset : surface_offset,
      .destination_offset_bytes =
         direction == R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE
            ? surface_offset : buffer_aspect_offset,
      .byte_count = aspect_bytes,
   };
   *out = candidate;
   return R300_ZB_ASPECT_COPY_OK;
}
