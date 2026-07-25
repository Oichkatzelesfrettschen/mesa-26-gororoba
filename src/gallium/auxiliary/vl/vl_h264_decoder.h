/*
 * SPDX-License-Identifier: MIT
 */

/*
 * r300-class H.264 decoder.  The CPU runs the serial entropy decode through a
 * vl_h264_vld_provider and the FP24 fragment substrate runs the fixed-function
 * back half (inverse transform, motion compensation, deblock).  This mirrors
 * vl_mpeg12_decoder: the create function returns a pipe_video_codec, and the
 * driver's create_video_codec dispatches to it.  It returns NULL when no entropy
 * provider is available, so H.264 stays fail-closed until one lands.
 */

#ifndef vl_h264_decoder_h
#define vl_h264_decoder_h

#include "pipe/p_video_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;

struct pipe_video_codec *
vl_create_h264_decoder(struct pipe_context *context,
                       const struct pipe_video_codec *templat);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_decoder_h */
