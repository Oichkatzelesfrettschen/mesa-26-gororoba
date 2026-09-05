/* SPDX-License-Identifier: MIT */

#include "r300_zb_depth_surface.h"

#include "r300_reg.h"
#include "r300_zb_depth_control_cell.h"

#include <errno.h>
#include <stddef.h>

const struct r300_zb_depth_surface r300_zb_depth_surface_z16_linear = {
   .name = "z16_linear",
   .depth_format = R300_DEPTHFORMAT_16BIT_INT_Z,
   .bytes_per_pixel = R300_ZB_DEPTH_CONTROL_DEPTH_CPP,
   .microtile = R300_ZB_MICROTILE_LINEAR,
   .macrotile = R300_ZB_MACROTILE_LINEAR,
   .width = R300_ZB_DEPTH_CONTROL_TARGET_WIDTH,
   .height = R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT,
   .pitch_pixels = R300_ZB_DEPTH_CONTROL_PITCH_PIXELS,
   .allocation_rows = R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS,
   .depth_sentinel = R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL,
   .host_addressable = true,
};

/* Same geometry as the Z16 surface, so the two differ in format and
 * tiling alone and a ladder that moves from one to the other changes one
 * variable at a time.  0x00800000 is the 24-bit half-depth value in the
 * packed Z24/S8 word's depth field, the Z24 counterpart of 0x8000.
 */
const struct r300_zb_depth_surface r300_zb_depth_surface_z24_macrotiled = {
   .name = "z24_macrotiled",
   .depth_format = R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL,
   .bytes_per_pixel = 4u,
   .microtile = R300_ZB_MICROTILE_TILED,
   .macrotile = R300_ZB_MACROTILE_TILED,
   .width = R300_ZB_DEPTH_CONTROL_TARGET_WIDTH,
   .height = R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT,
   .pitch_pixels = R300_ZB_DEPTH_CONTROL_PITCH_PIXELS,
   .allocation_rows = R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS,
   .depth_sentinel = 0x00800000u,
   .host_addressable = false,
};

uint32_t
r300_zb_depth_surface_tile_bits(const struct r300_zb_depth_surface *surface)
{
   if (surface == NULL)
      return 0;
   return R300_DEPTHMACROTILE((uint32_t)surface->macrotile) |
          R300_DEPTHMICROTILE((uint32_t)surface->microtile);
}

uint64_t
r300_zb_depth_surface_bytes(const struct r300_zb_depth_surface *surface)
{
   if (surface == NULL)
      return 0;
   return (uint64_t)surface->pitch_pixels *
          (uint64_t)surface->allocation_rows *
          (uint64_t)surface->bytes_per_pixel;
}

/* The cpp each ZB_FORMAT encoding stores, the same widths
 * r300_packet0_check reads out of ZB_FORMAT into track->zb.cpp: two bytes
 * for either 16-bit encoding, four for packed Z24/S8. */
static uint32_t
format_bytes_per_pixel(uint32_t depth_format, bool *known)
{
   *known = true;
   switch (depth_format) {
   case R300_DEPTHFORMAT_16BIT_INT_Z:
   case R300_DEPTHFORMAT_16BIT_13E3:
      return 2u;
   case R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL:
      return 4u;
   default:
      *known = false;
      return 0u;
   }
}

int
r300_zb_depth_surface_check(const struct r300_zb_depth_surface *surface)
{
   if (surface == NULL || surface->name == NULL)
      return -EINVAL;

   bool known = false;
   const uint32_t cpp = format_bytes_per_pixel(surface->depth_format, &known);
   if (!known || cpp != surface->bytes_per_pixel)
      return -EINVAL;

   if (surface->microtile != R300_ZB_MICROTILE_LINEAR &&
       surface->microtile != R300_ZB_MICROTILE_TILED &&
       surface->microtile != R300_ZB_MICROTILE_TILED_SQUARE)
      return -EINVAL;
   if (surface->macrotile != R300_ZB_MACROTILE_LINEAR &&
       surface->macrotile != R300_ZB_MACROTILE_TILED)
      return -EINVAL;

   if (surface->width == 0 || surface->height == 0)
      return -EINVAL;
   if (surface->pitch_pixels == 0 || surface->pitch_pixels % 4u != 0 ||
       surface->pitch_pixels > R300_DEPTHPITCH_MASK ||
       surface->pitch_pixels < surface->width)
      return -EINVAL;
   if (surface->allocation_rows < surface->height)
      return -EINVAL;

   /* A tiled surface's bytes follow a transform this tree does not carry,
    * so a descriptor that claims host addressing over one is refused
    * rather than trusted. */
   if (surface->host_addressable &&
       (surface->microtile != R300_ZB_MICROTILE_LINEAR ||
        surface->macrotile != R300_ZB_MACROTILE_LINEAR))
      return -EINVAL;

   return 0;
}
