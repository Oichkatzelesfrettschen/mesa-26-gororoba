/*
 * SPDX-License-Identifier: MIT
 */

/* RS482/R300-class hardware has no UVD/VCE video block, so the only
 * hardware-accelerated decode path is the g3dvl shader decoder: macroblocks
 * reconstructed by fragment shaders (IDCT, motion compensation, inverse zigzag
 * scan) on the 3D pipe.  This backend advertises MPEG-1/MPEG-2 and routes codec
 * creation to vl_create_mpeg12_decoder.  An experimental, opt-in H.264 decode
 * path (Constrained Baseline, the FP24 back half driven by a CPU entropy
 * provider) is gated off by default and advertised only when an entropy
 * provider exists; encode remains unsupported. */

#include "pipe/p_screen.h"
#include "pipe/p_video_codec.h"

#include "util/u_debug.h"
#include "util/u_video.h"

#include "vl/vl_h264_decoder.h"
#include "vl/vl_h264_vld_provider.h"
#include "vl/vl_video_buffer.h"
#include "vl/vl_mpeg12_decoder.h"

#include "r300_video.h"

#if VIDEO_CODEC_H264DEC
static bool
r300_h264_decode_available(enum pipe_video_profile profile)
{
   /* Opt-in and lowest-complexity only: Constrained Baseline is 4x4-transform
    * CAVLC with no B-frames, the first bring-up rung.  Advertise only when an
    * entropy provider exists, so H.264 is never reported without a way to
    * decode it. */
   if (!debug_get_bool_option("R300_H264_EXPERIMENTAL", false))
      return false;
   if (profile != PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE &&
       profile != PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE)
      return false;
   return vl_h264_vld_provider_any_available();
}
#endif

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

#if VIDEO_CODEC_H264DEC
   /* Experimental, opt-in H.264 decode: full-bitstream entrypoint only (the CPU
    * provider does the entropy decode, so there is no IDCT/MC entrypoint), and
    * only when a provider exists. */
   if (u_reduce_video_profile(profile) == PIPE_VIDEO_FORMAT_MPEG4_AVC &&
       r300_h264_decode_available(profile)) {
      switch (param) {
      case PIPE_VIDEO_CAP_SUPPORTED:
         return entrypoint == PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
      default:
         return 0;
      }
   }
#endif

   /* Only MPEG-1/MPEG-2 decode exists on this hardware (plus the gated H.264
    * path above). */
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

#if VIDEO_CODEC_H264DEC
   /* vl_create_h264_decoder returns NULL when no entropy provider exists, which
    * keeps an experimental build with the gate on but no provider fail-closed. */
   if (u_reduce_video_profile(templat->profile) == PIPE_VIDEO_FORMAT_MPEG4_AVC &&
       r300_h264_decode_available(templat->profile))
      return vl_create_h264_decoder(context, templat);
#endif

   return NULL;
}
