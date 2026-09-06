/* SPDX-License-Identifier: MIT */

#include "r300_zb_depth_layout.h"

#include "r300_reg.h"

#include <errno.h>
#include <stddef.h>

/* One row of the R300 tile table: the pixels a tile spans in each
 * dimension for one (cpp, microtile, macrotile) triple.  The values are
 * r300_get_pixel_alignment's table entries; a triple absent from this
 * array is a zero entry there, which that function asserts on. */
struct tile_shape {
   uint32_t bytes_per_pixel;
   enum r300_zb_microtile microtile;
   enum r300_zb_macrotile macrotile;
   uint32_t width;
   uint32_t height;
};

/* The depth formats reach cpp 2 and cpp 4 alone, so the table carries
 * those two rows of r300_get_pixel_alignment and no others.  Every
 * macrotile-linear entry multiplies to R300_ZB_MICROTILE_BYTES and every
 * macrotile-tiled entry to R300_ZB_MACROTILE_BYTES, and each macrotile
 * covers 64 microtiles; the test holds every row to both.
 *
 * Square microtiling appears at cpp 2 alone.  Its cpp-4 entries in
 * r300_get_pixel_alignment are {0, 0}, an alignment of zero pixels that
 * no width rounds to, so the mode has no encoding for a four-byte pixel
 * and its absence here refuses it. */
static const struct tile_shape tile_table[] = {
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

int
r300_zb_depth_layout_tile_pixels(uint32_t bytes_per_pixel,
                                 enum r300_zb_microtile microtile,
                                 enum r300_zb_macrotile macrotile,
                                 uint32_t *width_out, uint32_t *height_out)
{
   if (width_out == NULL || height_out == NULL)
      return -EINVAL;

   for (size_t i = 0; i < sizeof(tile_table) / sizeof(tile_table[0]); i++) {
      const struct tile_shape *shape = &tile_table[i];
      if (shape->bytes_per_pixel != bytes_per_pixel ||
          shape->microtile != microtile || shape->macrotile != macrotile)
         continue;
      *width_out = shape->width;
      *height_out = shape->height;
      return 0;
   }
   return -EINVAL;
}

/* Rounds value up to the next multiple of alignment, refusing rather
 * than wrapping when the rounded value leaves the 32-bit field the
 * layout stores it in. */
static bool
align_u32(uint32_t value, uint32_t alignment, uint32_t *out)
{
   if (alignment == 0)
      return false;
   const uint64_t rounded =
      ((uint64_t)value + alignment - 1u) / alignment * (uint64_t)alignment;
   if (rounded > UINT32_MAX)
      return false;
   *out = (uint32_t)rounded;
   return true;
}

int
r300_zb_depth_layout_compute(const struct r300_zb_depth_surface *surface,
                             uint32_t guard_bytes,
                             struct r300_zb_depth_layout *out)
{
   if (surface == NULL || out == NULL)
      return -EINVAL;
   if (r300_zb_depth_surface_check(surface) != 0)
      return -EINVAL;

   uint32_t tile_width, tile_height;
   if (r300_zb_depth_layout_tile_pixels(surface->bytes_per_pixel,
                                        surface->microtile,
                                        surface->macrotile, &tile_width,
                                        &tile_height) != 0)
      return -EINVAL;

   /* ZB_DEPTHPITCH carries the declared pitch and the storage extent is
    * computed from the same value, so the two agree only when the
    * declared pitch already sits on the tile grid.  A pitch below it
    * would make the register name one row stride and the tiling another. */
   if (surface->pitch_pixels % tile_width != 0)
      return -EINVAL;

   uint32_t storage_rows;
   if (!align_u32(surface->allocation_rows, tile_height, &storage_rows))
      return -EINVAL;

   /* r300_zb_depth_surface_check caps the pitch at R300_DEPTHPITCH_MASK
    * and the pixel at four bytes, so this product cannot exceed 65520
    * today and the bound never fires.  It stands so a later widening of
    * the DEPTHPITCH field refuses here rather than truncating into
    * pitch_bytes. */
   const uint64_t pitch_bytes =
      (uint64_t)surface->pitch_pixels * (uint64_t)surface->bytes_per_pixel;
   if (pitch_bytes > UINT32_MAX)
      return -EINVAL;

   const uint64_t storage_bytes = pitch_bytes * (uint64_t)storage_rows;

   /* A macrotiled surface's base is a macrotile boundary; the header
    * carries the CBZB and miptree-prefix arguments for it.  A microtiled
    * or linear surface needs the DEPTHOFFSET field's own granularity,
    * which R300_ZB_DEPTHOFFSET encodes at bits 31 to 5. */
   const uint32_t base_alignment =
      surface->macrotile == R300_ZB_MACROTILE_TILED ? R300_ZB_MACROTILE_BYTES
                                                    : 32u;

   /* The prefix guard displaces the envelope, so a guard that is not a
    * whole multiple of the base alignment would put the surface base
    * off its boundary.  Refusing here keeps the guard scheme and the
    * alignment rule from contradicting each other. */
   if (guard_bytes % base_alignment != 0)
      return -EINVAL;
   const uint64_t base_offset = guard_bytes;

   const uint64_t total_bytes = base_offset + storage_bytes + guard_bytes;
   /* ZB_DEPTHOFFSET stores a 32-bit byte offset and the kernel measures
    * the binding against a 32-bit size, so an allocation past that bound
    * has no encoding. */
   if (storage_bytes > UINT32_MAX || total_bytes > UINT32_MAX)
      return -EINVAL;

   *out = (struct r300_zb_depth_layout){
      .width = surface->width,
      .height = surface->height,
      .pitch_pixels = surface->pitch_pixels,
      .pitch_bytes = (uint32_t)pitch_bytes,
      .bytes_per_pixel = surface->bytes_per_pixel,
      .microtile_width =
         surface->macrotile == R300_ZB_MACROTILE_TILED ? 0u : tile_width,
      .microtile_height =
         surface->macrotile == R300_ZB_MACROTILE_TILED ? 0u : tile_height,
      .macrotile_width =
         surface->macrotile == R300_ZB_MACROTILE_TILED ? tile_width : 0u,
      .macrotile_height =
         surface->macrotile == R300_ZB_MACROTILE_TILED ? tile_height : 0u,
      .storage_rows = storage_rows,
      .allocation_rows = surface->allocation_rows,
      .storage_bytes = storage_bytes,
      .base_offset_bytes = base_offset,
      .base_alignment_bytes = base_alignment,
      .prefix_guard_offset_bytes = 0u,
      .prefix_guard_bytes = guard_bytes,
      .suffix_guard_offset_bytes = base_offset + storage_bytes,
      .suffix_guard_bytes = guard_bytes,
      .total_bytes = total_bytes,
      .kernel_bound_bytes = r300_zb_depth_surface_kernel_bound_bytes(surface),
      .maturity = R300_ZB_DEPTH_LAYOUT_BLOCK_ENVELOPE,
   };

   /* The microtile dimensions of a macrotiled surface are the microtile
    * shape at the same cpp with macrotiling cleared, which is what the
    * macrotile subdivides into.  A triple the table admits with
    * macrotiling set is admitted with it clear, so this lookup resolves. */
   if (surface->macrotile == R300_ZB_MACROTILE_TILED &&
       r300_zb_depth_layout_tile_pixels(surface->bytes_per_pixel,
                                        surface->microtile,
                                        R300_ZB_MACROTILE_LINEAR,
                                        &out->microtile_width,
                                        &out->microtile_height) != 0)
      return -EINVAL;

   return 0;
}

bool
r300_zb_depth_layout_is_guard_byte(const struct r300_zb_depth_layout *layout,
                                   uint64_t byte_offset)
{
   if (layout == NULL)
      return false;
   if (byte_offset >= layout->prefix_guard_offset_bytes &&
       byte_offset <
          layout->prefix_guard_offset_bytes + layout->prefix_guard_bytes)
      return true;
   return byte_offset >= layout->suffix_guard_offset_bytes &&
          byte_offset <
             layout->suffix_guard_offset_bytes + layout->suffix_guard_bytes;
}
