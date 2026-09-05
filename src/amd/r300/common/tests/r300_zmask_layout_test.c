/* SPDX-License-Identifier: MIT */

/* Cross-check of the common ZMASK layout against a transcription of the
 * Gallium derivation.  The transcription below reproduces
 * r300_setup_hyperz_properties and r300_pixels_to_dwords from
 * src/gallium/drivers/r300/r300_texture_desc.c by hand, with its own
 * alignment helpers, so the comparison runs between two independent
 * implementations rather than one calling the other.
 */

#include "r300_zmask_layout.h"

#include "amd_family.h"
#include "r300_chipset.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

/* align(): the power-of-two mask form Gallium uses for the height and
 * for the sixteen-pixel stride step.
 */
static unsigned
gallium_align(unsigned value, unsigned alignment)
{
   return (value + alignment - 1) & ~(alignment - 1);
}

/* util_align_npot(): the division form Gallium uses for the ZMASK x
 * block, whose three-pipe values of 48 and 96 are not powers of two.
 */
static unsigned
gallium_align_npot(unsigned value, unsigned alignment)
{
   if (value % alignment != 0)
      return value + alignment - (value % alignment);
   return value;
}

static unsigned
gallium_pixels_to_dwords(unsigned stride, unsigned height, unsigned xblock,
                         unsigned yblock)
{
   return (gallium_align_npot(stride, xblock) * gallium_align(height, yblock)) /
          (xblock * yblock);
}

struct gallium_zmask {
   unsigned zmask_dwords;
   unsigned zmask_stride_in_pixels;
   bool zcomp8x8;
};

/* r300_setup_hyperz_properties, ZMASK half only. */
static void
gallium_zmask_properties(unsigned stride_in_pixels, unsigned height,
                         unsigned blocksizebits, bool depth_or_stencil,
                         bool microtile, bool macrotile, unsigned nr_samples,
                         bool zcomp8x8_capable, unsigned pipes,
                         unsigned zmask_ram, struct gallium_zmask *out)
{
   static unsigned zmask_blocks_x_per_dw[4] = {4, 8, 12, 8};
   static unsigned zmask_blocks_y_per_dw[4] = {4, 4, 4, 8};

   out->zmask_dwords = 0;
   out->zmask_stride_in_pixels = 0;
   out->zcomp8x8 = false;

   if (!(depth_or_stencil && blocksizebits == 32 && microtile))
      return;

   unsigned stride = gallium_align(stride_in_pixels, 16);
   unsigned zcompsize = zcomp8x8_capable && macrotile && nr_samples <= 1 ? 8 : 4;
   unsigned zcomp_numdw = gallium_pixels_to_dwords(
      stride, height, zmask_blocks_x_per_dw[pipes - 1] * zcompsize,
      zmask_blocks_y_per_dw[pipes - 1] * zcompsize);

   if (blocksizebits == 32 && zcomp_numdw <= zmask_ram * pipes) {
      out->zmask_dwords = zcomp_numdw;
      out->zcomp8x8 = zcompsize == 8;
      out->zmask_stride_in_pixels = gallium_align_npot(
         stride, zmask_blocks_x_per_dw[pipes - 1] * zcompsize);
   }
}

static const uint32_t extents[] = {1,  2,   7,   8,    16,   17,
                                   64, 100, 256, 257, 1024, 2048};
#define EXTENT_COUNT (sizeof(extents) / sizeof(extents[0]))

/* Z16 at two bytes, Z24S8 and Z24X8 at four.  Both 32-bit formats are
 * depth-or-stencil of the same block size, so the derivation treats them
 * identically; the sweep carries both to prove it.
 */
struct format_case {
   const char *name;
   uint32_t bytes_per_pixel;
};
static const struct format_case formats[] = {
   {"Z16", 2},
   {"Z24S8", 4},
   {"Z24X8", 4},
};
#define FORMAT_COUNT (sizeof(formats) / sizeof(formats[0]))

static uint32_t
sweep(uint32_t pipes, uint32_t zmask_ram)
{
   uint32_t points = 0;

   for (size_t f = 0; f < FORMAT_COUNT; f++) {
      for (size_t w = 0; w < EXTENT_COUNT; w++) {
         for (size_t h = 0; h < EXTENT_COUNT; h++) {
            for (int micro = 0; micro <= 1; micro++) {
               for (int zcomp = 0; zcomp <= 1; zcomp++) {
                  /* 8x8 compression needs a macrotiled level, so the
                   * macrotile flag rides the zcomp8x8 axis. */
                  const bool macrotile = zcomp != 0;

                  struct r300_zmask_layout_params params = {
                     .stride_in_pixels = extents[w],
                     .height = extents[h],
                     .depth_bytes_per_pixel = formats[f].bytes_per_pixel,
                     .is_depth_or_stencil = true,
                     .microtile = micro != 0,
                     .macrotile = macrotile,
                     .num_samples = 1,
                     .zcomp8x8_capable = zcomp != 0,
                     .pipes = pipes,
                     .zmask_ram_dwords_per_pipe = zmask_ram,
                  };
                  struct r300_zmask_layout layout;
                  assert(r300_zmask_layout_compute(&params, &layout) == 0);

                  struct gallium_zmask ref;
                  gallium_zmask_properties(
                     extents[w], extents[h],
                     formats[f].bytes_per_pixel * 8, true, micro != 0,
                     macrotile, 1, zcomp != 0, pipes, zmask_ram, &ref);

                  assert(layout.dwords == ref.zmask_dwords);
                  assert(layout.stride_in_pixels ==
                         ref.zmask_stride_in_pixels);
                  assert(layout.zcomp8x8 == ref.zcomp8x8);
                  assert(layout.fits_zmask_ram == (ref.zmask_dwords != 0));
                  assert(layout.zmask_ram_dwords == zmask_ram * pipes);
                  points++;
               }
            }
         }
      }
   }
   return points;
}

static void
check_ram_table(void)
{
   assert(r300_zmask_ram_dwords_per_pipe(CHIP_RS480) == RV3xx_ZMASK_SIZE);
   assert(r300_zmask_ram_dwords_per_pipe(CHIP_RC410) == RV3xx_ZMASK_SIZE);
   assert(r300_zmask_ram_dwords_per_pipe(CHIP_RV350) == RV3xx_ZMASK_SIZE);
   assert(r300_zmask_ram_dwords_per_pipe(CHIP_R300) ==
          R300_ZMASK_SIZE_PER_PIPE);
   assert(r300_zmask_ram_dwords_per_pipe(CHIP_R580) ==
          R300_ZMASK_SIZE_PER_PIPE);
   /* The RS400, RS600, RS690 and RS740 IGPs carry no ZMASK RAM. */
   assert(r300_zmask_ram_dwords_per_pipe(CHIP_RS400) == 0);
   assert(r300_zmask_ram_dwords_per_pipe(CHIP_RS690) == 0);

   assert(r300_zmask_zcomp8x8_capable(CHIP_RS480));
   assert(!r300_zmask_zcomp8x8_capable(CHIP_R300));
}

static void
check_refusals(void)
{
   struct r300_zmask_layout layout;
   struct r300_zmask_layout_params params = {
      .stride_in_pixels = 64,
      .height = 64,
      .depth_bytes_per_pixel = 4,
      .is_depth_or_stencil = true,
      .microtile = true,
      .macrotile = true,
      .num_samples = 1,
      .zcomp8x8_capable = true,
      .pipes = 1,
      .zmask_ram_dwords_per_pipe = RV3xx_ZMASK_SIZE,
   };

   assert(r300_zmask_layout_compute(NULL, &layout) == -EINVAL);
   assert(r300_zmask_layout_compute(&params, NULL) == -EINVAL);

   params.pipes = 0;
   assert(r300_zmask_layout_compute(&params, &layout) == -EINVAL);
   params.pipes = R300_ZMASK_MAX_PIPES + 1;
   assert(r300_zmask_layout_compute(&params, &layout) == -EINVAL);
   params.pipes = 1;
   params.height = 0;
   assert(r300_zmask_layout_compute(&params, &layout) == -EINVAL);

   /* A non-depth format leaves the loop's outputs at zero. */
   params.height = 64;
   params.is_depth_or_stencil = false;
   assert(r300_zmask_layout_compute(&params, &layout) == 0);
   assert(layout.dwords == 0 && !layout.fits_zmask_ram);

   /* The RS480 budget refuses a level whose ZMASK exceeds 5120 dwords.
    * One pipe with 4x4 compression covers 16x16 pixels per dword, so
    * 2048 by 2048 needs 16384.
    */
   params.is_depth_or_stencil = true;
   params.macrotile = false;
   params.zcomp8x8_capable = false;
   params.stride_in_pixels = 2048;
   params.height = 2048;
   assert(r300_zmask_layout_compute(&params, &layout) == 0);
   assert(layout.dwords == 0 && !layout.fits_zmask_ram);
   assert(layout.zmask_ram_dwords == RV3xx_ZMASK_SIZE);
}

int
main(void)
{
   check_ram_table();
   check_refusals();

   uint32_t points = 0;
   for (uint32_t pipes = 1; pipes <= R300_ZMASK_MAX_PIPES; pipes++)
      points += sweep(pipes, RV3xx_ZMASK_SIZE);
   points += sweep(1, R300_ZMASK_SIZE_PER_PIPE);

   printf("r300 zmask layout: %u sweep points agree with the Gallium "
          "derivation\n",
          points);
   return 0;
}
