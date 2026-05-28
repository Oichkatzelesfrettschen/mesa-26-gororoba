/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Identity-map compute-as-raster lowering primitives.  Each helper turns
 * one Vulkan compute API concept (a bound storage buffer, a dispatch grid)
 * into the pipe_context calls the r300g replay path expects.
 */

#include "r300vk_identity_map.h"
#include "r300vk_device.h"

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"

struct pipe_sampler_view *
r300vk_identity_map_wrap_input_as_sampler_view(struct r300vk_device *device,
                                               struct pipe_resource *src_buf,
                                               unsigned width,
                                               unsigned height,
                                               enum pipe_format format)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen || !src_buf || width == 0 || height == 0)
      return NULL;

   /* Allocate a transient linear-tiled 2D texture matching the dispatch
    * shape.  PIPE_USAGE_DEFAULT so the GPU can both write (the
    * resource_copy_region below) and read (sampler view at draw time).
    * The texture is freed automatically when the returned sampler view's
    * last reference is dropped. */
   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target     = PIPE_TEXTURE_2D;
   templ.format     = format;
   templ.width0     = width;
   templ.height0    = height;
   templ.depth0     = 1;
   templ.array_size = 1;
   templ.last_level = 0;
   templ.nr_samples = 0;
   templ.usage      = PIPE_USAGE_DEFAULT;
   templ.bind       = PIPE_BIND_SAMPLER_VIEW;

   struct pipe_resource *tex = screen->resource_create(screen, &templ);
   if (!tex)
      return NULL;

   /* Copy the buffer contents into the texture.  r300g's
    * r300_resource_copy_region falls back to util_resource_copy_region for
    * the buffer-to-texture target combination (r300_blit.c:593), which
    * handles the row-pitch / format-layout details by mapping both sides
    * and memcpy-ing. */
   const unsigned bytes = width * height * util_format_get_blocksize(format);
   struct pipe_box src_box;
   memset(&src_box, 0, sizeof(src_box));
   src_box.width  = bytes;
   src_box.height = 1;
   src_box.depth  = 1;

   pipe->resource_copy_region(pipe,
                              tex,    /* dst */ 0 /* dst_level */,
                              0, 0, 0 /* dst x,y,z */,
                              src_buf, 0 /* src_level */,
                              &src_box);

   /* Create the sampler view.  The view holds an internal reference to the
    * texture; drop our local reference so the texture lifetime tracks the
    * view's. */
   struct pipe_sampler_view sv_templ;
   memset(&sv_templ, 0, sizeof(sv_templ));
   sv_templ.format             = format;
   sv_templ.target             = PIPE_TEXTURE_2D;
   sv_templ.u.tex.first_layer  = 0;
   sv_templ.u.tex.last_layer   = 0;
   sv_templ.u.tex.first_level  = 0;
   sv_templ.u.tex.last_level   = 0;
   sv_templ.swizzle_r          = PIPE_SWIZZLE_X;
   sv_templ.swizzle_g          = PIPE_SWIZZLE_Y;
   sv_templ.swizzle_b          = PIPE_SWIZZLE_Z;
   sv_templ.swizzle_a          = PIPE_SWIZZLE_W;

   struct pipe_sampler_view *sv =
      pipe->create_sampler_view(pipe, tex, &sv_templ);
   pipe_resource_reference(&tex, NULL);
   return sv;
}
