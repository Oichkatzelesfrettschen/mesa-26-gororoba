/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_VIDEO_H
#define R300_VIDEO_H

#include "pipe/p_video_enums.h"

struct pipe_screen;
struct pipe_context;
struct pipe_video_codec;

/* Report g3dvl MPEG-1/MPEG-2 shader-decode capabilities; 0 for everything else
 * (no UVD on RS485M/R300-class hardware). */
int
r300_get_video_param(struct pipe_screen *screen,
                     enum pipe_video_profile profile,
                     enum pipe_video_entrypoint entrypoint,
                     enum pipe_video_cap param);

/* Instantiate the g3dvl MPEG-1/MPEG-2 decoder; NULL for unsupported codecs. */
struct pipe_video_codec *
r300_create_video_codec(struct pipe_context *context,
                        const struct pipe_video_codec *templat);

#endif /* R300_VIDEO_H */
