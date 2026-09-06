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
   .depth_sentinel_code = R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL,
   .raw_allocation_mapping = true,
   .uniform_packed_initialization = true,
   .logical_pixel_addressing = true,
   .logical_image_readback = true,
};

/* The Z24 linear surface: the same geometry, tiling, and host
 * capabilities as the Z16 surface, differing in format alone.  It is the
 * first rung of the ladder, and the one rung that moves a single
 * variable: linear addressing holds at four bytes per pixel exactly as
 * it does at two, so a(x, y) = base + 4 * (y * pitch + x) names every
 * pixel and a host packs, initializes, and reads the surface back
 * without any transform.  What it establishes is packed-depth behavior:
 * where the depth and stencil components sit inside a 32-bit word, and
 * which words a passing fragment writes.
 *
 * 0x00800000 is the 24-bit half-depth code, the Z24 counterpart of the
 * Z16 cell's 0x8000; packed against a zero stencil it reaches memory as
 * 0x80000000.
 */
const struct r300_zb_depth_surface r300_zb_depth_surface_z24_linear = {
   .name = "z24_linear",
   .depth_format = R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL,
   .bytes_per_pixel = 4u,
   .microtile = R300_ZB_MICROTILE_LINEAR,
   .macrotile = R300_ZB_MACROTILE_LINEAR,
   .width = R300_ZB_DEPTH_CONTROL_TARGET_WIDTH,
   .height = R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT,
   .pitch_pixels = R300_ZB_DEPTH_CONTROL_PITCH_PIXELS,
   .allocation_rows = R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS,
   .depth_sentinel_code = 0x00800000u,
   .raw_allocation_mapping = true,
   .uniform_packed_initialization = true,
   .logical_pixel_addressing = true,
   .logical_image_readback = true,
};

/* The third rung: packed Z24/S8 microtiled and macrotile-linear, which
 * moves the microtile alone.  Its tile is 4x2 pixels, 32 bytes, the one
 * r300_get_pixel_alignment reports for a four-byte pixel, so this is
 * where a permutation first separates a logical coordinate from a
 * row-major byte and the last two capabilities go false.
 */
const struct r300_zb_depth_surface r300_zb_depth_surface_z24_microtiled = {
   .name = "z24_microtiled",
   .depth_format = R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL,
   .bytes_per_pixel = 4u,
   .microtile = R300_ZB_MICROTILE_TILED,
   .macrotile = R300_ZB_MACROTILE_LINEAR,
   .width = R300_ZB_DEPTH_CONTROL_TARGET_WIDTH,
   .height = R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT,
   .pitch_pixels = R300_ZB_DEPTH_CONTROL_PITCH_PIXELS,
   .allocation_rows = R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS,
   .depth_sentinel_code = 0x00800000u,
   .raw_allocation_mapping = true,
   .uniform_packed_initialization = true,
   .logical_pixel_addressing = false,
   .logical_image_readback = false,
};

/* The fourth rung: the microtiled surface above with macrotiling added,
 * so it too moves one variable.  This is the configuration
 * r300_zmask_layout admits and the only one its 8x8 compression case
 * accepts.  A uniform allocation of the sentinel word is invariant under
 * the tile permutation, so the host initializes this surface while the
 * transform that names a single pixel's byte stays absent.
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
   .depth_sentinel_code = 0x00800000u,
   .raw_allocation_mapping = true,
   .uniform_packed_initialization = true,
   .logical_pixel_addressing = false,
   .logical_image_readback = false,
};

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

/* The field widths and the shift each encoding gives depth and stencil.
 * Either 16-bit encoding stores the code alone and no stencil, so its
 * stencil mask is zero and any nonzero stencil refuses. */
static bool
format_fields(uint32_t depth_format, uint32_t *depth_mask,
              uint32_t *depth_shift, uint32_t *stencil_mask)
{
   switch (depth_format) {
   case R300_DEPTHFORMAT_16BIT_INT_Z:
   case R300_DEPTHFORMAT_16BIT_13E3:
      *depth_mask = 0xffffu;
      *depth_shift = 0u;
      *stencil_mask = 0u;
      return true;
   case R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL:
      *depth_mask = 0x00ffffffu;
      *depth_shift = 8u;
      *stencil_mask = 0xffu;
      return true;
   default:
      return false;
   }
}

int
r300_zb_depth_pack(const struct r300_zb_depth_surface *surface,
                   uint32_t depth_code, uint32_t stencil, uint32_t *word_out)
{
   uint32_t depth_mask, depth_shift, stencil_mask;
   if (surface == NULL || word_out == NULL ||
       !format_fields(surface->depth_format, &depth_mask, &depth_shift,
                      &stencil_mask))
      return -EINVAL;
   if (depth_code > depth_mask || stencil > stencil_mask)
      return -EINVAL;

   *word_out = (depth_code << depth_shift) | stencil;
   return 0;
}

int
r300_zb_depth_unpack(const struct r300_zb_depth_surface *surface,
                     uint32_t word, uint32_t *depth_code_out,
                     uint32_t *stencil_out)
{
   uint32_t depth_mask, depth_shift, stencil_mask;
   if (surface == NULL || depth_code_out == NULL || stencil_out == NULL ||
       !format_fields(surface->depth_format, &depth_mask, &depth_shift,
                      &stencil_mask))
      return -EINVAL;
   /* A bit outside the two fields belongs to no component the format
    * stores, so the word came from somewhere else. */
   if ((word & ~((depth_mask << depth_shift) | stencil_mask)) != 0)
      return -EINVAL;

   *depth_code_out = (word >> depth_shift) & depth_mask;
   *stencil_out = word & stencil_mask;
   return 0;
}

int
r300_zb_depth_surface_packed_sentinel(
   const struct r300_zb_depth_surface *surface, uint32_t *word_out)
{
   if (surface == NULL)
      return -EINVAL;
   return r300_zb_depth_pack(surface, surface->depth_sentinel_code, 0u,
                             word_out);
}

uint32_t
r300_zb_depth_surface_tile_bits(const struct r300_zb_depth_surface *surface)
{
   if (surface == NULL)
      return 0;
   return R300_DEPTHMACROTILE((uint32_t)surface->macrotile) |
          R300_DEPTHMICROTILE((uint32_t)surface->microtile);
}

uint64_t
r300_zb_depth_surface_kernel_bound_bytes(
   const struct r300_zb_depth_surface *surface)
{
   if (surface == NULL)
      return 0;
   return (uint64_t)surface->pitch_pixels *
          (uint64_t)surface->allocation_rows *
          (uint64_t)surface->bytes_per_pixel;
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
   /* Square microtiling applies to a two-byte pixel alone.  Its entries
    * in r300_get_pixel_alignment are {4, 4} and {32, 32} at 16 bits per
    * pixel and {0, 0} at every other width, and that function asserts on
    * a zero tile, so the mode has no alignment a wider pixel rounds to.
    * Packed Z24/S8 stores four bytes and refuses it here; both 16-bit
    * depth encodings keep it. */
   if (surface->microtile == R300_ZB_MICROTILE_TILED_SQUARE &&
       surface->bytes_per_pixel != 2u)
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

   /* The sentinel is a code, so it fits the format's depth field or the
    * surface names a value no packed word carries. */
   uint32_t sentinel_word;
   if (r300_zb_depth_surface_packed_sentinel(surface, &sentinel_word) != 0)
      return -EINVAL;

   /* Every capability rests on the ones beneath it: a host that cannot
    * map the allocation writes nothing into it, and a row-major readback
    * reads through the same transform that names a single pixel. */
   if (surface->uniform_packed_initialization &&
       !surface->raw_allocation_mapping)
      return -EINVAL;
   if (surface->logical_pixel_addressing && !surface->raw_allocation_mapping)
      return -EINVAL;
   if (surface->logical_image_readback && !surface->logical_pixel_addressing)
      return -EINVAL;

   /* A tiled surface's byte for a coordinate follows a transform this
    * tree does not carry, so a descriptor that claims logical addressing
    * over one is refused rather than trusted.  Uniform initialization
    * survives tiling, because a constant image is invariant under the
    * permutation, and it stays admitted here. */
   if (surface->logical_pixel_addressing &&
       (surface->microtile != R300_ZB_MICROTILE_LINEAR ||
        surface->macrotile != R300_ZB_MACROTILE_LINEAR))
      return -EINVAL;

   return 0;
}
