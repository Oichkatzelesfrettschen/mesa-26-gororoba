/* SPDX-License-Identifier: MIT */

#include "r300_zb_depth_layout.h"

#include "r300_reg.h"

#include <errno.h>
#include <stddef.h>

static bool
add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
   if (UINT64_MAX - a < b)
      return false;
   *out = a + b;
   return true;
}

static bool
mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
   if (a == 0u || b == 0u) {
      *out = 0u;
      return true;
   }
   if (a > UINT64_MAX / b)
      return false;
   *out = a * b;
   return true;
}

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

static int
linear_surface_check(const struct r300_zb_depth_surface *surface)
{
   if (surface == NULL)
      return -EINVAL;
   if (surface->microtile != R300_ZB_MICROTILE_LINEAR ||
       surface->macrotile != R300_ZB_MACROTILE_LINEAR)
      return -EINVAL;
   const bool z24 = surface->depth_format == R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL;
   const bool z16 = surface->depth_format == R300_DEPTHFORMAT_16BIT_INT_Z ||
                    surface->depth_format == R300_DEPTHFORMAT_16BIT_13E3;
   if ((!z24 && !z16) || surface->bytes_per_pixel != (z24 ? 4u : 2u) ||
       surface->width == 0 || surface->height == 0 ||
       surface->pitch_pixels < surface->width || surface->pitch_pixels % 4u ||
       surface->pitch_pixels > R300_DEPTHPITCH_MASK ||
       surface->allocation_rows < surface->height)
      return -EINVAL;
   return 0;
}

static int
linear_byte_offset(const struct r300_zb_depth_surface *surface,
                   uint64_t base_offset_bytes, uint32_t x, uint32_t y,
                   uint64_t *byte_offset_out)
{
   if (linear_surface_check(surface) != 0 || byte_offset_out == NULL)
      return -EINVAL;
   if (surface->microtile != R300_ZB_MICROTILE_LINEAR ||
       surface->macrotile != R300_ZB_MACROTILE_LINEAR)
      return -EINVAL;
   /* The render extent bounds the coordinate, not the allocation: a
    * caller reading the canary row addresses it as a byte range rather
    * than as a pixel. */
   if (x >= surface->width || y >= surface->height)
      return -EINVAL;
   if (surface->pitch_pixels == 0 || surface->bytes_per_pixel == 0)
      return -EINVAL;

   const uint64_t pixel_index =
      (uint64_t)y * (uint64_t)surface->pitch_pixels + (uint64_t)x;
   const uint64_t offset = pixel_index * (uint64_t)surface->bytes_per_pixel;
   /* Every term is bounded by the descriptor the validator admitted --
    * a pitch inside DEPTHPITCH, a height inside 32 bits, a pixel of at
    * most four bytes -- so the product cannot approach the 64-bit range
    * and only the caller's base can carry the sum out of it. */
   if (offset > UINT64_MAX - base_offset_bytes)
      return -EINVAL;

   *byte_offset_out = base_offset_bytes + offset;
   return 0;
}

static int
linear_coordinate(const struct r300_zb_depth_surface *surface,
                 uint64_t base_offset_bytes, uint64_t mapped_bytes,
                 uint64_t byte_offset,
                 struct r300_zb_depth_address_coordinate *out)
{
   if (surface == NULL || out == NULL)
      return -EINVAL;
   if (linear_surface_check(surface) != 0)
      return -EINVAL;

   const uint32_t cpp = surface->bytes_per_pixel;
   if (cpp == 0u || surface->pitch_pixels == 0u)
      return -EINVAL;
   if (byte_offset < base_offset_bytes)
      return -ERANGE;

   const uint64_t offset = byte_offset - base_offset_bytes;
   if (offset % cpp != 0u)
      return -ERANGE;

   uint64_t pitch_bytes;
   if (!mul_u64((uint64_t)surface->pitch_pixels, (uint64_t)cpp,
               &pitch_bytes))
      return -ERANGE;
   uint64_t storage_bytes;
   if (!mul_u64(pitch_bytes, (uint64_t)surface->allocation_rows,
               &storage_bytes))
      return -ERANGE;

   if (base_offset_bytes % cpp != 0 ||
       base_offset_bytes > mapped_bytes ||
       storage_bytes > mapped_bytes - base_offset_bytes ||
       offset >= storage_bytes || mapped_bytes < cpp || byte_offset > mapped_bytes - cpp)
      return -ERANGE;

   const uint64_t index = offset / cpp;
   const uint64_t x = index % (uint64_t)surface->pitch_pixels;
   const uint64_t y = index / (uint64_t)surface->pitch_pixels;

   *out = (struct r300_zb_depth_address_coordinate){
      .x = (uint32_t)x,
      .y = (uint32_t)y,
      .region = (x < surface->width && y < surface->height
                    ? R300_ZB_DEPTH_ADDRESS_LOGICAL
                    : R300_ZB_DEPTH_ADDRESS_PADDING),
   };
   return 0;
}

static int
r300_zb_depth_address_rs485m_tiled_surface_check(
   const struct r300_zb_depth_surface *surface)
{
   if (surface == NULL)
      return -EINVAL;
   if (surface->depth_format != R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL ||
       surface->bytes_per_pixel != 4u ||
       surface->microtile != R300_ZB_MICROTILE_TILED ||
       surface->width != 64u || surface->height != 64u ||
       (surface->macrotile != R300_ZB_MACROTILE_LINEAR &&
        surface->macrotile != R300_ZB_MACROTILE_TILED) ||
       surface->allocation_rows != 65u ||
       (surface->pitch_pixels != 64u && surface->pitch_pixels != 96u))
      return -EINVAL;
   return 0;
}

static int
r300_zb_depth_address_rs485m_tiled_byte_offset(
   const struct r300_zb_depth_surface *surface, uint64_t base_offset_bytes,
   uint32_t x, uint32_t y, uint64_t *byte_offset_out)
{
   if (surface == NULL || byte_offset_out == NULL)
      return -EINVAL;
   if (r300_zb_depth_address_rs485m_tiled_surface_check(surface) != 0)
      return -EINVAL;
   if (base_offset_bytes != 2048u && base_offset_bytes != 4096u)
      return -EINVAL;

   if (x >= surface->width || y >= surface->height)
      return -EINVAL;

   if (surface->macrotile == R300_ZB_MACROTILE_LINEAR) {
      const uint64_t block = (uint64_t)(y >> 1) * (surface->pitch_pixels >> 2) + (x >> 2);
      *byte_offset_out = base_offset_bytes + block * 32 +
                         4 * (4 * (y & 1) + (x & 3));
      return 0;
   }

   const uint32_t u = (x >> 2u) & 7u;
   const uint32_t v = (y >> 1u) & 7u;
   const uint32_t mx = x >> 5u;
   const uint32_t my = y >> 4u;

   const uint32_t l0 = u & 1u;
   const uint32_t l1 = v & 1u;
   const uint32_t l2 = ((u >> 1u) & 1u) ^ ((v >> 2u) & 1u);
   const uint32_t l3 = ((u >> 2u) & 1u) ^ ((v >> 1u) & 1u);
   const uint32_t l4 = ((u >> 2u) & 1u) ^ (my & 1u);
   const uint32_t l5 = ((v >> 2u) & 1u) ^ (mx & 1u);

   const uint32_t l = l0 | (l1 << 1u) | (l2 << 2u) | (l3 << 3u) |
                      (l4 << 4u) | (l5 << 5u);

   const uint32_t half_words = 4u * (4u * (y & 1u) + (x & 3u));
   const uint32_t words_per_macro = surface->pitch_pixels / 32u;
   uint64_t offset;
   if (!mul_u64((uint64_t)(my * words_per_macro + mx), 2048ull, &offset))
      return -ERANGE;

   uint64_t byte_offset;
   if (!add_u64(offset, 32ull * (uint64_t)l, &byte_offset))
      return -ERANGE;
   if (!add_u64(byte_offset, (uint64_t)half_words, &byte_offset))
      return -ERANGE;
   if (!add_u64(base_offset_bytes, byte_offset, &byte_offset))
      return -ERANGE;

   if (byte_offset < base_offset_bytes)
      return -ERANGE;
   if (byte_offset % 4ull != 0u)
      return -ERANGE;

   *byte_offset_out = byte_offset;
   return 0;
}

static int
r300_zb_depth_address_rs485m_tiled_coordinate(
   const struct r300_zb_depth_surface *surface, uint64_t base_offset_bytes,
   uint64_t mapped_bytes, uint64_t byte_offset,
   struct r300_zb_depth_address_coordinate *out)
{
   if (surface == NULL || out == NULL)
      return -EINVAL;
   if (r300_zb_depth_address_rs485m_tiled_surface_check(surface) != 0)
      return -EINVAL;
   if (base_offset_bytes != 2048u && base_offset_bytes != 4096u)
      return -EINVAL;

   if (surface->bytes_per_pixel != 4u)
      return -EINVAL;
   if (byte_offset < base_offset_bytes)
      return -ERANGE;

   uint64_t storage_rows_bytes;
   if (!mul_u64((uint64_t)surface->pitch_pixels * 4u,
               surface->macrotile == R300_ZB_MACROTILE_TILED ? 80u : 66u,
               &storage_rows_bytes))
      return -ERANGE;
   uint64_t offset;
   if (!add_u64(base_offset_bytes, storage_rows_bytes, &offset))
      return -ERANGE;
   if (offset > mapped_bytes || byte_offset >= offset ||
       mapped_bytes < 4u || byte_offset > mapped_bytes - 4u)
      return -ERANGE;
   if (byte_offset % 4ull != 0ull)
      return -ERANGE;
   uint64_t rel_offset = byte_offset - base_offset_bytes;

   if (surface->macrotile == R300_ZB_MACROTILE_LINEAR) {
      const uint32_t block = (uint32_t)(rel_offset / 32u);
      const uint32_t lane = (uint32_t)(rel_offset % 32u) / 4u;
      const uint32_t x = (block % (surface->pitch_pixels / 4u)) * 4u + (lane & 3u);
      const uint32_t y = (block / (surface->pitch_pixels / 4u)) * 2u + (lane >> 2u);
      *out = (struct r300_zb_depth_address_coordinate){x, y,
         x < surface->width && y < surface->height ?
         R300_ZB_DEPTH_ADDRESS_LOGICAL : R300_ZB_DEPTH_ADDRESS_PADDING};
      return 0;
   }

   const uint32_t word = (uint32_t)(rel_offset / 4ull);
   const uint32_t row_group = word / 512u;
   const uint32_t lane_group = word % 512u;
   const uint32_t l = lane_group / 8u;
   const uint32_t intra_lane = lane_group % 8u;

   if (l > 63u)
      return -ERANGE;

   const uint32_t pitch_groups = surface->pitch_pixels / 32u;
   if (pitch_groups == 0u)
      return -EINVAL;

   const uint32_t my = row_group / pitch_groups;
   const uint32_t mx = row_group - my * pitch_groups;

   const uint32_t l0 = l & 1u;
   const uint32_t l1 = (l >> 1u) & 1u;
   const uint32_t l2 = (l >> 2u) & 1u;
   const uint32_t l3 = (l >> 3u) & 1u;
   const uint32_t l4 = (l >> 4u) & 1u;
   const uint32_t l5 = (l >> 5u) & 1u;

   const uint32_t u0 = l0;
   const uint32_t v0 = l1;
   const uint32_t u2 = l4 ^ (my & 1u);
   const uint32_t v2 = l5 ^ (mx & 1u);
   const uint32_t u1 = l2 ^ v2;
   const uint32_t v1 = l3 ^ u2;

   const uint32_t u = u0 | (u1 << 1u) | (u2 << 2u);
   const uint32_t v = v0 | (v1 << 1u) | (v2 << 2u);

   const uint32_t x = (mx << 5u) | (u << 2u) | (intra_lane & 3u);
   const uint32_t y = (my << 4u) | (v << 1u) | ((intra_lane >> 2u) & 1u);

   *out = (struct r300_zb_depth_address_coordinate){
      .x = x,
      .y = y,
      .region = (x < surface->width && y < surface->height
                    ? R300_ZB_DEPTH_ADDRESS_LOGICAL
                    : R300_ZB_DEPTH_ADDRESS_PADDING),
   };
   return 0;
}

const struct r300_zb_depth_address_resolver r300_zb_depth_address_linear = {
   .name = "linear",
   .surface_check = linear_surface_check,
   .coordinate = linear_coordinate,
   .byte_offset = linear_byte_offset,
};

const struct r300_zb_depth_address_resolver
   r300_zb_depth_address_rs485m_tiled = {
      .name = "rs485m_tiled",
      .surface_check = r300_zb_depth_address_rs485m_tiled_surface_check,
      .coordinate = r300_zb_depth_address_rs485m_tiled_coordinate,
      .byte_offset = r300_zb_depth_address_rs485m_tiled_byte_offset,
   };

int
r300_zb_depth_address_checked(const struct r300_zb_depth_surface *surface,
                             uint64_t base_offset_bytes,
                             uint64_t mapped_bytes, uint32_t x, uint32_t y,
                             uint64_t *byte_offset_out)
{
   if (surface == NULL || surface->address_resolver == NULL ||
       surface->address_resolver->byte_offset == NULL ||
       surface->address_resolver->surface_check == NULL || byte_offset_out == NULL)
      return -EINVAL;

   if (!surface->logical_pixel_addressing ||
       r300_zb_depth_surface_check(surface) != 0)
      return -EINVAL;
   if (surface->address_resolver->surface_check(surface) != 0)
      return -EINVAL;

   uint32_t tile_width, tile_height;
   if (r300_zb_depth_layout_tile_pixels(surface->bytes_per_pixel,
                                        surface->microtile, surface->macrotile,
                                        &tile_width, &tile_height) != 0)
      return -EINVAL;
   const uint64_t rows = ((uint64_t)surface->allocation_rows + tile_height - 1u) /
                         tile_height * tile_height;
   const uint64_t storage_bytes = rows * surface->pitch_pixels * surface->bytes_per_pixel;
   if (base_offset_bytes >= mapped_bytes || storage_bytes > mapped_bytes - base_offset_bytes)
      return -ERANGE;
   if (surface->bytes_per_pixel == 0u)
      return -EINVAL;

   uint64_t byte_offset = 0u;
   const int rc =
      surface->address_resolver->byte_offset(surface, base_offset_bytes, x, y,
                                            &byte_offset);
   if (rc != 0)
      return rc;
   if (byte_offset % (uint64_t)surface->bytes_per_pixel != 0u)
      return -ERANGE;

   uint64_t byte_limit;
   if (!add_u64(byte_offset, (uint64_t)surface->bytes_per_pixel, &byte_limit))
      return -ERANGE;
   if (byte_limit > mapped_bytes)
      return -ERANGE;

   *byte_offset_out = byte_offset;
   return 0;
}

int
r300_zb_depth_packed_update(const struct r300_zb_depth_surface *surface,
                            uint32_t old_word, bool write_depth,
                            uint32_t depth_code, bool write_stencil,
                            uint32_t stencil, uint32_t stencil_write_mask,
                            uint32_t *word_out)
{
   if (surface == NULL || word_out == NULL)
      return -EINVAL;

   const uint32_t stencil_mask = r300_zb_depth_surface_stores_stencil(surface) ? 0xffu : 0u;

   uint32_t old_depth;
   uint32_t old_stencil;
   if (r300_zb_depth_unpack(surface, old_word, &old_depth, &old_stencil) != 0)
      return -EINVAL;

   uint32_t next_depth = old_depth;
   uint32_t next_stencil = old_stencil;

   if (write_depth) {
      next_depth = depth_code;
   }

   if (write_stencil) {
      if (stencil > stencil_mask)
         return -EINVAL;
      if ((stencil_write_mask & ~stencil_mask) != 0u)
         return -EINVAL;
      next_stencil = (old_stencil & ~stencil_write_mask) |
                     (stencil & stencil_write_mask);
   }

   return r300_zb_depth_pack(surface, next_depth, next_stencil, word_out);
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
