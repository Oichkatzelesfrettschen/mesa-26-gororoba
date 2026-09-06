/*
 * SPDX-License-Identifier: MIT
 *
 * Depth-surface descriptor: the format, tiling, geometry, sentinel, and
 * host capabilities of one depth surface, so a depth cell states its
 * surface as data instead of carrying it as constants.
 *
 * Four surfaces stand, one per rung of the ladder, and each rung moves
 * one variable: Z16 linear, Z24 linear, Z24 microtiled, Z24 microtiled
 * and macrotiled.  The Z16 linear surface is the one the depth control cell has always emitted, and its
 * fields equal that cell's macros exactly, so an emission taken from the
 * descriptor produces the stream the retained cell produced.  The Z24
 * linear surface changes the format alone, which establishes packed
 * depth behavior under addressing that already holds.  The Z24
 * microtiled surface changes the microtile, which is where a permutation
 * first separates a logical coordinate from a row-major byte.  The Z24
 * macrotiled surface adds macrotiling to it, and is the one the ZMASK
 * protocol needs: r300_zmask_layout admits a 32-bit microtiled surface
 * and its 8x8 compression case additionally requires macrotiling.
 *
 * The sentinel is a depth code, never a memory word.  R300 packs Z24/S8
 * with depth in bits 31:8 and stencil in bits 7:0, so the 24-bit
 * half-depth code 0x00800000 reaches memory as 0x80000000, and a code
 * copied straight into storage would name a depth of 0x008000 under a
 * stencil of 0x00.  r300_zb_depth_pack turns a code and a stencil into
 * the word; r300_zb_depth_unpack reads a word back into the pair; each
 * refuses a value wider than its field rather than truncating it.  Z16
 * stores the code itself, so the two coincide there and the distinction
 * shows up only at Z24.
 *
 * Host capability is four separate facts, because a tiled surface holds
 * some and not others.  raw_allocation_mapping says a host may map the
 * allocation and read its bytes as an unordered set of packed words.
 * uniform_packed_initialization says a host may write one repeated word
 * across the whole allocation, which is true of any uncompressed surface
 * whose tiling permutes complete pixels: a constant image is invariant
 * under a permutation.  logical_pixel_addressing says a host may compute
 * the byte a coordinate names, and logical_image_readback says it may
 * read the surface back as a row-major image; both require an address
 * transform.  Gallium never computes a tiled byte address -- r300_transfer.c
 * routes a tiled map through a linear shadow texture and lets the engine
 * move the bytes, and radeon_surface.c begins at CHIP_R600 -- so no
 * logical-to-physical transform for R300-class tiling exists in this
 * tree, and both tiled Z24 surfaces answer false to the last two until
 * one backed by its own authority exists.  The Z24 linear surface
 * holds all four, because linear addressing is the same arithmetic at
 * four bytes per pixel as at two.
 *
 * ZB_DEPTHPITCH carries the macrotile bit at 16 and the two-bit
 * microtile field at 17-18.  The kernel composes them from
 * reloc->tiling_flags in r300_packet0_check only when
 * RADEON_CS_KEEP_TILING_FLAGS is clear; the depth cell's submission sets
 * that flag, so the emitted word carries the tile bits this descriptor
 * declares.
 */

#ifndef R300_ZB_DEPTH_SURFACE_H
#define R300_ZB_DEPTH_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

/* ZB_DEPTHPITCH bits 17-18, the three microtile encodings; 3 is
 * reserved. */
enum r300_zb_microtile {
   R300_ZB_MICROTILE_LINEAR = 0,
   R300_ZB_MICROTILE_TILED = 1,
   R300_ZB_MICROTILE_TILED_SQUARE = 2,
};

/* ZB_DEPTHPITCH bit 16. */
enum r300_zb_macrotile {
   R300_ZB_MACROTILE_LINEAR = 0,
   R300_ZB_MACROTILE_TILED = 1,
};

struct r300_zb_depth_surface {
   const char *name;
   /* One complete ZB_FORMAT depth encoding. */
   uint32_t depth_format;
   /* Bytes the format stores per pixel, the width the kernel reads out of
    * ZB_FORMAT as track->zb.cpp and multiplies into its size bound. */
   uint32_t bytes_per_pixel;
   enum r300_zb_microtile microtile;
   enum r300_zb_macrotile macrotile;
   /* Render extent in pixels. */
   uint32_t width;
   uint32_t height;
   /* ZB_DEPTHPITCH pitch in pixels; R300_DEPTHPITCH_MASK reaches bits 2
    * through 13, so it is a multiple of four and at most 16380. */
   uint32_t pitch_pixels;
   /* Rows the allocation carries, one past the render extent so an
    * oracle reads the row past the extent as its canary. */
   uint32_t allocation_rows;
   /* The pre-draw depth code, in the format's depth field alone and
    * carrying no stencil.  It sits between the cell's two window-space
    * depths, so one comparison separates them.  The memory word comes
    * from r300_zb_depth_surface_packed_sentinel. */
   uint32_t depth_sentinel_code;
   /* A host may map the allocation and read its bytes as an unordered
    * set of packed words. */
   bool raw_allocation_mapping;
   /* A host may write one repeated packed word across the whole
    * allocation, padding included. */
   bool uniform_packed_initialization;
   /* A host may compute the byte a logical coordinate names. */
   bool logical_pixel_addressing;
   /* A host may read the surface back as a row-major image. */
   bool logical_image_readback;
};

/* The surface the depth control cell emits: Z16 integer, linear, 64x64
 * on a 64-pixel pitch with one allocation row past the extent, sentinel
 * code 0x8000 between window-space 0.25 and 0.75, every host capability
 * held. */
extern const struct r300_zb_depth_surface r300_zb_depth_surface_z16_linear;

/* The Z24 rung that keeps linear addressing: packed Z24/S8 on the Z16
 * surface's geometry, every host capability held, sentinel code
 * 0x00800000 reaching memory as 0x80000000. */
extern const struct r300_zb_depth_surface r300_zb_depth_surface_z24_linear;

/* The third rung: packed Z24/S8 microtiled on a macrotile-linear
 * surface, 4x2 pixels to a 32-byte tile.  Raw mapping and uniform
 * initialization hold; logical addressing and row-major readback answer
 * false, because the microtile permutes pixels within a row pair. */
extern const struct r300_zb_depth_surface r300_zb_depth_surface_z24_microtiled;

/* The surface the ZMASK ladder needs: packed Z24/S8, microtiled and
 * macrotiled, which is the configuration r300_zmask_layout admits at all
 * and the only one its 8x8 compression case accepts.  Sentinel code
 * 0x00800000 is the 24-bit half-depth value, the Z24 counterpart of the
 * Z16 cell's 0x8000, and it reaches memory as 0x80000000.  Raw mapping
 * and uniform initialization hold; logical addressing and row-major
 * readback answer false, so no cell that walks coordinates emits it
 * until an address model exists. */
extern const struct r300_zb_depth_surface r300_zb_depth_surface_z24_macrotiled;

/* The packed memory word one depth code and one stencil value make under
 * the surface's format: the code itself for either 16-bit encoding, and
 * (depth << 8) | stencil for packed Z24/S8.  Returns 0, or -EINVAL when
 * the surface is unreadable, the format unknown, the depth code wider
 * than the format's depth field, or the stencil wider than the format
 * stores -- a 16-bit surface carries no stencil, so a nonzero one
 * refuses. */
int r300_zb_depth_pack(const struct r300_zb_depth_surface *surface,
                       uint32_t depth_code, uint32_t stencil,
                       uint32_t *word_out);

/* The inverse: the depth code and stencil one packed word carries.
 * Returns 0, or -EINVAL when the surface is unreadable, the format
 * unknown, or the word carries a bit outside what the format stores. */
int r300_zb_depth_unpack(const struct r300_zb_depth_surface *surface,
                         uint32_t word, uint32_t *depth_code_out,
                         uint32_t *stencil_out);

/* The surface's sentinel as the word a host writes: the sentinel code
 * packed against a stencil of zero. */
int r300_zb_depth_surface_packed_sentinel(
   const struct r300_zb_depth_surface *surface, uint32_t *word_out);

/* The ZB_DEPTHPITCH tile bits the surface declares, for the submission
 * that keeps its own tiling flags. */
uint32_t r300_zb_depth_surface_tile_bits(
   const struct r300_zb_depth_surface *surface);

/* The bound the kernel's parser measures a depth binding against:
 * pitch * allocation_rows * cpp, the arithmetic r100_cs_track_check
 * computes as pitch * cpp * maxy.  An allocation meeting it is admitted
 * by the parser, which is a statement about admission alone.  A tiled
 * surface addresses beyond this, because tiling aligns the extent up to
 * whole microtiles and macrotiles, so a tiled allocation is sized from
 * the address model the surface declares rather than from this value. */
uint64_t r300_zb_depth_surface_kernel_bound_bytes(
   const struct r300_zb_depth_surface *surface);

/* Holds a surface to what the registers, the kernel, and the host
 * capabilities encode: a known format, a cpp that matches it, a pitch on
 * the four-pixel grid inside the DEPTHPITCH field and no smaller than
 * the width, an allocation covering the render extent, tile modes inside
 * their fields, a sentinel code that packs, and a capability set whose
 * members imply their prerequisites -- uniform initialization and
 * logical addressing each need raw mapping, row-major readback needs
 * logical addressing, and logical addressing needs a linear surface in
 * both tile dimensions.  Square microtiling additionally requires a
 * two-byte pixel: r300_get_pixel_alignment carries its tile shape at 16
 * bits per pixel alone.  Returns 0 or -EINVAL. */
int r300_zb_depth_surface_check(const struct r300_zb_depth_surface *surface);

#endif /* R300_ZB_DEPTH_SURFACE_H */
