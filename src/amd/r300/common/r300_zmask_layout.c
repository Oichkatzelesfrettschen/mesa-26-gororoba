/* SPDX-License-Identifier: MIT */

#include "r300_zmask_layout.h"

#include "r300_capabilities.h"
#include "r300_chipset.h"

#include "amd_family.h"

#include <errno.h>
#include <string.h>

/* Blocks one ZMASK dword covers per pipe count, from
 * r300_setup_hyperz_properties in r300_texture_desc.c.  With three pipes
 * the x product reaches 48 or 96, so the stride rounds up through a
 * division rather than a power-of-two mask.
 */
static const uint32_t zmask_blocks_x_per_dw[R300_ZMASK_MAX_PIPES] = {4, 8, 12, 8};
static const uint32_t zmask_blocks_y_per_dw[R300_ZMASK_MAX_PIPES] = {4, 4, 4, 8};

static bool
round_up(uint32_t value, uint32_t alignment, uint64_t *rounded)
{
   const uint64_t units = ((uint64_t)value + alignment - 1u) / alignment;
   *rounded = units * alignment;
   return *rounded <= UINT32_MAX;
}

/* r300_pixels_to_dwords: the aligned pixel area divided by the pixels one
 * dword covers.
 */
static bool
pixels_to_dwords(uint32_t stride, uint32_t height, uint32_t xblock,
                 uint32_t yblock, uint64_t *dwords)
{
   if (xblock == 0u || yblock == 0u)
      return false;

   const uint64_t xunits = ((uint64_t)stride + xblock - 1u) / xblock;
   const uint64_t yunits = ((uint64_t)height + yblock - 1u) / yblock;
   if (xunits == 0u || yunits == 0u || xunits > UINT64_MAX / yunits)
      return false;

   *dwords = xunits * yunits;
   return true;
}

int
r300_zmask_layout_compute(const struct r300_zmask_layout_params *params,
                          struct r300_zmask_layout *out)
{
   return r300_zmask_layout_compute_at_block(params, R300_ZCOMP_8X8, out);
}

int
r300_zmask_layout_compute_at_block(
   const struct r300_zmask_layout_params *params,
   enum r300_zmask_compression block, struct r300_zmask_layout *out)
{
   if (block != R300_ZCOMP_4X4 && block != R300_ZCOMP_8X8)
      return -EINVAL;
   if (params == NULL || out == NULL)
      return -EINVAL;
   if (params->pipes == 0u || params->pipes > R300_ZMASK_MAX_PIPES)
      return -EINVAL;
   if (params->height == 0u)
      return -EINVAL;
   if (params->stride_in_pixels == 0u)
      return -EINVAL;

   const uint64_t capacity = (uint64_t)params->zmask_ram_dwords_per_pipe *
                             params->pipes;
   if (capacity > UINT32_MAX)
      return -EINVAL;

   memset(out, 0, sizeof(*out));
   out->zmask_ram_dwords = (uint32_t)capacity;

   /* The gate r300_setup_hyperz_properties opens the ZMASK loop with: a
    * depth or stencil format of exactly 32 bits per pixel on a microtiled
    * level.  Every other level leaves the loop's outputs at zero.
    */
   if (!params->is_depth_or_stencil || params->depth_bytes_per_pixel != 4u ||
       !params->microtile)
      return 0;

   const uint32_t index = params->pipes - 1u;
   /* The level's own decision, taken when the caller asks for the larger
    * block.  A request for R300_ZCOMP_4X4 pins it, so the block size the
    * metadata count and the plane-equation register are both derived
    * from is one value rather than two. */
   const uint32_t zcompsize =
      (block == R300_ZCOMP_8X8 && params->zcomp8x8_capable &&
       params->macrotile && params->num_samples <= 1u)
         ? 8u
         : 4u;
   const uint32_t xblock = zmask_blocks_x_per_dw[index] * zcompsize;
   const uint32_t yblock = zmask_blocks_y_per_dw[index] * zcompsize;

   uint64_t stride;
   if (!round_up(params->stride_in_pixels, 16u, &stride))
      return 0;

   uint64_t dwords;
   if (!pixels_to_dwords((uint32_t)stride, params->height, xblock, yblock,
                         &dwords))
      return 0;

   if (dwords == 0u || dwords > capacity || dwords > UINT32_MAX)
      return 0;

   uint64_t pitch;
   if (!round_up((uint32_t)stride, xblock, &pitch) || pitch == 0u)
      return 0;

   out->dwords = (uint32_t)dwords;
   out->zcomp8x8 = zcompsize == 8u;
   out->stride_in_pixels = (uint32_t)pitch;
   out->fits_zmask_ram = true;
   return 0;
}

uint32_t
r300_zmask_ram_dwords_per_pipe(int family)
{
   switch (family) {
   case CHIP_R300:
   case CHIP_R350:
   case CHIP_R420:
   case CHIP_R423:
   case CHIP_R430:
   case CHIP_R480:
   case CHIP_R481:
   case CHIP_RV410:
   case CHIP_RV515:
   case CHIP_R520:
   case CHIP_RV530:
   case CHIP_RV560:
   case CHIP_RV570:
   case CHIP_R580:
      return R300_ZMASK_SIZE_PER_PIPE;
   case CHIP_RV350:
   case CHIP_RV370:
   case CHIP_RV380:
   case CHIP_RC410:
   case CHIP_RS480:
      return RV3xx_ZMASK_SIZE;
   default:
      return 0u;
   }
}

bool
r300_zmask_zcomp8x8_capable(int family)
{
   return family >= CHIP_RV350;
}
