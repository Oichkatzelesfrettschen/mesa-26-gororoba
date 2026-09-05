/*
 * SPDX-License-Identifier: MIT
 *
 * Depth-surface descriptor: the format, tiling, geometry, and host
 * addressability of one depth surface, so a depth cell states its
 * surface as data instead of carrying it as constants.
 *
 * Two surfaces stand.  The Z16 linear surface is the one the depth
 * control cell has always emitted, and its fields equal that cell's
 * macros exactly, so an emission taken from the descriptor produces the
 * stream the retained cell produced.  The Z24 macrotiled surface is the
 * one the ZMASK protocol needs -- r300_zmask_layout admits a 32-bit
 * microtiled surface and its 8x8 compression case additionally requires
 * macrotiling -- and it is defined here as the target the ladder aims
 * at.
 *
 * A macrotiled surface is not host-addressable.  Gallium never computes
 * a tiled byte address: r300_transfer.c routes a tiled map through a
 * linear shadow texture and lets the engine move the bytes, and
 * radeon_surface.c begins at CHIP_R600, so no logical-to-physical
 * transform for R300-class tiling exists in this tree.  A host fill that
 * assumed row y starts at base + y * pitch would scatter the depth
 * sentinel across tiles, so host_addressable states whether a host may
 * write or read the surface directly, and the Z24 surface answers false
 * until an address model backed by its own authority exists.
 *
 * ZB_DEPTHPITCH carries the tile bits in 16 and 17.  The kernel composes
 * them from reloc->tiling_flags in r300_packet0_check only when
 * RADEON_CS_KEEP_TILING_FLAGS is clear; the depth cell's submission sets
 * that flag, so the emitted word carries the tile bits this descriptor
 * declares.
 */

#ifndef R300_ZB_DEPTH_SURFACE_H
#define R300_ZB_DEPTH_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

/* ZB_DEPTHPITCH bit 17, the three microtile encodings. */
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
   /* The pre-draw depth value, in the format's own encoding.  It sits
    * between the cell's two window-space depths, so one comparison
    * separates them. */
   uint32_t depth_sentinel;
   /* Whether a host may address the surface as base + y * pitch * cpp.
    * A tiled surface answers false: the byte a logical coordinate names
    * follows a tile transform this tree does not carry. */
   bool host_addressable;
};

/* The surface the depth control cell emits: Z16 integer, linear, 64x64
 * on a 64-pixel pitch with one allocation row past the extent, sentinel
 * 0x8000 between window-space 0.25 and 0.75. */
extern const struct r300_zb_depth_surface r300_zb_depth_surface_z16_linear;

/* The surface the ZMASK ladder needs: packed Z24/S8, microtiled and
 * macrotiled, which is the configuration r300_zmask_layout admits at all
 * and the only one its 8x8 compression case accepts.  Sentinel 0x00800000
 * is the 24-bit half-depth value, the Z24 counterpart of the Z16 cell's
 * 0x8000.  It is not host-addressable, so no cell emits it until an
 * address model exists. */
extern const struct r300_zb_depth_surface r300_zb_depth_surface_z24_macrotiled;

/* The ZB_DEPTHPITCH tile bits the surface declares, for the submission
 * that keeps its own tiling flags. */
uint32_t r300_zb_depth_surface_tile_bits(
   const struct r300_zb_depth_surface *surface);

/* Bytes the allocation carries: pitch * allocation_rows * cpp.  A tiled
 * surface's true footprint is at least this, because tiling aligns the
 * extent up; the value is the kernel's own bound, which
 * r100_cs_track_check computes as pitch * cpp * maxy. */
uint64_t r300_zb_depth_surface_bytes(
   const struct r300_zb_depth_surface *surface);

/* Holds a surface to what the registers and the kernel encode: a known
 * format, a cpp that matches it, a pitch on the four-pixel grid inside
 * the DEPTHPITCH field and no smaller than the width, an allocation
 * covering the render extent, and tile modes inside their fields.
 * Returns 0 or -EINVAL. */
int r300_zb_depth_surface_check(const struct r300_zb_depth_surface *surface);

#endif /* R300_ZB_DEPTH_SURFACE_H */
