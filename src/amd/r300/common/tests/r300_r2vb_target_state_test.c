/*
 * SPDX-License-Identifier: MIT
 *
 * Exact-word and validation controls for the neutral R2VB producer
 * target prologue and cache-publication tail.
 */

/* The asserts carry the verdicts, so they stay live in NDEBUG builds. */
#undef NDEBUG

#include "r300_r2vb_target_state.h"

#include "r300_reg.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The expected stream is written from the packet grammar directly,
 * independent of the emitter, so a copied emitter mistake cannot make
 * both sides agree.  Reference case: 4x1 target, pitch 4,
 * ARGB32323232, RGBA identity select, non-R500 CCTL, one wpos constant
 * quad, one-pixel points.
 */
static const struct r300_r2vb_target_const_write reference_const = {
   .reg = 0x4600, /* R300_PFS_PARAM_0_X */
   .value = {0x3f0000, 0x3f0000, 0x3f0000, 0x3f0000},
};

static struct r300_r2vb_target_state_params
reference_params(void)
{
   struct r300_r2vb_target_state_params p = {
      .width = 4,
      .height = 1,
      .pitch_pixels = 4,
      .color_format = R300_COLOR_FORMAT_ARGB32323232,
      .us_out_fmt0 = R300_US_OUT_FMT_C4_32_FP | R300_C0_SEL_R |
                     R300_C1_SEL_G | R300_C2_SEL_B | R300_C3_SEL_A,
      .rb3d_cctl = 0,
      .color_offset_bytes = 0x100,
      .color_relocation_payload = 8,
      .point_size_sixths = 6,
      .const_writes = &reference_const,
      .const_write_count = 1,
   };
   return p;
}

static uint32_t
expected_reference_stream(uint32_t *ib)
{
   uint32_t n = 0;
#define REG(r, v) do { ib[n++] = CP_PACKET0(r, 0); ib[n++] = (v); } while (0)
   REG(R300_ZB_CNTL, 0);
   ib[n++] = CP_PACKET0(R300_GB_MSPOS0, 1);
   ib[n++] = 0x66666666;
   ib[n++] = 0x06666666;
   REG(R300_GB_AA_CONFIG, R300_GB_AA_CONFIG_AA_DISABLE);
   REG(R300_RB3D_AARESOLVE_CTL, 0);
   REG(R300_SC_SCREENDOOR, 0x00ffffff);
   ib[n++] = CP_PACKET0(R300_SC_SCISSORS_TL, 1);
   ib[n++] = (1440u << R300_SCISSORS_X_SHIFT) |
             (1440u << R300_SCISSORS_Y_SHIFT);
   ib[n++] = ((4u + 1439u) << R300_SCISSORS_X_SHIFT) |
             ((1u + 1439u) << R300_SCISSORS_Y_SHIFT);
   REG(R300_ZB_ZCACHE_CTLSTAT,
       R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
          R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
   REG(R300_RB3D_DSTCACHE_CTLSTAT,
       R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
          R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
   REG(RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);
   REG(R300_RB3D_CCTL, 0);
   REG(R300_RB3D_COLOROFFSET0, 0x100);
   ib[n++] = 0xc0001000;
   ib[n++] = 8;
   REG(R300_RB3D_COLORPITCH0, 4 | R300_COLOR_FORMAT_ARGB32323232);
   ib[n++] = CP_PACKET0(R300_US_OUT_FMT_0, 3);
   ib[n++] = R300_US_OUT_FMT_C4_32_FP | R300_C0_SEL_R | R300_C1_SEL_G |
             R300_C2_SEL_B | R300_C3_SEL_A;
   ib[n++] = R300_US_OUT_FMT_UNUSED;
   ib[n++] = R300_US_OUT_FMT_UNUSED;
   ib[n++] = R300_US_OUT_FMT_UNUSED;
   REG(R300_RB3D_ROPCNTL, 0);
   ib[n++] = CP_PACKET0(R300_RB3D_CBLEND, 2);
   ib[n++] = 0;
   ib[n++] = 0;
   ib[n++] = RB3D_COLOR_CHANNEL_MASK_BLUE_MASK0 |
             RB3D_COLOR_CHANNEL_MASK_GREEN_MASK0 |
             RB3D_COLOR_CHANNEL_MASK_RED_MASK0 |
             RB3D_COLOR_CHANNEL_MASK_ALPHA_MASK0;
   REG(R300_RB3D_DITHER_CTL, 0);
   REG(R300_FG_ALPHA_FUNC, R300_FG_ALPHA_FUNC_DISABLE);
   ib[n++] = CP_PACKET0(0x4600, 3);
   for (unsigned i = 0; i < 4; i++)
      ib[n++] = 0x3f0000;
   REG(R300_SU_CULL_MODE, 0);
   REG(R300_SC_CLIP_RULE, 0xFFFF);
   REG(R300_GA_POINT_SIZE,
       (6u << R300_POINTSIZE_Y_SHIFT) | (6u << R300_POINTSIZE_X_SHIFT));
   REG(R300_GA_POINT_MINMAX,
       (6u << R300_GA_POINT_MINMAX_MIN_SHIFT) |
          (6u << R300_GA_POINT_MINMAX_MAX_SHIFT));
   REG(R300_VAP_CLIP_CNTL, R300_CLIP_DISABLE);
   REG(R300_VAP_VTE_CNTL, R300_VTX_XY_FMT | R300_VTX_Z_FMT);
#undef REG
   return n;
}

int
main(void)
{
   const uint32_t total = r300_r2vb_target_state_dwords(1);
   assert(total == R300_R2VB_TARGET_STATE_FIXED_DWORDS + 5);

   uint32_t expected[128];
   assert(expected_reference_stream(expected) == total);

   uint32_t ib[128];
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, ib, total);
   struct r300_r2vb_target_state_params p = reference_params();
   uint32_t color_reloc = 0;
   assert(r300_r2vb_target_state_emit(&b, &p, &color_reloc) == 0);
   uint32_t count = 0;
   assert(r300_pm4_builder_finish(&b, &count) == 0);
   assert(count == total);
   assert(memcmp(ib, expected, count * 4) == 0);
   assert(ib[color_reloc] == 8 && ib[color_reloc - 1] == 0xc0001000u);

   /* The publication tail is the exact production-safe sequence. */
   uint32_t tail[R300_R2VB_PUBLICATION_TAIL_DWORDS];
   r300_pm4_builder_init(&b, tail, R300_R2VB_PUBLICATION_TAIL_DWORDS);
   assert(r300_r2vb_publication_tail_emit(&b) == 0);
   assert(r300_pm4_builder_finish(&b, &count) == 0);
   assert(count == R300_R2VB_PUBLICATION_TAIL_DWORDS);
   assert(tail[0] == CP_PACKET0(R300_ZB_ZCACHE_CTLSTAT, 0));
   assert(tail[1] == (R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                      R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE));
   assert(tail[2] == CP_PACKET0(R300_RB3D_DSTCACHE_CTLSTAT, 0));
   assert(tail[3] == (R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                      R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS));
   assert(tail[4] == CP_PACKET0(RADEON_WAIT_UNTIL, 0));
   assert(tail[5] == RADEON_WAIT_3D_IDLECLEAN);
   assert(tail[6] == CP_PACKET0(R300_VAP_PVS_STATE_FLUSH_REG, 0));
   assert(tail[7] == 0);

   /* Rejections, each without writing: bad extent, zero point size,
    * null constants with a nonzero count, one-short capacity.
    */
   struct r300_r2vb_target_state_params bad = reference_params();
   bad.width = 0;
   r300_pm4_builder_init(&b, ib, total);
   assert(r300_r2vb_target_state_emit(&b, &bad, &color_reloc) == -EINVAL);
   bad = reference_params();
   bad.height = 6752;
   r300_pm4_builder_init(&b, ib, total);
   assert(r300_r2vb_target_state_emit(&b, &bad, &color_reloc) == 0);
   bad.height++;
   r300_pm4_builder_init(&b, ib, total);
   assert(r300_r2vb_target_state_emit(&b, &bad, &color_reloc) == -EINVAL);
   bad = reference_params();
   bad.point_size_sixths = 0;
   r300_pm4_builder_init(&b, ib, total);
   assert(r300_r2vb_target_state_emit(&b, &bad, &color_reloc) == -EINVAL);
   bad = reference_params();
   bad.const_writes = NULL;
   r300_pm4_builder_init(&b, ib, total);
   assert(r300_r2vb_target_state_emit(&b, &bad, &color_reloc) == -EINVAL);

   memset(ib, 0, sizeof(ib));
   p = reference_params();
   r300_pm4_builder_init(&b, ib, total - 1);
   assert(r300_r2vb_target_state_emit(&b, &p, &color_reloc) == -ENOSPC);
   assert(b.count == 0 && ib[0] == 0);

   r300_pm4_builder_init(&b, tail, R300_R2VB_PUBLICATION_TAIL_DWORDS - 1);
   assert(r300_r2vb_publication_tail_emit(&b) == -ENOSPC);
   assert(b.count == 0);

   printf("r300_r2vb_target_state_test: prologue, tail, and validation "
          "controls held\n");
   return 0;
}
