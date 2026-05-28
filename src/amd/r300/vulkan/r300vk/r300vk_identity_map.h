/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Identity-map compute-as-raster lowering primitives.  The classifier
 * (r300_compute_admission.c r300_nir_detect_identity_map) recognizes
 * `out_buffer[gid] = in_buffer[gid]` at pipeline-create time; the helpers
 * here lower one such admitted kernel onto the RS482 fragment pipeline as a
 * fullscreen-quad draw that samples the input buffer wrapped as a
 * PIPE_TEXTURE_2D and writes via RB3D color export.  All r300vk Vulkan-API
 * concepts (buffer, descriptor set, dispatch group count) are resolved to
 * pipe_context primitives so the gallium-mediated submit path replays the
 * kernel as a graphics draw.
 *
 * Three helpers; each is its own subtask in the M-E build-out:
 *   M-E.3.1  r300vk_identity_map_wrap_input_as_sampler_view   <-- this file
 *   M-E.3.2  r300vk_identity_map_dispatch_replay              <-- follow-on
 *   M-E.4    bit-exact readback oracle for the dispatched draw
 */

#ifndef R300VK_IDENTITY_MAP_H
#define R300VK_IDENTITY_MAP_H

#include "r300vk_private.h"

#include "pipe/p_state.h"
#include "util/format/u_formats.h"

#ifdef __cplusplus
extern "C" {
#endif

struct r300vk_device;

/* Wrap the contents of a PIPE_BUFFER pipe_resource as a transient
 * PIPE_TEXTURE_2D + a pipe_sampler_view configured for NEAREST sampling.
 * The texture is allocated linear-tiled, populated by
 * pipe->resource_copy_region (which falls through to util_resource_copy_region
 * for the buffer-to-texture target combination per r300_blit.c:593), and
 * referenced by the returned sampler view.  The caller releases the view
 * with pipe_sampler_view_reference(&view, NULL); dropping the view's last
 * reference also drops the texture's last reference, so the transient
 * resource is freed automatically.
 *
 * width * height * util_format_get_blocksize(format) MUST equal the byte
 * count the caller intends to read out of src_buf; the helper does not
 * verify the buffer's actual size.
 *
 * Returns NULL on resource_create / create_sampler_view failure. */
struct pipe_sampler_view *
r300vk_identity_map_wrap_input_as_sampler_view(struct r300vk_device *device,
                                               struct pipe_resource *src_buf,
                                               unsigned width,
                                               unsigned height,
                                               enum pipe_format format);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_IDENTITY_MAP_H */
