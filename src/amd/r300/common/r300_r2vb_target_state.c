/* SPDX-License-Identifier: MIT */

#include "r300_r2vb_target_state.h"

#include "r300_reg.h"

#include <errno.h>
#include <stddef.h>

/* Non-R500 scissor coordinates carry a 1440 bias. The X and Y fields are
 * thirteen bits wide, so the largest biased coordinate is 8191 and the
 * largest encodable inclusive extent is 6752 pixels. */
#define R300_R2VB_TARGET_SCISSOR_BIAS 1440u
#define R300_R2VB_TARGET_SCISSOR_FIELD_MAX \
   (R300_SCISSORS_X_MASK >> R300_SCISSORS_X_SHIFT)
#define R300_R2VB_TARGET_MAX_EXTENT \
   (R300_R2VB_TARGET_SCISSOR_FIELD_MAX - \
    R300_R2VB_TARGET_SCISSOR_BIAS + 1u)

int
r300_r2vb_target_state_emit(struct r300_pm4_builder *b,
                            const struct r300_r2vb_target_state_params *params,
                            uint32_t *color_reloc_index)
{
   if (params == NULL || color_reloc_index == NULL)
      return -EINVAL;
   if (params->width == 0 || params->height == 0 ||
       params->width > R300_R2VB_TARGET_MAX_EXTENT ||
       params->height > R300_R2VB_TARGET_MAX_EXTENT)
      return -EINVAL;
   if (params->point_size_sixths == 0)
      return -EINVAL;
   if (params->const_write_count > 0 && params->const_writes == NULL)
      return -EINVAL;

   const uint32_t needed =
      r300_r2vb_target_state_dwords(params->const_write_count);
   if (!r300_pm4_builder_reserve(b, needed))
      return b->error;

   r300_pm4_reg(b, R300_ZB_CNTL, 0);
   const uint32_t mspos[2] = {0x66666666, 0x06666666};
   r300_pm4_packet0(b, R300_GB_MSPOS0, mspos, 2);
   r300_pm4_reg(b, R300_GB_AA_CONFIG, R300_GB_AA_CONFIG_AA_DISABLE);
   r300_pm4_reg(b, R300_RB3D_AARESOLVE_CTL, 0);
   r300_pm4_reg(b, R300_SC_SCREENDOOR, 0x00ffffff);
   const uint32_t scissors[2] = {
      (R300_R2VB_TARGET_SCISSOR_BIAS << R300_SCISSORS_X_SHIFT) |
         (R300_R2VB_TARGET_SCISSOR_BIAS << R300_SCISSORS_Y_SHIFT),
      ((params->width + R300_R2VB_TARGET_SCISSOR_BIAS - 1)
       << R300_SCISSORS_X_SHIFT) |
         ((params->height + R300_R2VB_TARGET_SCISSOR_BIAS - 1)
          << R300_SCISSORS_Y_SHIFT),
   };
   r300_pm4_packet0(b, R300_SC_SCISSORS_TL, scissors, 2);

   /* Flush the outgoing color buffer before retargeting
    * RB3D_COLOROFFSET0: the destination cache still holds the previous
    * surface's dirty tiles against the old offset, and a raw retarget
    * without the barrier drops them.
    */
   r300_pm4_reg(b, R300_ZB_ZCACHE_CTLSTAT,
                R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                   R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
   r300_pm4_reg(b, R300_RB3D_DSTCACHE_CTLSTAT,
                R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                   R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
   r300_pm4_reg(b, RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);
   r300_pm4_reg(b, R300_RB3D_CCTL, params->rb3d_cctl);
   r300_pm4_reg(b, R300_RB3D_COLOROFFSET0, params->color_offset_bytes);
   *color_reloc_index =
      r300_pm4_reloc_nop(b, params->color_relocation_payload);
   r300_pm4_reg(b, R300_RB3D_COLORPITCH0,
                params->pitch_pixels | params->color_format);

   /* The producer drives one output; FMT_1..3 read UNUSED so a stale
    * second target cannot receive fragments.
    */
   const uint32_t out_fmt[4] = {params->us_out_fmt0, R300_US_OUT_FMT_UNUSED,
                                R300_US_OUT_FMT_UNUSED,
                                R300_US_OUT_FMT_UNUSED};
   r300_pm4_packet0(b, R300_US_OUT_FMT_0, out_fmt, 4);
   r300_pm4_reg(b, R300_RB3D_ROPCNTL, 0);
   const uint32_t blend[3] = {
      0, 0,
      RB3D_COLOR_CHANNEL_MASK_BLUE_MASK0 | RB3D_COLOR_CHANNEL_MASK_GREEN_MASK0 |
         RB3D_COLOR_CHANNEL_MASK_RED_MASK0 |
         RB3D_COLOR_CHANNEL_MASK_ALPHA_MASK0};
   r300_pm4_packet0(b, R300_RB3D_CBLEND, blend, 3);
   r300_pm4_reg(b, R300_RB3D_DITHER_CTL, 0);
   r300_pm4_reg(b, R300_FG_ALPHA_FUNC, R300_FG_ALPHA_FUNC_DISABLE);

   for (uint32_t i = 0; i < params->const_write_count; i++)
      r300_pm4_packet0(b, params->const_writes[i].reg,
                       params->const_writes[i].value, 4);

   r300_pm4_reg(b, R300_SU_CULL_MODE, 0);
   r300_pm4_reg(b, R300_SC_CLIP_RULE, 0xFFFF);
   /* GA point size is in sixths of a pixel; 6 is one pixel, and the
    * minmax floor stays at one pixel whatever the override.
    */
   r300_pm4_reg(b, R300_GA_POINT_SIZE,
                (params->point_size_sixths << R300_POINTSIZE_Y_SHIFT) |
                   (params->point_size_sixths << R300_POINTSIZE_X_SHIFT));
   r300_pm4_reg(b, R300_GA_POINT_MINMAX,
                (6u << R300_GA_POINT_MINMAX_MIN_SHIFT) |
                   (params->point_size_sixths
                    << R300_GA_POINT_MINMAX_MAX_SHIFT));
   r300_pm4_reg(b, R300_VAP_CLIP_CNTL, R300_CLIP_DISABLE);
   r300_pm4_reg(b, R300_VAP_VTE_CNTL, R300_VTX_XY_FMT | R300_VTX_Z_FMT);

   return b->error;
}

int
r300_r2vb_publication_tail_emit(struct r300_pm4_builder *b)
{
   if (!r300_pm4_builder_reserve(b, R300_R2VB_PUBLICATION_TAIL_DWORDS))
      return b->error;

   r300_pm4_reg(b, R300_ZB_ZCACHE_CTLSTAT,
                R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                   R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
   r300_pm4_reg(b, R300_RB3D_DSTCACHE_CTLSTAT,
                R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                   R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
   r300_pm4_reg(b, RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);
   r300_pm4_reg(b, R300_VAP_PVS_STATE_FLUSH_REG, 0x0);
   return b->error;
}
