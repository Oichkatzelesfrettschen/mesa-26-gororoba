/*
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "util/macros.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include "pipe/p_context.h"
#include "pipe/p_video_state.h"

#include "vl_defines.h"
#include "vl_h264_cpu_mc.h"
#include "vl_h264_deblock_cpu.h"
#include "vl_h264_decoder.h"
#include "vl_h264_emit.h"
#include "vl_h264_intra_reconstruct.h"
#include "vl_h264_vld_provider.h"

struct vl_h264_decoder {
   struct pipe_video_codec base;
   struct pipe_context *context;
   struct vl_h264_vld_provider *provider;
   struct vl_h264_emit *emit;

   unsigned width_in_mbs;
   unsigned height_in_mbs;

   /* Per-frame per-macroblock contract: the provider fills it slice by slice,
    * the back half consumes it in end_frame. */
   struct vl_h264_slice_contract frame;
};

static void
vl_h264_begin_frame(struct pipe_video_codec *codec,
                    struct pipe_video_buffer *target,
                    struct pipe_picture_desc *picture)
{
   struct vl_h264_decoder *dec = (struct vl_h264_decoder *)codec;

   (void) target;
   (void) picture;

   /* Clear the per-frame contract; the provider repopulates the macroblocks it
    * decodes, leaving skipped macroblocks zeroed.  The slice type is the
    * provider's to set from the slice header (the replay provider from the
    * serialized contract), not the picture's profile. */
   memset(dec->frame.macroblocks, 0,
          dec->frame.num_macroblocks * sizeof(*dec->frame.macroblocks));
}

static void
vl_h264_decode_bitstream(struct pipe_video_codec *codec,
                         struct pipe_video_buffer *target,
                         struct pipe_picture_desc *picture,
                         unsigned num_buffers,
                         const void *const *buffers,
                         const unsigned *sizes)
{
   struct vl_h264_decoder *dec = (struct vl_h264_decoder *)codec;
   const struct pipe_h264_picture_desc *h264 =
      (const struct pipe_h264_picture_desc *)picture;
   unsigned i;

   (void) target;

   /* Each buffer is one slice's raw Annex B NAL bytes (the VA frontend has
    * already prepended the start code).  The CPU entropy provider turns them
    * into per-macroblock contract records the GPU back half consumes. */
   for (i = 0; i < num_buffers; ++i)
      dec->provider->decode_slice(dec->provider, h264,
                                  (const uint8_t *)buffers[i], sizes[i],
                                  &dec->frame);
}

/* PicNum of a short-term reference for frame decoding (ITU-T H.264 sec 8.2.4.1):
 * FrameNumWrap, the stored frame_num pulled back by MaxFrameNum once it is ahead
 * of the current frame_num. */
static int
short_term_pic_num(const struct pipe_h264_picture_desc *h264, unsigned r,
                   int curr_frame_num, int max_frame_num)
{
   int fn = (int) h264->frame_num_list[r];
   return (fn > curr_frame_num) ? fn - max_frame_num : fn;
}

/* Build RefPicList0 for a P slice (ITU-T H.264 sec 8.2.4.2.1): short-term
 * references ordered by descending frame_num, then long-term, then the slice's
 * ref_pic_list_modification reordering (sec 8.2.4.3.1) when frame carries it.
 * With no reordering the result is the plain descending-frame_num list.  list[0]
 * is the back half's reference; the multiref fixups index list[ref_idx].  Returns
 * the list length. */
static unsigned
build_ref_pic_list0(const struct pipe_h264_picture_desc *h264,
                    const struct vl_h264_slice_contract *frame,
                    const struct pipe_h264_sps *sps,
                    struct pipe_video_buffer **list)
{
   unsigned st[16], lt[16], nst = 0, nlt = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(h264->ref); i++) {
      if (!h264->ref[i])
         continue;
      if (h264->is_long_term[i])
         lt[nlt++] = i;
      else
         st[nst++] = i;
   }
   /* Order the short-term references by descending PicNum (sec 8.2.4.2.1).  PicNum
    * is FrameNumWrap, so at a frame_num wrap a reference whose FrameNum exceeds
    * the current frame_num is the older picture; a raw-FrameNum sort would place
    * it first.  Without an sps the wrap is unknown, so fall back to raw FrameNum,
    * which equals PicNum until a wrap occurs. */
   int curr = (int) h264->frame_num;
   int max_fn = sps ? (1 << (sps->log2_max_frame_num_minus4 + 4)) : 0;
   for (unsigned i = 1; i < nst; i++) {     /* insertion sort, the list is tiny */
      unsigned key = st[i];
      int key_pn = max_fn ? short_term_pic_num(h264, key, curr, max_fn)
                          : (int) h264->frame_num_list[key];
      int j = (int) i - 1;
      while (j >= 0 &&
             (max_fn ? short_term_pic_num(h264, st[j], curr, max_fn)
                     : (int) h264->frame_num_list[st[j]]) < key_pn) {
         st[j + 1] = st[j];
         j--;
      }
      st[j + 1] = key;
   }
   /* The default list as indices into h264->ref[]: short-term then long-term. */
   unsigned idx[VL_H264_MAX_REORDER_L0 + 1], n = 0;
   for (unsigned i = 0; i < nst && n + 1 < ARRAY_SIZE(idx); i++)
      idx[n++] = st[i];
   for (unsigned i = 0; i < nlt && n + 1 < ARRAY_SIZE(idx); i++)
      idx[n++] = lt[i];

   /* Reconstruct the reordering (sec 8.2.4.3.1 and 8.2.4.3.2).  Short-term
    * commands (modification_of_pic_nums_idc 0 and 1) select a reference by
    * PicNum; a long-term command (idc 2) selects by LongTermPicNum.  The shift
    * needs a list of at least num_ref_idx_l0_active entries.  RefPicList0 is the
    * leading num_ref_idx_l0_active entries, which is all the back half and the
    * multiref fixups read, so entries past it are left untouched. */
   unsigned num_active = h264->num_ref_idx_l0_active_minus1 + 1;
   if (frame && frame->num_reorder_l0 > 0 && num_active <= n &&
       num_active + 1 <= ARRAY_SIZE(idx) && sps) {
      int pred = curr;
      unsigned ref_idx = 0;
      for (unsigned c = 0; c < frame->num_reorder_l0 && ref_idx < num_active; c++) {
         unsigned op = frame->reorder_l0[c].idc;
         unsigned target = ARRAY_SIZE(h264->ref);
         if (op == 0 || op == 1) {
            int abs_diff = (int) frame->reorder_l0[c].value + 1;
            int no_wrap = (op == 0)
               ? ((pred - abs_diff < 0) ? pred - abs_diff + max_fn : pred - abs_diff)
               : ((pred + abs_diff >= max_fn) ? pred + abs_diff - max_fn : pred + abs_diff);
            pred = no_wrap;
            int pic_num = (no_wrap > curr) ? no_wrap - max_fn : no_wrap;
            for (unsigned r = 0; r < ARRAY_SIZE(h264->ref); r++) {
               if (!h264->ref[r] || h264->is_long_term[r])
                  continue;
               if (short_term_pic_num(h264, r, curr, max_fn) == pic_num) {
                  target = r;
                  break;
               }
            }
         } else if (op == 2) {
            /* LongTermPicNum is LongTermFrameIdx for frame coding, which the VA
             * reference carries in frame_num_list, the same field short-term
             * references use for FrameNum. */
            unsigned long_term_pic_num = frame->reorder_l0[c].value;
            for (unsigned r = 0; r < ARRAY_SIZE(h264->ref); r++) {
               if (!h264->ref[r] || !h264->is_long_term[r])
                  continue;
               if (h264->frame_num_list[r] == long_term_pic_num) {
                  target = r;
                  break;
               }
            }
         } else {
            continue;                  /* idc 3 ends the list */
         }
         if (target == ARRAY_SIZE(h264->ref))
            continue;                  /* not in the DPB; leave the default entry */
         for (unsigned cidx = num_active; cidx > ref_idx; cidx--)
            idx[cidx] = idx[cidx - 1];
         idx[ref_idx++] = target;
         unsigned nidx = ref_idx;
         for (unsigned cidx = ref_idx; cidx <= num_active; cidx++)
            if (idx[cidx] != target)
               idx[nidx++] = idx[cidx];
      }
   }

   for (unsigned i = 0; i < n; i++)
      list[i] = h264->ref[idx[i]];
   return n;
}

/* Reconstruct on the CPU the inter luma blocks the back half got wrong: the
 * diagonal-center quarter-pel positions whose 2D half-pel overflows FP24, and
 * every block that references a RefPicList0 entry past index 0 (the back half
 * samples only list0[0]).  Maps each reference's luma and the target, then
 * overwrites those blocks. */
static void
luma_mc_multiref(struct vl_h264_decoder *dec, struct pipe_surface *surfaces,
                 struct pipe_video_buffer **list0, unsigned n0)
{
   struct pipe_context *ctx = dec->context;
   unsigned w = dec->width_in_mbs, h = dec->height_in_mbs;

   struct vl_h264_ref_plane refs[16] = {0};
   struct pipe_transfer *rx[16] = {0};
   for (unsigned i = 0; i < n0 && i < 16; i++) {
      struct pipe_sampler_view **pv = list0[i]->get_sampler_view_planes(list0[i]);
      struct pipe_resource *tex = (pv && pv[0]) ? pv[0]->texture : NULL;
      if (!tex)
         continue;
      refs[i].pixels = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_READ, 0, 0,
                                        tex->width0, tex->height0, &rx[i]);
      refs[i].w = tex->width0;
      refs[i].h = tex->height0;
      refs[i].stride = rx[i] ? rx[i]->stride : 0;
   }

   struct pipe_transfer *tx;
   uint8_t *t = pipe_texture_map(ctx, surfaces[0].texture, 0, 0,
                                 PIPE_MAP_READ_WRITE, 0, 0, w * 16, h * 16, &tx);
   if (t && refs[0].pixels)
      vl_h264_cpu_luma_mc_multiref(dec->frame.macroblocks,
                                   dec->frame.num_macroblocks, w, h, refs, n0, t,
                                   tx->stride);
   if (t)
      pipe_texture_unmap(ctx, tx);
   for (unsigned i = 0; i < n0 && i < 16; i++)
      if (rx[i])
         pipe_texture_unmap(ctx, rx[i]);
}

/* Map one chroma component plane of every RefPicList0 entry past index 0 into a
 * ref_plane array.  plane is 1 for Cb (sampler view 1) and 2 for Cr (sampler view
 * 2 of planar Y8_U8_V8_420); on a packed NV12 reference (no third view) the
 * interleaved view is de-interleaved into scratch instead, returned in
 * scratch[i].  Entry 0 is skipped: the back half already produced it. */
static void
map_ref_chroma(struct pipe_context *ctx, struct pipe_video_buffer **list0,
               unsigned n0, unsigned plane, unsigned chroma_w, unsigned chroma_h,
               struct vl_h264_ref_plane *refs, struct pipe_transfer **rx,
               uint8_t **scratch)
{
   for (unsigned i = 1; i < n0 && i < 16; i++) {
      struct pipe_sampler_view **pv = list0[i]->get_sampler_view_planes(list0[i]);
      if (!pv)
         continue;
      if (pv[2] && pv[2]->texture) {
         struct pipe_resource *tex = pv[plane] ? pv[plane]->texture : NULL;
         if (!tex)
            continue;
         refs[i].pixels = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_READ, 0, 0,
                                           tex->width0, tex->height0, &rx[i]);
         refs[i].w = tex->width0;
         refs[i].h = tex->height0;
         refs[i].stride = rx[i] ? rx[i]->stride : 0;
      } else if (pv[1] && pv[1]->texture) {
         /* Packed NV12: de-interleave the requested component into scratch. */
         struct pipe_resource *tex = pv[1]->texture;
         struct pipe_transfer *itx;
         uint8_t *packed = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_READ, 0, 0,
                                            tex->width0, tex->height0, &itx);
         if (!packed)
            continue;
         scratch[i] = MALLOC(chroma_w * chroma_h);
         if (scratch[i]) {
            for (unsigned r = 0; r < chroma_h; r++)
               for (unsigned col = 0; col < chroma_w; col++)
                  scratch[i][r * chroma_w + col] =
                     packed[r * itx->stride + col * 2 + (plane - 1)];
            refs[i].pixels = scratch[i];
            refs[i].w = chroma_w;
            refs[i].h = chroma_h;
            refs[i].stride = chroma_w;
         }
         pipe_texture_unmap(ctx, itx);
      }
   }
}

/* Fix up on the CPU the inter chroma blocks the back half built from RefPicList0[0]
 * but which reference a later entry.  Mirrors luma_mc_multiref for the Cb and Cr
 * planes: maps each reference's chroma, recomputes the affected blocks, and writes
 * them back before the intra pass and the chroma deblock read the corrected
 * neighbors.  Handles the planar Y8_U8_V8_420 target directly and a packed NV12
 * target by de-interleaving into scratch, like reconstruct_intra. */
static void
chroma_mc_multiref(struct vl_h264_decoder *dec, struct pipe_surface *surfaces,
                   struct pipe_video_buffer **list0, unsigned n0)
{
   struct pipe_context *ctx = dec->context;
   unsigned w = dec->width_in_mbs, h = dec->height_in_mbs;
   unsigned chroma_w = w * 8, chroma_h = h * 8;
   if (n0 < 2)
      return; /* only RefPicList0[0] is in use; the back half covered it */

   struct vl_h264_ref_plane rcb[16] = {0}, rcr[16] = {0};
   struct pipe_transfer *rxb[16] = {0}, *rxr[16] = {0};
   uint8_t *scb[16] = {0}, *scr[16] = {0};
   map_ref_chroma(ctx, list0, n0, 1, chroma_w, chroma_h, rcb, rxb, scb);
   map_ref_chroma(ctx, list0, n0, 2, chroma_w, chroma_h, rcr, rxr, scr);

   if (surfaces[2].texture) {
      struct pipe_transfer *cbx, *crx;
      uint8_t *cb = pipe_texture_map(ctx, surfaces[1].texture, 0, 0,
                                     PIPE_MAP_READ_WRITE, 0, 0, chroma_w,
                                     chroma_h, &cbx);
      uint8_t *cr = pipe_texture_map(ctx, surfaces[2].texture, 0, 0,
                                     PIPE_MAP_READ_WRITE, 0, 0, chroma_w,
                                     chroma_h, &crx);
      if (cb && cr && cbx->stride == crx->stride)
         vl_h264_cpu_chroma_mc_multiref(dec->frame.macroblocks,
                                        dec->frame.num_macroblocks, w, h, rcb, rcr,
                                        n0, cb, cr, cbx->stride);
      if (cb)
         pipe_texture_unmap(ctx, cbx);
      if (cr)
         pipe_texture_unmap(ctx, crx);
   } else if (surfaces[1].texture) {
      struct pipe_transfer *cx;
      uint8_t *c = pipe_texture_map(ctx, surfaces[1].texture, 0, 0,
                                    PIPE_MAP_READ_WRITE, 0, 0, chroma_w, chroma_h,
                                    &cx);
      uint8_t *cb = c ? MALLOC(chroma_w * chroma_h) : NULL;
      uint8_t *cr = c ? MALLOC(chroma_w * chroma_h) : NULL;
      if (c && cb && cr) {
         for (unsigned r = 0; r < chroma_h; r++)
            for (unsigned col = 0; col < chroma_w; col++) {
               cb[r * chroma_w + col] = c[r * cx->stride + col * 2];
               cr[r * chroma_w + col] = c[r * cx->stride + col * 2 + 1];
            }
         vl_h264_cpu_chroma_mc_multiref(dec->frame.macroblocks,
                                        dec->frame.num_macroblocks, w, h, rcb, rcr,
                                        n0, cb, cr, chroma_w);
         for (unsigned r = 0; r < chroma_h; r++)
            for (unsigned col = 0; col < chroma_w; col++) {
               c[r * cx->stride + col * 2] = cb[r * chroma_w + col];
               c[r * cx->stride + col * 2 + 1] = cr[r * chroma_w + col];
            }
      }
      FREE(cb);
      FREE(cr);
      if (c)
         pipe_texture_unmap(ctx, cx);
   }

   for (unsigned i = 1; i < n0 && i < 16; i++) {
      if (rxb[i])
         pipe_texture_unmap(ctx, rxb[i]);
      if (rxr[i])
         pipe_texture_unmap(ctx, rxr[i]);
      FREE(scb[i]);
      FREE(scr[i]);
   }
}

/* Reconstruct the intra macroblocks on the CPU into the target planes (sec 8.3).
 * Every macroblock of an I frame is intra; the inter macroblocks of a P frame
 * carry a reference index of 0 and are skipped, so this fills the intra ones,
 * reading the inter neighbors the back half wrote.  The chroma plane is
 * interleaved NV12, so it is de-interleaved per component and re-interleaved. */
static void
reconstruct_intra(struct vl_h264_decoder *dec, struct pipe_surface *surfaces,
                  bool constrained_intra)
{
   struct pipe_context *ctx = dec->context;
   unsigned w = dec->width_in_mbs, h = dec->height_in_mbs;
   unsigned chroma_w = w * 8, chroma_h = h * 8;
   unsigned num_mbs = dec->frame.num_macroblocks;

   struct pipe_transfer *yx;
   uint8_t *y = pipe_texture_map(ctx, surfaces[0].texture, 0, 0,
                                 PIPE_MAP_READ_WRITE, 0, 0, w * 16, h * 16, &yx);
   if (y) {
      vl_h264_intra_reconstruct_luma(dec->frame.macroblocks, num_mbs, w, h, y,
                                     yx->stride, constrained_intra);
      /* The plane now holds the whole frame -- inter macroblocks from the back
       * half, intra from the pass above -- so the in-loop deblock runs once over
       * all luma edges. */
      vl_h264_deblock_cpu(&dec->frame, w, h, y, yx->stride, NULL, NULL, 0);
      pipe_texture_unmap(ctx, yx);
   }

   /* The decode target on this path is planar Y8_U8_V8_420: separate R8 Cb and
    * Cr planes (surfaces[1] and surfaces[2]), not packed NV12.  Reconstruct each
    * component directly in its own plane.  A packed NV12 target (no third plane)
    * is de-interleaved into scratch Cb/Cr and re-interleaved. */
   if (surfaces[2].texture) {
      struct pipe_transfer *cbx, *crx;
      uint8_t *cb = pipe_texture_map(ctx, surfaces[1].texture, 0, 0,
                                     PIPE_MAP_READ_WRITE, 0, 0, chroma_w,
                                     chroma_h, &cbx);
      uint8_t *cr = pipe_texture_map(ctx, surfaces[2].texture, 0, 0,
                                     PIPE_MAP_READ_WRITE, 0, 0, chroma_w,
                                     chroma_h, &crx);
      /* The Cb and Cr planes are identical R8 allocations, so they share a
       * stride; reconstruct both with it. */
      if (cb && cr && cbx->stride == crx->stride) {
         vl_h264_intra_reconstruct_chroma(dec->frame.macroblocks, num_mbs, w, h,
                                          cb, cr, cbx->stride, constrained_intra);
         vl_h264_deblock_cpu(&dec->frame, w, h, NULL, 0, cb, cr, cbx->stride);
      }
      if (cb)
         pipe_texture_unmap(ctx, cbx);
      if (cr)
         pipe_texture_unmap(ctx, crx);
      return;
   }

   struct pipe_transfer *cx;
   uint8_t *c = pipe_texture_map(ctx, surfaces[1].texture, 0, 0,
                                 PIPE_MAP_READ_WRITE, 0, 0, chroma_w, chroma_h,
                                 &cx);
   if (!c)
      return;
   uint8_t *cb = MALLOC(chroma_w * chroma_h);
   uint8_t *cr = MALLOC(chroma_w * chroma_h);
   if (cb && cr) {
      for (unsigned r = 0; r < chroma_h; r++)
         for (unsigned col = 0; col < chroma_w; col++) {
            cb[r * chroma_w + col] = c[r * cx->stride + col * 2];
            cr[r * chroma_w + col] = c[r * cx->stride + col * 2 + 1];
         }
      vl_h264_intra_reconstruct_chroma(dec->frame.macroblocks, num_mbs, w, h, cb,
                                       cr, chroma_w, constrained_intra);
      vl_h264_deblock_cpu(&dec->frame, w, h, NULL, 0, cb, cr, chroma_w);
      for (unsigned r = 0; r < chroma_h; r++)
         for (unsigned col = 0; col < chroma_w; col++) {
            c[r * cx->stride + col * 2] = cb[r * chroma_w + col];
            c[r * cx->stride + col * 2 + 1] = cr[r * chroma_w + col];
         }
   }
   FREE(cb);
   FREE(cr);
   pipe_texture_unmap(ctx, cx);
}

static int
vl_h264_end_frame(struct pipe_video_codec *codec,
                  struct pipe_video_buffer *target,
                  struct pipe_picture_desc *picture)
{
   struct vl_h264_decoder *dec = (struct vl_h264_decoder *)codec;
   const struct pipe_h264_picture_desc *h264 =
      (const struct pipe_h264_picture_desc *)picture;

   struct pipe_surface *target_surfaces = target->get_surfaces(target);
   if (!target_surfaces)
      return 0;

   /* Inter macroblocks predict from a reference, so the back half reconstructs
    * them when one is in the DPB.  An I frame has no reference and is left
    * entirely to the CPU intra pass below.  The back half samples RefPicList0[0];
    * blocks that reference a later list entry are fixed up on the CPU. */
   struct pipe_video_buffer *list0[16];
   unsigned n0 = build_ref_pic_list0(h264, &dec->frame,
                                     h264->pps ? h264->pps->sps : NULL, list0);
   struct pipe_sampler_view **ref_planes =
      n0 ? list0[0]->get_sampler_view_planes(list0[0]) : NULL;

   if (ref_planes && ref_planes[0]) {
      if (!dec->emit) {
         dec->emit = vl_h264_emit_create(dec->context);
         if (!dec->emit)
            return 0;
         /* The whole frame is deblocked on the CPU after reconstruct_intra, so
          * the inter emit must not also deblock. */
         vl_h264_emit_set_skip_deblock(dec->emit, true);
      }
      struct pipe_sampler_view *ref_luma = ref_planes[0];
      vl_h264_emit_luma_inter_unorm(dec->emit, &target_surfaces[0],
                                    dec->frame.width, dec->frame.height, ref_luma,
                                    ref_luma->texture->width0,
                                    ref_luma->texture->height0, &dec->frame);

      /* Planar Y8_U8_V8_420 carries Cb and Cr as separate R8 planes
       * (target_surfaces[2], ref_planes[2]); packed NV12 has neither, so the
       * emit takes NULL for the Cr surface and view and de-interleaves one R8G8
       * plane instead. */
      struct pipe_sampler_view *ref_chroma_cb = ref_planes[1];
      if (ref_chroma_cb) {
         struct pipe_surface *dst_cr =
            target_surfaces[2].texture ? &target_surfaces[2] : NULL;
         vl_h264_emit_chroma_inter_unorm(dec->emit, &target_surfaces[1], dst_cr,
                                         ref_chroma_cb->texture->width0,
                                         ref_chroma_cb->texture->height0,
                                         ref_chroma_cb, ref_planes[2],
                                         ref_chroma_cb->texture->width0,
                                         ref_chroma_cb->texture->height0,
                                         &dec->frame);
      }

      /* Fix up on the CPU the inter luma blocks the back half got wrong: the five
       * 2D diagonal half-pel positions f, i, j, k, q (the center half-pel j
       * overflows the FP24 integer-exact range, so the dispatch leaves them at the
       * integer position), and every block that references a RefPicList0 entry past
       * index 0 (the back half sampled only list0[0]).  This runs before the intra
       * pass and the deblock read the corrected neighbors. */
      luma_mc_multiref(dec, target_surfaces, list0, n0);

      /* The chroma emit above produced every block from RefPicList0[0]; correct
       * the blocks that reference a later entry on the CPU, before the intra pass
       * and the chroma deblock read them. */
      chroma_mc_multiref(dec, target_surfaces, list0, n0);
   }

   /* The intra macroblocks reconstruct on the CPU after the back half, so they
    * read any inter neighbors from the written planes.  constrained_intra_pred
    * bars those inter neighbors from intra prediction; a NULL pps leaves it
    * off. */
   bool constrained_intra =
      h264->pps && h264->pps->constrained_intra_pred_flag;
   reconstruct_intra(dec, target_surfaces, constrained_intra);
   return 0;
}

static void
vl_h264_flush(struct pipe_video_codec *codec)
{
   /* The back half flushes in end_frame, matching vl_mpeg12_flush. */
   (void) codec;
}

static void
vl_h264_destroy(struct pipe_video_codec *codec)
{
   struct vl_h264_decoder *dec = (struct vl_h264_decoder *)codec;

   if (dec->emit)
      vl_h264_emit_destroy(dec->emit);
   if (dec->provider)
      dec->provider->destroy(dec->provider);
   FREE(dec->frame.macroblocks);
   if (dec->context)
      dec->context->destroy(dec->context);
   FREE(dec);
}

struct pipe_video_codec *
vl_create_h264_decoder(struct pipe_context *context,
                       const struct pipe_video_codec *templat)
{
   struct vl_h264_vld_provider *provider;
   struct vl_h264_decoder *dec;
   unsigned num_mbs;

   assert(context);

   /* No entropy provider means no decoder: returning NULL keeps the decode
    * fail-closed, and the VA frontend's NULL check turns it into a clean error
    * rather than a half-built codec.  The most preferred available provider is
    * chosen, so a clean-room front end supersedes the bring-up replay without a
    * change here. */
   provider = vl_h264_vld_provider_create_available();
   if (!provider)
      return NULL;

   dec = CALLOC_STRUCT(vl_h264_decoder);
   if (!dec) {
      provider->destroy(provider);
      return NULL;
   }

   dec->base = *templat;
   dec->base.context = context;
   dec->context = pipe_create_multimedia_context(context->screen, false);
   dec->provider = provider;

   dec->width_in_mbs =
      align(dec->base.width, VL_MACROBLOCK_WIDTH) / VL_MACROBLOCK_WIDTH;
   dec->height_in_mbs =
      align(dec->base.height, VL_MACROBLOCK_HEIGHT) / VL_MACROBLOCK_HEIGHT;
   num_mbs = dec->width_in_mbs * dec->height_in_mbs;

   dec->frame.version = VL_H264_MB_CONTRACT_VERSION;
   dec->frame.width = dec->base.width;
   dec->frame.height = dec->base.height;
   dec->frame.provider = provider->kind;
   dec->frame.coeff_contract = VL_H264_COEFF_DEQUANTIZED;
   dec->frame.num_macroblocks = num_mbs;
   dec->frame.macroblocks = CALLOC(num_mbs, sizeof(*dec->frame.macroblocks));

   if (!dec->context || !dec->frame.macroblocks) {
      vl_h264_destroy(&dec->base);
      return NULL;
   }

   dec->base.destroy           = vl_h264_destroy;
   dec->base.begin_frame       = vl_h264_begin_frame;
   dec->base.decode_macroblock = NULL;   /* H.264 always uses decode_bitstream */
   dec->base.decode_bitstream  = vl_h264_decode_bitstream;
   dec->base.end_frame         = vl_h264_end_frame;
   dec->base.flush             = vl_h264_flush;

   return &dec->base;
}
