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

static uint32_t
round_up(uint32_t value, uint32_t alignment)
{
   return ((value + alignment - 1u) / alignment) * alignment;
}

/* r300_pixels_to_dwords: the aligned pixel area divided by the pixels one
 * dword covers.
 */
static uint32_t
pixels_to_dwords(uint32_t stride, uint32_t height, uint32_t xblock,
                 uint32_t yblock)
{
   return (round_up(stride, xblock) * round_up(height, yblock)) /
          (xblock * yblock);
}

int
r300_zmask_layout_compute(const struct r300_zmask_layout_params *params,
                          struct r300_zmask_layout *out)
{
   if (params == NULL || out == NULL)
      return -EINVAL;
   if (params->pipes == 0u || params->pipes > R300_ZMASK_MAX_PIPES)
      return -EINVAL;
   if (params->height == 0u)
      return -EINVAL;

   memset(out, 0, sizeof(*out));
   out->zmask_ram_dwords = params->zmask_ram_dwords_per_pipe * params->pipes;

   /* The gate r300_setup_hyperz_properties opens the ZMASK loop with: a
    * depth or stencil format of exactly 32 bits per pixel on a microtiled
    * level.  Every other level leaves the loop's outputs at zero.
    */
   if (!params->is_depth_or_stencil || params->depth_bytes_per_pixel != 4u ||
       !params->microtile)
      return 0;

   const uint32_t index = params->pipes - 1u;
   const uint32_t zcompsize =
      (params->zcomp8x8_capable && params->macrotile &&
       params->num_samples <= 1u)
         ? 8u
         : 4u;
   const uint32_t xblock = zmask_blocks_x_per_dw[index] * zcompsize;
   const uint32_t yblock = zmask_blocks_y_per_dw[index] * zcompsize;

   const uint32_t stride = round_up(params->stride_in_pixels, 16u);
   const uint32_t dwords =
      pixels_to_dwords(stride, params->height, xblock, yblock);

   if (dwords > out->zmask_ram_dwords)
      return 0;

   out->dwords = dwords;
   out->zcomp8x8 = zcompsize == 8u;
   out->stride_in_pixels = round_up(stride, xblock);
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
