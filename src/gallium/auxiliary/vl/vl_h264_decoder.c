/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
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

/* Build RefPicList0 for a P slice (ITU-T H.264 sec 8.2.4.2.1): the short-term
 * references ordered by descending frame_num, then the long-term references.
 * The bitstream's ref_pic_list_modification is consumed by the entropy provider
 * and not reconstructed here, so an explicitly reordered list falls back to this
 * default order, and a frame_num wrap (long sequence) is left to the descending
 * sort.  Returns the list length; list[0] is the back half's reference. */
static unsigned
build_ref_pic_list0(const struct pipe_h264_picture_desc *h264,
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
   for (unsigned i = 1; i < nst; i++) {     /* insertion sort, the list is tiny */
      unsigned key = st[i];
      int j = (int) i - 1;
      while (j >= 0 &&
             h264->frame_num_list[st[j]] < h264->frame_num_list[key]) {
         st[j + 1] = st[j];
         j--;
      }
      st[j + 1] = key;
   }
   unsigned n = 0;
   for (unsigned i = 0; i < nst; i++)
      list[n++] = h264->ref[st[i]];
   for (unsigned i = 0; i < nlt; i++)
      list[n++] = h264->ref[lt[i]];
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

/* Reconstruct the intra macroblocks on the CPU into the target planes (sec 8.3).
 * Every macroblock of an I frame is intra; the inter macroblocks of a P frame
 * carry a reference index of 0 and are skipped, so this fills the intra ones,
 * reading the inter neighbors the back half wrote.  The chroma plane is
 * interleaved NV12, so it is de-interleaved per component and re-interleaved. */
static void
reconstruct_intra(struct vl_h264_decoder *dec, struct pipe_surface *surfaces)
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
                                     yx->stride);
      /* The plane now holds the whole frame -- inter macroblocks from the back
       * half, intra from the pass above -- so the in-loop deblock runs once over
       * all luma edges. */
      if (!getenv("R300_H264_NO_DEBLOCK"))
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
                                          cb, cr, cbx->stride);
         if (!getenv("R300_H264_NO_DEBLOCK"))
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
                                       cr, chroma_w);
      if (!getenv("R300_H264_NO_DEBLOCK"))
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
   unsigned n0 = build_ref_pic_list0(h264, list0);
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
   }

   /* The intra macroblocks reconstruct on the CPU after the back half, so they
    * read any inter neighbors from the written planes. */
   reconstruct_intra(dec, target_surfaces);
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
