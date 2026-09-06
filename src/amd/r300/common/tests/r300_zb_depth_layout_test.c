/* SPDX-License-Identifier: MIT */

/* Holds the tiled layout calculation to the R300 tile table and to the
 * arithmetic r300_texture_get_stride and r300_texture_get_nblocksy
 * perform, and holds the guard scheme to its own boundaries.
 *
 * The release profiles compile with -DNDEBUG, which discards an assert
 * whole, side effects included.  Undefining it before <assert.h> keeps
 * every verdict here live in every profile.
 */
#undef NDEBUG

#include "r300_zb_depth_layout.h"
#include "r300_zb_depth_control_cell.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Every tile shape the table admits, held to two independent invariants.
 * The byte product: a microtile covers 32 bytes and a macrotile 2048,
 * which is what the R3xx-R5xx tiling description states and what every
 * nonzero entry of r300_get_pixel_alignment multiplies to.  And the area
 * ratio: each macrotile covers exactly 64 microtiles at the same pixel
 * width.  The products alone do not pin the table, because transposing
 * two rows between the two pixel widths -- 64x16 at cpp 2 against 32x16
 * at cpp 4 -- preserves both byte products; the ratio separates them. */
static void
test_tile_table_products(void)
{
   static const struct {
      uint32_t cpp;
      enum r300_zb_microtile micro;
      enum r300_zb_macrotile macro;
      uint32_t width;
      uint32_t height;
   } expected[] = {
      { 2u, R300_ZB_MICROTILE_LINEAR, R300_ZB_MACROTILE_LINEAR, 16u, 1u },
      { 2u, R300_ZB_MICROTILE_TILED, R300_ZB_MACROTILE_LINEAR, 8u, 2u },
      { 2u, R300_ZB_MICROTILE_TILED_SQUARE, R300_ZB_MACROTILE_LINEAR, 4u, 4u },
      { 2u, R300_ZB_MICROTILE_LINEAR, R300_ZB_MACROTILE_TILED, 128u, 8u },
      { 2u, R300_ZB_MICROTILE_TILED, R300_ZB_MACROTILE_TILED, 64u, 16u },
      { 2u, R300_ZB_MICROTILE_TILED_SQUARE, R300_ZB_MACROTILE_TILED, 32u, 32u },
      { 4u, R300_ZB_MICROTILE_LINEAR, R300_ZB_MACROTILE_LINEAR, 8u, 1u },
      { 4u, R300_ZB_MICROTILE_TILED, R300_ZB_MACROTILE_LINEAR, 4u, 2u },
      { 4u, R300_ZB_MICROTILE_LINEAR, R300_ZB_MACROTILE_TILED, 64u, 8u },
      { 4u, R300_ZB_MICROTILE_TILED, R300_ZB_MACROTILE_TILED, 32u, 16u },
   };

   for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
      uint32_t w = 0, h = 0;
      assert(r300_zb_depth_layout_tile_pixels(expected[i].cpp,
                                              expected[i].micro,
                                              expected[i].macro, &w, &h) == 0);
      assert(w == expected[i].width);
      assert(h == expected[i].height);
      const uint32_t bytes = w * h * expected[i].cpp;
      assert(bytes == (expected[i].macro == R300_ZB_MACROTILE_TILED
                          ? R300_ZB_MACROTILE_BYTES
                          : R300_ZB_MICROTILE_BYTES));

      /* The macrotile at this pixel width and microtile mode covers 64
       * of the microtiles the same width and mode name. */
      if (expected[i].macro != R300_ZB_MACROTILE_TILED)
         continue;
      uint32_t mw = 0, mh = 0;
      assert(r300_zb_depth_layout_tile_pixels(expected[i].cpp,
                                              expected[i].micro,
                                              R300_ZB_MACROTILE_LINEAR, &mw,
                                              &mh) == 0);
      assert((w * h) == 64u * (mw * mh));
   }
}

/* Square microtiling outside a two-byte pixel has no entry, which is the
 * {0, 0} r300_get_pixel_alignment reports there. */
static void
test_square_microtile_refuses_wide_pixels(void)
{
   uint32_t w = 0, h = 0;
   assert(r300_zb_depth_layout_tile_pixels(4u, R300_ZB_MICROTILE_TILED_SQUARE,
                                           R300_ZB_MACROTILE_LINEAR, &w,
                                           &h) == -EINVAL);
   assert(r300_zb_depth_layout_tile_pixels(4u, R300_ZB_MICROTILE_TILED_SQUARE,
                                           R300_ZB_MACROTILE_TILED, &w,
                                           &h) == -EINVAL);
   /* Two bytes keeps it. */
   assert(r300_zb_depth_layout_tile_pixels(2u, R300_ZB_MICROTILE_TILED_SQUARE,
                                           R300_ZB_MACROTILE_TILED, &w,
                                           &h) == 0);
   assert(w == 32u && h == 32u);

   /* And a surface declaring the pair is refused by the descriptor
    * validator before any layout is asked for. */
   struct r300_zb_depth_surface surface = r300_zb_depth_surface_z24_linear;
   surface.microtile = R300_ZB_MICROTILE_TILED_SQUARE;
   assert(r300_zb_depth_surface_check(&surface) == -EINVAL);
   struct r300_zb_depth_surface z16 = r300_zb_depth_surface_z16_linear;
   z16.microtile = R300_ZB_MICROTILE_TILED_SQUARE;
   z16.logical_pixel_addressing = false;
   z16.logical_image_readback = false;
   assert(r300_zb_depth_surface_check(&z16) == 0);
}

/* An unguarded linear layout reproduces the footprint the depth control
 * cell has always allocated, which is what keeps the Z16 golden's
 * allocation unchanged when a caller sizes it from the layout. */
static void
test_linear_layout_matches_the_retained_footprint(void)
{
   struct r300_zb_depth_layout layout;
   assert(r300_zb_depth_layout_compute(&r300_zb_depth_surface_z16_linear, 0u,
                                       &layout) == 0);
   assert(layout.pitch_bytes == 128u);
   assert(layout.storage_rows == R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS);
   assert(layout.storage_bytes == R300_ZB_DEPTH_CONTROL_DEPTH_BYTES);
   assert(layout.total_bytes == R300_ZB_DEPTH_CONTROL_DEPTH_BYTES);
   assert(layout.base_offset_bytes == 0u);
   assert(layout.base_alignment_bytes == 32u);
   assert(layout.prefix_guard_bytes == 0u);
   assert(layout.suffix_guard_bytes == 0u);
   /* Linear storage and the parser bound are the same product. */
   assert(layout.storage_bytes == layout.kernel_bound_bytes);
   assert(layout.maturity == R300_ZB_DEPTH_LAYOUT_BLOCK_ENVELOPE);

   /* Z24 linear doubles the pixel and nothing else. */
   assert(r300_zb_depth_layout_compute(&r300_zb_depth_surface_z24_linear, 0u,
                                       &layout) == 0);
   assert(layout.pitch_bytes == 256u);
   assert(layout.storage_rows == 65u);
   assert(layout.storage_bytes == 16640u);
   assert(layout.storage_bytes == layout.kernel_bound_bytes);
}

/* The microtiled rung keeps the parser bound, because its 2-row tile
 * divides an even allocation row count, and separates a logical
 * coordinate from a row-major byte for the first time. */
static void
test_microtiled_rung(void)
{
   struct r300_zb_depth_layout layout;
   assert(r300_zb_depth_layout_compute(&r300_zb_depth_surface_z24_microtiled,
                                       0u, &layout) == 0);
   assert(layout.microtile_width == 4u);
   assert(layout.microtile_height == 2u);
   /* Macrotile-linear reports no macrotile pair. */
   assert(layout.macrotile_width == 0u);
   assert(layout.macrotile_height == 0u);
   /* 65 rows round up to 66 under a 2-row tile: 256 bytes a row, 16896. */
   assert(layout.storage_rows == 66u);
   assert(layout.storage_bytes == 16896u);
   assert(layout.storage_bytes > layout.kernel_bound_bytes);
   assert(layout.base_alignment_bytes == 32u);

   /* The microtile permutes pixels inside a row pair, so the descriptor
    * declares no logical addressing and the validator holds it there. */
   assert(!r300_zb_depth_surface_z24_microtiled.logical_pixel_addressing);
   assert(!r300_zb_depth_surface_z24_microtiled.logical_image_readback);
   struct r300_zb_depth_surface claim = r300_zb_depth_surface_z24_microtiled;
   claim.logical_pixel_addressing = true;
   assert(r300_zb_depth_surface_check(&claim) == -EINVAL);

   /* Each rung moves one variable from the one before it. */
   const struct r300_zb_depth_surface *a = &r300_zb_depth_surface_z24_linear;
   const struct r300_zb_depth_surface *b =
      &r300_zb_depth_surface_z24_microtiled;
   const struct r300_zb_depth_surface *c =
      &r300_zb_depth_surface_z24_macrotiled;
   assert(a->microtile != b->microtile && a->macrotile == b->macrotile);
   assert(b->microtile == c->microtile && b->macrotile != c->macrotile);
}

/* The macrotiled envelope exceeds the parser bound, because tiling
 * rounds 65 allocation rows up to five whole 16-row macrotiles. */
static void
test_macrotiled_envelope_exceeds_the_parser_bound(void)
{
   struct r300_zb_depth_layout layout;
   assert(r300_zb_depth_layout_compute(&r300_zb_depth_surface_z24_macrotiled,
                                       R300_ZB_DEPTH_GUARD_BYTES,
                                       &layout) == 0);
   assert(layout.macrotile_width == 32u);
   assert(layout.macrotile_height == 16u);
   assert(layout.microtile_width == 4u);
   assert(layout.microtile_height == 2u);
   assert(layout.storage_rows == 80u);
   assert(layout.storage_bytes == 20480u);
   assert(layout.storage_bytes == 10u * R300_ZB_MACROTILE_BYTES);
   assert(layout.kernel_bound_bytes == 16640u);
   assert(layout.storage_bytes > layout.kernel_bound_bytes);
   /* The excess comes from the 65th row.  The render extent alone tiles
    * to four whole macrotile rows and 16384 bytes, inside the bound, so
    * an allocation sized from the bound holds every rendered row and
    * stops 3840 bytes inside the macrotile row the 65th shares with
    * rows 65 through 79. */
   assert(64u % layout.macrotile_height == 0u);
   assert((uint64_t)layout.pitch_bytes * 64u == 16384u);
   assert((uint64_t)layout.pitch_bytes * 64u < layout.kernel_bound_bytes);
   assert(layout.storage_bytes - layout.kernel_bound_bytes == 3840u);
   assert(layout.base_alignment_bytes == R300_ZB_MACROTILE_BYTES);
   assert(layout.base_offset_bytes == R300_ZB_DEPTH_GUARD_BYTES);
   assert(layout.base_offset_bytes % layout.base_alignment_bytes == 0u);
   assert(layout.total_bytes == 2u * R300_ZB_DEPTH_GUARD_BYTES + 20480u);
}

/* The guards bracket the envelope and touch none of it, and the byte a
 * linear surface would have used as its canary sits inside the tiled
 * envelope rather than past it. */
static void
test_guard_ranges(void)
{
   struct r300_zb_depth_layout layout;
   assert(r300_zb_depth_layout_compute(&r300_zb_depth_surface_z24_macrotiled,
                                       R300_ZB_DEPTH_GUARD_BYTES,
                                       &layout) == 0);

   assert(r300_zb_depth_layout_is_guard_byte(&layout, 0u));
   assert(r300_zb_depth_layout_is_guard_byte(&layout,
                                             R300_ZB_DEPTH_GUARD_BYTES - 1u));
   assert(!r300_zb_depth_layout_is_guard_byte(&layout,
                                              layout.base_offset_bytes));
   assert(!r300_zb_depth_layout_is_guard_byte(
      &layout, layout.base_offset_bytes + layout.storage_bytes - 1u));
   assert(r300_zb_depth_layout_is_guard_byte(&layout,
                                             layout.suffix_guard_offset_bytes));
   assert(r300_zb_depth_layout_is_guard_byte(&layout,
                                             layout.total_bytes - 1u));
   assert(!r300_zb_depth_layout_is_guard_byte(&layout, layout.total_bytes));

   /* Byte 64 * pitch_bytes -- the linear canary row -- lies within the
    * tiled storage envelope, which is why the guards moved outside it. */
   const uint64_t linear_canary =
      layout.base_offset_bytes + 64ull * layout.pitch_bytes;
   assert(!r300_zb_depth_layout_is_guard_byte(&layout, linear_canary));
   assert(linear_canary < layout.suffix_guard_offset_bytes);
}

/* A guard that is not a whole multiple of the base alignment would put
 * the surface base off its macrotile boundary. */
static void
test_guard_must_preserve_base_alignment(void)
{
   struct r300_zb_depth_layout layout;
   assert(r300_zb_depth_layout_compute(&r300_zb_depth_surface_z24_macrotiled,
                                       32u, &layout) == -EINVAL);
   assert(r300_zb_depth_layout_compute(&r300_zb_depth_surface_z24_macrotiled,
                                       R300_ZB_MACROTILE_BYTES, &layout) == 0);
   /* A linear surface takes the DEPTHOFFSET granularity instead, so 32
    * is admitted there. */
   assert(r300_zb_depth_layout_compute(&r300_zb_depth_surface_z24_linear, 32u,
                                       &layout) == 0);
   assert(layout.base_offset_bytes == 32u);
   assert(r300_zb_depth_layout_compute(&r300_zb_depth_surface_z24_linear, 8u,
                                       &layout) == -EINVAL);
}

/* A declared pitch below the tile grid would make ZB_DEPTHPITCH and the
 * storage extent name different row strides. */
static void
test_pitch_must_sit_on_the_tile_grid(void)
{
   struct r300_zb_depth_surface surface = r300_zb_depth_surface_z24_macrotiled;
   struct r300_zb_depth_layout layout;

   /* 32-pixel macrotile width: 64 is on the grid, 68 is not. */
   surface.pitch_pixels = 68u;
   surface.width = 68u;
   assert(r300_zb_depth_layout_compute(&surface, R300_ZB_MACROTILE_BYTES,
                                       &layout) == -EINVAL);
   surface.pitch_pixels = 96u;
   assert(r300_zb_depth_layout_compute(&surface, R300_ZB_MACROTILE_BYTES,
                                       &layout) == 0);
   assert(layout.pitch_bytes == 384u);
   assert(layout.storage_bytes == 384ull * 80ull);
}

/* Every product is computed in 64 bits and compared against the field it
 * lands in, so an extent no 32-bit offset reaches refuses rather than
 * truncating.  DEPTHPITCH tops out at 16380 pixels and the row count is
 * what carries a layout past the bound. */
static void
test_overflow_refuses(void)
{
   struct r300_zb_depth_surface surface = r300_zb_depth_surface_z24_macrotiled;
   struct r300_zb_depth_layout layout;

   surface.width = 16352u;
   surface.pitch_pixels = 16352u;
   surface.height = 16u;
   surface.allocation_rows = 16u;
   assert(r300_zb_depth_layout_compute(&surface, R300_ZB_MACROTILE_BYTES,
                                       &layout) == 0);
   assert(layout.pitch_bytes == 65408u);

   /* 65408 bytes a row needs 65664 rows to reach 2^32, so 65680 rows --
    * already a whole number of 16-row macrotiles -- lands past it. */
   surface.height = 65680u;
   surface.allocation_rows = 65680u;
   assert(r300_zb_depth_layout_compute(&surface, R300_ZB_MACROTILE_BYTES,
                                       &layout) == -EINVAL);

   /* A row count whose tile rounding leaves the 32-bit field refuses in
    * the rounding itself rather than in the product. */
   surface.height = UINT32_MAX - 2u;
   surface.allocation_rows = UINT32_MAX - 2u;
   assert(r300_zb_depth_layout_compute(&surface, R300_ZB_MACROTILE_BYTES,
                                       &layout) == -EINVAL);
}

/* A surface the descriptor validator refuses never reaches the tile
 * lookup, and null arguments refuse before anything is written. */
static void
test_refusals(void)
{
   struct r300_zb_depth_layout layout;
   assert(r300_zb_depth_layout_compute(NULL, 0u, &layout) == -EINVAL);
   assert(r300_zb_depth_layout_compute(&r300_zb_depth_surface_z24_linear, 0u,
                                       NULL) == -EINVAL);

   struct r300_zb_depth_surface surface = r300_zb_depth_surface_z24_linear;
   surface.bytes_per_pixel = 2u;
   assert(r300_zb_depth_layout_compute(&surface, 0u, &layout) == -EINVAL);

   uint32_t w = 0, h = 0;
   assert(r300_zb_depth_layout_tile_pixels(4u, R300_ZB_MICROTILE_TILED,
                                           R300_ZB_MACROTILE_TILED, NULL,
                                           &h) == -EINVAL);
   assert(r300_zb_depth_layout_tile_pixels(4u, R300_ZB_MICROTILE_TILED,
                                           R300_ZB_MACROTILE_TILED, &w,
                                           NULL) == -EINVAL);
   /* A pixel width no depth format stores has no row in the table. */
   assert(r300_zb_depth_layout_tile_pixels(8u, R300_ZB_MICROTILE_TILED,
                                           R300_ZB_MACROTILE_TILED, &w,
                                           &h) == -EINVAL);

   assert(!r300_zb_depth_layout_is_guard_byte(NULL, 0u));
}

int
main(void)
{
   test_tile_table_products();
   test_square_microtile_refuses_wide_pixels();
   test_linear_layout_matches_the_retained_footprint();
   test_microtiled_rung();
   test_macrotiled_envelope_exceeds_the_parser_bound();
   test_guard_ranges();
   test_guard_must_preserve_base_alignment();
   test_pitch_must_sit_on_the_tile_grid();
   test_overflow_refuses();
   test_refusals();
   printf("r300_zb_depth_layout_test: all checks passed\n");
   return 0;
}
