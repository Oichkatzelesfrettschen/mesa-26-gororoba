/*
 * SPDX-License-Identifier: MIT
 *
 * ZMASK RAM layout for one depth level on an R300-class part.
 *
 * ZMASK compresses a depth surface into an on-chip RAM: each compressed
 * tile takes two bits, so one dword covers sixteen tiles and the RAM
 * budget bounds the surface a level may compress.  The layout is a pure
 * function of the level's row pitch, its height, the compression block
 * size, and the pipe count; it decides the ZB_ZMASK_PITCH value and the
 * dword count the 3D_CLEAR_ZMASK payload carries.
 */

#ifndef R300_ZMASK_LAYOUT_H
#define R300_ZMASK_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/* The tile one ZMASK dword covers, in 4x4 or 8x8 compression blocks,
 * indexed by pipe count minus one.  r300_setup_hyperz_properties in
 * r300_texture_desc.c carries the same two tables under the names
 * zmask_blocks_x_per_dw and zmask_blocks_y_per_dw.
 */
#define R300_ZMASK_MAX_PIPES 4u

/* Inputs the ZMASK derivation reads.  stride_in_pixels is the level's
 * row pitch converted back to pixels, which r300_stride_to_width returns
 * from tex.stride_in_bytes; it equals the level width only for a linear
 * untiled level, and macrotile alignment inflates it well past the
 * width.  The derivation aligns it to sixteen before anything else.
 */
struct r300_zmask_layout_params {
   uint32_t stride_in_pixels;
   uint32_t height;
   /* Bytes per pixel of the depth format.  ZMASK admits the 32-bit
    * depth formats alone, so Z24S8 and Z24X8 share one path and Z16
    * compresses nothing.
    */
   uint32_t depth_bytes_per_pixel;
   bool is_depth_or_stencil;
   bool microtile;
   bool macrotile;
   uint32_t num_samples;
   /* caps.z_compress == R300_ZCOMP_8X8, which r300_parse_chipset sets
    * for RV350 and later, CHIP_RS480 included.
    */
   bool zcomp8x8_capable;
   /* r300_hyperz_pipe_count: the Z-pipe count on CHIP_RV530 and the GB
    * pipe count everywhere else.  CHIP_RS480 runs one pipe.
    */
   uint32_t pipes;
   /* caps.zmask_ram: the per-pipe dword budget. */
   uint32_t zmask_ram_dwords_per_pipe;
};

struct r300_zmask_layout {
   /* ZB_ZMASK_PITCH, in pixels, or zero when the level does not fit. */
   uint32_t stride_in_pixels;
   /* The 3D_CLEAR_ZMASK payload count, or zero. */
   uint32_t dwords;
   bool fits_zmask_ram;
   /* The budget the fit compares against: per-pipe dwords times pipes. */
   uint32_t zmask_ram_dwords;
   /* The compression block the fit was decided with: 8x8 needs a
    * macrotiled single-sample level on a capable part, otherwise 4x4.
    */
   bool zcomp8x8;
};

/* Computes the layout.  pipes outside [1, R300_ZMASK_MAX_PIPES] or a
 * zero height is -EINVAL; a level ZMASK never covers -- a non-depth
 * format, a format other than 32 bits per pixel, or an untiled level --
 * yields a zeroed layout and returns 0.
 */
int r300_zmask_layout_compute(const struct r300_zmask_layout_params *params,
                              struct r300_zmask_layout *out);

/* The per-pipe ZMASK RAM budget r300_parse_chipset assigns a family:
 * R300_ZMASK_SIZE_PER_PIPE on R300, R350 and R4xx and later, and
 * RV3xx_ZMASK_SIZE on the RV3xx parts and the RS4xx IGPs including
 * CHIP_RS480.  A family with no ZMASK RAM reports zero.
 */
uint32_t r300_zmask_ram_dwords_per_pipe(int family);

/* caps.z_compress: R300_ZCOMP_8X8 from CHIP_RV350 onward. */
bool r300_zmask_zcomp8x8_capable(int family);

#endif /* R300_ZMASK_LAYOUT_H */
