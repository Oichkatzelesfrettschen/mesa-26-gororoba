/*
 * SPDX-License-Identifier: MIT
 *
 * Tiled storage extent of one depth surface: the bytes a tiled depth
 * surface occupies, the base alignment it demands, and the guard ranges
 * that bracket it.
 *
 * This is a separate calculation from r300_zb_depth_surface_kernel_bound_bytes.
 * That helper computes pitch * allocation_rows * cpp, the product
 * r100_cs_track_check measures a depth binding against, and it is a
 * parser-footprint value alone.  A tiled surface addresses past it,
 * because tiling rounds the stored extent up to whole microtiles and
 * whole macrotiles, so tiled storage is sized here and admission is
 * measured there.
 *
 * The tile dimensions come from r300_get_pixel_alignment in
 * src/gallium/drivers/r300/r300_texture_desc.c, the in-tree R300 layout
 * authority, indexed by macrotile, log2(bytes per pixel), and microtile.
 * Its two depth-format rows in full, every nonzero entry:
 *
 *   cpp  micro   macro    tile w x h   bytes    class
 *   2    linear  linear   16 x 1       32       pitch alignment
 *   2    tiled   linear   8 x 2        32       microtile
 *   2    square  linear   4 x 4        32       microtile
 *   2    linear  tiled    128 x 8      2048     macrotile
 *   2    tiled   tiled    64 x 16      2048     macrotile
 *   2    square  tiled    32 x 32      2048     macrotile
 *   4    linear  linear   8 x 1        32       pitch alignment
 *   4    tiled   linear   4 x 2        32       microtile
 *   4    linear  tiled    64 x 8       2048     macrotile
 *   4    tiled   tiled    32 x 16      2048     macrotile
 *   4    square  either   0 x 0        --       no encoding
 *
 * Two invariants hold across the whole table and the self-test enforces
 * both.  Every macrotile-linear entry multiplies to 32 bytes and every
 * macrotile-tiled entry to 2048, the microblock and macrotile sizes the
 * R3xx-R5xx tiling description states.  And each macrotile covers
 * exactly 64 microtiles, so the ratio of the two areas at one cpp is 64
 * whichever microtile mode is selected -- which is what catches a pair
 * of rows transposed between the two pixel widths, since a transposition
 * preserves the byte products alone.  The square-microtile column is
 * zero outside cpp 2, and r300_get_pixel_alignment asserts on a zero
 * tile, so that mode has no encoding for a four-byte pixel.
 *
 * Storage takes the two rounding steps r300_texture_get_stride and
 * r300_texture_get_nblocksy take for a single-level non-scanout 2D
 * surface on a family other than RS690: align the width up to the tile
 * width to get the pitch, align the height up to the tile height to get
 * the stored rows, and multiply.  Those functions carry further
 * adjustments -- a doubled stride alignment for macrotiled NPOT
 * stride-addressed textures, a power-of-two height for mipmapped or
 * non-2D targets, a doubled height alignment on the CBZB path, and the
 * RS690 and scanout widenings inside r300_get_pixel_alignment -- none of
 * which this depth path reaches, and each of which only ever raises an
 * alignment a pitch on the tile grid already satisfies.
 *
 * A macrotiled surface therefore occupies a whole number of 2048-byte
 * macrotiles, and that is also its base alignment.  r300_setup_cbzb_flags
 * gives the constraint from the ZB unit's side: the midpoint ZB offset
 * the CBZB fast clear hands it returns garbage at certain surface sizes
 * unless it is 2048-aligned, and macrotiling is what supplies the
 * alignment.  r300_setup_miptree produces that placement, though not
 * because every level is macrotile-sized -- r300_texture_macro_switch
 * drops small levels to linear, and a linear level is strided and
 * height-aligned at 32-byte granularity.  It holds because that switch
 * compares u_minify(dim, level), non-increasing in level, against a
 * threshold that does not depend on the level, so the predicate is
 * monotone and the macrotiled levels form a prefix.  No macrotiled level
 * ever follows a linear one, so every macrotiled offset is a sum of
 * whole macrotiles.
 *
 * The guards sit outside the storage envelope rather than inside it.  A
 * linear surface can spend its last row as a canary, because
 * a(x, y) = base + cpp * (y * pitch + x) puts row 64 of a 64-row extent
 * exactly at the first byte past the extent.  A macrotiled surface
 * cannot: ceil(65 / 16) = 5 macrotile rows cover logical rows 0 through
 * 79, so the 65th row shares a macrotile with rows the surface renders.
 * The layout answers that with an explicit prefix guard, the storage
 * envelope, and a suffix guard, each a byte range a host initializes and
 * reads back separately.
 */

#ifndef R300_ZB_DEPTH_LAYOUT_H
#define R300_ZB_DEPTH_LAYOUT_H

#include "r300_zb_depth_surface.h"

#include <stdbool.h>
#include <stdint.h>

/* Bytes one microblock holds, the granularity a microtile covers. */
#define R300_ZB_MICROTILE_BYTES 32u
/* Bytes one macrotile holds. */
#define R300_ZB_MACROTILE_BYTES 2048u

/* The guard size a tiled surface takes: one macrotile on each side, so
 * a guard is never a partial tile and the storage envelope begins on a
 * macrotile boundary.  A caller passes its own size, and the linear
 * controls pass zero because their last allocation row already sits
 * exactly one row past the render extent. */
#define R300_ZB_DEPTH_GUARD_BYTES R300_ZB_MACROTILE_BYTES

/* How far the model that produced this layout is established. */
enum r300_zb_depth_layout_maturity {
   /* Tile dimensions and storage extent follow r300_get_pixel_alignment
    * and r300_setup_miptree, which decide how many bytes a level
    * occupies.  Which byte inside the envelope a logical coordinate
    * reaches is a separate transform, and no silicon observation of it
    * exists in this tree. */
   R300_ZB_DEPTH_LAYOUT_BLOCK_ENVELOPE = 0,
   /* The permutation inside the envelope is established by retained
    * observation as well. */
   R300_ZB_DEPTH_LAYOUT_RESOLVED_PERMUTATION = 1,
};

struct r300_zb_depth_layout {
   uint32_t width;
   uint32_t height;
   /* Pitch the surface declares and the pitch its tiling demands.  They
    * are equal when the declared pitch already sits on the tile grid,
    * which is the only case this layout admits: a pitch below the tile
    * width would make ZB_DEPTHPITCH and the storage extent disagree. */
   uint32_t pitch_pixels;
   uint32_t pitch_bytes;
   uint32_t bytes_per_pixel;
   /* Pixels one microtile and one macrotile span.  A surface that is
    * linear in one class reports 0 for that class's pair rather than a
    * usable alignment, so a consumer tests the mode before rounding
    * against either.  A macrotiled surface reports both: the macrotile
    * it is built from and the microtile that macrotile subdivides
    * into. */
   uint32_t microtile_width;
   uint32_t microtile_height;
   uint32_t macrotile_width;
   uint32_t macrotile_height;
   /* Rows the tiled extent stores, the allocation rows rounded up to a
    * whole tile height. */
   uint32_t storage_rows;
   /* Rows the surface's own allocation_rows names, before tile rounding. */
   uint32_t allocation_rows;
   /* Bytes the storage envelope spans: pitch_bytes * storage_rows. */
   uint64_t storage_bytes;
   /* Byte offset of the storage envelope inside the buffer object, and
    * the alignment that offset satisfies. */
   uint64_t base_offset_bytes;
   uint32_t base_alignment_bytes;
   /* Guard ranges, in buffer-object bytes.  The prefix ends where the
    * envelope begins and the suffix begins where it ends. */
   uint64_t prefix_guard_offset_bytes;
   uint64_t prefix_guard_bytes;
   uint64_t suffix_guard_offset_bytes;
   uint64_t suffix_guard_bytes;
   /* Bytes the whole buffer object holds: both guards plus the
    * envelope. */
   uint64_t total_bytes;
   /* r100_cs_track_check's depth bound with this surface's rows standing
    * in for the draw's: the kernel computes zb.pitch * zb.cpp * maxy and
    * then adds zb.offset, where maxy comes from the scissor the draw
    * establishes rather than from any descriptor field.  This models it
    * at maxy = allocation_rows, the largest row the surface declares, and
    * carries the product alone.  The quantity an allocation is compared
    * against is therefore base_offset_bytes + kernel_bound_bytes, not
    * this field by itself. */
   uint64_t kernel_bound_bytes;
   enum r300_zb_depth_layout_maturity maturity;
};

/* Computes the layout of surface, placing its storage envelope after a
 * prefix guard of guard_bytes and a suffix guard of the same size.
 * guard_bytes of zero produces an unguarded layout whose total equals
 * its storage extent, which is what a linear surface takes: its last
 * allocation row is already the first byte past the render extent.
 *
 * Returns 0, or -EINVAL when surface or out is null, the surface fails
 * r300_zb_depth_surface_check, its microtile and macrotile pair has no
 * entry in the R300 tile table, its declared pitch is not a multiple of
 * the tile width, guard_bytes is not a multiple of the base alignment,
 * or any product overflows the field that stores it.
 *
 * Every product is computed in 64 bits and compared against the bound of
 * the field it lands in, so an extent no allocation can hold refuses
 * here rather than producing a truncated size a placement then accepts.
 */
int r300_zb_depth_layout_compute(const struct r300_zb_depth_surface *surface,
                                 uint32_t guard_bytes,
                                 struct r300_zb_depth_layout *out);

/* The tile dimensions one (bytes per pixel, microtile, macrotile) triple
 * names, in pixels, as r300_get_pixel_alignment reports them.  Returns 0
 * and writes both dimensions, or -EINVAL when the triple has no entry --
 * which is how square microtiling refuses outside a two-byte pixel.
 *
 * Every admitted triple reports a nonzero pair, the fully linear one
 * included, where the table carries the pitch alignment a linear surface
 * takes rather than a tile.  A caller rounds against the returned value
 * unconditionally.  A refused triple writes neither output.
 */
int r300_zb_depth_layout_tile_pixels(uint32_t bytes_per_pixel,
                                     enum r300_zb_microtile microtile,
                                     enum r300_zb_macrotile macrotile,
                                     uint32_t *width_out,
                                     uint32_t *height_out);

/* True when byte_offset falls inside one of the layout's two guard
 * ranges.  A discovery oracle scans the whole allocation and separates
 * guard bytes from envelope bytes through this. */
bool r300_zb_depth_layout_is_guard_byte(const struct r300_zb_depth_layout *layout,
                                        uint64_t byte_offset);

#endif /* R300_ZB_DEPTH_LAYOUT_H */
