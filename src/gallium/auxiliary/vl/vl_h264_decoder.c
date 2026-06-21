/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include "pipe/p_context.h"
#include "pipe/p_video_state.h"

#include "vl_defines.h"
#include "vl_h264_decoder.h"
#include "vl_h264_vld_provider.h"

struct vl_h264_decoder {
   struct pipe_video_codec base;
   struct pipe_context *context;
   struct vl_h264_vld_provider *provider;

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
   const struct pipe_h264_picture_desc *h264 =
      (const struct pipe_h264_picture_desc *)picture;

   (void) target;

   /* Clear the per-frame contract; the provider repopulates the macroblocks it
    * decodes, leaving skipped macroblocks zeroed. */
   memset(dec->frame.macroblocks, 0,
          dec->frame.num_macroblocks * sizeof(*dec->frame.macroblocks));
   dec->frame.slice_type = h264->base.profile;
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

static int
vl_h264_end_frame(struct pipe_video_codec *codec,
                  struct pipe_video_buffer *target,
                  struct pipe_picture_desc *picture)
{
   struct vl_h264_decoder *dec = (struct vl_h264_decoder *)codec;

   (void) dec;
   (void) target;
   (void) picture;

   /* TODO: emit the FP24 back half from dec->frame.macroblocks.  For each
    *       macroblock dispatch the inverse-transform fragment program (the 4x4
    *       idct, or the 8x8 idct when transform_8x8 is set), then luma and
    *       chroma motion compensation and the deblock filter, into target, then
    *       texture_barrier and flush like vl_mpeg12_end_frame.  The contract
    *       coefficients are canonical raster pixel-natural
    *       (vl_h264_mb_contract.h), validated bit-exact against ffmpeg, so the
    *       fragment program runs a plain transform with no transpose.
    *       reason -- the per-stage fragment programs are the next bring-up rung,
    *       Constrained Baseline luma-only I-frame first, then chroma, then P
    *       frames, then High-profile 8x8.  tracking -- vl_h264_decoder back-half
    *       emission against vl_h264_mb_contract. */
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
    * rather than a half-built codec. */
   provider = vl_h264_vld_provider_create(VL_H264_VLD_PROVIDER_MESA_CAVLC);
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
