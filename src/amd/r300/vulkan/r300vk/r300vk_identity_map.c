/*
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
 * the first call; subsequent calls hit the cached integer. */
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

static bool
r300vk_idm_exact_opt_in_enabled(const char *env_name, const char *expected)
{
   const char *gate = getenv(env_name);
   return gate && strcmp(gate, expected) == 0;
}

static bool
r300vk_idm_format_supported(struct pipe_screen *screen, enum pipe_format fmt)
{
   return screen &&
          screen->is_format_supported(screen, fmt, PIPE_TEXTURE_2D, 0, 0,
                                      PIPE_BIND_SAMPLER_VIEW) &&
          screen->is_format_supported(screen, fmt, PIPE_TEXTURE_2D, 0, 0,
                                      PIPE_BIND_RENDER_TARGET);
}

static enum pipe_format
r300vk_identity_map_replay_format(struct r300vk_device *device,
                                  const struct r300vk_pipeline *pl)
{
   /* The identity-map theorem only proves bit-exact transport for UNORM8/16
    * and FP16 through the TEX -> fragment-temp -> RT path.  Keep FP32x4 behind
    * an exact opt-in: the R2VB ARGB32323232 proof covers CB/VB transport, not
    * this sampled fragment path, so FP32x4 here is an exploration lane rather
    * than a default correctness claim.
    *
    * After nir_lower_explicit_io the load_ssbo/store_ssbo identity pair retains
    * the vec4x32 width but not a reliable scalar base type.  The gate therefore
    * keys on a 4x32 transport shape only; using it for non-float payloads is a
    * user hazard accepted explicitly through the opt-in. */
   if (device && pl &&
       r300vk_idm_exact_opt_in_enabled(R300VK_IDENTITY_MAP_FP32X4_ENV,
                                       R300VK_IDENTITY_MAP_FP32X4_ENV_VALUE) &&
       pl->identity_map.value_components == 4 &&
       pl->identity_map.value_bit_size == 32 &&
       r300vk_idm_format_supported(device->screen,
                                   PIPE_FORMAT_R32G32B32A32_FLOAT)) {
      IDM_LOG("using experimental fp32x4 identity carrier");
      return PIPE_FORMAT_R32G32B32A32_FLOAT;
   }

   return PIPE_FORMAT_R8G8B8A8_UNORM;
}

static void
r300vk_identity_map_copy_rows(void *dst_map, unsigned dst_stride,
                              const void *src_map, unsigned src_stride,
                              unsigned width, unsigned height,
                              unsigned bpp, uint64_t total_elements)
{
   const uint8_t *src_bytes = (const uint8_t *)src_map;
   uint8_t       *dst_bytes = (uint8_t *)dst_map;
   const unsigned row_bytes = width * bpp;
   uint64_t remaining = total_elements * bpp;
   for (unsigned r = 0; r < height && remaining > 0; r++) {
      const uint64_t copy_bytes = (remaining > row_bytes) ? row_bytes : remaining;
      memcpy(dst_bytes + r * dst_stride,
             src_bytes + r * src_stride,
             (size_t)copy_bytes);
      remaining -= copy_bytes;
   }
}


/* Defined later in this file; resolve_buffers calls it before that point. */
static const struct r300vk_descriptor *
find_descriptor_by_binding(const struct r300vk_descriptor_set *set,
                           uint32_t binding_index);

static bool
r300vk_idm_resolve_buffers(const struct r300vk_descriptor_set *set,
                           uint32_t count,
                           const uint32_t *bindings,
                           const struct r300vk_descriptor **descs,
                           struct r300vk_buffer **bufs)
{
   for (uint32_t i = 0; i < count; i++) {
      descs[i] = find_descriptor_by_binding(set, bindings[i]);
      if (!descs[i] || !descs[i]->buf.buffer) {
         IDM_LOG("early-return descriptor-walk-miss (binding=%u)", bindings[i]);
         return false;
      }
      VK_FROM_HANDLE(r300vk_buffer, buf, descs[i]->buf.buffer);
      if (!buf || !buf->resource) {
         IDM_LOG("early-return null-pipe-resource (binding=%u)", bindings[i]);
         return false;
      }
      bufs[i] = buf;
   }
   return true;
}


/* Global invocation count = workgroup count x local workgroup size, per axis.
 * The dispatch records group_count_{x,y,z}; the pipeline records the kernel's
 * local_size_{x,y,z} (its SPIR-V LocalSize execution mode).  A kernel indexes
 * gl_GlobalInvocationID over gl_NumWorkGroups * gl_WorkGroupSize, so the raster
 * substrate must emit one fragment per (group x local) invocation, not one per
 * workgroup -- a kernel with local_size_x=64 and group_count_x=4 has 256
 * invocations, and laying out only 4 fragments leaves elements 4..255 reading
 * zero.  A zero local_size means the SPIR-V omitted the LocalSize literal;
 * treat it as 1 so a degenerate pipeline maps to its group count rather than
 * collapsing the whole grid to zero invocations. */
static uint64_t
r300vk_idm_total_invocations(const struct r300vk_cmd_dispatch *dispatch,
                             const struct r300vk_pipeline *pl)
{
   const uint64_t lsx = pl->local_size_x ? pl->local_size_x : 1u;
   const uint64_t lsy = pl->local_size_y ? pl->local_size_y : 1u;
   const uint64_t lsz = pl->local_size_z ? pl->local_size_z : 1u;
   return (uint64_t)dispatch->group_count_x * lsx *
          (uint64_t)dispatch->group_count_y * lsy *
          (uint64_t)dispatch->group_count_z * lsz;
}

static bool
r300vk_idm_compute_raster_grid(const struct r300vk_cmd_dispatch *dispatch,
                               const struct r300vk_pipeline *pl,
                               uint64_t *out_invocations,
                               unsigned *out_width,
                               unsigned *out_height)
{
   const uint64_t total_invocations =
      r300vk_idm_total_invocations(dispatch, pl);
   if (total_invocations == 0 || total_invocations > 2048u * 2048u) {
      IDM_LOG("early-return total_invocations=%llu out-of-bounds",
              (unsigned long long)total_invocations);
      return false;
   }
   *out_invocations = total_invocations;

   unsigned width = 2048;
   unsigned height = (unsigned)((total_invocations + 2047) / 2048);
   if (total_invocations <= 2048) {
      width = (unsigned)total_invocations;
      height = 1;
   }
   *out_width = width;
   *out_height = height;
   return true;
}

static bool
r300vk_idm_create_blend_acc_vbo(struct pipe_context *pipe,
                                struct pipe_resource *in_buf,
                                unsigned in_offset,
                                uint32_t N, uint32_t M,
                                struct pipe_resource **out_vb,
                                void **out_velems_cso)
{
   struct pipe_screen *screen = pipe->screen;
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
   if (!vb)
      return false;

   const uint32_t bin_mask = (M > 0 && (M & (M - 1)) == 0) ? (M - 1) : 0;
   const bool power_of_two_M = (bin_mask != 0);
   struct pipe_transfer *in_xfer = NULL;
   struct pipe_box in_box;
   memset(&in_box, 0, sizeof(in_box));
   in_box.x      = in_offset;
   in_box.width  = (unsigned)(N * sizeof(uint32_t));
   in_box.height = 1; in_box.depth = 1;
   const void *in_map = pipe->buffer_map(pipe, in_buf, 0,
                                         PIPE_MAP_READ, &in_box, &in_xfer);
   if (!in_map) {
      pipe_resource_reference(&vb, NULL);
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
      return false;
   }
   const uint32_t *in_words = (const uint32_t *)in_map;
   uint8_t        *vb_bytes = (uint8_t *)vb_map;
   const float inv_M = 2.0f / (float)M;
   for (uint32_t gid = 0; gid < N; gid++) {
      const uint32_t bin = power_of_two_M ? (gid & bin_mask) : (gid % M);
      const float pos_x = -1.0f + ((float)bin + 0.5f) * inv_M;
      const float pos_y = 0.0f;
      uint8_t *e = vb_bytes + (size_t)gid * vbo_stride;
      memcpy(e + 0, &pos_x, 4);
      memcpy(e + 4, &pos_y, 4);
      memcpy(e + 8, &in_words[gid], 4);
   }
   pipe->buffer_unmap(pipe, vb_xfer);
   pipe->buffer_unmap(pipe, in_xfer);

   struct pipe_vertex_element velems[2];
   memset(&velems, 0, sizeof(velems));
   velems[0].src_offset = 0; velems[0].src_stride = vbo_stride;
   velems[0].vertex_buffer_index = 0;
   velems[0].src_format = PIPE_FORMAT_R32G32_FLOAT;
   velems[1].src_offset = 8; velems[1].src_stride = vbo_stride;
   velems[1].vertex_buffer_index = 0;
   velems[1].src_format = PIPE_FORMAT_R8G8B8A8_UNORM;
   void *velems_cso = pipe->create_vertex_elements_state(pipe, 2, velems);
   if (!velems_cso) {
      pipe_resource_reference(&vb, NULL);
      return false;
   }
   *out_vb = vb;
   *out_velems_cso = velems_cso;
   return true;
}

static bool
r300vk_idm_create_zpass_vbo(struct pipe_context *pipe,
                            struct pipe_resource *in_buf,
                            unsigned in_offset,
                            uint32_t N,
                            struct pipe_resource **out_vb,
                            void **out_velems_cso)
{
   struct pipe_screen *screen = pipe->screen;
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
   if (!vb)
      return false;

   struct pipe_transfer *in_xfer = NULL;
   struct pipe_box in_box;
   memset(&in_box, 0, sizeof(in_box));
   in_box.x      = in_offset;
   in_box.width  = (unsigned)(N * sizeof(uint32_t));
   in_box.height = 1; in_box.depth = 1;
   const void *in_map = pipe->buffer_map(pipe, in_buf, 0,
                                         PIPE_MAP_READ, &in_box, &in_xfer);
   if (!in_map) {
      pipe_resource_reference(&vb, NULL);
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
      return false;
   }
   const uint32_t *in_words = (const uint32_t *)in_map;
   uint8_t        *vb_bytes = (uint8_t *)vb_map;
   const float inv_N = 2.0f / (float)N;
   for (uint32_t gid = 0; gid < N; gid++) {
      const float pos_x = -1.0f + ((float)gid + 0.5f) * inv_N;
      const float pos_y = 0.0f;
      const float pred  = (in_words[gid] != 0u) ? 1.0f : 0.0f;
      uint8_t *e = vb_bytes + (size_t)gid * vbo_stride;
      memcpy(e + 0, &pos_x, 4);
      memcpy(e + 4, &pos_y, 4);
      memcpy(e + 8, &pred,  4);
   }
   pipe->buffer_unmap(pipe, vb_xfer);
   pipe->buffer_unmap(pipe, in_xfer);

   struct pipe_vertex_element velems[2];
   memset(&velems, 0, sizeof(velems));
   velems[0].src_offset = 0; velems[0].src_stride = vbo_stride;
   velems[0].vertex_buffer_index = 0;
   velems[0].src_format = PIPE_FORMAT_R32G32_FLOAT;
   velems[1].src_offset = 8; velems[1].src_stride = vbo_stride;
   velems[1].vertex_buffer_index = 0;
   velems[1].src_format = PIPE_FORMAT_R32_FLOAT;
   void *velems_cso = pipe->create_vertex_elements_state(pipe, 2, velems);
   if (!velems_cso) {
      pipe_resource_reference(&vb, NULL);
      return false;
   }
   *out_vb = vb;
   *out_velems_cso = velems_cso;
   return true;
}


static bool
r300vk_idm_create_fullscreen_vbo(struct pipe_context *pipe,
                                 struct pipe_resource **out_vb,
                                 void **out_velems_cso)
{
   struct pipe_screen *screen = pipe->screen;
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
   if (!vb)
      return false;
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
      return false;
   }
   *out_vb = vb;
   *out_velems_cso = velems_cso;
   return true;
}

static bool
r300vk_identity_map_readback_rt(struct pipe_context *pipe,
                                struct pipe_resource *rt,
                                struct pipe_resource *out_buf,
                                unsigned out_offset,
                                unsigned width, unsigned height,
                                enum pipe_format fmt,
                                unsigned copy_bytes_per_row)
{
   bool copy_ok = false;
   struct pipe_box copy_box;
   memset(&copy_box, 0, sizeof(copy_box));
   copy_box.width = width;
   copy_box.height = height;
   copy_box.depth = 1;
   struct pipe_transfer *rt_xfer = NULL;
   const void *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                          &copy_box, &rt_xfer);
   if (rt_map) {
      struct pipe_transfer *out_xfer = NULL;
      struct pipe_box out_box;
      memset(&out_box, 0, sizeof(out_box));
      out_box.x      = out_offset;
      out_box.width  = copy_bytes_per_row * height;
      out_box.height = 1; out_box.depth = 1;
      void *out_bytes = pipe->buffer_map(pipe, out_buf, 0,
                                         PIPE_MAP_WRITE |
                                         PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                         &out_box, &out_xfer);
      if (out_bytes) {
         r300vk_identity_map_copy_rows(out_bytes, copy_bytes_per_row * height,
                                       rt_map, rt_xfer->stride,
                                       width, height,
                                       util_format_get_blocksize(fmt),
                                       copy_bytes_per_row);
         pipe->buffer_unmap(pipe, out_xfer);
         copy_ok = true;
      }
      pipe->texture_unmap(pipe, rt_xfer);
   }
   return copy_ok;
}

static bool
r300vk_idm_validate_prologue(struct r300vk_device *device,
                             const struct r300vk_pipeline *pl,
                             const struct r300vk_cmd_dispatch *dispatch,
                             const struct r300vk_cmd_bind_descriptor_sets *binds,
                             const struct r300vk_descriptor_set **out_set)
{
   if (!device || !device->pipe || !device->screen || !pl || !dispatch || !binds || binds->set_count == 0)
      return false;
   if (!pl->vs_cso || !pl->fs_cso)
      return false;
   if (binds->first_set != 0)
      return false;
   *out_set = binds->sets[0];
   if (!(*out_set) || !(*out_set)->layout)
      return false;
   return true;
}

static bool
r300vk_idm_seed_texture_from_buffer(struct pipe_context *pipe,
                                    struct pipe_resource *in_buf,
                                    unsigned in_offset,
                                    unsigned width, unsigned height,
                                    enum pipe_format fmt,
                                    struct pipe_resource **out_tex,
                                    struct pipe_sampler_view **out_sv)
{
   struct pipe_screen *screen = pipe->screen;
   struct pipe_resource tex_templ;
   memset(&tex_templ, 0, sizeof(tex_templ));
   tex_templ.target     = PIPE_TEXTURE_2D;
   tex_templ.format     = fmt;
   tex_templ.width0     = width;
   tex_templ.height0    = height;
   tex_templ.depth0     = 1;
   tex_templ.array_size = 1;
   tex_templ.usage      = PIPE_USAGE_DEFAULT;
   tex_templ.bind       = PIPE_BIND_SAMPLER_VIEW;
   struct pipe_resource *tex = screen->resource_create(screen, &tex_templ);
   if (!tex)
      return false;

   struct pipe_transfer *in_xfer = NULL;
   struct pipe_box in_box;
   memset(&in_box, 0, sizeof(in_box));
   in_box.x      = in_offset;
   in_box.width  = width * height * util_format_get_blocksize(fmt);
   in_box.height = 1; in_box.depth = 1;
   const void *in_map = pipe->buffer_map(pipe, in_buf, 0,
                                         PIPE_MAP_READ, &in_box, &in_xfer);
   if (in_map) {
      struct pipe_transfer *tex_xfer = NULL;
      struct pipe_box tex_box;
      memset(&tex_box, 0, sizeof(tex_box));
      tex_box.width  = width;
      tex_box.height = height;
      tex_box.depth  = 1;
      void *tex_map = pipe->texture_map(pipe, tex, 0,
                                        PIPE_MAP_WRITE |
                                        PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                        &tex_box, &tex_xfer);
      if (tex_map) {
         r300vk_identity_map_copy_rows(tex_map, tex_xfer->stride,
                                       in_map, width * util_format_get_blocksize(fmt),
                                       width, height,
                                       util_format_get_blocksize(fmt),
                                       width * util_format_get_blocksize(fmt));
         pipe->texture_unmap(pipe, tex_xfer);
      }
      pipe->buffer_unmap(pipe, in_xfer);
   }

   struct pipe_sampler_view sv_templ;
   memset(&sv_templ, 0, sizeof(sv_templ));
   sv_templ.format             = fmt;
   sv_templ.target             = PIPE_TEXTURE_2D;
   sv_templ.swizzle_r          = PIPE_SWIZZLE_X;
   sv_templ.swizzle_g          = PIPE_SWIZZLE_Y;
   sv_templ.swizzle_b          = PIPE_SWIZZLE_Z;
   sv_templ.swizzle_a          = PIPE_SWIZZLE_W;
   *out_sv = pipe->create_sampler_view(pipe, tex, &sv_templ);
   *out_tex = tex;
   return true;
}

static void
r300vk_identity_map_setup_draw_state(struct pipe_context *pipe,
                                      unsigned width, unsigned height,
                                      struct pipe_surface *rt_surf,
                                      void *blend_cso, void *rs_cso,
                                      void *dsa_cso, void *vs_cso,
                                      void *fs_cso, void *velems_cso)
{
   struct pipe_framebuffer_state fb;
   memset(&fb, 0, sizeof(fb));
   fb.width            = width;
   fb.height           = height;
   fb.nr_cbufs         = 1;
   fb.cbufs[0]         = *rt_surf;
   pipe->set_framebuffer_state(pipe, &fb);

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
   sc.maxx = width;
   sc.maxy = height;
   pipe->set_scissor_states(pipe, 0, 1, &sc);

   pipe->bind_blend_state(pipe, blend_cso);
   pipe->bind_rasterizer_state(pipe, rs_cso);
   pipe->bind_depth_stencil_alpha_state(pipe, dsa_cso);
   pipe->bind_vs_state(pipe, vs_cso);
   pipe->bind_fs_state(pipe, fs_cso);
   pipe->bind_vertex_elements_state(pipe, velems_cso);
}

struct pipe_sampler_view *
r300vk_identity_map_wrap_input_as_sampler_view(struct r300vk_device *device,
                                               struct pipe_resource *src_buf,
                                               unsigned byte_offset,
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
   u_box_1d(byte_offset, width * height * bpp, &src_box);
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
   r300vk_identity_map_copy_rows(dst_map, dst_xfer->stride,
                                 src_map, width * bpp,
                                 width, height, bpp,
                                 (uint64_t)width * height);
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

/* Recover the input + output STORAGE_BUFFER bindings positionally when the
 * detector left both at 0.  After nir_lower_explicit_io the load_ssbo /
 * store_ssbo binding source is a Vulkan descriptor handle, not a constant, so
 * the detectors cannot record the binding indices and the pattern structs keep
 * their zero-initialized defaults.  Resolving {0,0} directly maps both the
 * input and the output descriptor to binding 0, so the kernel would read its
 * own output buffer as input.  The contract for the recovered shapes is a
 * single input/value storage buffer followed by a single output storage buffer,
 * so the first STORAGE_BUFFER is the input and the second is the output. */
static bool
idm_recover_in_out_bindings(const struct r300vk_descriptor_set *set,
                            uint32_t *in_binding, uint32_t *out_binding)
{
   if (*in_binding == *out_binding && *in_binding == 0) {
      if (!nth_storage_buffer_binding(set, 0, in_binding) ||
          !nth_storage_buffer_binding(set, 1, out_binding))
         return false;
   }
   return true;
}

/* Map the kernel's total invocation count onto a 2D raster grid: a single
 * row up to the texture-axis cap (R300 = 2048), then add rows as needed.
 * The bit-exact identity-map lowering bounds this at 2048 x 2048 per dispatch
 * (the R300 maximum 2D texture extent on each axis); larger grids would tile
 * and dispatch multiple times. */
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
    * the compute-as-raster lowering today; binds->first_set is the slot the
    * recorded sets[] array starts at, so a non-zero first_set means
    * binds->sets[0] is for slot first_set, not slot 0, and the orchestrator
    * would resolve the wrong set if it indexed [0] blindly.  Multi-set support
    * is a later generalization. */
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
    * This is the contract the identity-map kernel class follows: a single
    * input storage buffer and a single output storage buffer, declared in
    * that order. */
   uint32_t in_binding = pl->identity_map.input_ssbo_binding;
   uint32_t out_binding = pl->identity_map.output_ssbo_binding;
   if (in_binding == out_binding && in_binding == 0) {
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
    * local_size is folded into the per-invocation index space already (the
    * compute-grid-to-raster-grid mapping takes the full group count).  The
    * first cut fixes local_size = 1 to keep the readback oracle simple; a
    * later generalization widens the index domain to local_size > 1. */
   /* 64-bit product so 2^32-wrapping group counts cannot smuggle a
    * small non-zero total past the zero-check.  The 2048*2048 axis cap
    * is enforced after derive_raster_extent below; here we only need to
    * keep the multiplication exact. */
   const uint64_t total_invocations =
      r300vk_idm_total_invocations(dispatch, pl);
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

   /* Default carrier is RGBA8 UNORM because the FP24 identity theorem proves a
    * bit-exact round-trip there.  A float4x32 kernel may opt into the
    * experimental FP32x4 transport lane explicitly; that mode is capability-
    * checked and treated as exploratory transport, not as an exactness proof. */
   const enum pipe_format fmt = r300vk_identity_map_replay_format(device, pl);

   /* derive_raster_extent maps one invocation to one texel of
    * util_format_get_blocksize(fmt) bytes, so the carrier's element size must
    * equal the kernel's stored element size.  A vec4x32 store (16 bytes) on the
    * default RGBA8 carrier (4 bytes) would sample only the first quarter of each
    * element and leave the rest stale; the opt-in FP32x4 carrier (16 bytes)
    * matches and is selected by replay_format above.  Reject a mismatch rather
    * than transport a fraction of each element. */
   const unsigned element_bytes =
      pl->identity_map.value_components * (pl->identity_map.value_bit_size / 8u);
   if (element_bytes != 0 && util_format_get_blocksize(fmt) != element_bytes) {
      IDM_LOG("early-return carrier-blocksize=%u != element-bytes=%u",
              util_format_get_blocksize(fmt), element_bytes);
      return false;
   }

   /* Wrap the input buffer as a 2D sampler view.  The view holds the
    * texture's only strong reference; drop the view at the end and the
    * texture is freed. */
   struct pipe_sampler_view *in_sv =
      r300vk_identity_map_wrap_input_as_sampler_view(device, in_buf->resource,
                                                     (unsigned)in_desc->buf.offset,
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
   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r300vk_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_resource_reference(&rt, NULL);
      pipe_sampler_view_reference(&in_sv, NULL);
      return false;
   }

   r300vk_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_blend_cso,
                                        device->identity_map_rasterizer_cso,
                                        device->identity_map_dsa_cso,
                                        pl->vs_cso, pl->fs_cso, velems_cso);
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
   bool copy_ok = false;
   {
      struct pipe_transfer *rt_xfer = NULL;
      const void *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                             &copy_box, &rt_xfer);
      if (rt_map) {
         struct pipe_transfer *out_xfer = NULL;
         struct pipe_box out_box;
         memset(&out_box, 0, sizeof(out_box));
         out_box.x      = (unsigned)out_desc->buf.offset;
         out_box.width  = width * height * util_format_get_blocksize(fmt);
         out_box.height = 1;
         out_box.depth  = 1;
         void *out_bytes = pipe->buffer_map(pipe, out_buf->resource, 0,
                                            PIPE_MAP_WRITE |
                                            PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                            &out_box, &out_xfer);
         if (out_bytes) {
            r300vk_identity_map_copy_rows(out_bytes, width * util_format_get_blocksize(fmt),
                                          rt_map, rt_xfer->stride,
                                          width, height,
                                          util_format_get_blocksize(fmt),
                                          total_invocations);
            pipe->buffer_unmap(pipe, out_xfer);
            copy_ok = true;
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
   IDM_LOG("orchestrator done copy_ok=%d", (int)copy_ok);
   return copy_ok;
}

/* Binary-map orchestrator: same shape as r300vk_identity_map_dispatch_replay
 * above but with two input ssbos wrapped as separate sampler views and bound
 * at fragment-stage sampler stages 0 + 1.  The synthesised FS
 * (r300vk_synthesize_binary_map_fs in r300vk_pipeline.c) reads both samplers,
 * applies the detected ALU op, and writes via RB3D color export -- the rest of
 * the pipeline state (blend / raster / dsa / sampler / VBO / framebuffer /
 * viewport / scissor) is identical to the identity-map path. */
/* Shared 2-in / 1-out compute-as-raster replay core.  Wraps two input SSBOs as
 * sampler views at fragment stages 0 + 1, draws the fullscreen quad with the
 * pipeline's synthesized VS + FS (pl->fs_cso -- the binary-map ALU FS or the
 * DP4 dot FS), and copies the RB3D color export back to the output SSBO.  The
 * caller passes the three ssbo bindings its detector captured; binary-map and
 * dp4 are thin wrappers that differ only in which captured bindings they pass
 * and which FS pl->fs_cso already holds. */
static bool
r300vk_two_in_one_out_dispatch_replay(struct r300vk_device *device,
                                      const struct r300vk_pipeline *pl,
                                      const struct r300vk_cmd_dispatch *dispatch,
                                      const struct r300vk_cmd_bind_descriptor_sets *binds,
                                      uint32_t cap_in_a, uint32_t cap_in_b,
                                      uint32_t cap_out,
                                      enum pipe_format input_fmt,
                                      enum pipe_format output_fmt,
                                      enum pipe_format output_buffer_fmt)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   IDM_LOG("2in1out entry pl=%p cap_in_a=%u cap_in_b=%u cap_out=%u "
           "set_count=%u gx=%u gy=%u gz=%u",
           (const void *)pl, cap_in_a, cap_in_b, cap_out,
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
    *  (1) The binary-map detector reads the constant binding sources from
    *      the kernel's load_ssbo / store_ssbo intrinsics
    *      (r300_nir_detect_binary_map sets
    *      binmap->{input_a,input_b,output}_ssbo_binding when nir_src_is_const
    *      returns true for each binding source).  When the kernel's NIR
    *      retains constant binding sources -- the common case for a GLSL
    *      kernel with explicit layout(binding=N) qualifiers -- the captured
    *      indices are authoritative and the orchestrator uses them so a
    *      non-commutative ALU op (isub / fsub) gets its operand order right
    *      for any binding declaration order.
    *  (2) When all three captured indices are zero (Vulkan forbids duplicate
    *      bindings within a set, so all-zero means the detector saw opaque
    *      post-explicit_io handles instead of constants), fall back to
    *      positional layout iteration: input_a = 1st STORAGE_BUFFER,
    *      input_b = 2nd, output = 3rd. */
   uint32_t in_a_binding = cap_in_a;
   uint32_t in_b_binding = cap_in_b;
   uint32_t out_binding  = cap_out;
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
      r300vk_idm_total_invocations(dispatch, pl);
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

   /* input_fmt is the per-element sampler-view format (UNORM8 4B for binary-map,
    * R32G32B32A32_FLOAT 16B for the dp4 vec4 inputs); fmt = output_fmt is the RT
    * and copy-back element format (UNORM8 for both). */
   const enum pipe_format fmt = output_fmt;
   struct pipe_sampler_view *sv_a =
      r300vk_identity_map_wrap_input_as_sampler_view(device, buf_in_a->resource,
                                                     (unsigned)desc_in_a->buf.offset,
                                                     width, height, input_fmt);
   if (!sv_a) {
      IDM_LOG("bin_map early-return wrap-input-a-failed");
      return false;
   }
   struct pipe_sampler_view *sv_b =
      r300vk_identity_map_wrap_input_as_sampler_view(device, buf_in_b->resource,
                                                     (unsigned)desc_in_b->buf.offset,
                                                     width, height, input_fmt);
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
   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r300vk_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_resource_reference(&rt, NULL);
      pipe_sampler_view_reference(&sv_a, NULL);
      pipe_sampler_view_reference(&sv_b, NULL);
      return false;
   }

   r300vk_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_blend_cso,
                                        device->identity_map_rasterizer_cso,
                                        device->identity_map_dsa_cso,
                                        pl->vs_cso, pl->fs_cso, velems_cso);

   /* Bind two sampler stages -- this is the binary-map-specific change. */
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
   bool copy_ok = false;
   {
      struct pipe_transfer *rt_xfer = NULL;
      const void *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                             &copy_box, &rt_xfer);
      if (rt_map) {
         struct pipe_transfer *out_xfer = NULL;
         struct pipe_box out_box;
         memset(&out_box, 0, sizeof(out_box));
         out_box.x      = (unsigned)desc_out->buf.offset;
         /* The render target carries the result in output_fmt; the output SSBO
          * element format is output_buffer_fmt (PIPE_FORMAT_NONE means it equals
          * the RT format -- the raw byte copy the encode-into-RT patterns use).
          * When they differ -- QMUL renders the quaternion to an FP16 RT but the
          * kernel's output is vec4 FP32 -- unpack each RT row to RGBA32_FLOAT,
          * the only conversion the substrate needs (R300 has no FP32 RT). */
         const enum pipe_format buf_fmt =
            output_buffer_fmt == PIPE_FORMAT_NONE ? fmt : output_buffer_fmt;
         const unsigned buf_bs = util_format_get_blocksize(buf_fmt);
         out_box.width  = width * height * buf_bs;
         out_box.height = 1;
         out_box.depth  = 1;
         void *out_bytes = pipe->buffer_map(pipe, buf_out->resource, 0,
                                            PIPE_MAP_WRITE |
                                            PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                            &out_box, &out_xfer);
         if (out_bytes) {
            if (buf_fmt == fmt) {
               r300vk_identity_map_copy_rows(out_bytes, width * buf_bs,
                                             rt_map, rt_xfer->stride,
                                             width, height, buf_bs,
                                             total_invocations);
            } else {
               /* util_format_unpack_rgba unpacks each RT row to RGBA32_FLOAT, so
                * output_buffer_fmt must be R32G32B32A32_FLOAT.  Clamp to
                * total_invocations so the trailing padding lanes of the last
                * raster row never overrun the output buffer. */
               uint8_t *dst = out_bytes;
               const uint8_t *src = rt_map;
               uint64_t remaining = total_invocations;
               for (unsigned r = 0; r < height && remaining; r++) {
                  unsigned n = remaining < width ? (unsigned)remaining : width;
                  util_format_unpack_rgba(fmt, dst, src, n);
                  dst += (size_t)n * buf_bs;
                  src += rt_xfer->stride;
                  remaining -= n;
               }
            }
            pipe->buffer_unmap(pipe, out_xfer);
            copy_ok = true;
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
   IDM_LOG("bin_map orchestrator done copy_ok=%d", (int)copy_ok);
   return copy_ok;
}

/* Binary-map and DP4 share the 2-in / 1-out replay core above; each passes the
 * three ssbo bindings its detector captured.  binary-map's FS reduces the two
 * sampled texels with the detected ALU op; dp4's FS dots them (pl->fs_cso
 * already holds the right synthesized FS). */
bool
r300vk_binary_map_dispatch_replay(struct r300vk_device *device,
                                  const struct r300vk_pipeline *pl,
                                  const struct r300vk_cmd_dispatch *dispatch,
                                  const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   /* A 32-bit float componentwise op -- the quaternion QADD/QSUB tier -- cannot
    * use the UNORM8 byte path: sampling FP32 SSBO bytes as UNORM8 misreads them,
    * and an unbounded float sum does not fit [0,1].  Sample the inputs as FP32,
    * render to an FP16 target (R300 has no FP32 RT), and unpack into the vec4
    * FP32 output, exactly as QMUL does; the binary-map FS is format-agnostic
    * (it samples, applies the op, and writes, with no byte encode), so the same
    * synthesized shader serves both domains. */
   if (pl->binary_map.value_is_float && pl->binary_map.value_bit_size == 32) {
      if (!device->screen->is_format_supported(device->screen,
             PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
             PIPE_BIND_SAMPLER_VIEW))
         return false;
      if (!device->screen->is_format_supported(device->screen,
             PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
             PIPE_BIND_RENDER_TARGET))
         return false;
      return r300vk_two_in_one_out_dispatch_replay(
         device, pl, dispatch, binds,
         pl->binary_map.input_a_ssbo_binding,
         pl->binary_map.input_b_ssbo_binding,
         pl->binary_map.output_ssbo_binding,
         PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
         PIPE_FORMAT_R32G32B32A32_FLOAT);
   }
   return r300vk_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->binary_map.input_a_ssbo_binding,
      pl->binary_map.input_b_ssbo_binding,
      pl->binary_map.output_ssbo_binding,
      PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_R8G8B8A8_UNORM,
      PIPE_FORMAT_NONE);
}

bool
r300vk_dp4_dispatch_replay(struct r300vk_device *device,
                           const struct r300vk_pipeline *pl,
                           const struct r300vk_cmd_dispatch *dispatch,
                           const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   /* The dp4 vec4 inputs are sampled as R32G32B32A32_FLOAT.  R300 supports FP32
    * texture sampling but NOT FP32 render targets (so the dot output is RGBA8
    * integer-encoded by the FS, not an FP32 RT).  Bail if this variant lacks
    * FP32 sampler support rather than mis-sample the inputs as bytes. */
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   return r300vk_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->dp4.input_a_ssbo_binding,
      pl->dp4.input_b_ssbo_binding,
      pl->dp4.output_ssbo_binding,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R8G8B8A8_UNORM,
      PIPE_FORMAT_NONE);
}

/* QMUL dispatch replay: the quaternion Hamilton product on the compute-as-raster
 * substrate.  Same two-in/one-out skeleton as DP4, but the inputs are the two
 * quaternions sampled as R32G32B32A32_FLOAT and the synthesized Hamilton FS
 * (r300vk_build_qmul_fs_nir) writes the four-lane product to an FP16
 * (R16G16B16A16_FLOAT) render target -- R300 samples FP32 but has no FP32 RT,
 * and the substrate's quaternion result is FP16-precise.  The copy-back unpacks
 * the FP16 target into the kernel's vec4 FP32 output buffer.  Bail unless both
 * the FP32 sampler view and the FP16 render target are supported rather than
 * mis-format the pass. */
bool
r300vk_qmul_dispatch_replay(struct r300vk_device *device,
                            const struct r300vk_pipeline *pl,
                            const struct r300vk_cmd_dispatch *dispatch,
                            const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r300vk_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->qmul.input_a_ssbo_binding,
      pl->qmul.input_b_ssbo_binding,
      pl->qmul.output_ssbo_binding,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* QDIV dispatch replay: the quaternion quotient a/b on the substrate.  Identical
 * two-in/one-out skeleton to QMUL -- the dividend a and divisor b sampled as
 * R32G32B32A32_FLOAT, the synthesized division FS (r300vk_build_qdiv_fs_nir) writes
 * a*inv(b) to an FP16 render target, and the copy-back unpacks it into the kernel's
 * vec4 FP32 output.  Bail unless both the FP32 sampler view and the FP16 render
 * target are supported. */
bool
r300vk_qdiv_dispatch_replay(struct r300vk_device *device,
                            const struct r300vk_pipeline *pl,
                            const struct r300vk_cmd_dispatch *dispatch,
                            const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r300vk_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->qdiv.input_a_ssbo_binding,
      pl->qdiv.input_b_ssbo_binding,
      pl->qdiv.output_ssbo_binding,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* QROTATE dispatch replay: rotate v by q on the substrate.  Same two-in/one-out
 * skeleton as QMUL -- inputs are the unit quaternion q and the vector v sampled
 * as R32G32B32A32_FLOAT, the synthesized sandwich FS (r300vk_build_qrotate_fs_nir)
 * writes q*embed(v)*conj(q) to an FP16 render target, and the copy-back unpacks
 * it into the kernel's vec4 FP32 output. */
bool
r300vk_qrotate_dispatch_replay(struct r300vk_device *device,
                               const struct r300vk_pipeline *pl,
                               const struct r300vk_cmd_dispatch *dispatch,
                               const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r300vk_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->qrotate.input_q_ssbo_binding,
      pl->qrotate.input_v_ssbo_binding,
      pl->qrotate.output_ssbo_binding,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* Shared 1-in / 1-out compute-as-raster replay core.  Same skeleton as
 * r300vk_two_in_one_out_dispatch_replay but a single input sampler stage and a
 * two-buffer positional fallback (input = first STORAGE_BUFFER, output =
 * second).  The single-lane quaternion ops -- QCONJ (sign flip) and QNORM (self
 * dot) -- sample one FP32 quaternion, render through the synthesized FS to an
 * FP16 target, and unpack into the kernel's vec4 FP32 output, the same FP16-RT /
 * FP32-readback conversion QMUL uses.  The caller passes the input + output
 * bindings its detector captured (0,0 triggers the positional fallback) and the
 * three formats. */
static bool
r300vk_one_in_one_out_dispatch_replay(struct r300vk_device *device,
                                      const struct r300vk_pipeline *pl,
                                      const struct r300vk_cmd_dispatch *dispatch,
                                      const struct r300vk_cmd_bind_descriptor_sets *binds,
                                      uint32_t cap_in, uint32_t cap_out,
                                      enum pipe_format input_fmt,
                                      enum pipe_format output_fmt,
                                      enum pipe_format output_buffer_fmt)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   IDM_LOG("1in1out entry pl=%p cap_in=%u cap_out=%u set_count=%u gx=%u gy=%u gz=%u",
           (const void *)pl, cap_in, cap_out,
           binds ? binds->set_count : 0,
           dispatch ? dispatch->group_count_x : 0,
           dispatch ? dispatch->group_count_y : 0,
           dispatch ? dispatch->group_count_z : 0);
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0) {
      IDM_LOG("1in1out early-return null-or-empty-binds");
      return false;
   }
   if (!pl->vs_cso || !pl->fs_cso) {
      IDM_LOG("1in1out early-return no-vs-or-fs-cso");
      return false;
   }
   if (binds->first_set != 0) {
      IDM_LOG("1in1out early-return first_set=%u (only slot 0)", binds->first_set);
      return false;
   }
   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("1in1out early-return no-set-or-layout");
      return false;
   }

   /* Binding resolution, same priority as the 2-in core: captured constant
    * bindings win; all-zero means the detector saw opaque post-explicit_io
    * handles, so fall back to positional layout iteration (input = 1st
    * STORAGE_BUFFER, output = 2nd). */
   uint32_t in_binding  = cap_in;
   uint32_t out_binding = cap_out;
   const bool detector_captured = (in_binding != 0 || out_binding != 0);
   if (!detector_captured) {
      if (!nth_storage_buffer_binding(set, 0, &in_binding) ||
          !nth_storage_buffer_binding(set, 1, &out_binding)) {
         IDM_LOG("1in1out early-return layout-has-fewer-than-two-storage-buffers");
         return false;
      }
   }
   IDM_LOG("1in1out bindings: in=%u out=%u source=%s",
           in_binding, out_binding, detector_captured ? "detector" : "positional");

   const struct r300vk_descriptor *desc_in =
      find_descriptor_by_binding(set, in_binding);
   const struct r300vk_descriptor *desc_out =
      find_descriptor_by_binding(set, out_binding);
   if (!desc_in || !desc_out) {
      IDM_LOG("1in1out early-return descriptor-walk-miss");
      return false;
   }
   if (!desc_in->buf.buffer || !desc_out->buf.buffer) {
      IDM_LOG("1in1out early-return null-vkbuffer-handle");
      return false;
   }
   VK_FROM_HANDLE(r300vk_buffer, buf_in,  desc_in->buf.buffer);
   VK_FROM_HANDLE(r300vk_buffer, buf_out, desc_out->buf.buffer);
   if (!buf_in || !buf_out || !buf_in->resource || !buf_out->resource) {
      IDM_LOG("1in1out early-return null-pipe-resource");
      return false;
   }

   const uint64_t total_invocations =
      r300vk_idm_total_invocations(dispatch, pl);
   if (total_invocations == 0 || total_invocations > 2048u * 2048u) {
      IDM_LOG("1in1out early-return total_invocations=%llu out-of-bounds",
              (unsigned long long)total_invocations);
      return false;
   }
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total_invocations, &width, &height);
   if (width > 2048 || height > 2048) {
      IDM_LOG("1in1out early-return extent-exceeds-2048-cap");
      return false;
   }

   const enum pipe_format fmt = output_fmt;
   struct pipe_sampler_view *sv =
      r300vk_identity_map_wrap_input_as_sampler_view(device, buf_in->resource,
                                                     (unsigned)desc_in->buf.offset,
                                                     width, height, input_fmt);
   if (!sv) {
      IDM_LOG("1in1out early-return wrap-input-failed");
      return false;
   }

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
      IDM_LOG("1in1out early-return rt-create-failed");
      pipe_sampler_view_reference(&sv, NULL);
      return false;
   }

   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = fmt;
   surf_templ.texture = rt;

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r300vk_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_resource_reference(&rt, NULL);
      pipe_sampler_view_reference(&sv, NULL);
      return false;
   }

   r300vk_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_blend_cso,
                                        device->identity_map_rasterizer_cso,
                                        device->identity_map_dsa_cso,
                                        pl->vs_cso, pl->fs_cso, velems_cso);
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1,
                             &device->identity_map_sampler_cso);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 1, 0, &sv);

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
   IDM_LOG("1in1out draw_vbo mode=triangle_strip count=4");
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   pipe->flush(pipe, NULL, 0);
   IDM_LOG("1in1out post-flush, beginning rt->buffer copy");

   struct pipe_box copy_box;
   memset(&copy_box, 0, sizeof(copy_box));
   copy_box.width = width; copy_box.height = height; copy_box.depth = 1;
   bool copy_ok = false;
   {
      struct pipe_transfer *rt_xfer = NULL;
      const void *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                             &copy_box, &rt_xfer);
      if (rt_map) {
         struct pipe_transfer *out_xfer = NULL;
         struct pipe_box out_box;
         memset(&out_box, 0, sizeof(out_box));
         out_box.x      = (unsigned)desc_out->buf.offset;
         /* output_buffer_fmt == PIPE_FORMAT_NONE means the output SSBO element
          * format equals the RT format (raw byte copy); otherwise the RT carries
          * the result in output_fmt and each row unpacks to output_buffer_fmt
          * (R32G32B32A32_FLOAT -- the FP16->FP32 conversion the substrate needs,
          * R300 having no FP32 RT). */
         const enum pipe_format buf_fmt =
            output_buffer_fmt == PIPE_FORMAT_NONE ? fmt : output_buffer_fmt;
         const unsigned buf_bs = util_format_get_blocksize(buf_fmt);
         out_box.width  = width * height * buf_bs;
         out_box.height = 1;
         out_box.depth  = 1;
         void *out_bytes = pipe->buffer_map(pipe, buf_out->resource, 0,
                                            PIPE_MAP_WRITE |
                                            PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                            &out_box, &out_xfer);
         if (out_bytes) {
            if (buf_fmt == fmt) {
               r300vk_identity_map_copy_rows(out_bytes, width * buf_bs,
                                             rt_map, rt_xfer->stride,
                                             width, height, buf_bs,
                                             total_invocations);
            } else {
               uint8_t *dst = out_bytes;
               const uint8_t *src = rt_map;
               uint64_t remaining = total_invocations;
               for (unsigned r = 0; r < height && remaining; r++) {
                  unsigned n = remaining < width ? (unsigned)remaining : width;
                  util_format_unpack_rgba(fmt, dst, src, n);
                  dst += (size_t)n * buf_bs;
                  src += rt_xfer->stride;
                  remaining -= n;
               }
            }
            pipe->buffer_unmap(pipe, out_xfer);
            copy_ok = true;
         }
         pipe->texture_unmap(pipe, rt_xfer);
      }
   }
   IDM_LOG("1in1out rt->buffer copy issued (out=%p, src=%p, box w=%u h=%u)",
           (const void *)buf_out->resource, (const void *)rt,
           copy_box.width, copy_box.height);

   struct pipe_sampler_view *no_view = NULL;
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 1, &no_view);
   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);

   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_sampler_view_reference(&sv, NULL);
   pipe_resource_reference(&vb, NULL);
   pipe_resource_reference(&rt, NULL);
   IDM_LOG("1in1out orchestrator done copy_ok=%d", (int)copy_ok);
   return copy_ok;
}

/* QCONJ dispatch replay: the quaternion conjugate on the substrate.  One input
 * quaternion sampled R32G32B32A32_FLOAT, the synthesized sign-flip FS
 * (r300vk_build_qconj_fs_nir) writes (a.x,-a.y,-a.z,-a.w) to an FP16 render
 * target, and the copy-back unpacks it into the kernel's vec4 FP32 output. */
bool
r300vk_qconj_dispatch_replay(struct r300vk_device *device,
                             const struct r300vk_pipeline *pl,
                             const struct r300vk_cmd_dispatch *dispatch,
                             const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r300vk_one_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->qconj.input_ssbo_binding, pl->qconj.output_ssbo_binding,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* QNORM dispatch replay: the quaternion squared norm on the substrate.  Same
 * one-in/one-out skeleton as QCONJ; the synthesized self-dot FS
 * (r300vk_build_qnorm_fs_nir) writes vec4(dot(a,a)) to the FP16 target, unpacked
 * into the kernel's vec4 FP32 output (the kernel reads lane 0). */
bool
r300vk_qnorm_dispatch_replay(struct r300vk_device *device,
                             const struct r300vk_pipeline *pl,
                             const struct r300vk_cmd_dispatch *dispatch,
                             const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r300vk_one_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->qnorm.input_ssbo_binding, pl->qnorm.output_ssbo_binding,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* QNORMALIZE dispatch: the same one-in/one-out skeleton as QNORM; the synthesized
 * normalize FS (r300vk_build_qnormalize_fs_nir) scales the sampled quaternion by
 * the US RSQ of its squared norm, written to the FP16 target and unpacked into the
 * kernel's vec4 FP32 output. */
bool
r300vk_qnormalize_dispatch_replay(struct r300vk_device *device,
                                  const struct r300vk_pipeline *pl,
                                  const struct r300vk_cmd_dispatch *dispatch,
                                  const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r300vk_one_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->qnormalize.input_ssbo_binding, pl->qnormalize.output_ssbo_binding,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* Unpack an FP16 (R16G16B16A16_FLOAT) render target into a vec4 FP32 output
 * buffer at out_offset.  R300 has no FP32 render target, so every octonion-half
 * result rides an FP16 target and converts here; shared by the two-pass route
 * and the MRT route.  Clamps to total so the trailing padding lanes of the last
 * raster row never overrun the output. */
static bool
omul_copy_fp16_rt_to_buffer(struct pipe_context *pipe, struct pipe_resource *rt,
                            struct pipe_resource *out_res, unsigned out_offset,
                            unsigned width, unsigned height, uint64_t total)
{
   struct pipe_box copy_box;
   memset(&copy_box, 0, sizeof(copy_box));
   copy_box.width = width; copy_box.height = height; copy_box.depth = 1;
   bool copy_ok = false;
   struct pipe_transfer *rt_xfer = NULL;
   const void *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                          &copy_box, &rt_xfer);
   if (rt_map) {
      struct pipe_transfer *out_xfer = NULL;
      struct pipe_box out_box;
      memset(&out_box, 0, sizeof(out_box));
      const unsigned buf_bs =
         util_format_get_blocksize(PIPE_FORMAT_R32G32B32A32_FLOAT);
      out_box.x      = out_offset;
      out_box.width  = width * height * buf_bs;
      out_box.height = 1;
      out_box.depth  = 1;
      void *out_bytes = pipe->buffer_map(pipe, out_res, 0,
                                         PIPE_MAP_WRITE |
                                         PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                         &out_box, &out_xfer);
      if (out_bytes) {
         uint8_t *dst = out_bytes;
         const uint8_t *src = rt_map;
         uint64_t remaining = total;
         for (unsigned r = 0; r < height && remaining; r++) {
            unsigned n = remaining < width ? (unsigned)remaining : width;
            util_format_unpack_rgba(PIPE_FORMAT_R16G16B16A16_FLOAT, dst, src, n);
            dst += (size_t)n * buf_bs;
            src += rt_xfer->stride;
            remaining -= n;
         }
         pipe->buffer_unmap(pipe, out_xfer);
         copy_ok = true;
      }
      pipe->texture_unmap(pipe, rt_xfer);
   }
   return copy_ok;
}

/* Run one octonion-product pass on the compute-as-raster substrate: bind the
 * four input sampler views (a,b,c,d), draw the fullscreen quad through pass_fs,
 * and unpack the FP16 render-target result into out_res at out_offset.  R300 has
 * no FP32 render target, so the quaternion-lane result rides an FP16 target and
 * the copy-back unpacks each row to RGBA32_FLOAT, the kernel's output format.
 * The vb/velems fullscreen quad is shared across the two passes. */
static bool
omul_run_pass_cb(struct pipe_context *pipe, struct pipe_screen *screen,
                 struct r300vk_device *device,
                 struct pipe_sampler_view *views[4], void *pass_fs, void *vs_cso,
                 struct pipe_resource *vb, void *velems_cso,
                 struct pipe_resource *out_res, unsigned out_offset,
                 unsigned width, unsigned height, uint64_t total,
                 const void *cb_data, unsigned cb_size)
{
   const enum pipe_format rtfmt = PIPE_FORMAT_R16G16B16A16_FLOAT;
   struct pipe_resource rt_templ;
   memset(&rt_templ, 0, sizeof(rt_templ));
   rt_templ.target     = PIPE_TEXTURE_2D;
   rt_templ.format     = rtfmt;
   rt_templ.width0     = width;
   rt_templ.height0    = height;
   rt_templ.depth0     = 1;
   rt_templ.array_size = 1;
   rt_templ.usage      = PIPE_USAGE_DEFAULT;
   rt_templ.bind       = PIPE_BIND_RENDER_TARGET;
   struct pipe_resource *rt = screen->resource_create(screen, &rt_templ);
   if (!rt)
      return false;

   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = rtfmt;
   surf_templ.texture = rt;

   r300vk_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_blend_cso,
                                        device->identity_map_rasterizer_cso,
                                        device->identity_map_dsa_cso,
                                        vs_cso, pass_fs, velems_cso);

   void *samplers[4] = { device->identity_map_sampler_cso,
                         device->identity_map_sampler_cso,
                         device->identity_map_sampler_cso,
                         device->identity_map_sampler_cso };
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 4, samplers);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 4, 0, views);

   /* Optional fragment CONST[0..] upload (the broadcast-matrix lowering puts the
    * 4x4 in the constant file instead of a texture).  user_buffer maps straight
    * into r300_set_constant_buffer with no GPU upload and is consumed at the draw
    * below, so cb_data only needs to outlive this call. */
   if (cb_data) {
      struct pipe_constant_buffer cb;
      memset(&cb, 0, sizeof(cb));
      cb.user_buffer = cb_data;
      cb.buffer_size = cb_size;
      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);
   }

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
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   pipe->flush(pipe, NULL, 0);

   bool copy_ok = omul_copy_fp16_rt_to_buffer(pipe, rt, out_res, out_offset,
                                              width, height, total);

   struct pipe_sampler_view *no_views[4] = { NULL, NULL, NULL, NULL };
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 4, no_views);
   if (cb_data)
      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, NULL);
   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);
   pipe_resource_reference(&rt, NULL);
   return copy_ok;
}

/* The 4-sampler / no-constant fast path used by every elementwise op: forwards
 * to omul_run_pass_cb with no fragment constant buffer. */
static bool
omul_run_pass(struct pipe_context *pipe, struct pipe_screen *screen,
              struct r300vk_device *device,
              struct pipe_sampler_view *views[4], void *pass_fs, void *vs_cso,
              struct pipe_resource *vb, void *velems_cso,
              struct pipe_resource *out_res, unsigned out_offset,
              unsigned width, unsigned height, uint64_t total)
{
   return omul_run_pass_cb(pipe, screen, device, views, pass_fs, vs_cso, vb,
                           velems_cso, out_res, out_offset, width, height,
                           total, NULL, 0);
}

/* Run the octonion product in ONE pass via two render targets: bind the four
 * input sampler views, draw through the MRT FS (which writes the lower half to
 * color output 0 and the upper to output 1), and unpack both FP16 targets into
 * the two output halves.  Half the draws and one set of sampler binds versus the
 * two-pass route; used when the screen supports two simultaneous FP16 render
 * targets.  The single-cbuf setup_draw_state helper cannot bind two targets, so
 * the framebuffer + viewport + scissor + state binds are inlined here. */
static bool
omul_run_mrt_pass(struct pipe_context *pipe, struct pipe_screen *screen,
                  struct r300vk_device *device,
                  struct pipe_sampler_view **views, unsigned nviews,
                  void *mrt_fs, void *vs_cso,
                  struct pipe_resource *vb, void *velems_cso,
                  struct pipe_resource *out_lo_res, unsigned out_lo_offset,
                  struct pipe_resource *out_hi_res, unsigned out_hi_offset,
                  unsigned width, unsigned height, uint64_t total)
{
   const enum pipe_format rtfmt = PIPE_FORMAT_R16G16B16A16_FLOAT;
   struct pipe_resource rt_templ;
   memset(&rt_templ, 0, sizeof(rt_templ));
   rt_templ.target     = PIPE_TEXTURE_2D;
   rt_templ.format     = rtfmt;
   rt_templ.width0     = width;
   rt_templ.height0    = height;
   rt_templ.depth0     = 1;
   rt_templ.array_size = 1;
   rt_templ.usage      = PIPE_USAGE_DEFAULT;
   rt_templ.bind       = PIPE_BIND_RENDER_TARGET;
   struct pipe_resource *rt0 = screen->resource_create(screen, &rt_templ);
   struct pipe_resource *rt1 = screen->resource_create(screen, &rt_templ);
   if (!rt0 || !rt1) {
      pipe_resource_reference(&rt0, NULL);
      pipe_resource_reference(&rt1, NULL);
      return false;
   }

   /* Two-cbuf framebuffer.  The blend CSO has independent_blend_enable = 0, so
    * its rt[0] colormask (RGBA) applies to both targets. */
   struct pipe_framebuffer_state fb;
   memset(&fb, 0, sizeof(fb));
   fb.width    = width;
   fb.height   = height;
   fb.nr_cbufs = 2;
   fb.cbufs[0].format  = rtfmt;
   fb.cbufs[0].texture = rt0;
   fb.cbufs[1].format  = rtfmt;
   fb.cbufs[1].texture = rt1;
   pipe->set_framebuffer_state(pipe, &fb);

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
   sc.maxx = width;
   sc.maxy = height;
   pipe->set_scissor_states(pipe, 0, 1, &sc);

   pipe->bind_blend_state(pipe, device->identity_map_blend_cso);
   pipe->bind_rasterizer_state(pipe, device->identity_map_rasterizer_cso);
   pipe->bind_depth_stencil_alpha_state(pipe, device->identity_map_dsa_cso);
   pipe->bind_vs_state(pipe, vs_cso);
   pipe->bind_fs_state(pipe, mrt_fs);
   pipe->bind_vertex_elements_state(pipe, velems_cso);

   void *samplers[4] = { device->identity_map_sampler_cso,
                         device->identity_map_sampler_cso,
                         device->identity_map_sampler_cso,
                         device->identity_map_sampler_cso };
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, nviews, samplers);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, nviews, 0, views);

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
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   pipe->flush(pipe, NULL, 0);

   bool ok_lo = omul_copy_fp16_rt_to_buffer(pipe, rt0, out_lo_res, out_lo_offset,
                                            width, height, total);
   bool ok_hi = omul_copy_fp16_rt_to_buffer(pipe, rt1, out_hi_res, out_hi_offset,
                                            width, height, total);

   struct pipe_sampler_view *no_views[4] = { NULL, NULL, NULL, NULL };
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, nviews, no_views);
   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);
   pipe_resource_reference(&rt0, NULL);
   pipe_resource_reference(&rt1, NULL);
   return ok_lo && ok_hi;
}

/* OMUL dispatch replay.  The octonion product fills an eight-wide result.  When
 * the screen supports two simultaneous FP16 render targets the dispatch prefers
 * route B (omul_run_mrt_pass: both halves in one draw via the MRT FS held in
 * pl->fs_cso_mrt); otherwise it falls back to route A, two single-output passes
 * sharing the four sampler views and the fullscreen quad (pl->fs_cso for the
 * lower half a*c - conj(d)*b, pl->fs_cso2 for the upper d*a + b*conj(c)).  Both
 * routes are capability-gated, not parallel: R300 is a single graphics pipe, so
 * running both would serialize and waste work -- B is just the cheaper path. */
bool
r300vk_omul_dispatch_replay(struct r300vk_device *device,
                            const struct r300vk_pipeline *pl,
                            const struct r300vk_cmd_dispatch *dispatch,
                            const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0)
      return false;
   if (!pl->vs_cso || !pl->fs_cso || !pl->fs_cso2)
      return false;
   if (binds->first_set != 0)
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32G32B32A32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R16G16B16A16_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_RENDER_TARGET))
      return false;

   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   /* Six bindings: a,b,c,d inputs then o_lo,o_hi outputs.  Captured constants
    * win; all-zero means opaque post-explicit_io handles, so fall back to the
    * first six STORAGE_BUFFERs in declaration order. */
   uint32_t bind[6] = { pl->omul.input_a_ssbo_binding,
                        pl->omul.input_b_ssbo_binding,
                        pl->omul.input_c_ssbo_binding,
                        pl->omul.input_d_ssbo_binding,
                        pl->omul.output_lo_ssbo_binding,
                        pl->omul.output_hi_ssbo_binding };
   if (bind[0] == 0 && bind[1] == 0 && bind[2] == 0 && bind[3] == 0 &&
       bind[4] == 0 && bind[5] == 0) {
      for (unsigned i = 0; i < 6; i++)
         if (!nth_storage_buffer_binding(set, i, &bind[i]))
            return false;
   }

   const struct r300vk_descriptor *desc[6];
   struct r300vk_buffer *buf[6];
   for (unsigned i = 0; i < 6; i++) {
      desc[i] = find_descriptor_by_binding(set, bind[i]);
      if (!desc[i] || !desc[i]->buf.buffer)
         return false;
      buf[i] = r300vk_buffer_from_handle(desc[i]->buf.buffer);
      if (!buf[i] || !buf[i]->resource)
         return false;
   }

   const uint64_t total = r300vk_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   struct pipe_sampler_view *views[4] = { NULL, NULL, NULL, NULL };
   for (unsigned i = 0; i < 4; i++) {
      views[i] = r300vk_identity_map_wrap_input_as_sampler_view(
         device, buf[i]->resource, (unsigned)desc[i]->buf.offset,
         width, height, PIPE_FORMAT_R32G32B32A32_FLOAT);
      if (!views[i]) {
         for (unsigned k = 0; k < i; k++)
            pipe_sampler_view_reference(&views[k], NULL);
         return false;
      }
   }

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r300vk_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      for (unsigned i = 0; i < 4; i++)
         pipe_sampler_view_reference(&views[i], NULL);
      return false;
   }

   /* Route B (MRT) is preferred when its FS was synthesized; R300VK_OMUL_FORCE_2PASS
    * forces the route-A fallback on the same hardware to exercise both paths. */
   bool ok;
   if (pl->fs_cso_mrt && !getenv("R300VK_OMUL_FORCE_2PASS")) {
      /* Route B: both halves in one MRT pass (synthesized only when the screen
       * supports two simultaneous FP16 render targets, so its presence is the
       * capability gate). */
      IDM_LOG("omul route=B (MRT 1-pass) w=%u h=%u total=%llu",
              width, height, (unsigned long long)total);
      ok = omul_run_mrt_pass(pipe, screen, device, views, 4, pl->fs_cso_mrt,
                             pl->vs_cso, vb, velems_cso,
                             buf[4]->resource, (unsigned)desc[4]->buf.offset,
                             buf[5]->resource, (unsigned)desc[5]->buf.offset,
                             width, height, total);
   } else {
      /* Route A: two single-output passes (the screen lacks 2-RT MRT support). */
      IDM_LOG("omul route=A (2-pass) w=%u h=%u total=%llu",
              width, height, (unsigned long long)total);
      bool ok_lo = omul_run_pass(pipe, screen, device, views, pl->fs_cso,
                                 pl->vs_cso, vb, velems_cso, buf[4]->resource,
                                 (unsigned)desc[4]->buf.offset, width, height, total);
      bool ok_hi = omul_run_pass(pipe, screen, device, views, pl->fs_cso2,
                                 pl->vs_cso, vb, velems_cso, buf[5]->resource,
                                 (unsigned)desc[5]->buf.offset, width, height, total);
      ok = ok_lo && ok_hi;
   }

   pipe->delete_vertex_elements_state(pipe, velems_cso);
   for (unsigned i = 0; i < 4; i++)
      pipe_sampler_view_reference(&views[i], NULL);
   pipe_resource_reference(&vb, NULL);
   return ok;
}

/* MAT4VEC dispatch replay: the general 4x4 vertex transform out = M*v on the
 * compute-as-raster substrate.  Unlike the per-element ops, the matrix is
 * BROADCAST -- the same four rows for every vertex -- so rather than wrap it as a
 * texture it is mapped once and uploaded into the fragment constant file
 * (CONST[0..3] = the four rows); the vertices are the only sampler, wrapped at
 * the dispatch extent.  The synthesized FS reads each const row, dots it against
 * the per-element vertex, and writes the transformed position to the FP16 RT,
 * unpacked into the kernel's vec4 FP32 output -- the same FP16-RT/FP32-readback
 * core as QMUL, via omul_run_pass_cb with the matrix as its constant buffer.
 * Dropping the matrix texture removes four TEX and their four coordinate-staging
 * MOVs, leaving 1 TEX + 4 DP4. */
bool
r300vk_mat4vec_dispatch_replay(struct r300vk_device *device,
                               const struct r300vk_pipeline *pl,
                               const struct r300vk_cmd_dispatch *dispatch,
                               const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0)
      return false;
   if (!pl->vs_cso || !pl->fs_cso)
      return false;
   if (binds->first_set != 0)
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32G32B32A32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R16G16B16A16_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_RENDER_TARGET))
      return false;

   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   /* Three bindings: matrix, vertices, output.  The detector captures the matrix
    * binding (and defaults vertices=1, output=2); if a lookup misses, fall back
    * to the first three STORAGE_BUFFERs in declaration order. */
   uint32_t bind[3] = { pl->mat4vec.matrix_ssbo_binding,
                        pl->mat4vec.vertex_ssbo_binding,
                        pl->mat4vec.output_ssbo_binding };
   const struct r300vk_descriptor *desc[3];
   struct r300vk_buffer *buf[3];
   for (unsigned i = 0; i < 3; i++) {
      desc[i] = find_descriptor_by_binding(set, bind[i]);
      if (!desc[i] || !desc[i]->buf.buffer) {
         if (!nth_storage_buffer_binding(set, i, &bind[i]))
            return false;
         desc[i] = find_descriptor_by_binding(set, bind[i]);
         if (!desc[i] || !desc[i]->buf.buffer)
            return false;
      }
      buf[i] = r300vk_buffer_from_handle(desc[i]->buf.buffer);
      if (!buf[i] || !buf[i]->resource)
         return false;
   }

   const uint64_t total = r300vk_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   /* The matrix is broadcast (one 4x4 for every element), so it goes into the
    * fragment constant file rather than a texture: map its 16 floats once and
    * hand them to the pass as CONST[0..3].  Only the per-element vertices need a
    * sampler (stage 0).  This drops the four matrix-row TEX and their four
    * coordinate-staging MOVs the texture variant compiled to. */
   float matrix[16];
   {
      struct pipe_transfer *mxfer = NULL;
      struct pipe_box mbox;
      memset(&mbox, 0, sizeof(mbox));
      mbox.x      = (int)desc[0]->buf.offset;
      mbox.width  = (int)sizeof(matrix);
      mbox.height = 1;
      mbox.depth  = 1;
      void *mptr = pipe->buffer_map(pipe, buf[0]->resource, 0, PIPE_MAP_READ,
                                    &mbox, &mxfer);
      if (!mptr)
         return false;
      memcpy(matrix, mptr, sizeof(matrix));
      pipe->buffer_unmap(pipe, mxfer);
   }

   struct pipe_sampler_view *views[4] = { NULL, NULL, NULL, NULL };
   /* views[0] = the per-element vertices at the dispatch extent. */
   views[0] = r300vk_identity_map_wrap_input_as_sampler_view(
      device, buf[1]->resource, (unsigned)desc[1]->buf.offset,
      width, height, PIPE_FORMAT_R32G32B32A32_FLOAT);
   if (!views[0])
      return false;

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r300vk_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_sampler_view_reference(&views[0], NULL);
      return false;
   }

   bool ok = omul_run_pass_cb(pipe, screen, device, views, pl->fs_cso,
                              pl->vs_cso, vb, velems_cso, buf[2]->resource,
                              (unsigned)desc[2]->buf.offset, width, height, total,
                              matrix, sizeof(matrix));

   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_sampler_view_reference(&views[0], NULL);
   pipe_resource_reference(&vb, NULL);
   return ok;
}

/* QFMUL dispatch replay: out[gid] = a[gid] * s.  The scalar s is BROADCAST, so it
 * is mapped once and uploaded into the fragment constant file (CONST[0].x) the
 * way MAT4VEC handles its broadcast matrix; only the per-element quaternions need
 * a sampler.  Three bindings: scalar, quaternions, output (defaults 0,1,2 with a
 * positional fallback). */
bool
r300vk_qfmul_dispatch_replay(struct r300vk_device *device,
                             const struct r300vk_pipeline *pl,
                             const struct r300vk_cmd_dispatch *dispatch,
                             const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0)
      return false;
   if (!pl->vs_cso || !pl->fs_cso)
      return false;
   if (binds->first_set != 0)
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32G32B32A32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R16G16B16A16_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_RENDER_TARGET))
      return false;

   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t bind[3] = { pl->qfmul.scalar_ssbo_binding,
                        pl->qfmul.quat_ssbo_binding,
                        pl->qfmul.output_ssbo_binding };
   const struct r300vk_descriptor *desc[3];
   struct r300vk_buffer *buf[3];
   for (unsigned i = 0; i < 3; i++) {
      desc[i] = find_descriptor_by_binding(set, bind[i]);
      if (!desc[i] || !desc[i]->buf.buffer) {
         if (!nth_storage_buffer_binding(set, i, &bind[i]))
            return false;
         desc[i] = find_descriptor_by_binding(set, bind[i]);
         if (!desc[i] || !desc[i]->buf.buffer)
            return false;
      }
      buf[i] = r300vk_buffer_from_handle(desc[i]->buf.buffer);
      if (!buf[i] || !buf[i]->resource)
         return false;
   }

   const uint64_t total = r300vk_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   /* Map the one broadcast float into CONST[0].x; the rest of the vec4 is zero
    * (the FS reads only .x). */
   float cbuf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
   {
      struct pipe_transfer *sxfer = NULL;
      struct pipe_box sbox;
      memset(&sbox, 0, sizeof(sbox));
      sbox.x      = (int)desc[0]->buf.offset;
      sbox.width  = (int)sizeof(float);
      sbox.height = 1;
      sbox.depth  = 1;
      void *sptr = pipe->buffer_map(pipe, buf[0]->resource, 0, PIPE_MAP_READ,
                                    &sbox, &sxfer);
      if (!sptr)
         return false;
      memcpy(&cbuf[0], sptr, sizeof(float));
      pipe->buffer_unmap(pipe, sxfer);
   }

   struct pipe_sampler_view *views[4] = { NULL, NULL, NULL, NULL };
   views[0] = r300vk_identity_map_wrap_input_as_sampler_view(
      device, buf[1]->resource, (unsigned)desc[1]->buf.offset,
      width, height, PIPE_FORMAT_R32G32B32A32_FLOAT);
   if (!views[0])
      return false;

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r300vk_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_sampler_view_reference(&views[0], NULL);
      return false;
   }

   bool ok = omul_run_pass_cb(pipe, screen, device, views, pl->fs_cso,
                              pl->vs_cso, vb, velems_cso, buf[2]->resource,
                              (unsigned)desc[2]->buf.offset, width, height, total,
                              cbuf, sizeof(cbuf));

   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_sampler_view_reference(&views[0], NULL);
   pipe_resource_reference(&vb, NULL);
   return ok;
}

/* Resolve n STORAGE_BUFFER bindings to descriptors + buffers for the octonion
 * elementwise ops: captured constant bindings in bind[] win; all-zero falls back
 * to the first n STORAGE_BUFFERs in declaration order (the inputs precede the
 * outputs in every octonion kernel). */
static bool
octonion_resolve_buffers(const struct r300vk_descriptor_set *set, uint32_t *bind,
                         unsigned n, const struct r300vk_descriptor **desc,
                         struct r300vk_buffer **buf)
{
   bool any = false;
   for (unsigned i = 0; i < n; i++)
      if (bind[i] != 0)
         any = true;
   if (!any) {
      for (unsigned i = 0; i < n; i++)
         if (!nth_storage_buffer_binding(set, i, &bind[i]))
            return false;
   }
   for (unsigned i = 0; i < n; i++) {
      desc[i] = find_descriptor_by_binding(set, bind[i]);
      if (!desc[i] || !desc[i]->buf.buffer)
         return false;
      buf[i] = r300vk_buffer_from_handle(desc[i]->buf.buffer);
      if (!buf[i] || !buf[i]->resource)
         return false;
   }
   return true;
}

/* ONORM dispatch: |(a,b)|^2 = dot(a,a)+dot(b,b).  Two inputs, one output -- the
 * 2-in/1-out core with the synthesized self-dot-sum FS in pl->fs_cso. */
bool
r300vk_onorm_dispatch_replay(struct r300vk_device *device,
                             const struct r300vk_pipeline *pl,
                             const struct r300vk_cmd_dispatch *dispatch,
                             const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r300vk_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->onorm.input_a_ssbo_binding, pl->onorm.input_b_ssbo_binding,
      pl->onorm.output_ssbo_binding,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* OCONJ dispatch: conj((a,b)) = (conj(a), -b) in one MRT pass.  Two inputs a,b
 * sampled at stages 0,1; the MRT FS (pl->fs_cso_mrt) writes conj(a) to o_lo and
 * -b to o_hi. */
bool
r300vk_oconj_dispatch_replay(struct r300vk_device *device,
                             const struct r300vk_pipeline *pl,
                             const struct r300vk_cmd_dispatch *dispatch,
                             const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0)
      return false;
   if (!pl->vs_cso || !pl->fs_cso_mrt || binds->first_set != 0)
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32G32B32A32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_SAMPLER_VIEW) ||
       !screen->is_format_supported(screen, PIPE_FORMAT_R16G16B16A16_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_RENDER_TARGET))
      return false;
   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t bind[4] = { pl->oconj.input_a_ssbo_binding,
                        pl->oconj.input_b_ssbo_binding,
                        pl->oconj.output_lo_ssbo_binding,
                        pl->oconj.output_hi_ssbo_binding };
   const struct r300vk_descriptor *desc[4];
   struct r300vk_buffer *buf[4];
   if (!octonion_resolve_buffers(set, bind, 4, desc, buf))
      return false;

   const uint64_t total = r300vk_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   struct pipe_sampler_view *views[2] = { NULL, NULL };
   for (unsigned i = 0; i < 2; i++) {
      views[i] = r300vk_identity_map_wrap_input_as_sampler_view(
         device, buf[i]->resource, (unsigned)desc[i]->buf.offset,
         width, height, PIPE_FORMAT_R32G32B32A32_FLOAT);
      if (!views[i]) {
         for (unsigned k = 0; k < i; k++)
            pipe_sampler_view_reference(&views[k], NULL);
         return false;
      }
   }
   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r300vk_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      for (unsigned i = 0; i < 2; i++)
         pipe_sampler_view_reference(&views[i], NULL);
      return false;
   }

   bool ok = omul_run_mrt_pass(pipe, screen, device, views, 2, pl->fs_cso_mrt,
                               pl->vs_cso, vb, velems_cso,
                               buf[2]->resource, (unsigned)desc[2]->buf.offset,
                               buf[3]->resource, (unsigned)desc[3]->buf.offset,
                               width, height, total);

   pipe->delete_vertex_elements_state(pipe, velems_cso);
   for (unsigned i = 0; i < 2; i++)
      pipe_sampler_view_reference(&views[i], NULL);
   pipe_resource_reference(&vb, NULL);
   return ok;
}

/* OADD/OSUB dispatch: out = (a,b) (+|-) (c,d) in one MRT pass.  The four inputs
 * are bound as stage0=a, stage1=c, stage2=b, stage3=d so the MRT FS reads a
 * contiguous pair per half (o_lo = stage0 op stage1 = a op c, o_hi = stage2 op
 * stage3 = b op d). */
bool
r300vk_oaddsub_dispatch_replay(struct r300vk_device *device,
                               const struct r300vk_pipeline *pl,
                               const struct r300vk_cmd_dispatch *dispatch,
                               const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0)
      return false;
   if (!pl->vs_cso || !pl->fs_cso_mrt || binds->first_set != 0)
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32G32B32A32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_SAMPLER_VIEW) ||
       !screen->is_format_supported(screen, PIPE_FORMAT_R16G16B16A16_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_RENDER_TARGET))
      return false;
   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t bind[6] = { pl->oaddsub.input_a_ssbo_binding,
                        pl->oaddsub.input_b_ssbo_binding,
                        pl->oaddsub.input_c_ssbo_binding,
                        pl->oaddsub.input_d_ssbo_binding,
                        pl->oaddsub.output_lo_ssbo_binding,
                        pl->oaddsub.output_hi_ssbo_binding };
   const struct r300vk_descriptor *desc[6];
   struct r300vk_buffer *buf[6];
   if (!octonion_resolve_buffers(set, bind, 6, desc, buf))
      return false;

   const uint64_t total = r300vk_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   /* View order a,c,b,d so the contiguous-pair MRT FS computes o_lo=a op c and
    * o_hi=b op d.  src[] indexes buf[]: a=0, c=2, b=1, d=3. */
   const unsigned src[4] = { 0, 2, 1, 3 };
   struct pipe_sampler_view *views[4] = { NULL, NULL, NULL, NULL };
   for (unsigned i = 0; i < 4; i++) {
      views[i] = r300vk_identity_map_wrap_input_as_sampler_view(
         device, buf[src[i]]->resource, (unsigned)desc[src[i]]->buf.offset,
         width, height, PIPE_FORMAT_R32G32B32A32_FLOAT);
      if (!views[i]) {
         for (unsigned k = 0; k < i; k++)
            pipe_sampler_view_reference(&views[k], NULL);
         return false;
      }
   }
   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r300vk_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      for (unsigned i = 0; i < 4; i++)
         pipe_sampler_view_reference(&views[i], NULL);
      return false;
   }

   bool ok = omul_run_mrt_pass(pipe, screen, device, views, 4, pl->fs_cso_mrt,
                               pl->vs_cso, vb, velems_cso,
                               buf[4]->resource, (unsigned)desc[4]->buf.offset,
                               buf[5]->resource, (unsigned)desc[5]->buf.offset,
                               width, height, total);

   pipe->delete_vertex_elements_state(pipe, velems_cso);
   for (unsigned i = 0; i < 4; i++)
      pipe_sampler_view_reference(&views[i], NULL);
   pipe_resource_reference(&vb, NULL);
   return ok;
}

/* ODIV dispatch: out = x / y = x * inv(y) in two single-output passes.  The four
 * inputs are bound straight -- stage0=xlo, stage1=xhi, stage2=ylo, stage3=yhi --
 * and each FS forms inv(y) = conj(y)*rcp(|y|^2): pl->fs_cso writes the lower half
 * to o_lo, pl->fs_cso2 the upper half to o_hi.  Division splits into two passes
 * because the combined MRT form is 73 ALU ops, over the 64-ALU R300 limit. */
bool
r300vk_odiv_dispatch_replay(struct r300vk_device *device,
                            const struct r300vk_pipeline *pl,
                            const struct r300vk_cmd_dispatch *dispatch,
                            const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0)
      return false;
   if (!pl->vs_cso || !pl->fs_cso || !pl->fs_cso2 || binds->first_set != 0)
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32G32B32A32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_SAMPLER_VIEW) ||
       !screen->is_format_supported(screen, PIPE_FORMAT_R16G16B16A16_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_RENDER_TARGET))
      return false;
   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t bind[6] = { pl->odiv.input_xlo_ssbo_binding,
                        pl->odiv.input_xhi_ssbo_binding,
                        pl->odiv.input_ylo_ssbo_binding,
                        pl->odiv.input_yhi_ssbo_binding,
                        pl->odiv.output_lo_ssbo_binding,
                        pl->odiv.output_hi_ssbo_binding };
   const struct r300vk_descriptor *desc[6];
   struct r300vk_buffer *buf[6];
   if (!octonion_resolve_buffers(set, bind, 6, desc, buf))
      return false;

   const uint64_t total = r300vk_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   struct pipe_sampler_view *views[4] = { NULL, NULL, NULL, NULL };
   for (unsigned i = 0; i < 4; i++) {
      views[i] = r300vk_identity_map_wrap_input_as_sampler_view(
         device, buf[i]->resource, (unsigned)desc[i]->buf.offset,
         width, height, PIPE_FORMAT_R32G32B32A32_FLOAT);
      if (!views[i]) {
         for (unsigned k = 0; k < i; k++)
            pipe_sampler_view_reference(&views[k], NULL);
         return false;
      }
   }
   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r300vk_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      for (unsigned i = 0; i < 4; i++)
         pipe_sampler_view_reference(&views[i], NULL);
      return false;
   }

   /* Route A: the lower half to o_lo, then the upper half to o_hi.  Each pass
    * recomputes inv(y); the reciprocal is a few ALU ops, far cheaper than the
    * instruction budget the combined MRT form would need. */
   bool ok_lo = omul_run_pass(pipe, screen, device, views, pl->fs_cso,
                              pl->vs_cso, vb, velems_cso, buf[4]->resource,
                              (unsigned)desc[4]->buf.offset, width, height, total);
   bool ok_hi = omul_run_pass(pipe, screen, device, views, pl->fs_cso2,
                              pl->vs_cso, vb, velems_cso, buf[5]->resource,
                              (unsigned)desc[5]->buf.offset, width, height, total);
   bool ok = ok_lo && ok_hi;

   pipe->delete_vertex_elements_state(pipe, velems_cso);
   for (unsigned i = 0; i < 4; i++)
      pipe_sampler_view_reference(&views[i], NULL);
   pipe_resource_reference(&vb, NULL);
   return ok;
}

/* Allocate a scratch FP32x4 buffer holding `total` octonion-half elements -- the
 * intermediate t = x*v that OTRANS materializes between its two products.  STAGING
 * usage so the FP16->FP32 copy-back (write) and the input-wrap (read) both take
 * the direct CPU-map path, matching the other octonion-half transfers. */
static struct pipe_resource *
otrans_create_scratch(struct pipe_screen *screen, uint64_t total)
{
   struct pipe_resource bt;
   memset(&bt, 0, sizeof(bt));
   bt.target     = PIPE_BUFFER;
   bt.format     = PIPE_FORMAT_R8_UNORM;
   bt.bind       = PIPE_BIND_SAMPLER_VIEW;
   bt.usage      = PIPE_USAGE_STAGING;
   bt.width0     = (unsigned)(total * 16);
   bt.height0    = 1;
   bt.depth0     = 1;
   bt.array_size = 1;
   return screen->resource_create(screen, &bt);
}

/* OTRANS dispatch: out = x*v*conj(x) as two octonion products through a scratch
 * intermediate t.  The four inputs bind straight -- stage0=xlo, stage1=xhi,
 * stage2=vlo, stage3=vhi.  Pass 1 runs t = x*v (the OMUL half-shaders) to two
 * scratch FP32 buffers; pass 2 runs out = t*conj(x), sampling t at stages 0,1 and
 * x at stages 2,3 and forming conj(x) inline.  Four single-output passes: the
 * combined sandwich is 32 DP4s, far past the 64-ALU R300 fragment limit. */
bool
r300vk_otrans_dispatch_replay(struct r300vk_device *device,
                              const struct r300vk_pipeline *pl,
                              const struct r300vk_cmd_dispatch *dispatch,
                              const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0)
      return false;
   if (!pl->vs_cso || !pl->fs_cso || !pl->fs_cso2 || !pl->fs_cso3 ||
       !pl->fs_cso4 || binds->first_set != 0)
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32G32B32A32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_SAMPLER_VIEW) ||
       !screen->is_format_supported(screen, PIPE_FORMAT_R16G16B16A16_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_RENDER_TARGET))
      return false;
   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t bind[6] = { pl->otrans.input_xlo_ssbo_binding,
                        pl->otrans.input_xhi_ssbo_binding,
                        pl->otrans.input_vlo_ssbo_binding,
                        pl->otrans.input_vhi_ssbo_binding,
                        pl->otrans.output_lo_ssbo_binding,
                        pl->otrans.output_hi_ssbo_binding };
   const struct r300vk_descriptor *desc[6];
   struct r300vk_buffer *buf[6];
   if (!octonion_resolve_buffers(set, bind, 6, desc, buf))
      return false;

   const uint64_t total = r300vk_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   /* x and v inputs as FP32 sampler views for pass 1 (OMUL(x,v)). */
   struct pipe_sampler_view *xv[4] = { NULL, NULL, NULL, NULL };
   for (unsigned i = 0; i < 4; i++) {
      xv[i] = r300vk_identity_map_wrap_input_as_sampler_view(
         device, buf[i]->resource, (unsigned)desc[i]->buf.offset,
         width, height, PIPE_FORMAT_R32G32B32A32_FLOAT);
      if (!xv[i]) {
         for (unsigned k = 0; k < i; k++)
            pipe_sampler_view_reference(&xv[k], NULL);
         return false;
      }
   }

   struct pipe_resource *t_lo = otrans_create_scratch(screen, total);
   struct pipe_resource *t_hi = otrans_create_scratch(screen, total);
   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   bool ok = t_lo && t_hi &&
             r300vk_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso);

   /* Pass 1: t = x*v to the scratch halves. */
   if (ok)
      ok = omul_run_pass(pipe, screen, device, xv, pl->fs_cso, pl->vs_cso, vb,
                         velems_cso, t_lo, 0, width, height, total) &&
           omul_run_pass(pipe, screen, device, xv, pl->fs_cso2, pl->vs_cso, vb,
                         velems_cso, t_hi, 0, width, height, total);

   /* Pass 2: out = t*conj(x), sampling t at 0,1 and x at 2,3. */
   if (ok) {
      struct pipe_sampler_view *tx[4] = { NULL, NULL, NULL, NULL };
      tx[0] = r300vk_identity_map_wrap_input_as_sampler_view(
         device, t_lo, 0, width, height, PIPE_FORMAT_R32G32B32A32_FLOAT);
      tx[1] = r300vk_identity_map_wrap_input_as_sampler_view(
         device, t_hi, 0, width, height, PIPE_FORMAT_R32G32B32A32_FLOAT);
      tx[2] = r300vk_identity_map_wrap_input_as_sampler_view(
         device, buf[0]->resource, (unsigned)desc[0]->buf.offset, width, height,
         PIPE_FORMAT_R32G32B32A32_FLOAT);
      tx[3] = r300vk_identity_map_wrap_input_as_sampler_view(
         device, buf[1]->resource, (unsigned)desc[1]->buf.offset, width, height,
         PIPE_FORMAT_R32G32B32A32_FLOAT);
      ok = tx[0] && tx[1] && tx[2] && tx[3] &&
           omul_run_pass(pipe, screen, device, tx, pl->fs_cso3, pl->vs_cso, vb,
                         velems_cso, buf[4]->resource,
                         (unsigned)desc[4]->buf.offset, width, height, total) &&
           omul_run_pass(pipe, screen, device, tx, pl->fs_cso4, pl->vs_cso, vb,
                         velems_cso, buf[5]->resource,
                         (unsigned)desc[5]->buf.offset, width, height, total);
      for (unsigned i = 0; i < 4; i++)
         pipe_sampler_view_reference(&tx[i], NULL);
   }

   if (velems_cso)
      pipe->delete_vertex_elements_state(pipe, velems_cso);
   for (unsigned i = 0; i < 4; i++)
      pipe_sampler_view_reference(&xv[i], NULL);
   pipe_resource_reference(&vb, NULL);
   pipe_resource_reference(&t_lo, NULL);
   pipe_resource_reference(&t_hi, NULL);
   return ok;
}

/* Shared three-in/one-out fused-quaternion dispatch: bind a,b,c as FP32 sampler
 * views (the fourth sampler slot is a harmless duplicate of view 0 the FS never
 * reads), draw the single-output FS to an FP16 target, and unpack into the
 * kernel's vec4 FP32 output.  QFMADD (a*b+c) and QFMMUL (a*b*c) differ only in the
 * FS, both one pass under the 64-ALU fragment limit. */
static bool
r300vk_qfm3_run(struct r300vk_device *device, const struct r300vk_pipeline *pl,
                const struct r300vk_cmd_dispatch *dispatch,
                const struct r300vk_cmd_bind_descriptor_sets *binds,
                uint32_t a_bind, uint32_t b_bind, uint32_t c_bind,
                uint32_t out_bind, void *fs_cso)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0)
      return false;
   if (!pl->vs_cso || !fs_cso || binds->first_set != 0)
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32G32B32A32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_SAMPLER_VIEW) ||
       !screen->is_format_supported(screen, PIPE_FORMAT_R16G16B16A16_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_RENDER_TARGET))
      return false;
   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t bind[4] = { a_bind, b_bind, c_bind, out_bind };
   const struct r300vk_descriptor *desc[4];
   struct r300vk_buffer *buf[4];
   if (!octonion_resolve_buffers(set, bind, 4, desc, buf))
      return false;

   const uint64_t total = r300vk_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   struct pipe_sampler_view *views[4] = { NULL, NULL, NULL, NULL };
   for (unsigned i = 0; i < 3; i++) {
      views[i] = r300vk_identity_map_wrap_input_as_sampler_view(
         device, buf[i]->resource, (unsigned)desc[i]->buf.offset,
         width, height, PIPE_FORMAT_R32G32B32A32_FLOAT);
      if (!views[i]) {
         for (unsigned k = 0; k < i; k++)
            pipe_sampler_view_reference(&views[k], NULL);
         return false;
      }
   }
   pipe_sampler_view_reference(&views[3], views[0]); /* dummy stage 3, FS ignores */

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   bool ok = r300vk_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso);
   if (ok)
      ok = omul_run_pass(pipe, screen, device, views, fs_cso, pl->vs_cso, vb,
                         velems_cso, buf[3]->resource,
                         (unsigned)desc[3]->buf.offset, width, height, total);

   if (velems_cso)
      pipe->delete_vertex_elements_state(pipe, velems_cso);
   for (unsigned i = 0; i < 4; i++)
      pipe_sampler_view_reference(&views[i], NULL);
   pipe_resource_reference(&vb, NULL);
   return ok;
}

/* QFMADD dispatch: out = a*b + c in one pass (pl->fs_cso is the QFMADD FS). */
bool
r300vk_qfmadd_dispatch_replay(struct r300vk_device *device,
                              const struct r300vk_pipeline *pl,
                              const struct r300vk_cmd_dispatch *dispatch,
                              const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   return r300vk_qfm3_run(device, pl, dispatch, binds,
                          pl->qfmadd.input_a_ssbo_binding,
                          pl->qfmadd.input_b_ssbo_binding,
                          pl->qfmadd.input_c_ssbo_binding,
                          pl->qfmadd.output_ssbo_binding, pl->fs_cso);
}

/* QFMMUL dispatch: out = a*b*c = (a*b)*c in one pass (pl->fs_cso is the QFMMUL FS). */
bool
r300vk_qfmmul_dispatch_replay(struct r300vk_device *device,
                              const struct r300vk_pipeline *pl,
                              const struct r300vk_cmd_dispatch *dispatch,
                              const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   return r300vk_qfm3_run(device, pl, dispatch, binds,
                          pl->qfmmul.input_a_ssbo_binding,
                          pl->qfmmul.input_b_ssbo_binding,
                          pl->qfmmul.input_c_ssbo_binding,
                          pl->qfmmul.output_ssbo_binding, pl->fs_cso);
}

/* Multi-tap gather orchestrator: identity-map skeleton (one input sampler
 * view, two storage buffers) plus a per-dispatch fragment constant carrying
 * the neighbor texel displacement.  The synthesized FS
 * (r300vk_synthesize_multitap_gather_fs) samples the input at three
 * neighborhood offsets and sums them; everything else (RT, VBO, framebuffer,
 * viewport, scissor, draw, copy-back) is the identity-map path. */
bool
r300vk_multitap_gather_dispatch_replay(struct r300vk_device *device,
                                       const struct r300vk_pipeline *pl,
                                       const struct r300vk_cmd_dispatch *dispatch,
                                       const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   IDM_LOG("gather entry pl=%p is_multitap_gather=%d tap_count=%u "
           "set_count=%u gx=%u gy=%u gz=%u",
           (const void *)pl,
           pl ? (int)pl->multitap_gather.is_multitap_gather : -1,
           pl ? (unsigned)pl->multitap_gather.tap_count : 0,
           binds ? binds->set_count : 0,
           dispatch ? dispatch->group_count_x : 0,
           dispatch ? dispatch->group_count_y : 0,
           dispatch ? dispatch->group_count_z : 0);
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0) {
      IDM_LOG("gather early-return null-or-empty-binds");
      return false;
   }
   if (!pl->vs_cso || !pl->fs_cso) {
      IDM_LOG("gather early-return no-vs-or-fs-cso");
      return false;
   }
   if (binds->first_set != 0) {
      IDM_LOG("gather early-return first_set=%u (only slot 0)",
              binds->first_set);
      return false;
   }
   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("gather early-return no-set-or-layout");
      return false;
   }
   uint32_t in_binding  = pl->multitap_gather.input_ssbo_binding;
   uint32_t out_binding = pl->multitap_gather.output_ssbo_binding;
   if (!idm_recover_in_out_bindings(set, &in_binding, &out_binding)) {
      IDM_LOG("gather early-return layout-has-fewer-than-two-storage-buffers");
      return false;
   }
   uint32_t bindings[2] = { in_binding, out_binding };
   const struct r300vk_descriptor *descs[2] = {0};
   struct r300vk_buffer *bufs[2] = {0};
   if (!r300vk_idm_resolve_buffers(set, 2, bindings, descs, bufs))
      return false;
   const struct r300vk_descriptor *in_desc = descs[0];
   const struct r300vk_descriptor *out_desc = descs[1];
   struct r300vk_buffer *in_buf = bufs[0];
   struct r300vk_buffer *out_buf = bufs[1];

   const uint64_t total_invocations =
      r300vk_idm_total_invocations(dispatch, pl);
   if (total_invocations == 0 || total_invocations > 2048u) {
      IDM_LOG("gather early-return total_invocations=%llu out-of-bounds (1D box-3 limit)",
              (unsigned long long)total_invocations);
      return false;
   }
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total_invocations, &width, &height);
   if (width > 2048 || height > 2048) {
      IDM_LOG("gather early-return extent-exceeds-2048-cap");
      return false;
   }

   const enum pipe_format fmt = PIPE_FORMAT_R8G8B8A8_UNORM;
   struct pipe_sampler_view *in_sv =
      r300vk_identity_map_wrap_input_as_sampler_view(device, in_buf->resource,
                                                     (unsigned)in_desc->buf.offset,
                                                     width, height, fmt);
   if (!in_sv) {
      IDM_LOG("gather early-return wrap-input-failed");
      return false;
   }

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
      IDM_LOG("gather early-return rt-create-failed");
      pipe_sampler_view_reference(&in_sv, NULL);
      return false;
   }

   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = fmt;
   surf_templ.texture = rt;

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
      pipe_sampler_view_reference(&in_sv, NULL);
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
      pipe_sampler_view_reference(&in_sv, NULL);
      return false;
   }

   r300vk_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_blend_cso,
                                        device->identity_map_rasterizer_cso,
                                        device->identity_map_dsa_cso,
                                        pl->vs_cso, pl->fs_cso, velems_cso);

   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1,
                             &device->identity_map_sampler_cso);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 1, 0, &in_sv);

   /* The neighbor texel displacement is 1/width in normalized texcoord X.
    * height == 1 for <= 2048 elements (derive_raster_extent), so the row
    * spans the full [0,1] in X and 1/width lands exactly one texel over;
    * the .y/.z/.w stay 0 so the offset taps remain in row 0.  The FS reads
    * this as CONST[0].  user_buffer feeds r300_set_constant_buffer directly
    * (it maps cb->user_buffer with no GPU upload), so the float4 is consumed
    * before this stack frame unwinds at the draw below. */
   const float texel_delta[4] = { 1.0f / (float)width, 0.0f, 0.0f, 0.0f };
   struct pipe_constant_buffer cb;
   memset(&cb, 0, sizeof(cb));
   cb.user_buffer = texel_delta;
   cb.buffer_size = sizeof(texel_delta);
   pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);
   IDM_LOG("gather const upload texel_delta.x=1/%u", width);

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
   IDM_LOG("gather draw_vbo mode=triangle_strip count=4");
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   pipe->flush(pipe, NULL, 0);
   IDM_LOG("gather post-flush, beginning rt->buffer copy");

   struct pipe_box copy_box;
   memset(&copy_box, 0, sizeof(copy_box));
   copy_box.width = width; copy_box.height = height; copy_box.depth = 1;
   bool copy_ok = false;
   {
      struct pipe_transfer *rt_xfer = NULL;
      const void *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                             &copy_box, &rt_xfer);
      if (rt_map) {
         struct pipe_transfer *out_xfer = NULL;
         struct pipe_box out_box;
         memset(&out_box, 0, sizeof(out_box));
         out_box.x      = (unsigned)out_desc->buf.offset;
         out_box.width  = width * height * util_format_get_blocksize(fmt);
         out_box.height = 1;
         out_box.depth  = 1;
         void *out_bytes = pipe->buffer_map(pipe, out_buf->resource, 0,
                                            PIPE_MAP_WRITE |
                                            PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                            &out_box, &out_xfer);
         if (out_bytes) {
            r300vk_identity_map_copy_rows(out_bytes, width * util_format_get_blocksize(fmt),
                                          rt_map, rt_xfer->stride,
                                          width, height,
                                          util_format_get_blocksize(fmt),
                                          total_invocations);
            pipe->buffer_unmap(pipe, out_xfer);
            copy_ok = true;
         }
         pipe->texture_unmap(pipe, rt_xfer);
      }
   }
   IDM_LOG("gather rt->buffer copy issued (out=%p, src=%p, box w=%u h=%u)",
           (const void *)out_buf->resource, (const void *)rt,
           copy_box.width, copy_box.height);

   /* Tear down the sampler view, the fragment constant, and the rest. */
   struct pipe_sampler_view *no_view = NULL;
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 1, &no_view);
   pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, NULL);
   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);

   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_sampler_view_reference(&in_sv, NULL);
   pipe_resource_reference(&vb, NULL);
   pipe_resource_reference(&rt, NULL);
   IDM_LOG("gather orchestrator done copy_ok=%d", (int)copy_ok);
   return copy_ok;
}

bool
r300vk_predicated_store_dispatch_replay(struct r300vk_device *device,
                                        const struct r300vk_pipeline *pl,
                                        const struct r300vk_cmd_dispatch *dispatch,
                                        const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   IDM_LOG("predstore entry pl=%p is_predicated_store=%d set_count=%u "
           "gx=%u gy=%u gz=%u",
           (const void *)pl,
           pl ? (int)pl->predicated_store.is_predicated_store : -1,
           binds ? binds->set_count : 0,
           dispatch ? dispatch->group_count_x : 0,
           dispatch ? dispatch->group_count_y : 0,
           dispatch ? dispatch->group_count_z : 0);
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0) {
      IDM_LOG("predstore early-return null-or-empty-binds");
      return false;
   }
   if (!pl->vs_cso || !pl->fs_cso) {
      IDM_LOG("predstore early-return no-vs-or-fs-cso");
      return false;
   }
   if (binds->first_set != 0) {
      IDM_LOG("predstore early-return first_set=%u (only slot 0)",
              binds->first_set);
      return false;
   }
   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("predstore early-return no-set-or-layout");
      return false;
   }

   /* Positional binding resolution (binding 0 = predicate, 1 = value,
    * 2 = output) -- the same convention as M-F.3 / M-G.3 / Entry 6; the
    * detector's binding fields stay 0 post-explicit_io. */
   uint32_t pred_binding = 0, val_binding = 0, out_binding = 0;
   if (!nth_storage_buffer_binding(set, 0, &pred_binding) ||
       !nth_storage_buffer_binding(set, 1, &val_binding) ||
       !nth_storage_buffer_binding(set, 2, &out_binding)) {
      IDM_LOG("predstore early-return layout-has-fewer-than-three-storage-buffers");
      return false;
   }
   IDM_LOG("predstore bindings: pred=%u val=%u out=%u",
           pred_binding, val_binding, out_binding);
   const struct r300vk_descriptor *pred_desc =
      find_descriptor_by_binding(set, pred_binding);
   const struct r300vk_descriptor *val_desc =
      find_descriptor_by_binding(set, val_binding);
   const struct r300vk_descriptor *out_desc =
      find_descriptor_by_binding(set, out_binding);
   if (!pred_desc || !val_desc || !out_desc ||
       !pred_desc->buf.buffer || !val_desc->buf.buffer ||
       !out_desc->buf.buffer) {
      IDM_LOG("predstore early-return descriptor-walk-miss");
      return false;
   }
   VK_FROM_HANDLE(r300vk_buffer, pred_buf, pred_desc->buf.buffer);
   VK_FROM_HANDLE(r300vk_buffer, val_buf,  val_desc->buf.buffer);
   VK_FROM_HANDLE(r300vk_buffer, out_buf,  out_desc->buf.buffer);
   if (!pred_buf || !val_buf || !out_buf ||
       !pred_buf->resource || !val_buf->resource || !out_buf->resource) {
      IDM_LOG("predstore early-return null-pipe-resource");
      return false;
   }

   const uint64_t total_invocations =
      r300vk_idm_total_invocations(dispatch, pl);
   if (total_invocations == 0 || total_invocations > 2048u * 2048u) {
      IDM_LOG("predstore early-return total_invocations=%llu out-of-bounds",
              (unsigned long long)total_invocations);
      return false;
   }
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total_invocations, &width, &height);
   if (width > 2048 || height > 2048) {
      IDM_LOG("predstore early-return extent-exceeds-2048-cap");
      return false;
   }

   const enum pipe_format fmt = PIPE_FORMAT_R8G8B8A8_UNORM;
   const unsigned bpp = util_format_get_blocksize(fmt);

   /* Wrap the predicate (sampler 0) and value (sampler 1) buffers as
    * PIPE_TEXTURE_2D + NEAREST sampler views, the same input wrap M-E / M-F
    * use. */
   struct pipe_sampler_view *sv_pred =
      r300vk_identity_map_wrap_input_as_sampler_view(device, pred_buf->resource,
                                                     (unsigned)pred_desc->buf.offset,
                                                     width, height, fmt);
   if (!sv_pred) {
      IDM_LOG("predstore early-return pred-wrap-failed");
      return false;
   }
   struct pipe_sampler_view *sv_val =
      r300vk_identity_map_wrap_input_as_sampler_view(device, val_buf->resource,
                                                     (unsigned)val_desc->buf.offset,
                                                     width, height, fmt);
   if (!sv_val) {
      pipe_sampler_view_reference(&sv_pred, NULL);
      IDM_LOG("predstore early-return val-wrap-failed");
      return false;
   }

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
      pipe_sampler_view_reference(&sv_val, NULL);
      pipe_sampler_view_reference(&sv_pred, NULL);
      IDM_LOG("predstore early-return rt-create-failed");
      return false;
   }

   /* Seed the render target from out_data's pre-existing contents.  A masked
    * fragment (predicate false) is KILL_IF-discarded and performs no ROP
    * write, so its RT texel keeps this seed -- that is how a masked cell stays
    * untouched.  No pipe->clear is issued: clearing would erase the baseline.
    * Map and per-row memcpy out -> RT honoring the texture transfer stride
    * (the buffer-to-texture direction r300g cannot do via resource_copy_region;
    * the same path the multipass seed uses). */
   {
      struct pipe_transfer *out_xfer = NULL;
      struct pipe_box out_box;
      memset(&out_box, 0, sizeof(out_box));
      out_box.width  = width * height * bpp;
      out_box.height = 1; out_box.depth = 1;
      const void *out_map = pipe->buffer_map(pipe, out_buf->resource, 0,
                                             PIPE_MAP_READ, &out_box, &out_xfer);
      if (!out_map) {
         pipe_resource_reference(&rt, NULL);
         pipe_sampler_view_reference(&sv_val, NULL);
         pipe_sampler_view_reference(&sv_pred, NULL);
         IDM_LOG("predstore early-return out-seed-map-failed");
         return false;
      }
      struct pipe_transfer *rt_xfer = NULL;
      struct pipe_box rt_box;
      memset(&rt_box, 0, sizeof(rt_box));
      rt_box.width = width; rt_box.height = height; rt_box.depth = 1;
      void *rt_seed = pipe->texture_map(pipe, rt, 0,
                                        PIPE_MAP_WRITE |
                                        PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                        &rt_box, &rt_xfer);
      if (!rt_seed) {
         pipe->buffer_unmap(pipe, out_xfer);
         pipe_resource_reference(&rt, NULL);
         pipe_sampler_view_reference(&sv_val, NULL);
         pipe_sampler_view_reference(&sv_pred, NULL);
         IDM_LOG("predstore early-return rt-seed-map-failed");
         return false;
      }
      r300vk_identity_map_copy_rows(rt_seed, rt_xfer->stride,
                                    out_map, width * bpp,
                                    width, height, bpp,
                                    total_invocations);
      pipe->texture_unmap(pipe, rt_xfer);
      pipe->buffer_unmap(pipe, out_xfer);
   }
   IDM_LOG("predstore seeded RT from out");

   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = fmt;
   surf_templ.texture = rt;

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
      pipe_sampler_view_reference(&sv_val, NULL);
      pipe_sampler_view_reference(&sv_pred, NULL);
      IDM_LOG("predstore early-return vbo-create-failed");
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
      pipe_sampler_view_reference(&sv_val, NULL);
      pipe_sampler_view_reference(&sv_pred, NULL);
      return false;
   }

   r300vk_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_blend_cso,
                                        device->identity_map_rasterizer_cso,
                                        device->identity_map_dsa_cso,
                                        pl->vs_cso, pl->fs_cso, velems_cso);
   void *samplers[2] = { device->identity_map_sampler_cso,
                         device->identity_map_sampler_cso };
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 2, samplers);
   struct pipe_sampler_view *views[2] = { sv_pred, sv_val };
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
   IDM_LOG("predstore draw_vbo mode=triangle_strip count=4");
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   pipe->flush(pipe, NULL, 0);
   IDM_LOG("predstore post-flush, beginning rt->buffer copy");

   struct pipe_box copy_box;
   memset(&copy_box, 0, sizeof(copy_box));
   copy_box.width = width; copy_box.height = height; copy_box.depth = 1;
   bool copy_ok = false;
   {
      struct pipe_transfer *rt_xfer = NULL;
      const void *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                             &copy_box, &rt_xfer);
      if (rt_map) {
         struct pipe_transfer *out_xfer = NULL;
         struct pipe_box out_box;
         memset(&out_box, 0, sizeof(out_box));
         out_box.x      = (unsigned)out_desc->buf.offset;
         out_box.width  = width * height * bpp;
         out_box.height = 1; out_box.depth = 1;
         void *out_bytes = pipe->buffer_map(pipe, out_buf->resource, 0,
                                            PIPE_MAP_WRITE |
                                            PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                            &out_box, &out_xfer);
         if (out_bytes) {
            r300vk_identity_map_copy_rows(out_bytes, width * bpp,
                                          rt_map, rt_xfer->stride,
                                          width, height, bpp,
                                          total_invocations);
            pipe->buffer_unmap(pipe, out_xfer);
            copy_ok = true;
         }
         pipe->texture_unmap(pipe, rt_xfer);
      }
   }
   IDM_LOG("predstore copy issued copy_ok=%d", (int)copy_ok);

   struct pipe_sampler_view *no_views[2] = { NULL, NULL };
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 2, no_views);
   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);

   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_sampler_view_reference(&sv_val, NULL);
   pipe_sampler_view_reference(&sv_pred, NULL);
   pipe_resource_reference(&vb, NULL);
   pipe_resource_reference(&rt, NULL);
   IDM_LOG("predstore orchestrator done copy_ok=%d", (int)copy_ok);
   return copy_ok;
}

/* Blend-acc-reduction orchestrator.  Decomposes
 * atomicAdd(out[gid & MASK], in[gid]) into N point fragments accumulating
 * the per-gid value into M bins through RB3D COMB_FCN_ADD blending.
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

   /* Detector binding-index priority with a positional fallback (same policy
    * as r300vk_binary_map_dispatch_replay): value = first STORAGE_BUFFER,
    * output = second, when the detector could not record constant indices. */
   uint32_t value_binding  = pl->blend_acc_reduction.value_ssbo_binding;
   uint32_t output_binding = pl->blend_acc_reduction.output_ssbo_binding;
   if (!idm_recover_in_out_bindings(set, &value_binding, &output_binding)) {
      IDM_LOG("blend_acc early-return layout-has-fewer-than-two-storage-buffers");
      return false;
   }
   uint32_t bindings[2] = { value_binding, output_binding };
   const struct r300vk_descriptor *descs[2] = {0};
   struct r300vk_buffer *bufs[2] = {0};
   if (!r300vk_idm_resolve_buffers(set, 2, bindings, descs, bufs))
      return false;
   const struct r300vk_descriptor *in_desc = descs[0];
   const struct r300vk_descriptor *out_desc = descs[1];
   struct r300vk_buffer *in_buf = bufs[0];
   struct r300vk_buffer *out_buf = bufs[1];

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
   /* Total invocations from the dispatch grid (64-bit product guard +
    * 2048 x 2048 axis cap). */
   const uint64_t total_invocations =
      r300vk_idm_total_invocations(dispatch, pl);
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
   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r300vk_idm_create_blend_acc_vbo(pipe, in_buf->resource, (unsigned)in_desc->buf.offset, N, M, &vb, &velems_cso)) {
      pipe_resource_reference(&rt, NULL);
      IDM_LOG("blend_acc early-return vbo-create-failed");
      return false;
   }

   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = fmt;
   surf_templ.texture = rt;

   r300vk_identity_map_setup_draw_state(pipe, M, 1, &surf_templ,
                                        device->blend_acc_reduction_blend_cso,
                                        device->identity_map_rasterizer_cso,
                                        device->identity_map_dsa_cso,
                                        pl->vs_cso, pl->fs_cso, velems_cso);

   /* Clear the 1xM RT to 0 before the blend-add draw. */
   {
      union pipe_color_union zero;
      memset(&zero, 0, sizeof(zero));
      pipe->clear(pipe, PIPE_CLEAR_COLOR0, ~0u, 0, NULL, &zero, 0.0, 0);
   }

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
   bool copy_ok = false;
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
         out_box.x      = (unsigned)out_desc->buf.offset;
         out_box.width  = (unsigned)out_byte_size;
         out_box.height = 1; out_box.depth = 1;
         void *out_bytes = pipe->buffer_map(pipe, out_buf->resource, 0,
                                            PIPE_MAP_WRITE |
                                            PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                            &out_box, &out_xfer);
         if (out_bytes) {
            r300vk_identity_map_copy_rows(out_bytes, (unsigned)out_byte_size,
                                          rt_map, rt_xfer->stride,
                                          M, 1, 4, M);
            pipe->buffer_unmap(pipe, out_xfer);
            copy_ok = true;
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
   IDM_LOG("blend_acc orchestrator done copy_ok=%d", (int)copy_ok);
   return copy_ok;
}

/* ZPASS coverage-count reduction orchestrator.  Decomposes
 * `if (in_data[gid] != 0u) atomicAdd(count_out, 1u)` into N point fragments
 * whose KILL_IF gates discard the false-predicate fragments; the depth/
 * stencil unit's ZPASS counter (ZB_ZPASS_DATA / ZB_ZPASS_ADDR) counts
 * surviving fragments, exposed to userspace as PIPE_QUERY_OCCLUSION_COUNTER
 * through r300_query.c.
 *
 * Shape differs from blend_acc_reduction_dispatch_replay in three load-
 * bearing places:
 *
 *   1. The output buffer is a single uint32 counter, not a 1xM histogram
 *      RT row.  The RT itself is still 1xN (one pixel per point fragment)
 *      because each draw point needs a distinct rasterization slot to
 *      avoid Z-test deduplication; the RT contents are discarded, only
 *      the ZPASS query result matters.
 *   2. The VBO entries carry (vec2 pos, float predicate) instead of
 *      (vec2 pos, packed-RGBA8 value).  The CPU stage reads in_data[gid]
 *      and bakes 1.0f if (in_data[gid] != 0u) else 0.0f.
 *   3. A pipe_query (PIPE_QUERY_OCCLUSION_COUNTER) brackets the draw;
 *      get_query_result(wait=true) returns the u64 sum of surviving
 *      fragments, truncated to u32 and written to count_out[0].
 *
 * No blend (only the ZPASS path matters); RT clear unnecessary (RT
 * contents never read).  Other surfaces reuse the identity-map
 * orchestrator's shape verbatim. */
bool
r300vk_zpass_reduction_dispatch_replay(struct r300vk_device *device,
                                       const struct r300vk_pipeline *pl,
                                       const struct r300vk_cmd_dispatch *dispatch,
                                       const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   IDM_LOG("zpass entry pl=%p is_zpass=%d set_count=%u gx=%u gy=%u gz=%u",
           (const void *)pl,
           pl ? (int)pl->zpass_reduction.is_zpass_reduction : -1,
           binds ? binds->set_count : 0,
           dispatch ? dispatch->group_count_x : 0,
           dispatch ? dispatch->group_count_y : 0,
           dispatch ? dispatch->group_count_z : 0);
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0) {
      IDM_LOG("zpass early-return null-or-empty-binds");
      return false;
   }
   if (!pipe->create_query || !pipe->begin_query || !pipe->end_query ||
       !pipe->get_query_result || !pipe->destroy_query) {
      IDM_LOG("zpass early-return pipe-query-vtable-missing");
      return false;
   }
   if (!pl->vs_cso || !pl->fs_cso) {
      IDM_LOG("zpass early-return no-vs-or-fs-cso");
      return false;
   }
   if (binds->first_set != 0) {
      IDM_LOG("zpass early-return first_set=%u (only slot 0 supported)",
              binds->first_set);
      return false;
   }
   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("zpass early-return no-set-or-layout");
      return false;
   }
   uint32_t value_binding  = pl->zpass_reduction.value_ssbo_binding;
   uint32_t output_binding = pl->zpass_reduction.output_ssbo_binding;
   if (!idm_recover_in_out_bindings(set, &value_binding, &output_binding)) {
      IDM_LOG("zpass early-return layout-has-fewer-than-two-storage-buffers");
      return false;
   }
   uint32_t bindings[2] = { value_binding, output_binding };
   const struct r300vk_descriptor *descs[2] = {0};
   struct r300vk_buffer *bufs[2] = {0};
   if (!r300vk_idm_resolve_buffers(set, 2, bindings, descs, bufs))
      return false;
   const struct r300vk_descriptor *in_desc = descs[0];
   const struct r300vk_descriptor *out_desc = descs[1];
   struct r300vk_buffer *in_buf = bufs[0];
   struct r300vk_buffer *out_buf = bufs[1];

   /* Output buffer must hold at least one uint32.  Excess capacity is
    * fine -- the orchestrator only writes the first 4 bytes (the
    * surviving-fragment count). */
   const uint64_t out_byte_size = out_buf->size;
   if (out_byte_size < sizeof(uint32_t)) {
      IDM_LOG("zpass early-return output-too-small=%llu",
              (unsigned long long)out_byte_size);
      return false;
   }
   uint64_t total_invocations = 0;
   unsigned width = 0, height = 0;
   if (!r300vk_idm_compute_raster_grid(dispatch, pl, &total_invocations, &width, &height))
      return false;
   const uint32_t N = (uint32_t)total_invocations;
   IDM_LOG("dispatch N=%u", N);

   const enum pipe_format fmt = PIPE_FORMAT_R8G8B8A8_UNORM;

   /* 1 x N RT: one rasterization slot per gid so distinct (x, 0) point
    * fragments do not collapse under per-pixel deduplication.  RT
    * contents are unused; only the ZPASS counter matters. */
   struct pipe_resource rt_templ;
   memset(&rt_templ, 0, sizeof(rt_templ));
   rt_templ.target     = PIPE_TEXTURE_2D;
   rt_templ.format     = fmt;
   rt_templ.width0     = N;
   rt_templ.height0    = 1;
   rt_templ.depth0     = 1;
   rt_templ.array_size = 1;
   rt_templ.usage      = PIPE_USAGE_DEFAULT;
   rt_templ.bind       = PIPE_BIND_RENDER_TARGET;
   struct pipe_resource *rt = screen->resource_create(screen, &rt_templ);
   if (!rt) {
      IDM_LOG("zpass early-return rt-create-failed");
      return false;
   }
   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r300vk_idm_create_zpass_vbo(pipe, in_buf->resource, (unsigned)in_desc->buf.offset, N, &vb, &velems_cso)) {
      pipe_resource_reference(&rt, NULL);
      IDM_LOG("zpass early-return vbo-create-failed");
      return false;
   }

   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = fmt;
   surf_templ.texture = rt;

   r300vk_identity_map_setup_draw_state(pipe, N, 1, &surf_templ,
                                        device->identity_map_blend_cso,
                                        device->identity_map_rasterizer_cso,
                                        device->identity_map_dsa_cso,
                                        pl->vs_cso, pl->fs_cso, velems_cso);

   struct pipe_vertex_buffer vb_state;
   memset(&vb_state, 0, sizeof(vb_state));
   vb_state.buffer.resource = vb;
   pipe->set_vertex_buffers(pipe, 1, &vb_state);

   /* Bracket the draw with PIPE_QUERY_OCCLUSION_COUNTER.  r300_query.c
    * wraps the ZB ZPASS register pair behind this query type and returns
    * a uint64 fragment sum via get_query_result. */
   struct pipe_query *q = pipe->create_query(
      pipe, PIPE_QUERY_OCCLUSION_COUNTER, 0);
   if (!q) {
      pipe->set_vertex_buffers(pipe, 0, NULL);
      struct pipe_framebuffer_state empty_fb;
      memset(&empty_fb, 0, sizeof(empty_fb));
      pipe->set_framebuffer_state(pipe, &empty_fb);
      pipe->delete_vertex_elements_state(pipe, velems_cso);
      pipe_resource_reference(&vb, NULL);
      pipe_resource_reference(&rt, NULL);
      IDM_LOG("zpass early-return create_query-failed");
      return false;
   }
   if (!pipe->begin_query(pipe, q)) {
      pipe->destroy_query(pipe, q);
      pipe->set_vertex_buffers(pipe, 0, NULL);
      struct pipe_framebuffer_state empty_fb;
      memset(&empty_fb, 0, sizeof(empty_fb));
      pipe->set_framebuffer_state(pipe, &empty_fb);
      pipe->delete_vertex_elements_state(pipe, velems_cso);
      pipe_resource_reference(&vb, NULL);
      pipe_resource_reference(&rt, NULL);
      IDM_LOG("zpass early-return begin_query-failed");
      return false;
   }

   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode           = MESA_PRIM_POINTS;
   info.instance_count = 1;
   info.max_index      = N - 1;
   struct pipe_draw_start_count_bias draw = { .start = 0, .count = N };
   IDM_LOG("zpass draw_vbo mode=points count=%u", N);
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   pipe->end_query(pipe, q);
   pipe->flush(pipe, NULL, 0);

   union pipe_query_result qr;
   memset(&qr, 0, sizeof(qr));
   const bool query_ok = pipe->get_query_result(pipe, q, true /* wait */, &qr);
   pipe->destroy_query(pipe, q);
   if (!query_ok) {
      pipe->set_vertex_buffers(pipe, 0, NULL);
      struct pipe_framebuffer_state empty_fb;
      memset(&empty_fb, 0, sizeof(empty_fb));
      pipe->set_framebuffer_state(pipe, &empty_fb);
      pipe->delete_vertex_elements_state(pipe, velems_cso);
      pipe_resource_reference(&vb, NULL);
      pipe_resource_reference(&rt, NULL);
      IDM_LOG("zpass early-return get_query_result-failed");
      return false;
   }
   /* Saturate u64->u32: the ZPASS counter on RS482 is a per-pipe u32
    * accumulator that the kernel sums across pipes into a u64; for the
    * first-cut probe N is bounded at 2048*2048, well under UINT32_MAX. */
   const uint64_t raw_sum = qr.u64;
   const uint32_t saturated = (raw_sum > UINT32_MAX)
      ? UINT32_MAX : (uint32_t)raw_sum;
   IDM_LOG("zpass query u64=%llu saturated_u32=%u",
           (unsigned long long)raw_sum, saturated);

   bool copy_ok = false;
   {
      struct pipe_box out_box;
      memset(&out_box, 0, sizeof(out_box));
      out_box.x      = (unsigned)out_desc->buf.offset;
      out_box.width  = (unsigned)sizeof(uint32_t);
      out_box.height = 1; out_box.depth = 1;
      struct pipe_transfer *out_xfer = NULL;
      void *out_bytes = pipe->buffer_map(pipe, out_buf->resource, 0,
                                         PIPE_MAP_WRITE |
                                         PIPE_MAP_DISCARD_RANGE,
                                         &out_box, &out_xfer);
      if (out_bytes) {
         memcpy(out_bytes, &saturated, sizeof(uint32_t));
         pipe->buffer_unmap(pipe, out_xfer);
         copy_ok = true;
      }
   }

   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);
   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_resource_reference(&vb, NULL);
   pipe_resource_reference(&rt, NULL);
   IDM_LOG("zpass orchestrator done copy_ok=%d", (int)copy_ok);
   return copy_ok;
}

/* Multipass FBO ping-pong scan orchestrator.  Realizes the per-element
 * self-iterated kernel `x = in[gid]; for (k < pass_count) x = x * 2u;
 * out[gid] = x` as pass_count dependent fragment passes: two RGBA8 textures
 * alternate as sampler source and render target, each pass doubling the texel
 * the synthesised FS samples from the prior pass's output.  The substrate verb
 * is multipass FBO ping-pong.
 *
 * Differs from the single-pass identity-map orchestrator in two places:
 *
 *   1. pass_count is read from a third storage buffer (binding 2) at
 *      replay time -- the runtime value the kernel's loop bound carries,
 *      which is also what the multipass-scan detector keyed on.  No
 *      push-constant plumbing exists (r300vk advertises maxPushConstantsSize
 *      but has no R300VK_CMD_PUSH_CONSTANTS recording path), so the count
 *      rides the existing descriptor machinery.
 *   2. Two textures alternate src/dst across pass_count draws; the prior
 *      pass's RT becomes the next pass's sampler input.
 *
 * Bounds: pass_count is clamped to [0, 16].  pass_count == 0 copies the
 * input straight through (zero doublings).  Above 16 the per-byte UNORM8
 * doubling would saturate for any non-zero input, so the orchestrator
 * rejects rather than silently clamp (the read-back oracle would otherwise
 * see saturated bytes). */
bool
r300vk_multipass_scan_dispatch_replay(struct r300vk_device *device,
                                      const struct r300vk_pipeline *pl,
                                      const struct r300vk_cmd_dispatch *dispatch,
                                      const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   IDM_LOG("multipass entry pl=%p is_multipass=%d set_count=%u gx=%u gy=%u gz=%u",
           (const void *)pl,
           pl ? (int)pl->multipass_scan.is_multipass_scan : -1,
           binds ? binds->set_count : 0,
           dispatch ? dispatch->group_count_x : 0,
           dispatch ? dispatch->group_count_y : 0,
           dispatch ? dispatch->group_count_z : 0);
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0) {
      IDM_LOG("multipass early-return null-or-empty-binds");
      return false;
   }
   if (!pl->vs_cso || !pl->fs_cso) {
      IDM_LOG("multipass early-return no-vs-or-fs-cso");
      return false;
   }
   if (binds->first_set != 0) {
      IDM_LOG("multipass early-return first_set=%u (only slot 0 supported)",
              binds->first_set);
      return false;
   }
   const struct r300vk_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("multipass early-return no-set-or-layout");
      return false;
   }

   /* Positional binding resolution (binding 0 = input data, 1 = output,
    * 2 = params holding pass_count) -- same convention as the binary-map and
    * blend-acc layout fallback; the detector's binding fields stay 0
    * post-explicit_io. */
   uint32_t in_binding = 0, out_binding = 0, params_binding = 0;
   if (!nth_storage_buffer_binding(set, 0, &in_binding) ||
       !nth_storage_buffer_binding(set, 1, &out_binding) ||
       !nth_storage_buffer_binding(set, 2, &params_binding)) {
      IDM_LOG("multipass early-return layout-has-fewer-than-three-storage-buffers");
      return false;
   }
   const struct r300vk_descriptor *in_desc =
      find_descriptor_by_binding(set, in_binding);
   const struct r300vk_descriptor *out_desc =
      find_descriptor_by_binding(set, out_binding);
   const struct r300vk_descriptor *params_desc =
      find_descriptor_by_binding(set, params_binding);
   if (!in_desc || !out_desc || !params_desc ||
       !in_desc->buf.buffer || !out_desc->buf.buffer ||
       !params_desc->buf.buffer) {
      IDM_LOG("multipass early-return descriptor-walk-miss");
      return false;
   }
   VK_FROM_HANDLE(r300vk_buffer, in_buf,     in_desc->buf.buffer);
   VK_FROM_HANDLE(r300vk_buffer, out_buf,    out_desc->buf.buffer);
   VK_FROM_HANDLE(r300vk_buffer, params_buf, params_desc->buf.buffer);
   if (!in_buf || !out_buf || !params_buf ||
       !in_buf->resource || !out_buf->resource || !params_buf->resource) {
      IDM_LOG("multipass early-return null-pipe-resource");
      return false;
   }

   /* Read pass_count (first uint32 of the params buffer). */
   uint32_t pass_count = 0;
   {
      struct pipe_transfer *p_xfer = NULL;
      struct pipe_box p_box;
      memset(&p_box, 0, sizeof(p_box));
      p_box.x      = (unsigned)params_desc->buf.offset;
      p_box.width = (unsigned)sizeof(uint32_t);
      p_box.height = 1; p_box.depth = 1;
      const void *p_map = pipe->buffer_map(pipe, params_buf->resource, 0,
                                           PIPE_MAP_READ, &p_box, &p_xfer);
      if (!p_map) {
         IDM_LOG("multipass early-return params-map-failed");
         return false;
      }
      memcpy(&pass_count, p_map, sizeof(uint32_t));
      pipe->buffer_unmap(pipe, p_xfer);
   }
   if (pass_count > 16) {
      IDM_LOG("multipass early-return pass_count=%u exceeds-unorm8-envelope", pass_count);
      return false;
   }
   IDM_LOG("multipass pass_count=%u", pass_count);

   const uint64_t total_invocations =
      r300vk_idm_total_invocations(dispatch, pl);
   if (total_invocations == 0 || total_invocations > 2048u * 2048u) {
      IDM_LOG("multipass early-return total_invocations=%llu out-of-bounds",
              (unsigned long long)total_invocations);
      return false;
   }
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total_invocations, &width, &height);
   if (width > 2048 || height > 2048) {
      IDM_LOG("multipass early-return extent-exceeds-2048-cap");
      return false;
   }

   const enum pipe_format fmt = PIPE_FORMAT_R8G8B8A8_UNORM;
   const unsigned bpp = util_format_get_blocksize(fmt);

   /* Two textures alternating as sampler source / render target.  Each is
    * both RENDER_TARGET (as the pass's dst) and SAMPLER_VIEW (as the next
    * pass's src). */
   struct pipe_resource tex_templ;
   memset(&tex_templ, 0, sizeof(tex_templ));
   tex_templ.target     = PIPE_TEXTURE_2D;
   tex_templ.format     = fmt;
   tex_templ.width0     = width;
   tex_templ.height0    = height;
   tex_templ.depth0     = 1;
   tex_templ.array_size = 1;
   tex_templ.usage      = PIPE_USAGE_DEFAULT;
   tex_templ.bind       = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
   struct pipe_resource *tex[2] = {
      screen->resource_create(screen, &tex_templ),
      screen->resource_create(screen, &tex_templ),
   };
   if (!tex[0] || !tex[1]) {
      pipe_resource_reference(&tex[0], NULL);
      pipe_resource_reference(&tex[1], NULL);
      IDM_LOG("multipass early-return tex-create-failed");
      return false;
   }

   /* Seed tex[0] with the input buffer bytes (map + per-row memcpy, the
    * same PIPE_BUFFER->PIPE_TEXTURE_2D path the identity-map input wrap
    * uses; resource_copy_region cannot do the buffer->texture direction on
    * r300g). */
   {
      struct pipe_transfer *in_xfer = NULL;
      struct pipe_box in_box;
      memset(&in_box, 0, sizeof(in_box));
      in_box.x      = (unsigned)in_desc->buf.offset;
      in_box.width  = width * height * bpp;
      in_box.height = 1; in_box.depth = 1;
      const void *in_map = pipe->buffer_map(pipe, in_buf->resource, 0,
                                            PIPE_MAP_READ, &in_box, &in_xfer);
      if (!in_map) {
         pipe_resource_reference(&tex[0], NULL);
         pipe_resource_reference(&tex[1], NULL);
         IDM_LOG("multipass early-return in-map-failed");
         return false;
      }
      struct pipe_transfer *t_xfer = NULL;
      struct pipe_box t_box;
      memset(&t_box, 0, sizeof(t_box));
      t_box.width = width; t_box.height = height; t_box.depth = 1;
      void *t_map = pipe->texture_map(pipe, tex[0], 0,
                                      PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                      &t_box, &t_xfer);
      if (!t_map) {
         pipe->buffer_unmap(pipe, in_xfer);
         pipe_resource_reference(&tex[0], NULL);
         pipe_resource_reference(&tex[1], NULL);
         IDM_LOG("multipass early-return seed-tex-map-failed");
         return false;
      }
      r300vk_identity_map_copy_rows(t_map, t_xfer->stride,
                                    in_map, width * bpp,
                                    width, height, bpp,
                                    total_invocations);
      pipe->texture_unmap(pipe, t_xfer);
      pipe->buffer_unmap(pipe, in_xfer);
   }

   /* Fullscreen-quad VBO (pos.xy, texcoord.xy), identical to the
    * identity-map orchestrator's quad. */
   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r300vk_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_resource_reference(&tex[0], NULL);
      pipe_resource_reference(&tex[1], NULL);
      return false;
   }

   /* State that is constant across all passes: blend off, no cull, depth
    * off, NEAREST sampler, the doubling FS + passthrough VS, the velems,
    * the fullscreen VB, viewport, scissor. */
   pipe->bind_blend_state(pipe, device->identity_map_blend_cso);
   pipe->bind_rasterizer_state(pipe, device->identity_map_rasterizer_cso);
   pipe->bind_depth_stencil_alpha_state(pipe, device->identity_map_dsa_cso);
   pipe->bind_vs_state(pipe, pl->vs_cso);
   pipe->bind_fs_state(pipe, pl->fs_cso);
   pipe->bind_vertex_elements_state(pipe, velems_cso);
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1,
                             &device->identity_map_sampler_cso);
   struct pipe_vertex_buffer vb_state;
   memset(&vb_state, 0, sizeof(vb_state));
   vb_state.buffer.resource = vb;
   pipe->set_vertex_buffers(pipe, 1, &vb_state);

   struct pipe_viewport_state vp;
   memset(&vp, 0, sizeof(vp));
   vp.scale[0] = (float)width * 0.5f; vp.scale[1] = (float)height * 0.5f;
   vp.scale[2] = 0.5f;
   vp.translate[0] = (float)width * 0.5f; vp.translate[1] = (float)height * 0.5f;
   vp.translate[2] = 0.5f;
   pipe->set_viewport_states(pipe, 0, 1, &vp);
   struct pipe_scissor_state sc = {0};
   sc.maxx = width; sc.maxy = height;
   pipe->set_scissor_states(pipe, 0, 1, &sc);

   /* The ping-pong loop.  tex[src_idx] holds the current value; each pass
    * samples it, doubles, and writes tex[src_idx ^ 1], which becomes the
    * next pass's source.  After pass_count passes the result is in
    * tex[pass_count & 1]; pass_count == 0 leaves it in tex[0] (= input). */
   unsigned src_idx = 0;
   bool passes_ok = true;
   for (uint32_t k = 0; k < pass_count; k++) {
      const unsigned dst_idx = src_idx ^ 1u;

      struct pipe_sampler_view sv_templ;
      memset(&sv_templ, 0, sizeof(sv_templ));
      sv_templ.format            = fmt;
      sv_templ.target            = PIPE_TEXTURE_2D;
      sv_templ.u.tex.first_layer = 0;
      sv_templ.u.tex.last_layer  = 0;
      sv_templ.u.tex.first_level = 0;
      sv_templ.u.tex.last_level  = 0;
      sv_templ.swizzle_r = PIPE_SWIZZLE_X;
      sv_templ.swizzle_g = PIPE_SWIZZLE_Y;
      sv_templ.swizzle_b = PIPE_SWIZZLE_Z;
      sv_templ.swizzle_a = PIPE_SWIZZLE_W;
      struct pipe_sampler_view *sv =
         pipe->create_sampler_view(pipe, tex[src_idx], &sv_templ);
      if (!sv) {
         IDM_LOG("multipass pass=%u sampler-view-failed (fail closed)", k);
         passes_ok = false;
         break;
      }

      struct pipe_surface surf_templ;
      memset(&surf_templ, 0, sizeof(surf_templ));
      surf_templ.format  = fmt;
      surf_templ.texture = tex[dst_idx];
      struct pipe_framebuffer_state fb;
      memset(&fb, 0, sizeof(fb));
      fb.width = width; fb.height = height;
      fb.nr_cbufs = 1; fb.cbufs[0] = surf_templ;
      pipe->set_framebuffer_state(pipe, &fb);

      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 1, 0, &sv);

      struct pipe_draw_info info;
      memset(&info, 0, sizeof(info));
      info.mode = MESA_PRIM_TRIANGLE_STRIP;
      info.instance_count = 1;
      info.max_index = 3;
      struct pipe_draw_start_count_bias draw = { .start = 0, .count = 4 };
      pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
      pipe->flush(pipe, NULL, 0);

      struct pipe_sampler_view *no_view = NULL;
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 1, &no_view);
      pipe_sampler_view_reference(&sv, NULL);
      IDM_LOG("multipass pass=%u src=%u dst=%u done", k, src_idx, dst_idx);
      src_idx = dst_idx;
   }

   /* Copy the final texture (tex[src_idx]) back to the output buffer. */
   bool copy_ok = r300vk_identity_map_readback_rt(pipe, tex[src_idx], out_buf->resource,
                                                  (unsigned)out_desc->buf.offset,
                                                  width, height, fmt,
                                                  width * util_format_get_blocksize(fmt));

   IDM_LOG("multipass copy issued final_tex=%u", src_idx);

   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);
   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_resource_reference(&vb, NULL);
   pipe_resource_reference(&tex[0], NULL);
   pipe_resource_reference(&tex[1], NULL);
   IDM_LOG("multipass orchestrator done passes_ok=%d copy_ok=%d",
           (int)passes_ok, (int)copy_ok);
   return passes_ok && copy_ok;
}
