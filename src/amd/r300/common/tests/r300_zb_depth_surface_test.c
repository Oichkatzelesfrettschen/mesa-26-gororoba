/*
 * SPDX-License-Identifier: MIT
 *
 * Holds the depth-surface descriptors to the registers and the kernel
 * they encode, and holds the Z16 linear descriptor to the constants the
 * retained depth cell emits, so a cell taken from the descriptor
 * produces the stream the cell produced from its macros.
 */

/* The asserts carry the verdicts, so they stay live in NDEBUG builds. */
#undef NDEBUG
#include <assert.h>

#include "r300_reg.h"
#include "r300_zb_depth_control_cell.h"
#include "r300_zb_depth_surface.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The Z16 descriptor is the retained cell restated as data.  Every field
 * equals the macro the cell emits from, so the two cannot drift. */
static void
test_z16_matches_the_cell(void)
{
   const struct r300_zb_depth_surface *s = &r300_zb_depth_surface_z16_linear;
   assert(r300_zb_depth_surface_check(s) == 0);
   assert(s->depth_format == R300_DEPTHFORMAT_16BIT_INT_Z);
   assert(s->bytes_per_pixel == R300_ZB_DEPTH_CONTROL_DEPTH_CPP);
   assert(s->width == R300_ZB_DEPTH_CONTROL_TARGET_WIDTH);
   assert(s->height == R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT);
   assert(s->pitch_pixels == R300_ZB_DEPTH_CONTROL_PITCH_PIXELS);
   assert(s->allocation_rows == R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS);
   assert(s->depth_sentinel == R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL);
   assert(s->microtile == R300_ZB_MICROTILE_LINEAR);
   assert(s->macrotile == R300_ZB_MACROTILE_LINEAR);
   assert(s->host_addressable);
   assert(r300_zb_depth_surface_tile_bits(s) == 0u);
   assert(r300_zb_depth_surface_bytes(s) ==
          R300_ZB_DEPTH_CONTROL_DEPTH_BYTES);
}

/* The surface the ZMASK ladder needs: packed Z24/S8 at four bytes per
 * pixel, microtiled and macrotiled, which is the configuration
 * r300_zmask_layout admits and the only one its 8x8 case accepts.  It
 * carries the Z16 geometry, so a ladder moving between the two changes
 * format and tiling alone.  It is not host-addressable, and the tile bits
 * it declares are the two ZB_DEPTHPITCH fields.
 */
static void
test_z24_macrotiled_shape(void)
{
   const struct r300_zb_depth_surface *s =
      &r300_zb_depth_surface_z24_macrotiled;
   assert(r300_zb_depth_surface_check(s) == 0);
   assert(s->depth_format == R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL);
   assert(s->bytes_per_pixel == 4u);
   assert(s->microtile == R300_ZB_MICROTILE_TILED);
   assert(s->macrotile == R300_ZB_MACROTILE_TILED);
   assert(s->depth_sentinel == 0x00800000u);
   assert(!s->host_addressable);
   assert(s->width == r300_zb_depth_surface_z16_linear.width);
   assert(s->height == r300_zb_depth_surface_z16_linear.height);
   assert(s->pitch_pixels == r300_zb_depth_surface_z16_linear.pitch_pixels);
   assert(r300_zb_depth_surface_tile_bits(s) ==
          (R300_DEPTHMACROTILE_ENABLE | R300_DEPTHMICROTILE_TILED));
}

/* Each refusal alone: a descriptor differing from a legal one in exactly
 * one field is refused by that field. */
static void
test_check_refusals(void)
{
   assert(r300_zb_depth_surface_check(NULL) == -EINVAL);

   const struct r300_zb_depth_surface base =
      r300_zb_depth_surface_z16_linear;
   assert(r300_zb_depth_surface_check(&base) == 0);

   struct r300_zb_depth_surface m;

   m = base;
   m.name = NULL;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   m = base;
   m.depth_format = 3u;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   /* A known format with the wrong width: the kernel reads cpp out of
    * ZB_FORMAT, so a descriptor claiming another width would bound the
    * allocation against a size the kernel never computes. */
   m = base;
   m.bytes_per_pixel = 4u;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   m = base;
   m.microtile = (enum r300_zb_microtile)3;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   m = base;
   m.macrotile = (enum r300_zb_macrotile)2;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   m = base;
   m.width = 0u;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   m = base;
   m.height = 0u;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   m = base;
   m.pitch_pixels = 62u;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   m = base;
   m.pitch_pixels = R300_DEPTHPITCH_MASK + 4u;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   /* A pitch narrower than the render extent puts the row past its own
    * row. */
   m = base;
   m.pitch_pixels = 32u;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   m = base;
   m.allocation_rows = base.height - 1u;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   /* Host addressing over a tiled surface is the claim that would
    * scatter a sentinel across tiles, so it is refused at the
    * descriptor. */
   m = r300_zb_depth_surface_z24_macrotiled;
   m.host_addressable = true;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   m = base;
   m.microtile = R300_ZB_MICROTILE_TILED;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   /* The largest legal pitch and the widest extent under it both hold,
    * so the bound is a bound and not an off-by-one. */
   m = base;
   m.pitch_pixels = R300_DEPTHPITCH_MASK;
   assert(m.pitch_pixels % 4u == 0u);
   assert(r300_zb_depth_surface_check(&m) == 0);
}

int
main(void)
{
   test_z16_matches_the_cell();
   test_z24_macrotiled_shape();
   test_check_refusals();
   printf("r300_zb_depth_surface: z16 identity, z24 shape, and every "
          "check refusal hold\n");
   return 0;
}
