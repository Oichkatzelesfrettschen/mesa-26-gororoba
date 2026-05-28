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
   /* Only kernels binding their ssbos at Vulkan set slot 0 are supported by
    * the M-E lowering today; binds->first_set is the slot the recorded
    * sets[] array starts at, so a non-zero first_set means binds->sets[0]
    * is for slot first_set, not slot 0, and the orchestrator would resolve
    * the wrong set if it indexed [0] blindly.  Multi-set support is M-J
    * generalization. */
   if (binds->first_set != 0) {
      IDM_LOG("early-return first_set=%u (only slot 0 supported)",
              binds->first_set);
      return false;
   }
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
   /* 64-bit product so 2^32-wrapping group counts cannot smuggle a
    * small non-zero total past the zero-check.  The 2048*2048 axis cap
    * is enforced after derive_raster_extent below; here we only need to
    * keep the multiplication exact. */
   const uint64_t total_invocations =
      (uint64_t)dispatch->group_count_x *
      (uint64_t)dispatch->group_count_y *
      (uint64_t)dispatch->group_count_z;
   if (total_invocations == 0 || total_invocations > 2048u * 2048u) {
      IDM_LOG("early-return total_invocations=%llu out-of-bounds",
              (unsigned long long)total_invocations);
      return false;
   }

   unsigned width = 0, height = 0;
   /* total_invocations was 64-bit for overflow safety; the 2048*2048 ceiling
    * check above guarantees it fits in 32 bits at this point. */
   derive_raster_extent((uint32_t)total_invocations, &width, &height);
   IDM_LOG("raster extent total=%llu width=%u height=%u",
           (unsigned long long)total_invocations, width, height);
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
   /* PIPE_USAGE_STREAM: written once by buffer_subdata right after create,
    * read by the GPU on the draw, then released.  IMMUTABLE would be a
    * semantic lie -- the buffer IS written after create -- and only worked
    * accidentally because r300_buffer_create lands every vertex buffer in
    * GTT regardless of usage. */
   vb_templ.usage      = PIPE_USAGE_STREAM;
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

/* M-F binary-map orchestrator: same shape as
 * r300vk_identity_map_dispatch_replay above but with two input ssbos
 * wrapped as separate sampler views and bound at fragment-stage stages
 * 0 + 1.  The synthesised FS (r300vk_synthesize_binary_map_fs in
 * r300vk_pipeline.c) reads both samplers, applies the detected ALU op,
 * and writes via RB3D color export -- the rest of the pipeline state
 * (blend / raster / dsa / sampler / VBO / framebuffer / viewport /
 * scissor) is identical to the identity-map path. */
bool
r300vk_binary_map_dispatch_replay(struct r300vk_device *device,
                                  const struct r300vk_pipeline *pl,
                                  const struct r300vk_cmd_dispatch *dispatch,
                                  const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   IDM_LOG("bin_map entry pl=%p is_binary_map=%d alu_op=%u "
           "set_count=%u gx=%u gy=%u gz=%u",
           (const void *)pl,
           pl ? (int)pl->binary_map.is_binary_map : -1,
           pl ? (unsigned)pl->binary_map.alu_op : 0,
           binds ? binds->set_count : 0,
           dispatch ? dispatch->group_count_x : 0,
           dispatch ? dispatch->group_count_y : 0,
           dispatch ? dispatch->group_count_z : 0);
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0) {
      IDM_LOG("bin_map early-return null-or-empty-binds");
      return false;
   }
   if (!pl->vs_cso || !pl->fs_cso) {
      IDM_LOG("bin_map early-return no-vs-or-fs-cso");
      return false;
   }
   if (binds->first_set != 0) {
      IDM_LOG("bin_map early-return first_set=%u (only slot 0)",
              binds->first_set);
      return false;
   }
   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("bin_map early-return no-set-or-layout");
      return false;
   }

   /* Binding-index resolution.  Two sources, in priority order:
    *  (1) The M-F.1 detector reads the constant binding sources from
    *      the kernel's load_ssbo / store_ssbo intrinsics (see
    *      r300_compute_admission.c r300_nir_detect_binary_map: it
    *      sets binmap->{input_a,input_b,output}_ssbo_binding when
    *      nir_src_is_const returns true for each binding source).
    *      When the kernel's NIR retains constant binding sources --
    *      the common case for a GLSL kernel with explicit
    *      layout(binding=N) qualifiers -- the captured indices are
    *      authoritative and the orchestrator MUST use them so a
    *      non-commutative ALU op (SUB, DIV, MOD, SHL, SHR) gets its
    *      operand order right for any binding declaration order.
    *  (2) When all three captured indices are zero (Vulkan forbids
    *      duplicate bindings within a set, so all-zero means the
    *      detector saw opaque post-explicit_io handles instead of
    *      constants), fall back to positional layout iteration:
    *      input_a = 1st STORAGE_BUFFER, input_b = 2nd, output = 3rd.
    *      This preserves the compute_exhaustive_kernels/
    *      07_admissible_binary_map_*.comp probe's pre-explicit_io
    *      path. */
   uint32_t in_a_binding = pl->binary_map.input_a_ssbo_binding;
   uint32_t in_b_binding = pl->binary_map.input_b_ssbo_binding;
   uint32_t out_binding  = pl->binary_map.output_ssbo_binding;
   const bool detector_captured = (in_a_binding != 0 || in_b_binding != 0 ||
                                   out_binding  != 0);
   if (!detector_captured) {
      if (!nth_storage_buffer_binding(set, 0, &in_a_binding) ||
          !nth_storage_buffer_binding(set, 1, &in_b_binding) ||
          !nth_storage_buffer_binding(set, 2, &out_binding)) {
         IDM_LOG("bin_map early-return layout-has-fewer-than-three-storage-buffers");
         return false;
      }
   }
   IDM_LOG("bin_map bindings: in_a=%u in_b=%u out=%u source=%s",
           in_a_binding, in_b_binding, out_binding,
           detector_captured ? "detector" : "positional");

   const struct r300vk_descriptor *desc_in_a =
      find_descriptor_by_binding(set, in_a_binding);
   const struct r300vk_descriptor *desc_in_b =
      find_descriptor_by_binding(set, in_b_binding);
   const struct r300vk_descriptor *desc_out =
      find_descriptor_by_binding(set, out_binding);
   if (!desc_in_a || !desc_in_b || !desc_out) {
      IDM_LOG("bin_map early-return descriptor-walk-miss");
      return false;
   }
   if (!desc_in_a->buf.buffer || !desc_in_b->buf.buffer ||
       !desc_out->buf.buffer) {
      IDM_LOG("bin_map early-return null-vkbuffer-handle");
      return false;
   }
   VK_FROM_HANDLE(r300vk_buffer, buf_in_a, desc_in_a->buf.buffer);
   VK_FROM_HANDLE(r300vk_buffer, buf_in_b, desc_in_b->buf.buffer);
   VK_FROM_HANDLE(r300vk_buffer, buf_out,  desc_out->buf.buffer);
   if (!buf_in_a || !buf_in_b || !buf_out ||
       !buf_in_a->resource || !buf_in_b->resource || !buf_out->resource) {
      IDM_LOG("bin_map early-return null-pipe-resource");
      return false;
   }

   const uint64_t total_invocations =
      (uint64_t)dispatch->group_count_x *
      (uint64_t)dispatch->group_count_y *
      (uint64_t)dispatch->group_count_z;
   if (total_invocations == 0 || total_invocations > 2048u * 2048u) {
      IDM_LOG("bin_map early-return total_invocations=%llu out-of-bounds",
              (unsigned long long)total_invocations);
      return false;
   }
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total_invocations, &width, &height);
   if (width > 2048 || height > 2048) {
      IDM_LOG("bin_map early-return extent-exceeds-2048-cap");
      return false;
   }

   const enum pipe_format fmt = PIPE_FORMAT_R8G8B8A8_UNORM;
   struct pipe_sampler_view *sv_a =
      r300vk_identity_map_wrap_input_as_sampler_view(device, buf_in_a->resource,
                                                     width, height, fmt);
   if (!sv_a) {
      IDM_LOG("bin_map early-return wrap-input-a-failed");
      return false;
   }
   struct pipe_sampler_view *sv_b =
      r300vk_identity_map_wrap_input_as_sampler_view(device, buf_in_b->resource,
                                                     width, height, fmt);
   if (!sv_b) {
      pipe_sampler_view_reference(&sv_a, NULL);
      IDM_LOG("bin_map early-return wrap-input-b-failed");
      return false;
   }
   IDM_LOG("bin_map wrap sv_a=%p sv_b=%p", (const void *)sv_a, (const void *)sv_b);

   /* Output RT: identical to identity-map. */
   struct pipe_resource rt_templ;
   memset(&rt_templ, 0, sizeof(rt_templ));
   rt_templ.target     = PIPE_TEXTURE_2D;
   rt_templ.format     = fmt;
   rt_templ.width0     = width;
   rt_templ.height0    = height;
   rt_templ.depth0     = 1;
   rt_templ.array_size = 1;
   rt_templ.usage      = PIPE_USAGE_DEFAULT;
   rt_templ.bind       = PIPE_BIND_RENDER_TARGET;
   struct pipe_resource *rt = screen->resource_create(screen, &rt_templ);
   if (!rt) {
      IDM_LOG("bin_map early-return rt-create-failed");
      pipe_sampler_view_reference(&sv_b, NULL);
      pipe_sampler_view_reference(&sv_a, NULL);
      return false;
   }

   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = fmt;
   surf_templ.texture = rt;

   /* Fullscreen quad VBO + velems: identical to identity-map. */
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
   vb_templ.usage      = PIPE_USAGE_STREAM;
   vb_templ.bind       = PIPE_BIND_VERTEX_BUFFER;
   struct pipe_resource *vb = screen->resource_create(screen, &vb_templ);
   if (!vb) {
      pipe_resource_reference(&rt, NULL);
      pipe_sampler_view_reference(&sv_b, NULL);
      pipe_sampler_view_reference(&sv_a, NULL);
      return false;
   }
   pipe->buffer_subdata(pipe, vb, PIPE_MAP_WRITE, 0, sizeof(verts), verts);

   struct pipe_vertex_element velems[2];
   memset(&velems, 0, sizeof(velems));
   velems[0].src_offset = 0; velems[0].src_stride = 16;
   velems[0].vertex_buffer_index = 0;
   velems[0].src_format = PIPE_FORMAT_R32G32_FLOAT;
   velems[1].src_offset = 8; velems[1].src_stride = 16;
   velems[1].vertex_buffer_index = 0;
   velems[1].src_format = PIPE_FORMAT_R32G32_FLOAT;
   void *velems_cso = pipe->create_vertex_elements_state(pipe, 2, velems);
   if (!velems_cso) {
      pipe_resource_reference(&vb, NULL);
      pipe_resource_reference(&rt, NULL);
      pipe_sampler_view_reference(&sv_b, NULL);
      pipe_sampler_view_reference(&sv_a, NULL);
      return false;
   }

   struct pipe_framebuffer_state fb;
   memset(&fb, 0, sizeof(fb));
   fb.width = width;  fb.height = height;
   fb.nr_cbufs = 1;   fb.cbufs[0] = surf_templ;
   pipe->set_framebuffer_state(pipe, &fb);

   struct pipe_viewport_state vp;
   memset(&vp, 0, sizeof(vp));
   vp.scale[0] = (float)width  * 0.5f; vp.scale[1] = (float)height * 0.5f;
   vp.scale[2] = 0.5f;
   vp.translate[0] = (float)width * 0.5f; vp.translate[1] = (float)height * 0.5f;
   vp.translate[2] = 0.5f;
   pipe->set_viewport_states(pipe, 0, 1, &vp);

   struct pipe_scissor_state sc = {0};
   sc.maxx = width; sc.maxy = height;
   pipe->set_scissor_states(pipe, 0, 1, &sc);

   /* Bind state + two sampler stages -- this is the M-F-specific change. */
   pipe->bind_blend_state(pipe, device->identity_map_blend_cso);
   pipe->bind_rasterizer_state(pipe, device->identity_map_rasterizer_cso);
   pipe->bind_depth_stencil_alpha_state(pipe, device->identity_map_dsa_cso);
   pipe->bind_vs_state(pipe, pl->vs_cso);
   pipe->bind_fs_state(pipe, pl->fs_cso);
   pipe->bind_vertex_elements_state(pipe, velems_cso);
   void *samplers[2] = { device->identity_map_sampler_cso,
                         device->identity_map_sampler_cso };
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 2, samplers);
   struct pipe_sampler_view *views[2] = { sv_a, sv_b };
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 2, 0, views);

   struct pipe_vertex_buffer vb_state;
   memset(&vb_state, 0, sizeof(vb_state));
   vb_state.buffer.resource = vb;
   pipe->set_vertex_buffers(pipe, 1, &vb_state);

   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode           = MESA_PRIM_TRIANGLE_STRIP;
   info.instance_count = 1;
   info.max_index      = 3;
   struct pipe_draw_start_count_bias draw = { .start = 0, .count = 4 };
   IDM_LOG("bin_map draw_vbo mode=triangle_strip count=4");
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   pipe->flush(pipe, NULL, 0);
   IDM_LOG("bin_map post-flush, beginning rt->buffer copy");

   struct pipe_box copy_box;
   memset(&copy_box, 0, sizeof(copy_box));
   copy_box.width = width; copy_box.height = height; copy_box.depth = 1;
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
         void *out_bytes = pipe->buffer_map(pipe, buf_out->resource, 0,
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
   IDM_LOG("bin_map rt->buffer copy issued (out=%p, src=%p, box w=%u h=%u)",
           (const void *)buf_out->resource, (const void *)rt,
           copy_box.width, copy_box.height);

   /* Tear down two sampler stages then the rest. */
   struct pipe_sampler_view *no_views[2] = { NULL, NULL };
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 2, no_views);
   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);

   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_sampler_view_reference(&sv_b, NULL);
   pipe_sampler_view_reference(&sv_a, NULL);
   pipe_resource_reference(&vb, NULL);
   pipe_resource_reference(&rt, NULL);
   IDM_LOG("bin_map orchestrator done OK");
   return true;
}

/* M-G blend-acc-reduction orchestrator.  Decomposes
 * `atomicAdd(out[gid & MASK], in[gid])` into N point fragments accumulating
 * the per-gid value into M bins through RB3D `COMB_FCN_ADD` blending.
 *
 * Shape differs from binary_map_dispatch_replay in three load-bearing
 * places (named at each site below):
 *
 *   1. Output RT extent is 1 x M (M = out_buf->size / 4 = histogram bin
 *      count), not the W x H matching the input texture.
 *   2. VBO carries N entries each (vec2 pos_ndc, uint32_t value_rgba8),
 *      with the input buffer's per-gid uint32 value PRE-STAGED at
 *      orchestrator time via pipe->buffer_map.  The draw is N point
 *      primitives (MESA_PRIM_POINTS) instead of a 4-vertex
 *      TRIANGLE_STRIP.
 *   3. Blend state is the device-cached
 *      blend_acc_reduction_blend_cso (ADD / ONE / ONE) instead of the
 *      blend-disabled identity_map_blend_cso.
 *
 * Other surfaces (rasterizer / dsa / sampler CSOs, framebuffer + viewport
 * + scissor setup, copy-back path) reuse the identity-map orchestrator's
 * shape verbatim. */
bool
r300vk_blend_acc_reduction_dispatch_replay(struct r300vk_device *device,
                                           const struct r300vk_pipeline *pl,
                                           const struct r300vk_cmd_dispatch *dispatch,
                                           const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   IDM_LOG("blend_acc entry pl=%p is_blend_acc=%d set_count=%u gx=%u gy=%u gz=%u",
           (const void *)pl,
           pl ? (int)pl->blend_acc_reduction.is_blend_acc_reduction : -1,
           binds ? binds->set_count : 0,
           dispatch ? dispatch->group_count_x : 0,
           dispatch ? dispatch->group_count_y : 0,
           dispatch ? dispatch->group_count_z : 0);
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0) {
      IDM_LOG("blend_acc early-return null-or-empty-binds");
      return false;
   }
   if (!pl->vs_cso || !pl->fs_cso) {
      IDM_LOG("blend_acc early-return no-vs-or-fs-cso");
      return false;
   }
   if (binds->first_set != 0) {
      IDM_LOG("blend_acc early-return first_set=%u (only slot 0 supported)",
              binds->first_set);
      return false;
   }
   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("blend_acc early-return no-set-or-layout");
      return false;
   }

   /* Detector binding-index priority + positional fallback (same policy
    * as M-F.3 binary_map_dispatch_replay, mesa #295). */
   uint32_t in_binding  = pl->blend_acc_reduction.value_ssbo_binding;
   uint32_t out_binding = pl->blend_acc_reduction.output_ssbo_binding;
   const bool detector_captured = (in_binding != 0 || out_binding != 0);
   if (!detector_captured) {
      if (!nth_storage_buffer_binding(set, 0, &in_binding) ||
          !nth_storage_buffer_binding(set, 1, &out_binding)) {
         IDM_LOG("blend_acc early-return layout-has-fewer-than-two-storage-buffers");
         return false;
      }
   }
   IDM_LOG("blend_acc bindings: in=%u out=%u source=%s",
           in_binding, out_binding,
           detector_captured ? "detector" : "positional");

   const struct r300vk_descriptor *in_desc =
      find_descriptor_by_binding(set, in_binding);
   const struct r300vk_descriptor *out_desc =
      find_descriptor_by_binding(set, out_binding);
   if (!in_desc || !out_desc || !in_desc->buf.buffer || !out_desc->buf.buffer) {
      IDM_LOG("blend_acc early-return descriptor-walk-miss");
      return false;
   }
   VK_FROM_HANDLE(r300vk_buffer, in_buf,  in_desc->buf.buffer);
   VK_FROM_HANDLE(r300vk_buffer, out_buf, out_desc->buf.buffer);
   if (!in_buf || !out_buf || !in_buf->resource || !out_buf->resource) {
      IDM_LOG("blend_acc early-return null-pipe-resource");
      return false;
   }

   /* Difference 1: output RT extent is 1 x M.  M = histogram bin count,
    * derived from the output buffer size (each bin holds one uint32). */
   const uint64_t out_byte_size = out_buf->size;
   if (out_byte_size == 0 || (out_byte_size % sizeof(uint32_t)) != 0) {
      IDM_LOG("blend_acc early-return malformed-output-size=%llu",
              (unsigned long long)out_byte_size);
      return false;
   }
   const uint32_t M = (uint32_t)(out_byte_size / sizeof(uint32_t));
   if (M == 0 || M > 2048) {
      IDM_LOG("blend_acc early-return M-out-of-range=%u", M);
      return false;
   }
   /* Total invocations from the dispatch grid (M-E numeric envelope:
    * 64-bit product guard + 2048 x 2048 axis cap). */
   const uint64_t total_invocations =
      (uint64_t)dispatch->group_count_x *
      (uint64_t)dispatch->group_count_y *
      (uint64_t)dispatch->group_count_z;
   if (total_invocations == 0 || total_invocations > 2048u * 2048u) {
      IDM_LOG("blend_acc early-return total_invocations=%llu out-of-bounds",
              (unsigned long long)total_invocations);
      return false;
   }
   const uint32_t N = (uint32_t)total_invocations;
   IDM_LOG("blend_acc M=%u N=%u", M, N);

   const enum pipe_format fmt = PIPE_FORMAT_R8G8B8A8_UNORM;

   /* Difference 1 (cont): 1 x M RT instead of W x H. */
   struct pipe_resource rt_templ;
   memset(&rt_templ, 0, sizeof(rt_templ));
   rt_templ.target     = PIPE_TEXTURE_2D;
   rt_templ.format     = fmt;
   rt_templ.width0     = M;
   rt_templ.height0    = 1;
   rt_templ.depth0     = 1;
   rt_templ.array_size = 1;
   rt_templ.usage      = PIPE_USAGE_DEFAULT;
   rt_templ.bind       = PIPE_BIND_RENDER_TARGET;
   struct pipe_resource *rt = screen->resource_create(screen, &rt_templ);
   if (!rt) {
      IDM_LOG("blend_acc early-return rt-create-failed");
      return false;
   }

   /* Difference 2: VBO carries N entries (vec2 pos_ndc, uint32 rgba8 value).
    * Per-entry stride = 12 bytes.  Stage the per-gid input value from
    * in_buf->resource at orchestrator time so the FS receives the value
    * via the rasterizer interpolator without needing a TEX op. */
   const uint32_t vbo_stride = 12u;
   const uint64_t vbo_bytes  = (uint64_t)N * vbo_stride;
   struct pipe_resource vb_templ;
   memset(&vb_templ, 0, sizeof(vb_templ));
   vb_templ.target     = PIPE_BUFFER;
   vb_templ.format     = PIPE_FORMAT_R8_UNORM;
   vb_templ.width0     = (unsigned)vbo_bytes;
   vb_templ.height0    = 1;
   vb_templ.depth0     = 1;
   vb_templ.array_size = 1;
   vb_templ.usage      = PIPE_USAGE_STREAM;
   vb_templ.bind       = PIPE_BIND_VERTEX_BUFFER;
   struct pipe_resource *vb = screen->resource_create(screen, &vb_templ);
   if (!vb) {
      pipe_resource_reference(&rt, NULL);
      IDM_LOG("blend_acc early-return vbo-create-failed");
      return false;
   }

   /* CPU-stage the VBO from the input buffer.  Each gid contributes one
    * point primitive at NDC position (bin_to_ndc(gid & (M-1)), 0) with the
    * input value packed as RGBA8.  The bin mask is derived from M-1 (only
    * power-of-2 M is supported in this first cut; non-power-of-2 M falls
    * through with the current bin = gid % M scalar arithmetic). */
   const uint32_t bin_mask = (M > 0 && (M & (M - 1)) == 0) ? (M - 1) : 0;
   const bool power_of_two_M = (bin_mask != 0);
   {
      struct pipe_transfer *in_xfer = NULL;
      struct pipe_box in_box;
      memset(&in_box, 0, sizeof(in_box));
      in_box.width  = (unsigned)(N * sizeof(uint32_t));
      in_box.height = 1; in_box.depth = 1;
      const void *in_map = pipe->buffer_map(pipe, in_buf->resource, 0,
                                            PIPE_MAP_READ, &in_box, &in_xfer);
      if (!in_map) {
         pipe_resource_reference(&vb, NULL);
         pipe_resource_reference(&rt, NULL);
         IDM_LOG("blend_acc early-return in-map-failed");
         return false;
      }
      struct pipe_transfer *vb_xfer = NULL;
      struct pipe_box vb_box;
      memset(&vb_box, 0, sizeof(vb_box));
      vb_box.width  = (unsigned)vbo_bytes;
      vb_box.height = 1; vb_box.depth = 1;
      void *vb_map = pipe->buffer_map(pipe, vb, 0,
                                      PIPE_MAP_WRITE |
                                      PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                      &vb_box, &vb_xfer);
      if (!vb_map) {
         pipe->buffer_unmap(pipe, in_xfer);
         pipe_resource_reference(&vb, NULL);
         pipe_resource_reference(&rt, NULL);
         IDM_LOG("blend_acc early-return vbo-map-failed");
         return false;
      }
      const uint32_t *in_words = (const uint32_t *)in_map;
      uint8_t        *vb_bytes = (uint8_t *)vb_map;
      const float inv_M = 2.0f / (float)M;  /* NDC step per bin */
      for (uint32_t gid = 0; gid < N; gid++) {
         const uint32_t bin = power_of_two_M ? (gid & bin_mask) : (gid % M);
         /* Bin center in NDC X: -1 + (bin + 0.5) * (2/M). */
         const float pos_x = -1.0f + ((float)bin + 0.5f) * inv_M;
         const float pos_y = 0.0f;
         uint8_t *e = vb_bytes + (size_t)gid * vbo_stride;
         memcpy(e + 0, &pos_x, 4);
         memcpy(e + 4, &pos_y, 4);
         memcpy(e + 8, &in_words[gid], 4);  /* packed RGBA8 value */
      }
      pipe->buffer_unmap(pipe, vb_xfer);
      pipe->buffer_unmap(pipe, in_xfer);
   }
   IDM_LOG("blend_acc VBO staged N=%u entries (%llu bytes)",
           N, (unsigned long long)vbo_bytes);

   struct pipe_vertex_element velems[2];
   memset(&velems, 0, sizeof(velems));
   velems[0].src_offset = 0; velems[0].src_stride = vbo_stride;
   velems[0].vertex_buffer_index = 0;
   velems[0].src_format = PIPE_FORMAT_R32G32_FLOAT;     /* position xy */
   velems[1].src_offset = 8; velems[1].src_stride = vbo_stride;
   velems[1].vertex_buffer_index = 0;
   velems[1].src_format = PIPE_FORMAT_R8G8B8A8_UNORM;   /* color (UNORM8) */
   void *velems_cso = pipe->create_vertex_elements_state(pipe, 2, velems);
   if (!velems_cso) {
      pipe_resource_reference(&vb, NULL);
      pipe_resource_reference(&rt, NULL);
      return false;
   }

   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = fmt;
   surf_templ.texture = rt;

   struct pipe_framebuffer_state fb;
   memset(&fb, 0, sizeof(fb));
   fb.width = M;  fb.height = 1;
   fb.nr_cbufs = 1; fb.cbufs[0] = surf_templ;
   pipe->set_framebuffer_state(pipe, &fb);

   struct pipe_viewport_state vp;
   memset(&vp, 0, sizeof(vp));
   vp.scale[0] = (float)M * 0.5f; vp.scale[1] = 0.5f;
   vp.scale[2] = 0.5f;
   vp.translate[0] = (float)M * 0.5f; vp.translate[1] = 0.5f;
   vp.translate[2] = 0.5f;
   pipe->set_viewport_states(pipe, 0, 1, &vp);

   struct pipe_scissor_state sc = {0};
   sc.maxx = M; sc.maxy = 1;
   pipe->set_scissor_states(pipe, 0, 1, &sc);

   /* Clear the 1xM RT to 0 before the blend-add draw.  resource_create
    * leaves the texture contents implementation-defined; for blend ADD
    * (dest + src) to produce the correct per-bin sum, dest MUST start at
    * 0.  Without this clear, an initial M-G.5 bundle (20260528T162452Z)
    * read every cell at exactly 3x the expected value (got=0x30303030
    * for expected=0x10101010), consistent with garbage dest contributing
    * an extra 2x the per-fragment value through the accumulation.  An
    * explicit pipe->clear with the COLOR0 mask zeroes the RT through the
    * RB3D fast-clear path before the per-point fragments accumulate. */
   {
      union pipe_color_union zero;
      memset(&zero, 0, sizeof(zero));
      /* pipe->clear signature (p_context.h:723): buffers + color_clear_mask
       * + stencil_clear_mask + scissor_state + color + depth + stencil. */
      pipe->clear(pipe, PIPE_CLEAR_COLOR0, ~0u, 0, NULL, &zero, 0.0, 0);
   }

   /* Difference 3: blend state = ADD/(ONE,ONE) instead of disabled. */
   if (!device->blend_acc_reduction_blend_cso) {
      IDM_LOG("blend_acc early-return no-cached-blend-cso");
      pipe->delete_vertex_elements_state(pipe, velems_cso);
      pipe_resource_reference(&vb, NULL);
      pipe_resource_reference(&rt, NULL);
      return false;
   }
   pipe->bind_blend_state(pipe, device->blend_acc_reduction_blend_cso);
   pipe->bind_rasterizer_state(pipe, device->identity_map_rasterizer_cso);
   pipe->bind_depth_stencil_alpha_state(pipe, device->identity_map_dsa_cso);
   pipe->bind_vs_state(pipe, pl->vs_cso);
   pipe->bind_fs_state(pipe, pl->fs_cso);
   pipe->bind_vertex_elements_state(pipe, velems_cso);

   struct pipe_vertex_buffer vb_state;
   memset(&vb_state, 0, sizeof(vb_state));
   vb_state.buffer.resource = vb;
   pipe->set_vertex_buffers(pipe, 1, &vb_state);

   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode           = MESA_PRIM_POINTS;
   info.instance_count = 1;
   info.max_index      = N - 1;
   struct pipe_draw_start_count_bias draw = { .start = 0, .count = N };
   IDM_LOG("blend_acc draw_vbo mode=points count=%u", N);
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   pipe->flush(pipe, NULL, 0);

   /* Copy the 1 x M RT back to the output buffer.  out_byte_size already
    * equals M * sizeof(uint32_t), so the row spans the whole output. */
   {
      struct pipe_box copy_box;
      memset(&copy_box, 0, sizeof(copy_box));
      copy_box.width = M; copy_box.height = 1; copy_box.depth = 1;
      struct pipe_transfer *rt_xfer = NULL;
      const void *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                             &copy_box, &rt_xfer);
      if (rt_map) {
         struct pipe_transfer *out_xfer = NULL;
         struct pipe_box out_box;
         memset(&out_box, 0, sizeof(out_box));
         out_box.width  = (unsigned)out_byte_size;
         out_box.height = 1; out_box.depth = 1;
         void *out_bytes = pipe->buffer_map(pipe, out_buf->resource, 0,
                                            PIPE_MAP_WRITE |
                                            PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                            &out_box, &out_xfer);
         if (out_bytes) {
            memcpy(out_bytes, rt_map, (size_t)out_byte_size);
            pipe->buffer_unmap(pipe, out_xfer);
         }
         pipe->texture_unmap(pipe, rt_xfer);
      }
   }
   IDM_LOG("blend_acc rt->buffer copy issued (out=%p, src=%p, M=%u)",
           (const void *)out_buf->resource, (const void *)rt, M);

   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);
   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_resource_reference(&vb, NULL);
   pipe_resource_reference(&rt, NULL);
   IDM_LOG("blend_acc orchestrator done OK");
   return true;
}
