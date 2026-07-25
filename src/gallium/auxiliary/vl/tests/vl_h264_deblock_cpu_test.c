/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Verification of the CPU in-loop deblock (vl_h264_deblock_cpu), the decode
 * path's whole-frame deblock that runs after the inter back half and the CPU
 * intra pass.  It filters a luma plane and both chroma planes in place and is
 * checked sample-for-sample against the shared integer reference in
 * vl_h264_deblock_ref.h -- the same boundary-strength derivation (sec 8.7.2.1),
 * Table 8-16/8-17 thresholds, QP averaging (sec 8.7.2.2), and the normal (bS
 * 1..3) and strong (bS 4) filters that the GPU emit deblock is proven against,
 * itself bit-exact against ffmpeg.
 *
 * The fixture is non-vacuous and diverse: a 4x4-macroblock frame mixes intra
 * macroblocks (strength 3 internal, 4 on boundaries), inter macroblocks with a
 * coded block (strength 2) and a one-sample motion delta (strength 1), and a
 * uniform inter macroblock (strength 0), with per-macroblock QP and alpha/beta
 * offsets that exercise the qPav thresholds, one disable_deblock_idc skip, and a
 * sharp interior step that gates the activity test off.  The test asserts the
 * filter changed samples on every plane, so a silently-disabled filter fails.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vl_h264_deblock_cpu.h"
#include "vl_h264_deblock_ref.h"

#define MBW 4
#define MBH 4
#define LW (MBW * 16)
#define LH (MBH * 16)
#define CW (MBW * 8)
#define CH (MBH * 8)

static struct vl_h264_mb_contract mbs[MBW * MBH];

static void
build_contract(struct vl_h264_slice_contract *slice)
{
   memset(mbs, 0, sizeof(mbs));
   unsigned n = 0;
   for (int my = 0; my < MBH; ++my)
      for (int mx = 0; mx < MBW; ++mx) {
         struct vl_h264_mb_contract *mb = &mbs[n++];
         mb->mb_x = mx;
         mb->mb_y = my;
         mb->transform_8x8 = 0;
         mb->qp_y = 20 + ((mx * 3 + my * 5) % 24);
         mb->qp_cb = 18 + ((mx + my) % 20);
         mb->qp_cr = 22 + ((mx * 2 + my) % 18);
         mb->slice_alpha_c0_offset_div2 = (int8_t)((mx % 3) - 1);
         mb->slice_beta_offset_div2 = (int8_t)((my % 3) - 1);
         mb->disable_deblock_idc = (mx == 3 && my == 3) ? 1 : 0;
         int kind = (mx + my) % 3;
         for (int b = 0; b < 16; ++b) {
            if (kind == 0) {
               mb->ref_l0[b] = -1;
               mb->ref_l1[b] = -1;
            } else if (kind == 1) {
               mb->ref_l0[b] = 0;
               mb->ref_l1[b] = -1;
               mb->mv_l0[b][0] = (int16_t)((b % 2) ? 6 : 0);
               if (b % 4 == 0)
                  mb->coeff4x4[b][0] = 5;
            } else {
               mb->ref_l0[b] = 0;
               mb->ref_l1[b] = -1;
               mb->mv_l0[b][0] = 2;
               mb->mv_l0[b][1] = 2;
            }
         }
      }
   slice->version = 0;
   slice->width = LW;
   slice->height = LH;
   slice->num_macroblocks = n;
   slice->macroblocks = mbs;
}

int
main(void)
{
   struct vl_h264_slice_contract slice;
   build_contract(&slice);

   uint8_t *y8 = malloc(LW * LH), *cb8 = malloc(CW * CH), *cr8 = malloc(CW * CH);
   int *yr = malloc(LW * LH * sizeof(int));
   int *cbr = malloc(CW * CH * sizeof(int)), *crr = malloc(CW * CH * sizeof(int));

   for (int yy = 0; yy < LH; ++yy)
      for (int xx = 0; xx < LW; ++xx) {
         int v = 40 + (xx + yy) / 2 + ((xx / 8 + yy / 8) % 4);
         if (xx >= 16 && xx < 32 && yy >= 16 && yy < 32)
            v = (xx & 4) ? 200 : 40;
         if (v > 255)
            v = 255;
         y8[yy * LW + xx] = (uint8_t)v;
         yr[yy * LW + xx] = v;
      }
   for (int yy = 0; yy < CH; ++yy)
      for (int xx = 0; xx < CW; ++xx) {
         int vb = 60 + (xx + yy) / 2 + ((xx / 4) % 3);
         int vr = 80 + (xx * 2 + yy) / 3;
         if (vb > 255)
            vb = 255;
         if (vr > 255)
            vr = 255;
         cb8[yy * CW + xx] = (uint8_t)vb;
         cbr[yy * CW + xx] = vb;
         cr8[yy * CW + xx] = (uint8_t)vr;
         crr[yy * CW + xx] = vr;
      }

   vl_h264_deblock_cpu(&slice, MBW, MBH, y8, LW, NULL, NULL, 0);
   vl_h264_deblock_cpu(&slice, MBW, MBH, NULL, 0, cb8, cr8, CW);

   unsigned cl = deblock_reference(yr, LW, LH, &slice);
   unsigned cb_c = deblock_chroma_reference(cbr, CW, CH, &slice, false);
   unsigned cr_c = deblock_chroma_reference(crr, CW, CH, &slice, true);

   int bad = 0;
   for (int i = 0; i < LW * LH; ++i)
      if (y8[i] != yr[i]) {
         if (bad < 8)
            printf("luma @%d mine=%d ref=%d (x=%d y=%d)\n", i, y8[i], yr[i],
                   i % LW, i / LW);
         bad++;
      }
   for (int i = 0; i < CW * CH; ++i) {
      if (cb8[i] != cbr[i]) {
         if (bad < 12)
            printf("cb @%d mine=%d ref=%d\n", i, cb8[i], cbr[i]);
         bad++;
      }
      if (cr8[i] != crr[i]) {
         if (bad < 16)
            printf("cr @%d mine=%d ref=%d\n", i, cr8[i], crr[i]);
         bad++;
      }
   }

   int status = 0;
   if (cl == 0 || cb_c == 0 || cr_c == 0) {
      printf("FAIL: vacuous fixture, reference changed nothing (luma=%u cb=%u "
             "cr=%u)\n", cl, cb_c, cr_c);
      status = 1;
   }
   if (bad) {
      printf("FAIL: %d CPU-deblock samples differ from the reference\n", bad);
      status = 1;
   }
   if (!status)
      printf("PASS: CPU deblock bit-exact vs reference (luma=%u cb=%u cr=%u "
             "changed)\n", cl, cb_c, cr_c);

   free(y8); free(cb8); free(cr8); free(yr); free(cbr); free(crr);
   return status;
}
