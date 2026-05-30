/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/* RS482/R300-class hardware has no UVD/VCE video block, so the only
 * hardware-accelerated decode path is the g3dvl shader decoder: MPEG-1/MPEG-2
 * macroblocks reconstructed by fragment shaders (IDCT, motion compensation,
 * inverse zigzag scan) on the 3D pipe.  This backend advertises that one codec
 * and routes codec creation to vl_create_mpeg12_decoder; everything else --
 * H.264, HEVC, encode -- is unsupported and reported as such. */

#include "pipe/p_screen.h"
#include "pipe/p_video_codec.h"

#include "util/u_video.h"

#include "vl/vl_video_buffer.h"
#include "vl/vl_mpeg12_decoder.h"

#include "r300_video.h"

int
r300_get_video_param(struct pipe_screen *screen,
                     enum pipe_video_profile profile,
                     enum pipe_video_entrypoint entrypoint,
                     enum pipe_video_cap param)
{
   /* The VA frontend probes the profile-independent caps with
    * PIPE_VIDEO_PROFILE_UNKNOWN before it has selected a codec.  In particular
    * vlVaCreateSurfaces2 sets interlaced = !SUPPORTS_PROGRESSIVE, so reporting
    * that cap only for the MPEG12 profile leaves it 0 and allocates every
    * decode target as a two-layer interlaced array (array_size=2,
    * height halved) that the g3dvl shader decoder -- which renders progressive
    * frames only and asserts !target->interlaced -- cannot draw into.  Answer
    * the profile-independent caps for any profile; gate only the codec-specific
    * caps on MPEG-1/MPEG-2. */
   switch (param) {
   case PIPE_VIDEO_CAP_SUPPORTS_PROGRESSIVE:
      return 1;
   case PIPE_VIDEO_CAP_MAX_WIDTH:
   case PIPE_VIDEO_CAP_MAX_HEIGHT:
      /* The decode surfaces are sampled as 2D textures, so the frame size is
       * bounded by the max 2D texture: 2048 on R300/R400, 4096 on R500. */
      return vl_video_buffer_max_size(screen);
   default:
      break;
   }

   /* Only MPEG-1/MPEG-2 decode exists on this hardware. */
   if (u_reduce_video_profile(profile) != PIPE_VIDEO_FORMAT_MPEG12)
      return 0;

   switch (param) {
   case PIPE_VIDEO_CAP_SUPPORTED:
      /* The shader decoder accepts the full-bitstream, IDCT and
       * motion-compensation entrypoints, but never encode. */
      return entrypoint == PIPE_VIDEO_ENTRYPOINT_BITSTREAM ||
             entrypoint == PIPE_VIDEO_ENTRYPOINT_IDCT ||
             entrypoint == PIPE_VIDEO_ENTRYPOINT_MC;
   default:
      return 0;
   }
}

struct pipe_video_codec *
r300_create_video_codec(struct pipe_context *context,
                        const struct pipe_video_codec *templat)
{
   if (templat->entrypoint == PIPE_VIDEO_ENTRYPOINT_ENCODE)
      return NULL;

   /* vl_create_mpeg12_decoder lives in libgalliumvl and has no stub; reference
    * it only when the MPEG-1/2 codec is built in, so a video-disabled r300 build
    * (which links the vl stub) does not pull an undefined symbol. */
#if VIDEO_CODEC_MPEG12DEC
   if (u_reduce_video_profile(templat->profile) == PIPE_VIDEO_FORMAT_MPEG12)
      return vl_create_mpeg12_decoder(context, templat);
#endif

   return NULL;
}
