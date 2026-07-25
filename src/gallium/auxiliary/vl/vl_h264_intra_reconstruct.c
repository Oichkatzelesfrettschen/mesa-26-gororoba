/*
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>

#include "vl_h264_intra_reconstruct.h"

/*
 * Clean-room from ITU-T H.264 sec 8.3 (intra prediction) and 8.5.12.2 (the 4x4
 * inverse transform).  No third-party decoder source was consulted.
 */

/* Clip1 for 8-bit samples (sec 5.7). */
static int
clip1(int v)
{
   return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* 4x4 luma block scan index to raster grid position, and the inverse (sec
 * 6.4.3). */
static const uint8_t blk_x[16] = { 0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3 };
static const uint8_t blk_y[16] = { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3 };

static unsigned
xy_to_blk(unsigned bx, unsigned by)
{
   for (unsigned i = 0; i < 16; i++)
      if (blk_x[i] == bx && blk_y[i] == by)
         return i;
   return 0;
}

/*
 * A reconstructed sample at (ax, ay) is available to predict block cur_blk of
 * macroblock cur_mb when four conditions hold: the sample lies inside the
 * frame, its macroblock is already decoded, its macroblock shares cur_mb's
 * slice, and -- under constrained_intra_pred -- its macroblock is not inter.
 *
 * Decode order inside a macroblock follows the 4x4 scan index, so a sample in
 * cur_mb is decoded only once its 4x4 block precedes cur_blk.  The top-right
 * predictor sample of blocks 3 and 11 sits in a block with a later scan index,
 * so this scan-order test reports that sample unavailable on its own and the
 * spec's top-right special case is unnecessary.
 *
 * Constrained Baseline disables FMO, so a slice is a contiguous ascending
 * macroblock range; a neighbor macroblock is in an earlier slice exactly when
 * its raster address is below slice_first_mb.
 *
 * constrained_intra_pred bars intra prediction from an inter-coded neighbor:
 * that neighbor carries samples predicted from a reference picture, which must
 * not seed intra prediction.  The contract marks an inter macroblock with a
 * non-negative list-0 reference index.  cur_mb is the intra macroblock under
 * reconstruction, so its own samples stay available.
 */
static bool
sample_available(int ax, int ay, unsigned width, unsigned height,
                 unsigned width_in_mbs, unsigned cur_mb, unsigned slice_first_mb,
                 unsigned cur_blk, const struct vl_h264_mb_contract *mbs,
                 bool constrained_intra)
{
   if (ax < 0 || ay < 0 || (unsigned)ax >= width || (unsigned)ay >= height)
      return false;
   unsigned nb_mb = ((unsigned)ay / 16) * width_in_mbs + (unsigned)ax / 16;
   if (nb_mb < slice_first_mb)
      return false;
   if (constrained_intra && nb_mb != cur_mb && mbs[nb_mb].ref_l0[0] >= 0)
      return false;
   if (nb_mb < cur_mb)
      return true;
   if (nb_mb > cur_mb)
      return false;
   return xy_to_blk(((unsigned)ax % 16) / 4, ((unsigned)ay % 16) / 4) < cur_blk;
}

/* The Intra_4x4 neighbor samples: A is p[0..7,-1] (top and top-right), L is
 * p[-1,0..3] (left), tl is p[-1,-1]. */
struct neighbors4 {
   int a[8];
   bool a_av[8];
   int l[4];
   bool l_av[4];
   int tl;
   bool tl_av;
};

static void
build_neighbors4(const uint8_t *luma, unsigned stride, unsigned width,
                 unsigned height, unsigned width_in_mbs, unsigned mb_x,
                 unsigned mb_y, unsigned slice_first_mb, unsigned s,
                 const struct vl_h264_mb_contract *mbs, bool constrained_intra,
                 struct neighbors4 *n)
{
   unsigned cur_mb = mb_y * width_in_mbs + mb_x;
   int x0 = mb_x * 16 + blk_x[s] * 4;
   int y0 = mb_y * 16 + blk_y[s] * 4;

   for (int k = 0; k < 8; k++) {
      n->a_av[k] = sample_available(x0 + k, y0 - 1, width, height, width_in_mbs,
                                    cur_mb, slice_first_mb, s, mbs,
                                    constrained_intra);
      n->a[k] = n->a_av[k] ? luma[(y0 - 1) * (int)stride + x0 + k] : 0;
   }
   for (int k = 0; k < 4; k++) {
      n->l_av[k] = sample_available(x0 - 1, y0 + k, width, height, width_in_mbs,
                                    cur_mb, slice_first_mb, s, mbs,
                                    constrained_intra);
      n->l[k] = n->l_av[k] ? luma[(y0 + k) * (int)stride + x0 - 1] : 0;
   }
   n->tl_av = sample_available(x0 - 1, y0 - 1, width, height, width_in_mbs,
                               cur_mb, slice_first_mb, s, mbs, constrained_intra);
   n->tl = n->tl_av ? luma[(y0 - 1) * (int)stride + x0 - 1] : 0;

   /* Above-right substitution (sec 8.3.1.2): when p[4..7,-1] are unavailable but
    * p[3,-1] is available, p[3,-1] stands in for them. */
   if (!n->a_av[4] && n->a_av[3])
      for (int k = 4; k < 8; k++) {
         n->a[k] = n->a[3];
         n->a_av[k] = true;
      }
}

/* p[k,-1] and p[-1,k] with k == -1 resolving to the top-left sample. */
static int ptop(const struct neighbors4 *n, int k) { return k < 0 ? n->tl : n->a[k]; }
static int pleft(const struct neighbors4 *n, int k) { return k < 0 ? n->tl : n->l[k]; }

/* Each Intra_4x4 mode (sec 8.3.1.2.1-9) as a predicted sample at (x, y). */

static int
pred4_dc(const struct neighbors4 *n)
{
   bool tav = n->a_av[0] && n->a_av[1] && n->a_av[2] && n->a_av[3];
   bool lav = n->l_av[0] && n->l_av[1] && n->l_av[2] && n->l_av[3];
   int top = n->a[0] + n->a[1] + n->a[2] + n->a[3];
   int left = n->l[0] + n->l[1] + n->l[2] + n->l[3];
   if (tav && lav)
      return (top + left + 4) >> 3;
   if (lav)
      return (left + 2) >> 2;
   if (tav)
      return (top + 2) >> 2;
   return 128;
}

static int
pred4_ddl(const struct neighbors4 *n, int x, int y)
{
   if (x == 3 && y == 3)
      return (n->a[6] + 3 * n->a[7] + 2) >> 2;
   return (n->a[x + y] + 2 * n->a[x + y + 1] + n->a[x + y + 2] + 2) >> 2;
}

static int
pred4_ddr(const struct neighbors4 *n, int x, int y)
{
   if (x > y)
      return (ptop(n, x - y - 2) + 2 * ptop(n, x - y - 1) + ptop(n, x - y) + 2) >> 2;
   if (x < y)
      return (pleft(n, y - x - 2) + 2 * pleft(n, y - x - 1) + pleft(n, y - x) + 2) >> 2;
   return (n->a[0] + 2 * n->tl + n->l[0] + 2) >> 2;
}

static int
pred4_vr(const struct neighbors4 *n, int x, int y)
{
   int z = 2 * x - y;
   int t = x - (y >> 1);
   if (z >= 0 && (z & 1) == 0)
      return (ptop(n, t - 1) + ptop(n, t) + 1) >> 1;
   if (z > 0)
      return (ptop(n, t - 2) + 2 * ptop(n, t - 1) + ptop(n, t) + 2) >> 2;
   if (z == -1)
      return (n->l[0] + 2 * n->tl + n->a[0] + 2) >> 2;
   return (pleft(n, y - 1) + 2 * pleft(n, y - 2) + pleft(n, y - 3) + 2) >> 2;
}

static int
pred4_hd(const struct neighbors4 *n, int x, int y)
{
   int z = 2 * y - x;
   int t = y - (x >> 1);
   if (z >= 0 && (z & 1) == 0)
      return (pleft(n, t - 1) + pleft(n, t) + 1) >> 1;
   if (z > 0)
      return (pleft(n, t - 2) + 2 * pleft(n, t - 1) + pleft(n, t) + 2) >> 2;
   if (z == -1)
      return (n->l[0] + 2 * n->tl + n->a[0] + 2) >> 2;
   return (ptop(n, x - 1) + 2 * ptop(n, x - 2) + ptop(n, x - 3) + 2) >> 2;
}

static int
pred4_vl(const struct neighbors4 *n, int x, int y)
{
   int t = x + (y >> 1);
   if (y == 0 || y == 2)
      return (n->a[t] + n->a[t + 1] + 1) >> 1;
   return (n->a[t] + 2 * n->a[t + 1] + n->a[t + 2] + 2) >> 2;
}

static int
pred4_hu(const struct neighbors4 *n, int x, int y)
{
   int z = x + 2 * y;
   int t = y + (x >> 1);
   if (z < 4)
      return (z & 1) ? (n->l[t] + 2 * n->l[t + 1] + n->l[t + 2] + 2) >> 2
                     : (n->l[t] + n->l[t + 1] + 1) >> 1;
   if (z == 4)
      return (n->l[t] + n->l[t + 1] + 1) >> 1;
   if (z == 5)
      return (n->l[2] + 3 * n->l[3] + 2) >> 2;
   return n->l[3];
}

/* One Intra_4x4 predicted sample at (x, y), dispatched by mode. */
static int
predict4_sample(const struct neighbors4 *n, int mode, int x, int y)
{
   switch (mode) {
   case 0:  return n->a[x];        /* Vertical */
   case 1:  return n->l[y];        /* Horizontal */
   case 2:  return pred4_dc(n);
   case 3:  return pred4_ddl(n, x, y);
   case 4:  return pred4_ddr(n, x, y);
   case 5:  return pred4_vr(n, x, y);
   case 6:  return pred4_hd(n, x, y);
   case 7:  return pred4_vl(n, x, y);
   default: return pred4_hu(n, x, y);
   }
}

/* Build the neighbor samples and run the Intra_4x4 mode.  mbs and
 * constrained_intra carry the inter-neighbor exclusion; the public entry below
 * passes NULL and false for single-block validation with no macroblock array. */
static void
predict_intra_4x4(const uint8_t *luma, unsigned stride, unsigned width_in_mbs,
                  unsigned height_in_mbs, unsigned mb_x, unsigned mb_y, unsigned s,
                  unsigned slice_first_mb, const struct vl_h264_mb_contract *mbs,
                  bool constrained_intra, int mode, int16_t pred[16])
{
   struct neighbors4 n;
   build_neighbors4(luma, stride, width_in_mbs * 16, height_in_mbs * 16,
                    width_in_mbs, mb_x, mb_y, slice_first_mb, s, mbs,
                    constrained_intra, &n);
   for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++)
         pred[y * 4 + x] = (int16_t)predict4_sample(&n, mode, x, y);
}

void
vl_h264_intra_predict_4x4(const uint8_t *luma, unsigned stride,
                          unsigned width_in_mbs, unsigned height_in_mbs,
                          unsigned mb_x, unsigned mb_y, unsigned s,
                          unsigned slice_first_mb, int mode, int16_t pred[16])
{
   predict_intra_4x4(luma, stride, width_in_mbs, height_in_mbs, mb_x, mb_y, s,
                     slice_first_mb, NULL, false, mode, pred);
}

/* Predict and reconstruct one Intra_4x4 block, writing Clip1(pred + residual)
 * back into the plane so the next block in the wavefront reads it. */
static void
reconstruct_block4(const uint8_t *luma_ro, uint8_t *luma, unsigned stride,
                   unsigned width_in_mbs, unsigned height_in_mbs, unsigned mb_x,
                   unsigned mb_y, unsigned slice_first_mb, unsigned s,
                   const struct vl_h264_mb_contract *mbs, bool constrained_intra,
                   int mode, const int16_t coeff[16])
{
   int16_t pred[16], residual[16];
   predict_intra_4x4(luma_ro, stride, width_in_mbs, height_in_mbs, mb_x, mb_y, s,
                     slice_first_mb, mbs, constrained_intra, mode, pred);
   vl_h264_idct4(coeff, residual);

   int bx = mb_x * 16 + blk_x[s] * 4;
   int by = mb_y * 16 + blk_y[s] * 4;
   for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++)
         luma[(by + y) * (int)stride + bx + x] =
            (uint8_t)clip1(pred[y * 4 + x] + residual[y * 4 + x]);
}

static void
predict16_dc(const int top[16], const int left[16], bool top_av, bool left_av,
             int pred[256])
{
   int st = 0, sl = 0;
   for (int k = 0; k < 16; k++) {
      st += top[k];
      sl += left[k];
   }
   int dc;
   if (top_av && left_av)
      dc = (st + sl + 16) >> 5;
   else if (left_av)
      dc = (sl + 8) >> 4;
   else if (top_av)
      dc = (st + 8) >> 4;
   else
      dc = 128;
   for (int i = 0; i < 256; i++)
      pred[i] = dc;
}

static void
predict16_plane(const int top[16], const int left[16], int tl, int pred[256])
{
   int h = 0, v = 0;
   for (int i = 0; i < 8; i++) {
      h += (i + 1) * (top[8 + i] - (6 - i < 0 ? tl : top[6 - i]));
      v += (i + 1) * (left[8 + i] - (6 - i < 0 ? tl : left[6 - i]));
   }
   int a = 16 * (left[15] + top[15]);
   int b = (5 * h + 32) >> 6;
   int c = (5 * v + 32) >> 6;
   for (int y = 0; y < 16; y++)
      for (int x = 0; x < 16; x++)
         pred[y * 16 + x] = clip1((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
}

/* Intra_16x16 prediction (sec 8.3.2) for the whole macroblock: top is
 * p[0..15,-1], left is p[-1,0..15], tl is p[-1,-1].  Writes predL[y*16 + x]. */
static void
predict_16x16(const uint8_t *luma, unsigned stride, unsigned mb_x,
              unsigned mb_y, bool top_av, bool left_av, int mode, int pred[256])
{
   int x0 = mb_x * 16, y0 = mb_y * 16;
   int top[16] = { 0 }, left[16] = { 0 };

   for (int k = 0; k < 16; k++) {
      top[k] = top_av ? luma[(y0 - 1) * (int)stride + x0 + k] : 0;
      left[k] = left_av ? luma[(y0 + k) * (int)stride + x0 - 1] : 0;
   }
   int tl = (left_av && top_av) ? luma[(y0 - 1) * (int)stride + x0 - 1] : 0;

   switch (mode) {
   case 0: /* Vertical */
      for (int y = 0; y < 16; y++)
         for (int x = 0; x < 16; x++)
            pred[y * 16 + x] = top[x];
      break;
   case 1: /* Horizontal */
      for (int y = 0; y < 16; y++)
         for (int x = 0; x < 16; x++)
            pred[y * 16 + x] = left[y];
      break;
   case 2:
      predict16_dc(top, left, top_av, left_av, pred);
      break;
   default: /* 3: Plane */
      predict16_plane(top, left, tl, pred);
      break;
   }
}

void
vl_h264_intra_reconstruct_luma(const struct vl_h264_mb_contract *mbs,
                               unsigned num_mbs, unsigned width_in_mbs,
                               unsigned height_in_mbs, uint8_t *luma,
                               unsigned stride, bool constrained_intra)
{
   for (unsigned addr = 0; addr < num_mbs; addr++) {
      const struct vl_h264_mb_contract *mb = &mbs[addr];
      unsigned mb_x = (unsigned)mb->mb_x, mb_y = (unsigned)mb->mb_y;
      unsigned slice_first_mb = (unsigned)mb->slice_first_mb;

      /* In a P frame the inter macroblocks are reconstructed by the GPU back
       * half from the reference; this pass fills only the intra macroblocks,
       * which a reference index of -1 marks.  An I frame is all intra. */
      if (mb->ref_l0[0] >= 0)
         continue;

      if (mb->mb_type == 0) {
         /* Intra_4x4: each block reads the plane the previous block wrote. */
         for (unsigned s = 0; s < 16; s++)
            reconstruct_block4(luma, luma, stride, width_in_mbs, height_in_mbs,
                               mb_x, mb_y, slice_first_mb, s, mbs,
                               constrained_intra, mb->intra4x4_pred_mode[s],
                               mb->coeff4x4[blk_y[s] * 4 + blk_x[s]]);
      } else {
         /* Intra_16x16: predict the whole macroblock, then add each 4x4
          * block's residual (the DC from the Hadamard plus the AC).  The above
          * and left neighbor macroblocks are available only when they exist and
          * lie in this slice; the mb_y > 0 and mb_x > 0 tests guard
          * the unsigned address subtractions.  Under constrained_intra_pred an
          * inter-coded neighbor is unavailable as well, which a non-negative
          * list-0 reference index marks. */
         int pred[256];
         unsigned cur_addr = mb_y * width_in_mbs + mb_x;
         bool top_av = mb_y > 0 && cur_addr - width_in_mbs >= slice_first_mb &&
                       !(constrained_intra &&
                         mbs[cur_addr - width_in_mbs].ref_l0[0] >= 0);
         bool left_av = mb_x > 0 && cur_addr - 1 >= slice_first_mb &&
                        !(constrained_intra && mbs[cur_addr - 1].ref_l0[0] >= 0);
         predict_16x16(luma, stride, mb_x, mb_y, top_av, left_av,
                       (mb->mb_type - 1) % 4, pred);
         for (unsigned s = 0; s < 16; s++) {
            int16_t residual[16];
            vl_h264_idct4(mb->coeff4x4[blk_y[s] * 4 + blk_x[s]], residual);
            int bx = blk_x[s] * 4, by = blk_y[s] * 4;
            for (int y = 0; y < 4; y++)
               for (int x = 0; x < 4; x++)
                  luma[(mb_y * 16 + by + y) * stride + mb_x * 16 + bx + x] =
                     (uint8_t)clip1(pred[(by + y) * 16 + bx + x]
                                    + residual[y * 4 + x]);
         }
      }
   }
}

/* One-dimensional inverse core transform (sec 8.5.12.2): the H.264 inverse basis
 * with the two half-weight entries as arithmetic right shifts. */
static void
idct4_1d(const int z[4], int out[4])
{
   int a = z[0] + z[2];
   int b = z[0] - z[2];
   int c = (z[1] >> 1) - z[3];
   int d = z[1] + (z[3] >> 1);
   out[0] = a + d;
   out[1] = b + c;
   out[2] = b - c;
   out[3] = a - d;
}

void
vl_h264_idct4(const int16_t coeff[16], int16_t residual[16])
{
   int rows[16] = { 0 }, cols[16] = { 0 };

   for (int r = 0; r < 4; r++) {
      int z[4] = { coeff[r * 4], coeff[r * 4 + 1], coeff[r * 4 + 2],
                   coeff[r * 4 + 3] };
      int o[4];
      idct4_1d(z, o);
      for (int c = 0; c < 4; c++)
         rows[r * 4 + c] = o[c];
   }

   for (int c = 0; c < 4; c++) {
      int z[4] = { rows[c], rows[4 + c], rows[8 + c], rows[12 + c] };
      int o[4];
      idct4_1d(z, o);
      for (int r = 0; r < 4; r++)
         cols[r * 4 + c] = o[r];
   }

   for (int i = 0; i < 16; i++)
      residual[i] = (int16_t)((cols[i] + 32) >> 6);
}

/* The 4:2:0 chroma neighbors: top is p[0..7,-1], left is p[-1,0..7], tl is
 * p[-1,-1], each from one earlier macroblock so a flag per side suffices. */
struct neighbors_c {
   int top[8];
   int left[8];
   int tl;
   bool top_av;
   bool left_av;
};

static int cctop(const struct neighbors_c *n, int k) { return k < 0 ? n->tl : n->top[k]; }
static int ccleft(const struct neighbors_c *n, int k) { return k < 0 ? n->tl : n->left[k]; }

/* A chroma DC quadrant value (sec 8.3.3.1): the average of both candidate sums
 * when allowed and available, else the first available, else 128. */
static int
chroma_dc_avg(int sum_a, bool av_a, int sum_b, bool av_b, bool allow_both)
{
   if (allow_both && av_a && av_b)
      return (sum_a + sum_b + 4) >> 3;
   if (av_a)
      return (sum_a + 2) >> 2;
   if (av_b)
      return (sum_b + 2) >> 2;
   return 128;
}

static void
predict_chroma_dc(const struct neighbors_c *n, int pred[64])
{
   int t04 = n->top[0] + n->top[1] + n->top[2] + n->top[3];
   int t47 = n->top[4] + n->top[5] + n->top[6] + n->top[7];
   int l04 = n->left[0] + n->left[1] + n->left[2] + n->left[3];
   int l47 = n->left[4] + n->left[5] + n->left[6] + n->left[7];

   /* Each 4x4 quadrant has its own source preference (sec 8.3.3.1). */
   int q[2][2];
   q[0][0] = chroma_dc_avg(t04, n->top_av, l04, n->left_av, true);
   q[1][0] = chroma_dc_avg(t47, n->top_av, l04, n->left_av, false);
   q[0][1] = chroma_dc_avg(l47, n->left_av, t04, n->top_av, false);
   q[1][1] = chroma_dc_avg(t47, n->top_av, l47, n->left_av, true);

   for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++)
         pred[y * 8 + x] = q[x / 4][y / 4];
}

static void
predict_chroma_plane(const struct neighbors_c *n, int pred[64])
{
   int h = 0, v = 0;
   for (int i = 0; i < 4; i++) {
      h += (i + 1) * (n->top[4 + i] - cctop(n, 2 - i));
      v += (i + 1) * (n->left[4 + i] - ccleft(n, 2 - i));
   }
   int a = 16 * (n->left[7] + n->top[7]);
   int b = (17 * h + 16) >> 5;
   int c = (17 * v + 16) >> 5;
   for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++)
         pred[y * 8 + x] = clip1((a + b * (x - 3) + c * (y - 3) + 16) >> 5);
}

static void
predict_chroma(const struct neighbors_c *n, int mode, int pred[64])
{
   switch (mode) {
   case 0:
      predict_chroma_dc(n, pred);
      break;
   case 1: /* Horizontal */
      for (int y = 0; y < 8; y++)
         for (int x = 0; x < 8; x++)
            pred[y * 8 + x] = n->left[y];
      break;
   case 2: /* Vertical */
      for (int y = 0; y < 8; y++)
         for (int x = 0; x < 8; x++)
            pred[y * 8 + x] = n->top[x];
      break;
   default: /* 3: Plane */
      predict_chroma_plane(n, pred);
      break;
   }
}

static void
build_chroma_neighbors(const uint8_t *plane, unsigned stride, unsigned mb_x,
                       unsigned mb_y, bool top_av, bool left_av,
                       struct neighbors_c *n)
{
   int x0 = mb_x * 8, y0 = mb_y * 8;
   n->top_av = top_av;
   n->left_av = left_av;
   for (int k = 0; k < 8; k++) {
      n->top[k] = n->top_av ? plane[(y0 - 1) * (int)stride + x0 + k] : 0;
      n->left[k] = n->left_av ? plane[(y0 + k) * (int)stride + x0 - 1] : 0;
   }
   n->tl = (n->top_av && n->left_av) ? plane[(y0 - 1) * (int)stride + x0 - 1] : 0;
}

/* Reconstruct one chroma component's plane (sec 8.3.3): predict each macroblock's
 * 8x8 from the neighbors, then add the four 4x4 residual blocks. */
static void
reconstruct_chroma_plane(const struct vl_h264_mb_contract *mbs, unsigned num_mbs,
                         unsigned width_in_mbs, uint8_t *plane, unsigned stride,
                         unsigned comp, bool constrained_intra)
{
   for (unsigned addr = 0; addr < num_mbs; addr++) {
      const struct vl_h264_mb_contract *mb = &mbs[addr];
      unsigned mb_x = (unsigned)mb->mb_x, mb_y = (unsigned)mb->mb_y;
      unsigned slice_first_mb = (unsigned)mb->slice_first_mb;
      struct neighbors_c n;
      int pred[64];

      /* Only the intra macroblocks; the inter ones are the GPU back half's. */
      if (mb->ref_l0[0] >= 0)
         continue;

      /* The chroma neighbor macroblocks follow the same slice availability as
       * luma: present only when the above or left macroblock exists and lies in
       * this slice.  Under constrained_intra_pred an inter-coded neighbor is
       * unavailable as well, which a non-negative list-0 reference index marks. */
      unsigned cur_addr = mb_y * width_in_mbs + mb_x;
      bool top_av = mb_y > 0 && cur_addr - width_in_mbs >= slice_first_mb &&
                    !(constrained_intra &&
                      mbs[cur_addr - width_in_mbs].ref_l0[0] >= 0);
      bool left_av = mb_x > 0 && cur_addr - 1 >= slice_first_mb &&
                     !(constrained_intra && mbs[cur_addr - 1].ref_l0[0] >= 0);
      build_chroma_neighbors(plane, stride, mb_x, mb_y, top_av, left_av, &n);
      predict_chroma(&n, mb->intra_chroma_pred_mode, pred);

      for (unsigned blk = 0; blk < 4; blk++) {
         int16_t residual[16];
         vl_h264_idct4(mb->coeff4x4[VL_H264_LUMA_4X4_BLOCKS + comp * 4 + blk],
                       residual);
         int qx = (blk % 2) * 4, qy = (blk / 2) * 4;
         for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
               plane[(mb_y * 8 + qy + y) * stride + mb_x * 8 + qx + x] =
                  (uint8_t)clip1(pred[(qy + y) * 8 + qx + x] + residual[y * 4 + x]);
      }
   }
}

void
vl_h264_intra_reconstruct_chroma(const struct vl_h264_mb_contract *mbs,
                                 unsigned num_mbs, unsigned width_in_mbs,
                                 unsigned height_in_mbs, uint8_t *cb, uint8_t *cr,
                                 unsigned stride, bool constrained_intra)
{
   (void)height_in_mbs;
   reconstruct_chroma_plane(mbs, num_mbs, width_in_mbs, cb, stride, 0,
                            constrained_intra);
   reconstruct_chroma_plane(mbs, num_mbs, width_in_mbs, cr, stride, 1,
                            constrained_intra);
}
