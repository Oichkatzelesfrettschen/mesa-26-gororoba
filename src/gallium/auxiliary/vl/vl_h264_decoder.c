/*
 * Copyright (c) 2026 Terascale Functionalists
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
#include "vl_h264_decoder.h"
#include "vl_h264_emit.h"
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

static int
vl_h264_end_frame(struct pipe_video_codec *codec,
                  struct pipe_video_buffer *target,
                  struct pipe_picture_desc *picture)
{
   struct vl_h264_decoder *dec = (struct vl_h264_decoder *)codec;
   const struct pipe_h264_picture_desc *h264 =
      (const struct pipe_h264_picture_desc *)picture;
   struct pipe_sampler_view **ref_planes = NULL;
   struct pipe_surface *target_surfaces;
   struct pipe_sampler_view *ref_luma;

   /* This rung reconstructs inter macroblocks, so it needs a reference frame: an
    * intra-only frame with no decoded reference in the DPB is left to the
    * intra-prediction rung and produces nothing here.  The first available
    * reference is the prediction source -- multiple references per frame are a
    * separate rung. */
   for (unsigned i = 0; i < ARRAY_SIZE(h264->ref) && !ref_planes; ++i)
      if (h264->ref[i])
         ref_planes = h264->ref[i]->get_sampler_view_planes(h264->ref[i]);
   if (!ref_planes || !ref_planes[0])
      return 0;
   ref_luma = ref_planes[0];

   target_surfaces = target->get_surfaces(target);
   if (!target_surfaces)
      return 0;

   if (!dec->emit) {
      dec->emit = vl_h264_emit_create(dec->context);
      if (!dec->emit)
         return 0;
   }

   /* Luma reconstruction over the whole frame: the orchestrator
    * motion-compensates each macroblock from the reference, adds the inverse
    * transform of its residual, and writes Clip1(prediction + residual) into the
    * target Y plane.  The deblock filter is a separate rung. */
   vl_h264_emit_luma_inter_unorm(dec->emit, &target_surfaces[0], dec->frame.width,
                                 dec->frame.height, ref_luma,
                                 ref_luma->texture->width0,
                                 ref_luma->texture->height0, &dec->frame);

   /* Chroma reconstruction into the interleaved NV12 chroma plane.  The chroma
    * planes are half resolution, so their dimensions come from the chroma plane
    * resource rather than the luma frame size. */
   struct pipe_sampler_view *ref_chroma = ref_planes[1];
   if (ref_chroma)
      vl_h264_emit_chroma_inter_unorm(dec->emit, &target_surfaces[1],
                                      ref_chroma->texture->width0,
                                      ref_chroma->texture->height0, ref_chroma,
                                      ref_chroma->texture->width0,
                                      ref_chroma->texture->height0, &dec->frame);
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
