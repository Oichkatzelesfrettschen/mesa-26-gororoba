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
   assert(s->depth_sentinel_code == R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL);
   assert(s->microtile == R300_ZB_MICROTILE_LINEAR);
   assert(s->macrotile == R300_ZB_MACROTILE_LINEAR);
   assert(s->raw_allocation_mapping);
   assert(s->uniform_packed_initialization);
   assert(s->logical_pixel_addressing);
   assert(s->logical_image_readback);
   assert(r300_zb_depth_surface_tile_bits(s) == 0u);
   assert(r300_zb_depth_surface_kernel_bound_bytes(s) ==
          R300_ZB_DEPTH_CONTROL_DEPTH_BYTES);

   /* Z16 stores the code itself, so the packed word equals the sentinel
    * and the cell's existing memory writes keep their meaning. */
   uint32_t word = 0u;
   assert(r300_zb_depth_surface_packed_sentinel(s, &word) == 0);
   assert(word == R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL);
}

/* The Z24 linear rung: the Z16 geometry and tiling under a packed
 * Z24/S8 format, so it moves the format alone.  Linear addressing is the
 * same arithmetic at four bytes per pixel as at two, so every host
 * capability holds and the sentinel code reaches memory shifted into the
 * depth field.
 */
static void
test_z24_linear_moves_the_format_alone(void)
{
   const struct r300_zb_depth_surface *z16 =
      &r300_zb_depth_surface_z16_linear;
   const struct r300_zb_depth_surface *s =
      &r300_zb_depth_surface_z24_linear;
   assert(r300_zb_depth_surface_check(s) == 0);

   /* Geometry, tiling, and every capability equal the Z16 rung. */
   assert(s->width == z16->width);
   assert(s->height == z16->height);
   assert(s->pitch_pixels == z16->pitch_pixels);
   assert(s->allocation_rows == z16->allocation_rows);
   assert(s->microtile == z16->microtile);
   assert(s->macrotile == z16->macrotile);
   assert(s->raw_allocation_mapping == z16->raw_allocation_mapping);
   assert(s->uniform_packed_initialization ==
          z16->uniform_packed_initialization);
   assert(s->logical_pixel_addressing == z16->logical_pixel_addressing);
   assert(s->logical_image_readback == z16->logical_image_readback);
   assert(s->logical_pixel_addressing);
   assert(s->logical_image_readback);

   /* The format and its consequences are what differ. */
   assert(s->depth_format == R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL);
   assert(s->bytes_per_pixel == 4u);
   assert(s->depth_sentinel_code == 0x00800000u);
   assert(r300_zb_depth_surface_tile_bits(s) == 0u);
   assert(r300_zb_depth_surface_kernel_bound_bytes(s) ==
          2u * (uint64_t)R300_ZB_DEPTH_CONTROL_DEPTH_BYTES);

   uint32_t word = 0u;
   assert(r300_zb_depth_surface_packed_sentinel(s, &word) == 0);
   assert(word == 0x80000000u);

   /* The three Z24 rungs carry one format and one sentinel and differ in
    * tiling, one class per step: the microtiled rung changes the
    * microtile and the macrotiled rung changes the macrotile. */
   const struct r300_zb_depth_surface *t =
      &r300_zb_depth_surface_z24_microtiled;
   const struct r300_zb_depth_surface *m =
      &r300_zb_depth_surface_z24_macrotiled;
   assert(t->depth_format == s->depth_format);
   assert(m->depth_format == s->depth_format);
   assert(t->depth_sentinel_code == s->depth_sentinel_code);
   assert(m->depth_sentinel_code == s->depth_sentinel_code);
   assert(r300_zb_depth_surface_check(t) == 0);
   assert(t->microtile != s->microtile && t->macrotile == s->macrotile);
   assert(m->microtile == t->microtile && m->macrotile != t->macrotile);
}

/* Square microtiling has a tile shape at 16 bits per pixel alone, so a
 * four-byte pixel declaring it names a mode r300_get_pixel_alignment
 * reports as {0, 0}.
 */
static void
test_square_microtile_needs_a_two_byte_pixel(void)
{
   struct r300_zb_depth_surface s = r300_zb_depth_surface_z24_linear;
   s.microtile = R300_ZB_MICROTILE_TILED_SQUARE;
   s.logical_pixel_addressing = false;
   s.logical_image_readback = false;
   assert(r300_zb_depth_surface_check(&s) == -EINVAL);

   /* Ordinary microtiling under the same format is admitted, so the
    * refusal names the microtile mode. */
   s.microtile = R300_ZB_MICROTILE_TILED;
   assert(r300_zb_depth_surface_check(&s) == 0);

   /* Both 16-bit encodings keep square microtiling. */
   struct r300_zb_depth_surface z = r300_zb_depth_surface_z16_linear;
   z.microtile = R300_ZB_MICROTILE_TILED_SQUARE;
   z.logical_pixel_addressing = false;
   z.logical_image_readback = false;
   assert(r300_zb_depth_surface_check(&z) == 0);
   z.depth_format = R300_DEPTHFORMAT_16BIT_13E3;
   assert(r300_zb_depth_surface_check(&z) == 0);
}

/* The surface the ZMASK ladder needs: packed Z24/S8 at four bytes per
 * pixel, microtiled and macrotiled, which is the configuration
 * r300_zmask_layout admits and the only one its 8x8 case accepts.  It
 * carries the Z16 geometry and adds macrotiling to the Z24 microtiled
 * rung.  It is not host-addressable, and the tile bits it declares are
 * the two ZB_DEPTHPITCH fields.
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
   assert(s->depth_sentinel_code == 0x00800000u);
   /* Raw bytes and a uniform packed fill hold; the coordinate transform
    * both of the last two need does not exist, so both answer false. */
   assert(s->raw_allocation_mapping);
   assert(s->uniform_packed_initialization);
   assert(!s->logical_pixel_addressing);
   assert(!s->logical_image_readback);
   assert(s->width == r300_zb_depth_surface_z16_linear.width);
   assert(s->height == r300_zb_depth_surface_z16_linear.height);
   assert(s->pitch_pixels == r300_zb_depth_surface_z16_linear.pitch_pixels);
   assert(r300_zb_depth_surface_tile_bits(s) ==
          (R300_DEPTHMACROTILE_ENABLE | R300_DEPTHMICROTILE_TILED));

   /* The sentinel is a depth code, and R300 packs Z24/S8 with depth in
    * bits 31:8, so the word a host writes is the code shifted up.  A
    * code copied straight into memory would name depth 0x008000 under
    * stencil 0x00 instead. */
   uint32_t word = 0u;
   assert(r300_zb_depth_surface_packed_sentinel(s, &word) == 0);
   assert(word == 0x80000000u);
}

/* pack and unpack are inverses over both formats, and each refuses a
 * value wider than the field it would truncate. */
static void
test_pack_round_trip(void)
{
   const struct r300_zb_depth_surface *z16 =
      &r300_zb_depth_surface_z16_linear;
   const struct r300_zb_depth_surface *z24 =
      &r300_zb_depth_surface_z24_macrotiled;
   uint32_t word = 0u, depth = 0u, stencil = 0u;

   /* Coordinate-tagged codes rather than one constant: a permutation or
    * a lost shift shows up only where the values differ. */
   for (uint32_t i = 0; i < 256u; i++) {
      const uint32_t d24 = (i * 0x00010101u) & 0x00ffffffu;
      const uint32_t s8 = (i * 7u) & 0xffu;
      assert(r300_zb_depth_pack(z24, d24, s8, &word) == 0);
      assert(word == ((d24 << 8) | s8));
      assert(r300_zb_depth_unpack(z24, word, &depth, &stencil) == 0);
      assert(depth == d24 && stencil == s8);

      const uint32_t d16 = (i * 0x0101u) & 0xffffu;
      assert(r300_zb_depth_pack(z16, d16, 0u, &word) == 0);
      assert(word == d16);
      assert(r300_zb_depth_unpack(z16, word, &depth, &stencil) == 0);
      assert(depth == d16 && stencil == 0u);
   }

   /* The widest representable pair of each format packs; one more bit
    * refuses rather than truncating. */
   assert(r300_zb_depth_pack(z24, 0x00ffffffu, 0xffu, &word) == 0);
   assert(word == 0xffffffffu);
   assert(r300_zb_depth_pack(z24, 0x01000000u, 0u, &word) == -EINVAL);
   assert(r300_zb_depth_pack(z24, 0u, 0x100u, &word) == -EINVAL);
   assert(r300_zb_depth_pack(z16, 0xffffu, 0u, &word) == 0);
   assert(r300_zb_depth_pack(z16, 0x10000u, 0u, &word) == -EINVAL);
   /* A 16-bit surface stores no stencil, so any stencil refuses. */
   assert(r300_zb_depth_pack(z16, 0u, 1u, &word) == -EINVAL);
   /* A word carrying a bit outside the format's fields came from
    * somewhere the format does not describe. */
   assert(r300_zb_depth_unpack(z16, 0x10000u, &depth, &stencil) == -EINVAL);

   assert(r300_zb_depth_pack(NULL, 0u, 0u, &word) == -EINVAL);
   assert(r300_zb_depth_pack(z24, 0u, 0u, NULL) == -EINVAL);
   assert(r300_zb_depth_unpack(z24, 0u, NULL, &stencil) == -EINVAL);
   assert(r300_zb_depth_surface_packed_sentinel(NULL, &word) == -EINVAL);

   struct r300_zb_depth_surface unknown = *z16;
   unknown.depth_format = 3u;
   assert(r300_zb_depth_pack(&unknown, 0u, 0u, &word) == -EINVAL);
   assert(r300_zb_depth_unpack(&unknown, 0u, &depth, &stencil) == -EINVAL);
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

   /* Logical addressing over a tiled surface is the claim that would
    * scatter a sentinel across tiles, so it is refused at the
    * descriptor.  Row-major readback rides on it and refuses with it. */
   m = r300_zb_depth_surface_z24_macrotiled;
   m.logical_pixel_addressing = true;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   m = r300_zb_depth_surface_z24_macrotiled;
   m.logical_image_readback = true;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   /* Each capability needs the one beneath it: nothing is written into
    * an allocation no host maps, and no image is read back without the
    * transform that names a pixel. */
   m = base;
   m.raw_allocation_mapping = false;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   m = base;
   m.logical_pixel_addressing = false;
   assert(r300_zb_depth_surface_check(&m) == -EINVAL);

   /* Dropping the top two capabilities alone leaves a legal linear
    * surface a host maps and fills uniformly. */
   m = base;
   m.logical_pixel_addressing = false;
   m.logical_image_readback = false;
   assert(r300_zb_depth_surface_check(&m) == 0);

   /* A sentinel code wider than the format's depth field names no packed
    * word. */
   m = base;
   m.depth_sentinel_code = 0x10000u;
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
   test_z24_linear_moves_the_format_alone();
   test_square_microtile_needs_a_two_byte_pixel();
   test_z24_macrotiled_shape();
   test_pack_round_trip();
   test_check_refusals();
   printf("r300_zb_depth_surface: z16 identity, all three z24 rungs, the "
          "square-microtile pixel width, pack/unpack round trip, and "
          "every check refusal hold\n");
   return 0;
}
