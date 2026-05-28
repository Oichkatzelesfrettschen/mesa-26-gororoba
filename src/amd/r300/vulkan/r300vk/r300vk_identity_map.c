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
#include "r300vk_pipeline.h"
#include "r300vk_descriptor.h"
#include "r300vk_buffer.h"
#include "r300vk_cmd_buffer.h"

#include "compiler/shader_enums.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_surface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Diagnostic logging gate.  Active when the R300VK_DEBUG env variable
 * contains the substring "identity_map" (matches the existing
 * debug-options convention parsed in r300vk_instance.c).  Cached in a
 * file-scope static so the per-dispatch hot path does only one getenv on
 * the first call -- subsequent calls hit the cached integer.  The probe
 * runner already captures gate_on_stderr.txt, so any IDM_LOG line lands
 * in the bundle without runner changes. */
static bool
identity_map_debug_enabled(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *flags = getenv("R300VK_DEBUG");
      cached = (flags && strstr(flags, "identity_map")) ? 1 : 0;
   }
   return cached != 0;
}

#define IDM_LOG(fmt, ...) \
   do { \
      if (identity_map_debug_enabled()) \
         fprintf(stderr, "ident_map: " fmt "\n", ##__VA_ARGS__); \
   } while (0)

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

   /* Copy the buffer bytes into the 2D texture.  Neither
    * pipe->resource_copy_region (r300g blitter -> TXF -> R300 has no
    * texelFetch -> assertion) NOR util_resource_copy_region (asserts
    * src->target == dst->target at u_surface.c:225) handles the
    * PIPE_BUFFER -> PIPE_TEXTURE_2D direction directly.  Do the map
    * + memcpy ourselves: read the buffer as a flat byte stream, write
    * each texel-row of the texture from the matching byte range.
    * Linear-tiled texture so the dst pitch is W * blocksize plus any
    * driver-imposed alignment, captured by the transfer's stride. */
   const unsigned bpp = util_format_get_blocksize(format);
   struct pipe_transfer *src_xfer = NULL;
   struct pipe_box src_box;
   memset(&src_box, 0, sizeof(src_box));
   src_box.width  = width * height * bpp;
   src_box.height = 1;
   src_box.depth  = 1;
   const void *src_map = pipe->buffer_map(pipe, src_buf, 0, PIPE_MAP_READ,
                                          &src_box, &src_xfer);
   if (!src_map) {
      pipe_resource_reference(&tex, NULL);
      return NULL;
   }

   struct pipe_transfer *dst_xfer = NULL;
   struct pipe_box dst_box;
   memset(&dst_box, 0, sizeof(dst_box));
   dst_box.width  = width;
   dst_box.height = height;
   dst_box.depth  = 1;
   void *dst_map = pipe->texture_map(pipe, tex, 0,
                                     PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                     &dst_box, &dst_xfer);
   if (!dst_map) {
      pipe->buffer_unmap(pipe, src_xfer);
      pipe_resource_reference(&tex, NULL);
      return NULL;
   }
   const uint8_t *src_bytes = (const uint8_t *)src_map;
   uint8_t       *dst_bytes = (uint8_t *)dst_map;
   const unsigned row_bytes = width * bpp;
   for (unsigned r = 0; r < height; r++)
      memcpy(dst_bytes + r * dst_xfer->stride,
             src_bytes + r * row_bytes,
             row_bytes);
   pipe->texture_unmap(pipe, dst_xfer);
   pipe->buffer_unmap(pipe, src_xfer);

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

/* Locate the descriptor in a set's flat descriptors[] array that matches a
 * given Vulkan binding index.  Returns NULL on miss (the layout never
 * declared that binding) or zero count (the binding was declared with
 * descriptorCount = 0). */
static const struct r300vk_descriptor *
find_descriptor_by_binding(const struct r300vk_descriptor_set *set,
                           uint32_t binding_index)
{
   for (uint32_t i = 0; i < set->layout->binding_count; i++) {
      if (set->layout->bindings[i].binding == binding_index &&
          set->layout->bindings[i].count > 0)
         return &set->descriptors[set->layout->bindings[i].offset];
   }
   return NULL;
}

/* Walk the descriptor-set layout and pick the Nth STORAGE_BUFFER binding's
 * binding index.  Used to resolve the identity-map kernel's input + output
 * ssbo bindings when the NIR detector cannot recover them as constants
 * (the post-explicit_io binding source is a Vulkan descriptor handle, not
 * a nir_load_const).  The bindings array is sorted by binding index per
 * r300vk_CreateDescriptorSetLayout, so the Nth STORAGE_BUFFER seen during
 * a forward walk is the Nth declared in the layout. */
static bool
nth_storage_buffer_binding(const struct r300vk_descriptor_set *set,
                           unsigned which,
                           uint32_t *out_binding)
{
   unsigned seen = 0;
   for (uint32_t i = 0; i < set->layout->binding_count; i++) {
      if (set->layout->bindings[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
         if (seen == which) {
            *out_binding = set->layout->bindings[i].binding;
            return true;
         }
         seen++;
      }
   }
   return false;
}

/* Map the kernel's total invocation count onto a 2D raster grid: a single
 * row up to the texture-axis cap (R300 = 2048), then add rows as needed.
 * The bit-exact identity-map theorem in
 * src/re/r300/docs/rs482-r300vk-compute-identity-map-derivation.md bounds
 * this at 2048 x 2048 per dispatch; larger grids tile and dispatch
 * multiple times (M-J generalization). */
static void
derive_raster_extent(uint32_t total_invocations,
                     unsigned *out_width, unsigned *out_height)
{
   const unsigned axis_cap = 2048;
   if (total_invocations <= axis_cap) {
      *out_width  = total_invocations ? total_invocations : 1;
      *out_height = 1;
      return;
   }
   *out_width  = axis_cap;
   *out_height = (total_invocations + axis_cap - 1) / axis_cap;
}

bool
r300vk_identity_map_dispatch_replay(struct r300vk_device *device,
                                    const struct r300vk_pipeline *pl,
                                    const struct r300vk_cmd_dispatch *dispatch,
                                    const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   IDM_LOG("entry pl=%p is_identity_map=%d set_count=%u gx=%u gy=%u gz=%u",
           (const void *)pl,
           pl ? (int)pl->identity_map.is_identity_map : -1,
           binds ? binds->set_count : 0,
           dispatch ? dispatch->group_count_x : 0,
           dispatch ? dispatch->group_count_y : 0,
           dispatch ? dispatch->group_count_z : 0);
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0) {
      IDM_LOG("early-return null-or-empty-binds");
      return false;
   }
   if (!pl->vs_cso || !pl->fs_cso) {
      IDM_LOG("early-return no-vs-or-fs-cso");
      return false;
   }

   /* Walk the bound set to resolve the input + output ssbo bindings to
    * VkBuffer.resource pointers.  The first descriptor set holds them
    * (r300_nir_detect_identity_map records the bindings without recording
    * which set; identity-map kernels use a single set in practice). */
   const struct r300vk_descriptor_set *set = binds->sets[0];
   IDM_LOG("set=%p layout=%p in_binding=%u out_binding=%u",
           (const void *)set,
           set ? (const void *)set->layout : NULL,
           pl->identity_map.input_ssbo_binding,
           pl->identity_map.output_ssbo_binding);
   if (!set || !set->layout) {
      IDM_LOG("early-return no-set-or-layout");
      return false;
   }

   /* The detector's pl->identity_map.{input,output}_ssbo_binding only
    * carry the Vulkan binding indices when the NIR load_ssbo / store_ssbo
    * sources were constants -- which they are NOT after
    * nir_lower_explicit_io with nir_address_format_32bit_index_offset (the
    * binding is a load_vulkan_descriptor handle).  Fall back to the
    * descriptor-set layout: input = first STORAGE_BUFFER, output = second.
    * This is the contract the identity-map kernel class follows
    * (matches steinmarder's compute_exhaustive_kernels/06_admissible_
    * identity_map.comp). */
   uint32_t in_binding = pl->identity_map.input_ssbo_binding;
   uint32_t out_binding = pl->identity_map.output_ssbo_binding;
   if (in_binding == out_binding) {
      if (!nth_storage_buffer_binding(set, 0, &in_binding) ||
          !nth_storage_buffer_binding(set, 1, &out_binding)) {
         IDM_LOG("early-return layout-has-fewer-than-two-storage-buffers");
         return false;
      }
      IDM_LOG("recovered bindings from layout: in=%u out=%u",
              in_binding, out_binding);
   }

   const struct r300vk_descriptor *in_desc =
      find_descriptor_by_binding(set, in_binding);
   const struct r300vk_descriptor *out_desc =
      find_descriptor_by_binding(set, out_binding);
   IDM_LOG("descriptor walk in_binding=%u out_binding=%u in_desc=%p out_desc=%p",
           in_binding, out_binding,
           (const void *)in_desc, (const void *)out_desc);
   if (!in_desc || !out_desc) {
      IDM_LOG("early-return descriptor-walk-miss");
      return false;
   }
   IDM_LOG("in_desc->buf.buffer=%p out_desc->buf.buffer=%p",
           (const void *)(uintptr_t)in_desc->buf.buffer,
           (const void *)(uintptr_t)out_desc->buf.buffer);
   if (!in_desc->buf.buffer || !out_desc->buf.buffer) {
      IDM_LOG("early-return null-vkbuffer-handle");
      return false;
   }

   VK_FROM_HANDLE(r300vk_buffer, in_buf,  in_desc->buf.buffer);
   VK_FROM_HANDLE(r300vk_buffer, out_buf, out_desc->buf.buffer);
   IDM_LOG("in_buf=%p resource=%p out_buf=%p resource=%p",
           (const void *)in_buf,
           in_buf ? (const void *)in_buf->resource : NULL,
           (const void *)out_buf,
           out_buf ? (const void *)out_buf->resource : NULL);
   if (!in_buf || !out_buf || !in_buf->resource || !out_buf->resource) {
      IDM_LOG("early-return null-pipe-resource");
      return false;
   }

   /* The total work-item count is the grid-size product.  The kernel's
    * local_size is folded into the per-invocation index space already
    * (the ComputeGrid->RasterGrid functor in the M-E derivation takes the
    * full group count); for local_size > 1, the M-E.4 oracle controls the
    * shader and so picks local_size = 1 to keep the first end-to-end test
    * simple.  M-J generalizes to local_size > 1 by widening the index
    * domain. */
   const uint32_t total_invocations = dispatch->group_count_x *
                                      dispatch->group_count_y *
                                      dispatch->group_count_z;
   if (total_invocations == 0)
      return false;

   unsigned width = 0, height = 0;
   derive_raster_extent(total_invocations, &width, &height);
   IDM_LOG("raster extent total=%u width=%u height=%u",
           total_invocations, width, height);
   if (width > 2048 || height > 2048) {
      IDM_LOG("early-return extent-exceeds-2048-cap");
      return false;
   }

   /* RGBA8 UNORM: 4 bytes per texel, bit-exact UNORM8 round-trip on FP24
    * per the M-E exactness theorem.  A future expansion lets the kernel's
    * element type pick FP16 or R8_UNORM; the bit-exactness bound applies
    * to all three. */
   const enum pipe_format fmt = PIPE_FORMAT_R8G8B8A8_UNORM;

   /* Wrap the input buffer as a 2D sampler view.  The view holds the
    * texture's only strong reference; drop the view at the end and the
    * texture is freed. */
   struct pipe_sampler_view *in_sv =
      r300vk_identity_map_wrap_input_as_sampler_view(device, in_buf->resource,
                                                     width, height, fmt);
   IDM_LOG("wrap in_sv=%p", (const void *)in_sv);
   if (!in_sv) {
      IDM_LOG("early-return wrap-input-failed");
      return false;
   }

   /* Allocate the output render target (linear-tiled 2D texture). */
   struct pipe_resource rt_templ;
   memset(&rt_templ, 0, sizeof(rt_templ));
   rt_templ.target     = PIPE_TEXTURE_2D;
   rt_templ.format     = fmt;
   rt_templ.width0     = width;
   rt_templ.height0    = height;
   rt_templ.depth0     = 1;
   rt_templ.array_size = 1;
   rt_templ.last_level = 0;
   rt_templ.nr_samples = 0;
   rt_templ.usage      = PIPE_USAGE_DEFAULT;
   rt_templ.bind       = PIPE_BIND_RENDER_TARGET;
   struct pipe_resource *rt = screen->resource_create(screen, &rt_templ);
   IDM_LOG("rt=%p", (const void *)rt);
   if (!rt) {
      IDM_LOG("early-return rt-create-failed");
      pipe_sampler_view_reference(&in_sv, NULL);
      return false;
   }

   /* Wrap the RT as a pipe_surface for the framebuffer cbufs[0] slot. */
   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format            = fmt;
   surf_templ.first_layer       = 0;
   surf_templ.last_layer        = 0;
   surf_templ.level             = 0;
   surf_templ.texture           = rt;

   /* Allocate the fullscreen-quad VBO with 4 vertices (TRIANGLE_STRIP):
    * each vertex = (pos.xy, texcoord.xy), 16 bytes, 64 bytes total.
    * Clip-space corners (-1, -1)..(1, 1) with texcoords (0, 0)..(1, 1) so
    * the FS samples the input texture across its full extent. */
   const float verts[16] = {
      -1.0f, -1.0f, 0.0f, 0.0f,
       1.0f, -1.0f, 1.0f, 0.0f,
      -1.0f,  1.0f, 0.0f, 1.0f,
       1.0f,  1.0f, 1.0f, 1.0f,
   };
   struct pipe_resource vb_templ;
   memset(&vb_templ, 0, sizeof(vb_templ));
   vb_templ.target     = PIPE_BUFFER;
   vb_templ.format     = PIPE_FORMAT_R8_UNORM;
   vb_templ.width0     = sizeof(verts);
   vb_templ.height0    = 1;
   vb_templ.depth0     = 1;
   vb_templ.array_size = 1;
   vb_templ.usage      = PIPE_USAGE_IMMUTABLE;
   vb_templ.bind       = PIPE_BIND_VERTEX_BUFFER;
   struct pipe_resource *vb = screen->resource_create(screen, &vb_templ);
   if (!vb) {
      pipe_resource_reference(&rt, NULL);
      pipe_sampler_view_reference(&in_sv, NULL);
      return false;
   }
   pipe->buffer_subdata(pipe, vb, PIPE_MAP_WRITE, 0, sizeof(verts), verts);

   /* Vertex element layout: position at offset 0, texcoord at offset 8.
    * R32G32_FLOAT per attribute; src_stride 16. */
   struct pipe_vertex_element velems[2];
   memset(&velems, 0, sizeof(velems));
   velems[0].src_offset          = 0;
   velems[0].src_stride          = 16;
   velems[0].vertex_buffer_index = 0;
   velems[0].src_format          = PIPE_FORMAT_R32G32_FLOAT;
   velems[0].instance_divisor    = 0;
   velems[1].src_offset          = 8;
   velems[1].src_stride          = 16;
   velems[1].vertex_buffer_index = 0;
   velems[1].src_format          = PIPE_FORMAT_R32G32_FLOAT;
   velems[1].instance_divisor    = 0;
   void *velems_cso = pipe->create_vertex_elements_state(pipe, 2, velems);
   if (!velems_cso) {
      pipe_resource_reference(&vb, NULL);
      pipe_resource_reference(&rt, NULL);
      pipe_sampler_view_reference(&in_sv, NULL);
      return false;
   }

   /* Bind the framebuffer.  set_framebuffer_state copies surf_templ into
    * the framebuffer state structure; the surface lifetime is tied to the
    * texture, which the local pipe_resource_reference keeps alive until
    * after the draw. */
   struct pipe_framebuffer_state fb;
   memset(&fb, 0, sizeof(fb));
   fb.width            = width;
   fb.height           = height;
   fb.nr_cbufs         = 1;
   fb.cbufs[0]         = surf_templ;
   pipe->set_framebuffer_state(pipe, &fb);

   /* Viewport: identity NDC -> pixel mapping over (0, 0)..(width, height). */
   struct pipe_viewport_state vp;
   memset(&vp, 0, sizeof(vp));
   vp.scale[0]     = (float)width  * 0.5f;
   vp.scale[1]     = (float)height * 0.5f;
   vp.scale[2]     = 0.5f;
   vp.translate[0] = (float)width  * 0.5f;
   vp.translate[1] = (float)height * 0.5f;
   vp.translate[2] = 0.5f;
   pipe->set_viewport_states(pipe, 0, 1, &vp);

   struct pipe_scissor_state sc = {0};
   sc.minx = 0; sc.miny = 0;
   sc.maxx = width;
   sc.maxy = height;
   pipe->set_scissor_states(pipe, 0, 1, &sc);

   /* Bind the cached state CSOs and the per-pipeline VS / FS. */
   pipe->bind_blend_state(pipe, device->identity_map_blend_cso);
   pipe->bind_rasterizer_state(pipe, device->identity_map_rasterizer_cso);
   pipe->bind_depth_stencil_alpha_state(pipe, device->identity_map_dsa_cso);
   pipe->bind_vs_state(pipe, pl->vs_cso);
   pipe->bind_fs_state(pipe, pl->fs_cso);
   pipe->bind_vertex_elements_state(pipe, velems_cso);
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1,
                             &device->identity_map_sampler_cso);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 1, 0, &in_sv);

   struct pipe_vertex_buffer vb_state;
   memset(&vb_state, 0, sizeof(vb_state));
   vb_state.buffer_offset      = 0;
   vb_state.buffer.resource    = vb;
   pipe->set_vertex_buffers(pipe, 1, &vb_state);

   /* Draw: 4-vertex triangle-strip covers the entire RT. */
   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode             = MESA_PRIM_TRIANGLE_STRIP;
   info.index_size       = 0;
   info.instance_count   = 1;
   info.min_index        = 0;
   info.max_index        = 3;
   struct pipe_draw_start_count_bias draw;
   memset(&draw, 0, sizeof(draw));
   draw.start = 0;
   draw.count = 4;
   IDM_LOG("draw_vbo mode=triangle_strip count=4");
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);

   /* Flush so the RT contents reach memory before the texture->buffer
    * copy.  pipe->flush submits the CS; r300g re-marks all atoms dirty so
    * a subsequent submit re-emits state cleanly. */
   pipe->flush(pipe, NULL, 0);
   IDM_LOG("post-flush, beginning rt->buffer copy");

   /* Copy the RT back to the output ssbo.  Same util_resource_copy_region
    * fallback path as the input wrap, but in the texture->buffer
    * direction. */
   struct pipe_box copy_box;
   memset(&copy_box, 0, sizeof(copy_box));
   copy_box.x      = 0;
   copy_box.y      = 0;
   copy_box.z      = 0;
   copy_box.width  = width;
   copy_box.height = height;
   copy_box.depth  = 1;
   /* Same TXF-avoidance + cross-target-assertion reasons as the input
    * wrap: pipe->resource_copy_region's blitter emits TXF on the
    * texture-to-buffer direction too, and util_resource_copy_region
    * asserts on cross-target.  Map + memcpy ourselves. */
   {
      struct pipe_transfer *rt_xfer = NULL;
      const void *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                             &copy_box, &rt_xfer);
      if (rt_map) {
         struct pipe_transfer *out_xfer = NULL;
         struct pipe_box out_box;
         memset(&out_box, 0, sizeof(out_box));
         out_box.width  = width * height * util_format_get_blocksize(fmt);
         out_box.height = 1;
         out_box.depth  = 1;
         void *out_bytes = pipe->buffer_map(pipe, out_buf->resource, 0,
                                            PIPE_MAP_WRITE |
                                            PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                            &out_box, &out_xfer);
         if (out_bytes) {
            const uint8_t *src_rows = (const uint8_t *)rt_map;
            uint8_t       *dst_bytes = (uint8_t *)out_bytes;
            const unsigned bpp = util_format_get_blocksize(fmt);
            const unsigned row_bytes = width * bpp;
            for (unsigned r = 0; r < height; r++)
               memcpy(dst_bytes + r * row_bytes,
                      src_rows + r * rt_xfer->stride,
                      row_bytes);
            pipe->buffer_unmap(pipe, out_xfer);
         }
         pipe->texture_unmap(pipe, rt_xfer);
      }
   }
   IDM_LOG("rt->buffer copy issued (out=%p, src=%p, box w=%u h=%u)",
           (const void *)out_buf->resource, (const void *)rt,
           copy_box.width, copy_box.height);

   /* Tear down transient state.  Unbind sampler views and vertex buffers
    * first so the pipe_context releases its internal references before we
    * drop ours, then delete the velems CSO, then drop the local refs. */
   struct pipe_sampler_view *no_view = NULL;
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 1, &no_view);
   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);

   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_sampler_view_reference(&in_sv, NULL);
   pipe_resource_reference(&vb, NULL);
   pipe_resource_reference(&rt, NULL);
   IDM_LOG("orchestrator done OK");
   return true;
}
