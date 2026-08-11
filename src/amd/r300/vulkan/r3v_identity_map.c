/*
 * SPDX-License-Identifier: MIT
 *
 * Identity-map compute-as-raster lowering primitives.  Each helper turns
 * one Vulkan compute API concept (a bound storage buffer, a dispatch grid)
 * into the pipe_context calls the r300g replay path expects.
 */

#include "r3v_identity_map.h"
#include "r3v_device.h"
#include "r3v_pipeline.h"
#include "r3v_descriptor.h"
#include "r3v_buffer.h"
#include "r3v_cmd_buffer.h"

#include "r300/r300_grid_fold.h"

#include "compiler/shader_enums.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "compiler/nir/nir_opcodes.h"
#include "util/format/u_format.h"
#include "util/log.h"
#include "util/u_inlines.h"
#include "util/u_surface.h"

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define IDM_LOG(fmt, ...) \
   do { \
      if (device && device->dbg_identity_map) \
         mesa_logi("r3v: ident_map: " fmt, ##__VA_ARGS__); \
   } while (0)

static bool
r3v_idm_exact_opt_in_enabled(const char *env_name, const char *legacy_name,
                             const char *expected)
{
   /* Canonical R3V_ name first, then pre-rename R300VK_ for retained runbooks. */
   const char *gate = r3v_getenv_compat(env_name, legacy_name);
   return gate && strcmp(gate, expected) == 0;
}

static bool
r3v_idm_format_supported(struct pipe_screen *screen, enum pipe_format fmt)
{
   return screen &&
          screen->is_format_supported(screen, fmt, PIPE_TEXTURE_2D, 0, 0,
                                      PIPE_BIND_SAMPLER_VIEW) &&
          screen->is_format_supported(screen, fmt, PIPE_TEXTURE_2D, 0, 0,
                                      PIPE_BIND_RENDER_TARGET);
}

static enum pipe_format
r3v_identity_map_replay_format(struct r3v_device *device,
                                  const struct r3v_pipeline *pl)
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
       r3v_idm_exact_opt_in_enabled(R3V_IDENTITY_MAP_FP32X4_ENV,
                                       "R300VK_IDENTITY_MAP_FP32X4_EXPERIMENTAL",
                                       R3V_IDENTITY_MAP_FP32X4_ENV_VALUE) &&
       pl->identity_map.value_components == 4 &&
       pl->identity_map.value_bit_size == 32 &&
       r3v_idm_format_supported(device->screen,
                                   PIPE_FORMAT_R32G32B32A32_FLOAT)) {
      IDM_LOG("using experimental fp32x4 identity carrier");
      return PIPE_FORMAT_R32G32B32A32_FLOAT;
   }

   return PIPE_FORMAT_R8G8B8A8_UNORM;
}

static void
r3v_identity_map_copy_rows(void *dst_map, unsigned dst_stride,
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

/* Copy the rendered RT rows back into the output SSBO, resolving the three carrier
 * shapes the dispatch-replay cores use.  Shared by the one-in and two-in cores so
 * both gain the same readback semantics from one place:
 *   buf_fmt == fmt        raw byte copy (the encode-into-RT patterns, where the
 *                         RT format already is the output element format);
 *   buf_fmt == R32_FLOAT  scalar carrier -- util_format_unpack_rgba always yields
 *                         four floats per pixel, so unpack each row to a staging
 *                         buffer and gather the X lane at the 4-byte stride;
 *   otherwise             the vec4 float path -- unpack each row straight to the
 *                         R32G32B32A32 output (buf_bs == 16).
 * Returns false only when the scalar staging allocation fails. */
static bool
r3v_idm_copy_rt_rows_to_buffer(void *out_bytes, const uint8_t *rt_map,
                                  unsigned rt_stride, unsigned width,
                                  unsigned height, uint64_t total_invocations,
                                  enum pipe_format fmt, enum pipe_format buf_fmt,
                                  unsigned buf_bs)
{
   if (buf_fmt == fmt) {
      r3v_identity_map_copy_rows(out_bytes, width * buf_bs, rt_map, rt_stride,
                                    width, height, buf_bs, total_invocations);
      return true;
   }
   if (buf_fmt == PIPE_FORMAT_R32_FLOAT) {
      float *row_rgba = malloc((size_t)width * 4 * sizeof(float));
      if (!row_rgba)
         return false;
      float *dst = out_bytes;
      const uint8_t *src = rt_map;
      uint64_t remaining = total_invocations;
      for (unsigned r = 0; r < height && remaining; r++) {
         unsigned n = remaining < width ? (unsigned)remaining : width;
         util_format_unpack_rgba(fmt, row_rgba, src, n);
         for (unsigned i = 0; i < n; i++)
            dst[i] = row_rgba[4 * i];
         dst += n;
         src += rt_stride;
         remaining -= n;
      }
      free(row_rgba);
      return true;
   }
   uint8_t *dst = out_bytes;
   const uint8_t *src = rt_map;
   uint64_t remaining = total_invocations;
   for (unsigned r = 0; r < height && remaining; r++) {
      unsigned n = remaining < width ? (unsigned)remaining : width;
      util_format_unpack_rgba(fmt, dst, src, n);
      dst += (size_t)n * buf_bs;
      src += rt_stride;
      remaining -= n;
   }
   return true;
}


/* Forward declaration used by buffer-resolution helpers. */
static const struct r3v_descriptor *
find_descriptor_by_binding(const struct r3v_descriptor_set *set,
                           uint32_t binding_index);

static bool
r3v_idm_resolve_buffers(struct r3v_device *device,
                           const struct r3v_descriptor_set *set,
                           uint32_t count,
                           const uint32_t *bindings,
                           const struct r3v_descriptor **descs,
                           struct r3v_buffer **bufs)
{
   for (uint32_t i = 0; i < count; i++) {
      descs[i] = find_descriptor_by_binding(set, bindings[i]);
      if (!descs[i] || !descs[i]->buf.buffer) {
         IDM_LOG("early-return descriptor-walk-miss (binding=%u)", bindings[i]);
         return false;
      }
      VK_FROM_HANDLE(r3v_buffer, buf, descs[i]->buf.buffer);
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
r3v_idm_total_invocations(const struct r3v_cmd_dispatch *dispatch,
                             const struct r3v_pipeline *pl)
{
   const uint64_t lsx = pl->local_size_x ? pl->local_size_x : 1u;
   const uint64_t lsy = pl->local_size_y ? pl->local_size_y : 1u;
   const uint64_t lsz = pl->local_size_z ? pl->local_size_z : 1u;
   const uint64_t factors[6] = {
      dispatch->group_count_x, lsx,
      dispatch->group_count_y, lsy,
      dispatch->group_count_z, lsz,
   };
   uint64_t total = 1;

   for (unsigned i = 0; i < ARRAY_SIZE(factors); i++) {
      if (factors[i] != 0 && total > UINT64_MAX / factors[i])
         return 0;
      total *= factors[i];
   }

   return total;
}

static bool
r3v_idm_element_byte_count(uint64_t total_elements, unsigned blocksize,
                              uint64_t *out_bytes)
{
   if (blocksize != 0 && total_elements > UINT64_MAX / blocksize)
      return false;
   const uint64_t total_bytes = total_elements * blocksize;
   if (total_bytes > INT_MAX)
      return false;
   *out_bytes = total_bytes;
   return true;
}

static unsigned
r3v_idm_buffer_write_flags(unsigned byte_offset, uint64_t total_bytes,
                              const struct pipe_resource *buffer)
{
   if (byte_offset == 0 && total_bytes >= buffer->width0)
      return PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE;
   return PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE;
}

static void
r3v_set_index_reject(const char **out_reason, const char *reason)
{
   if (out_reason)
      *out_reason = reason;
}

static bool
r3v_affine_iota_dispatch_shape_exact(const struct r3v_pipeline *pl,
                                        const struct r3v_cmd_dispatch *dispatch,
                                        const char **out_reason)
{
   if (!pl->affine_iota.is_affine_iota)
      return true;

   const uint64_t tx = (uint64_t)dispatch->group_count_x *
                       (pl->local_size_x ? pl->local_size_x : 1u);
   const uint64_t ty = (uint64_t)dispatch->group_count_y *
                       (pl->local_size_y ? pl->local_size_y : 1u);
   const uint64_t tz = (uint64_t)dispatch->group_count_z *
                       (pl->local_size_z ? pl->local_size_z : 1u);
   if (!tx || !ty || !tz) {
      r3v_set_index_reject(out_reason, "empty-grid");
      return false;
   }

   const uint32_t stride = pl->affine_iota.stride;
   const bool is_3d = pl->affine_iota.stride_y ||
                      pl->affine_iota.stride_z;
   if (pl->affine_iota.output_offset_stride != 4 ||
       pl->affine_iota.output_offset_offset != 0) {
      r3v_set_index_reject(out_reason,
                              "affine-iota output offset is not out[gid]");
      return false;
   }

   if (is_3d) {
      if ((uint64_t)pl->affine_iota.stride_y != (uint64_t)stride * tx ||
          (uint64_t)pl->affine_iota.stride_z != (uint64_t)stride * tx * ty ||
          (uint64_t)pl->affine_iota.output_offset_stride_y != 4ull * tx ||
          (uint64_t)pl->affine_iota.output_offset_stride_z !=
             4ull * tx * ty) {
         r3v_set_index_reject(out_reason,
                                 "affine-iota non-canonical 3D flatten");
         return false;
      }
      struct r300_grid_fold fold;
      if (tx > UINT32_MAX || ty > UINT32_MAX || tz > UINT32_MAX ||
          !r300_grid_fold_3d((uint32_t)tx, (uint32_t)ty, (uint32_t)tz,
                             &fold)) {
         r3v_set_index_reject(out_reason,
                                 "raster-fold (affine-iota 3D grid)");
         return false;
      }
      return true;
   }

   if (pl->affine_iota.output_offset_stride_y ||
       pl->affine_iota.output_offset_stride_z) {
      r3v_set_index_reject(out_reason,
                              "affine-iota 1D output offset has y/z stride");
      return false;
   }
   if (ty > 1 || tz > 1) {
      r3v_set_index_reject(out_reason,
                              "affine-iota non-1D dispatch");
      return false;
   }
   struct r300_grid_fold fold;
   if (tx > UINT32_MAX || !r300_grid_fold_1d(tx, &fold)) {
      r3v_set_index_reject(out_reason,
                              "raster-fold (affine-iota 1D grid)");
      return false;
   }
   return true;
}

static bool
r3v_const_fill_output_offset_exact(const struct r3v_pipeline *pl,
                                      const struct r3v_cmd_dispatch *dispatch,
                                      const char **out_reason)
{
   const struct r300_compute_index_pattern *ip = &pl->index_consumption;
   if (!ip->store_offset_valid || !ip->store_offset_global_invocation_only ||
       ip->store_offset_stride != 4 || ip->store_offset_offset != 0) {
      r3v_set_index_reject(out_reason,
                              "const-fill output offset is not out[gid]");
      return false;
   }

   if (!ip->store_offset_stride_y && !ip->store_offset_stride_z)
      return true;

   const uint64_t tx = (uint64_t)dispatch->group_count_x *
                       (pl->local_size_x ? pl->local_size_x : 1u);
   const uint64_t ty = (uint64_t)dispatch->group_count_y *
                       (pl->local_size_y ? pl->local_size_y : 1u);
   const uint64_t tz = (uint64_t)dispatch->group_count_z *
                       (pl->local_size_z ? pl->local_size_z : 1u);
   if (!tx || !ty || !tz) {
      r3v_set_index_reject(out_reason, "empty-grid");
      return false;
   }

   if (tx > UINT64_MAX / 4ull || tx > UINT64_MAX / ty ||
       tx * ty > UINT64_MAX / 4ull) {
      r3v_set_index_reject(out_reason,
                              "const-fill output offset overflows");
      return false;
   }

   const uint64_t expected_y = 4ull * tx;
   const uint64_t expected_z = 4ull * tx * ty;
   if ((ip->store_offset_stride_y &&
        (uint64_t)ip->store_offset_stride_y != expected_y) ||
       (ip->store_offset_stride_z &&
        (uint64_t)ip->store_offset_stride_z != expected_z) ||
       (ty > 1 && (uint64_t)ip->store_offset_stride_y != expected_y) ||
       (tz > 1 && (uint64_t)ip->store_offset_stride_z != expected_z)) {
      r3v_set_index_reject(out_reason,
                              "const-fill non-canonical output flatten");
      return false;
   }

   return true;
}

bool
r3v_dispatch_index_exact(const struct r3v_pipeline *pl,
                            const struct r3v_cmd_dispatch *dispatch,
                            const char **out_reason)
{
   r3v_set_index_reject(out_reason, NULL);
   if (!pl || !dispatch) {
      r3v_set_index_reject(out_reason, "null dispatch state");
      return false;
   }
   const struct r300_compute_index_pattern *ip = &pl->index_consumption;
   if (pl->const_fill.is_const_fill)
      return r3v_const_fill_output_offset_exact(pl, dispatch, out_reason);

   const uint64_t total = r3v_idm_total_invocations(dispatch, pl);
   enum r300_grid_index_class cls;
   uint32_t stride = 1, offset = 0;

   switch (ip->consumption) {
   case R300_COMPUTE_INDEX_NONE:
      cls = R300_GRID_INDEX_NONE;
      break;
   case R300_COMPUTE_INDEX_ADDRESS_ONLY:
      cls = R300_GRID_INDEX_COORD;
      break;
   case R300_COMPUTE_INDEX_VALUE_AFFINE:
      stride = ip->stride_valid ? ip->stride : 1;
      offset = ip->stride_valid ? ip->offset : 0;
      cls = (stride == 1 && offset == 0) ? R300_GRID_INDEX_LINEAR
                                         : R300_GRID_INDEX_STRIDED;
      break;
   case R300_COMPUTE_INDEX_VALUE_AFFINE_3D: {
      /* Per-axis maximum of ax * x + ay * y + az * z + b over the dispatch
       * grid.  Under the canonical flatten the replay validates
       * (ay == ax * tx, az == ax * tx * ty) this equals the 1D strided
       * bound ax * (total - 1) + b exactly; non-canonical strides are
       * bounded here all the same and then refused at replay. */
      const uint64_t tx = (uint64_t)dispatch->group_count_x *
                          (pl->local_size_x ? pl->local_size_x : 1u);
      const uint64_t ty = (uint64_t)dispatch->group_count_y *
                          (pl->local_size_y ? pl->local_size_y : 1u);
      const uint64_t tz = (uint64_t)dispatch->group_count_z *
                          (pl->local_size_z ? pl->local_size_z : 1u);
      if (!tx || !ty || !tz) {
         r3v_set_index_reject(out_reason, "empty-grid");
         return false;
      }
      const uint64_t max_value = (uint64_t)ip->stride * (tx - 1) +
                                 (uint64_t)ip->stride_y * (ty - 1) +
                                 (uint64_t)ip->stride_z * (tz - 1) +
                                 ip->offset;
      if (max_value > R300_FP24_EXACT_INT_CEILING) {
         r3v_set_index_reject(out_reason,
            "index-ceiling (materialized 3D index exceeds FP24 2^17)");
         return false;
      }
      return r3v_affine_iota_dispatch_shape_exact(pl, dispatch,
                                                     out_reason);
   }
   case R300_COMPUTE_INDEX_VALUE_GENERAL:
   default:
      /* The index reaches a stored value through a non-affine chain: no
       * exactness bound is derivable, so no honest fold exists at any
       * invocation count. */
      r3v_set_index_reject(out_reason,
                              "index-value-general (no derivable FP24 bound)");
      return false;
   }

   if (!r300_grid_index_exact(cls, total, stride, offset)) {
      r3v_set_index_reject(out_reason,
         (cls == R300_GRID_INDEX_LINEAR ||
          cls == R300_GRID_INDEX_STRIDED)
            ? "index-ceiling (materialized index exceeds FP24 2^17)"
            : "raster-fold (invocations exceed 2048x2048)");
      return false;
   }
   return r3v_affine_iota_dispatch_shape_exact(pl, dispatch, out_reason);
}

static bool
r3v_idm_compute_raster_grid(struct r3v_device *device,
                               const struct r3v_cmd_dispatch *dispatch,
                               const struct r3v_pipeline *pl,
                               uint64_t *out_invocations,
                               unsigned *out_width,
                               unsigned *out_height)
{
   const uint64_t total_invocations =
      r3v_idm_total_invocations(dispatch, pl);
   struct r300_grid_fold fold;
   if (!r300_grid_fold_1d(total_invocations, &fold)) {
      IDM_LOG("early-return total_invocations=%llu out-of-bounds",
              (unsigned long long)total_invocations);
      return false;
   }
   *out_invocations = total_invocations;
   *out_width = fold.width;
   *out_height = fold.height;
   return true;
}

static bool
r3v_idm_create_blend_acc_vbo(struct pipe_context *pipe,
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
r3v_idm_create_zpass_vbo(struct pipe_context *pipe,
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
r3v_idm_create_fullscreen_vbo(struct pipe_context *pipe,
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

/* Copy a rendered RT back into a linear buffer, row by row, bounded by the
 * kernel's element count.  copy_bytes_per_row is the DESTINATION stride
 * (packed rows: width * blocksize); total_elements bounds the final
 * partial row.  The original form passed copy_bytes_per_row * height as
 * the destination stride and copy_bytes_per_row as the element count,
 * which landed every row after the first outside the mapped box -- single-
 * row folds masked it until the log4 verb's 16-row output measured row 0
 * exact and rows 1+ untouched on silicon. */
static bool
r3v_identity_map_readback_rt(struct pipe_context *pipe,
                                struct pipe_resource *rt,
                                struct pipe_resource *out_buf,
                                unsigned out_offset,
                                unsigned width, unsigned height,
                                enum pipe_format fmt,
                                unsigned copy_bytes_per_row,
                                uint64_t total_elements)
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
      uint64_t total_bytes = 0;
      if (!r3v_idm_element_byte_count(total_elements,
                                         util_format_get_blocksize(fmt),
                                         &total_bytes)) {
         pipe->texture_unmap(pipe, rt_xfer);
         return false;
      }
      out_box.x      = out_offset;
      out_box.width  = (int)total_bytes;
      out_box.height = 1; out_box.depth = 1;
      unsigned map_flags =
         r3v_idm_buffer_write_flags(out_offset, total_bytes, out_buf);
      void *out_bytes = pipe->buffer_map(pipe, out_buf, 0,
                                         map_flags,
                                         &out_box, &out_xfer);
      if (out_bytes) {
         r3v_identity_map_copy_rows(out_bytes, copy_bytes_per_row,
                                       rt_map, rt_xfer->stride,
                                       width, height,
                                       util_format_get_blocksize(fmt),
                                       total_elements);
         pipe->buffer_unmap(pipe, out_xfer);
         copy_ok = true;
      }
      pipe->texture_unmap(pipe, rt_xfer);
   }
   return copy_ok;
}

static bool
r3v_idm_validate_prologue(struct r3v_device *device,
                             const struct r3v_pipeline *pl,
                             const struct r3v_cmd_dispatch *dispatch,
                             const struct r3v_cmd_bind_descriptor_sets *binds,
                             const struct r3v_descriptor_set **out_set)
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
r3v_idm_seed_texture_from_buffer(struct pipe_context *pipe,
                                    struct pipe_resource *in_buf,
                                    unsigned in_offset,
                                    unsigned width, unsigned height,
                                    enum pipe_format fmt,
                                    uint64_t total_elements,
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
   uint64_t total_bytes = 0;
   if (!r3v_idm_element_byte_count(total_elements,
                                      util_format_get_blocksize(fmt),
                                      &total_bytes)) {
      pipe_resource_reference(&tex, NULL);
      return false;
   }
   in_box.x      = in_offset;
   in_box.width  = (int)total_bytes;
   in_box.height = 1; in_box.depth = 1;
   const void *in_map = pipe->buffer_map(pipe, in_buf, 0,
                                         PIPE_MAP_READ, &in_box, &in_xfer);
   if (!in_map) {
      pipe_resource_reference(&tex, NULL);
      return false;
   }

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
   if (!tex_map) {
      pipe->buffer_unmap(pipe, in_xfer);
      pipe_resource_reference(&tex, NULL);
      return false;
   }

   r3v_identity_map_copy_rows(tex_map, tex_xfer->stride,
                                 in_map, width * util_format_get_blocksize(fmt),
                                 width, height,
                                 util_format_get_blocksize(fmt),
                                 total_elements);
   pipe->texture_unmap(pipe, tex_xfer);
   pipe->buffer_unmap(pipe, in_xfer);

   struct pipe_sampler_view sv_templ;
   memset(&sv_templ, 0, sizeof(sv_templ));
   sv_templ.format             = fmt;
   sv_templ.target             = PIPE_TEXTURE_2D;
   sv_templ.swizzle_r          = PIPE_SWIZZLE_X;
   sv_templ.swizzle_g          = PIPE_SWIZZLE_Y;
   sv_templ.swizzle_b          = PIPE_SWIZZLE_Z;
   sv_templ.swizzle_a          = PIPE_SWIZZLE_W;
   *out_sv = pipe->create_sampler_view(pipe, tex, &sv_templ);
   if (!*out_sv) {
      pipe_resource_reference(&tex, NULL);
      return false;
   }
   *out_tex = tex;
   return true;
}

static void
r3v_identity_map_setup_draw_state(struct pipe_context *pipe,
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
r3v_identity_map_wrap_input_as_sampler_view(struct r3v_device *device,
                                               struct pipe_resource *src_buf,
                                               unsigned byte_offset,
                                               unsigned width,
                                               unsigned height,
                                               uint64_t total_elements,
                                               enum pipe_format format)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen || !src_buf || width == 0 || height == 0 ||
       total_elements == 0)
      return NULL;

   /* Allocate a transient 2D texture matching the dispatch shape.
    * PIPE_USAGE_DEFAULT allows the GPU to read it through a sampler view at
    * draw time. The texture is freed automatically when the returned
    * sampler view's last reference is dropped. */
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

   /* Copy the buffer bytes into the 2D texture. The r300 blitter copy path
    * uses unsupported TXF instructions, and util_resource_copy_region accepts
    * matching resource targets only. Map the buffer as a flat byte stream and
    * write each texture row from the matching byte range. The transfer stride
    * supplies the destination row pitch. */
   const unsigned bpp = util_format_get_blocksize(format);
   uint64_t total_bytes = 0;
   if (!r3v_idm_element_byte_count(total_elements, bpp, &total_bytes)) {
      pipe_resource_reference(&tex, NULL);
      return NULL;
   }
   if (bpp == 0 || byte_offset > src_buf->width0 ||
       total_bytes > src_buf->width0 - byte_offset) {
      pipe_resource_reference(&tex, NULL);
      return NULL;
   }
   struct pipe_transfer *src_xfer = NULL;
   struct pipe_box src_box;
   u_box_1d(byte_offset, (int)total_bytes, &src_box);
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
   r3v_identity_map_copy_rows(dst_map, dst_xfer->stride,
                                 src_map, width * bpp,
                                 width, height, bpp,
                                 total_elements);
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
static const struct r3v_descriptor *
find_descriptor_by_binding(const struct r3v_descriptor_set *set,
                           uint32_t binding_index)
{
   for (uint32_t i = 0; i < set->layout->binding_count; i++) {
      if (set->layout->bindings[i].binding == binding_index &&
          set->layout->bindings[i].count > 0)
         return &set->descriptors[set->layout->bindings[i].offset];
   }
   return NULL;
}

static bool
storage_buffer_binding_is_compute_usable(const struct r3v_dsl_binding *binding)
{
   return binding->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
          binding->count > 0 &&
          (binding->stage_flags & VK_SHADER_STAGE_COMPUTE_BIT);
}

/* Walk the descriptor-set layout and pick the Nth compute-visible
 * STORAGE_BUFFER binding's binding index.  Used to resolve the identity-map
 * kernel's input + output ssbo bindings when the NIR detector cannot recover
 * them as constants (the post-explicit_io binding source is a Vulkan
 * descriptor handle, not a nir_load_const).  The bindings array is sorted by
 * binding index per r3v_CreateDescriptorSetLayout, so the Nth usable
 * STORAGE_BUFFER seen during a forward walk is the Nth declared for compute
 * replay in the layout. */
static bool
nth_storage_buffer_binding(const struct r3v_descriptor_set *set,
                           unsigned which,
                           uint32_t *out_binding)
{
   unsigned seen = 0;
   for (uint32_t i = 0; i < set->layout->binding_count; i++) {
      if (storage_buffer_binding_is_compute_usable(&set->layout->bindings[i])) {
         if (seen == which) {
            *out_binding = set->layout->bindings[i].binding;
            return true;
         }
         seen++;
      }
   }
   return false;
}

static unsigned
compute_storage_buffer_binding_count(const struct r3v_descriptor_set *set)
{
   unsigned count = 0;
   for (uint32_t i = 0; i < set->layout->binding_count; i++) {
      if (storage_buffer_binding_is_compute_usable(&set->layout->bindings[i]))
         count++;
   }
   return count;
}

/* Resolve a detector's role bindings without using binding 0 as an unknown
 * sentinel.  A complete capture is authoritative, including zero-valued
 * bindings.  An all-unknown capture uses the positional contract only when
 * the descriptor layout contains exactly one compute-visible storage buffer
 * per role.  The exact-count check keeps an opaque source from silently
 * selecting a prefix of an unrelated layout.  A partial capture has no safe
 * reconstruction because the missing role could be interleaved anywhere in
 * binding order, so dispatch refuses it. */
static bool
idm_resolve_binding_roles(struct r3v_device *device,
                          const struct r3v_descriptor_set *set,
                          unsigned role_count, uint32_t *bindings,
                          unsigned valid_mask)
{
   const enum r300_compute_binding_capture_state state =
      r300_compute_binding_capture_classify(valid_mask, role_count);
   if (state == R300_COMPUTE_BINDINGS_COMPLETE)
      return true;
   if (state == R300_COMPUTE_BINDINGS_PARTIAL) {
      IDM_LOG("binding resolution refused partial capture mask=0x%x roles=%u",
              valid_mask, role_count);
      return false;
   }

   if (compute_storage_buffer_binding_count(set) != role_count) {
      IDM_LOG("binding resolution refused opaque layout storage-buffer-count=%u expected=%u",
              compute_storage_buffer_binding_count(set), role_count);
      return false;
   }
   for (unsigned i = 0; i < role_count; i++) {
      if (!nth_storage_buffer_binding(set, i, &bindings[i]))
         return false;
   }
   return true;
}

/* Output-only shapes have no input binding to disambiguate a positional
 * fallback.  When the detector cannot recover a constant store binding, the
 * layout must contain exactly one compute-visible STORAGE_BUFFER or the
 * destination is not recoverable without risking writes to an unrelated buffer. */
static bool
single_storage_buffer_binding(const struct r3v_descriptor_set *set,
                              uint32_t *out_binding)
{
   bool found = false;
   for (uint32_t i = 0; i < set->layout->binding_count; i++) {
      if (!storage_buffer_binding_is_compute_usable(&set->layout->bindings[i]))
         continue;
      if (found)
         return false;
      *out_binding = set->layout->bindings[i].binding;
      found = true;
   }
   return found;
}

static bool
single_storage_buffer_binding_excluding(const struct r3v_descriptor_set *set,
                                        uint32_t excluded_binding,
                                        uint32_t *out_binding)
{
   bool found = false;
   for (uint32_t i = 0; i < set->layout->binding_count; i++) {
      const struct r3v_dsl_binding *binding = &set->layout->bindings[i];
      if (!storage_buffer_binding_is_compute_usable(binding) ||
          binding->binding == excluded_binding)
         continue;
      if (found)
         return false;
      *out_binding = binding->binding;
      found = true;
   }
   return found;
}

/* Recover the input + output STORAGE_BUFFER bindings positionally when the
 * detector left both at 0.  After nir_lower_explicit_io the load_ssbo /
 * store_ssbo binding source is a Vulkan descriptor handle, not a constant, so
 * the detectors cannot record the binding indices and the pattern structs keep
 * their zero-initialized defaults.  Resolving {0,0} directly maps both the
 * input and the output descriptor to binding 0, so the kernel would read its
 * own output buffer as input.  The contract for the recovered shapes is a
 * single compute-visible input/value storage buffer followed by a single
 * compute-visible output storage buffer, so the first usable STORAGE_BUFFER is
 * the input and the second is the output. */
static bool
idm_recover_in_out_bindings(const struct r3v_descriptor_set *set,
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
   struct r300_grid_fold fold;
   if (!r300_grid_fold_1d(total_invocations ? total_invocations : 1, &fold)) {
      /* Callers bounds-check total against 2048x2048 before folding; an
       * out-of-range count here is a caller bug, not a recoverable state. */
      *out_width  = 1;
      *out_height = 1;
      return;
   }
   *out_width  = fold.width;
   *out_height = fold.height;
}


bool
r3v_identity_map_dispatch_replay(struct r3v_device *device,
                                    const struct r3v_pipeline *pl,
                                    const struct r3v_cmd_dispatch *dispatch,
                                    const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!device)
      return false;

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
    * requires set-index-aware binding capture. */
   if (binds->first_set != 0) {
      IDM_LOG("early-return first_set=%u (only slot 0 supported)",
              binds->first_set);
      return false;
   }
   const struct r3v_descriptor_set *set = binds->sets[0];
   IDM_LOG("set=%p layout=%p in_binding=%u out_binding=%u",
           (const void *)set,
           set ? (const void *)set->layout : NULL,
           pl->identity_map.input_ssbo_binding,
           pl->identity_map.output_ssbo_binding);
   if (!set || !set->layout) {
      IDM_LOG("early-return no-set-or-layout");
      return false;
   }

   /* The detector carries an explicit validity bit for each binding.  A
    * complete capture is authoritative, including binding zero.  When both
    * sources are opaque, the descriptor-set contract supplies input = first
    * and output = second compute-visible STORAGE_BUFFER; a partial capture is
    * refused because the missing role cannot be recovered safely. */
   uint32_t in_binding = pl->identity_map.input_ssbo_binding;
   uint32_t out_binding = pl->identity_map.output_ssbo_binding;
   const unsigned binding_valid_mask =
      (pl->identity_map.input_ssbo_binding_valid ? 1u : 0u) |
      (pl->identity_map.output_ssbo_binding_valid ? 2u : 0u);
   uint32_t bindings[2] = { in_binding, out_binding };
   if (!idm_resolve_binding_roles(device, set, 2, bindings,
                                  binding_valid_mask)) {
      IDM_LOG("early-return identity-binding-resolution");
      return false;
   }
   in_binding = bindings[0];
   out_binding = bindings[1];
   IDM_LOG("resolved identity bindings: in=%u out=%u source=%s",
           in_binding, out_binding,
           binding_valid_mask == 0 ? "positional" : "detector");

   const struct r3v_descriptor *in_desc =
      find_descriptor_by_binding(set, in_binding);
   const struct r3v_descriptor *out_desc =
      find_descriptor_by_binding(set, out_binding);
   IDM_LOG("descriptor walk in_binding=%u out_binding=%u in_desc=%p out_desc=%p",
           in_binding, out_binding,
           (const void *)in_desc, (const void *)out_desc);
   if (!in_desc || !out_desc) {
      IDM_LOG("early-return descriptor-walk-miss");
      return false;
   }
   IDM_LOG("in_desc->buf.buffer=%" PRIu64 " out_desc->buf.buffer=%" PRIu64,
           (uint64_t)in_desc->buf.buffer, (uint64_t)out_desc->buf.buffer);
   if (!in_desc->buf.buffer || !out_desc->buf.buffer) {
      IDM_LOG("early-return null-vkbuffer-handle");
      return false;
   }

   VK_FROM_HANDLE(r3v_buffer, in_buf,  in_desc->buf.buffer);
   VK_FROM_HANDLE(r3v_buffer, out_buf, out_desc->buf.buffer);
   IDM_LOG("in_buf=%p resource=%p out_buf=%p resource=%p",
           (const void *)in_buf,
           in_buf ? (const void *)in_buf->resource : NULL,
           (const void *)out_buf,
           out_buf ? (const void *)out_buf->resource : NULL);
   if (!in_buf || !out_buf || !in_buf->resource || !out_buf->resource) {
      IDM_LOG("early-return null-pipe-resource");
      return false;
   }

   /* The replay grid emits one fragment for each real invocation:
    * group_count_x/y/z multiplied by local_size_x/y/z.  The checked helper
    * returns zero on overflow so wrapped products cannot smuggle a small
    * non-zero total past the admission gate. */
   const uint64_t total_invocations =
      r3v_idm_total_invocations(dispatch, pl);
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
   const enum pipe_format fmt = r3v_identity_map_replay_format(device, pl);

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
      r3v_identity_map_wrap_input_as_sampler_view(device, in_buf->resource,
                                                     (unsigned)in_desc->buf.offset,
                                                     width, height,
                                                     total_invocations, fmt);
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
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_resource_reference(&rt, NULL);
      pipe_sampler_view_reference(&in_sv, NULL);
      return false;
   }

   r3v_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_cso.blend,
                                        device->identity_map_cso.rasterizer,
                                        device->identity_map_cso.dsa,
                                        pl->vs_cso, pl->fs_cso, velems_cso);
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1,
                             &device->identity_map_cso.sampler);
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

   /* Copy the RT back to the output SSBO with the same explicit map/copy path
    * used for the input wrap. */
   struct pipe_box copy_box;
   memset(&copy_box, 0, sizeof(copy_box));
   copy_box.x      = 0;
   copy_box.y      = 0;
   copy_box.z      = 0;
   copy_box.width  = width;
   copy_box.height = height;
   copy_box.depth  = 1;
   /* The r300 blitter copy path uses unsupported TXF instructions and
    * util_resource_copy_region accepts matching resource targets only. */
   bool copy_ok = false;
   {
      struct pipe_transfer *rt_xfer = NULL;
      const void *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                             &copy_box, &rt_xfer);
      if (rt_map) {
         struct pipe_transfer *out_xfer = NULL;
         struct pipe_box out_box;
         memset(&out_box, 0, sizeof(out_box));
         const unsigned blocksize = util_format_get_blocksize(fmt);
         uint64_t out_byte_count = 0;
         out_box.x      = (unsigned)out_desc->buf.offset;
         if (r3v_idm_element_byte_count(total_invocations, blocksize,
                                           &out_byte_count)) {
            out_box.width  = (int)out_byte_count;
            out_box.height = 1;
            out_box.depth  = 1;
            void *out_bytes = pipe->buffer_map(
               pipe, out_buf->resource, 0,
               r3v_idm_buffer_write_flags((unsigned)out_desc->buf.offset,
                                             out_byte_count,
                                             out_buf->resource),
               &out_box, &out_xfer);
            if (out_bytes) {
               r3v_identity_map_copy_rows(out_bytes, width * blocksize,
                                             rt_map, rt_xfer->stride,
                                             width, height, blocksize,
                                             total_invocations);
               pipe->buffer_unmap(pipe, out_xfer);
               copy_ok = true;
            }
         }
         pipe->texture_unmap(pipe, rt_xfer);
      }
   }
   IDM_LOG("rt->buffer copy issued (out=%p, src=%p, box w=%d h=%d)",
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

static bool
r3v_one_in_one_out_dispatch_replay(struct r3v_device *device,
                                      const struct r3v_pipeline *pl,
                                      const struct r3v_cmd_dispatch *dispatch,
                                      const struct r3v_cmd_bind_descriptor_sets *binds,
                                      uint32_t cap_in, uint32_t cap_out,
                                      bool detector_captured,
                                      enum pipe_format input_fmt,
                                      enum pipe_format output_fmt,
                                      enum pipe_format output_buffer_fmt);

/* Unary affine-map replay for vec4 and scalar FP32.  The generated fragment
 * program applies c0/c1 to the sampled value, writes through the FP16
 * render-target carrier, and unpacks to the kernel's FP32 output.  Other
 * component counts or bit widths need their own carrier and reject here
 * rather than sampling bytes as UNORM colors. */
bool
r3v_unary_map_dispatch_replay(struct r3v_device *device,
                                 const struct r3v_pipeline *pl,
                                 const struct r3v_cmd_dispatch *dispatch,
                                 const struct r3v_cmd_bind_descriptor_sets *binds,
                                 const uint8_t *push_data)
{
   const bool uses_push = pl->unary_map.mul_const_from_push ||
                          pl->unary_map.add_const_from_push;
   bool ok = false;

   /* A push-derived c0/c1 reads the constant file: bind the queue walk's
    * 128-byte push window at FS CONST[0] so the FS const-file reads (byte
    * offset N -> CONST[N/16] component (N%16)/4) see the pushed values.  The
    * window lives on the caller's stack and the draw flushes inside the
    * replay core, so user_buffer needs no copy; clear the binding after the
    * draw so a later draw cannot read a stale window. */
   if (uses_push) {
      if (!push_data) {
         IDM_LOG("unary_map early-return push-derived constants without a "
                 "push window");
         return false;
      }
      struct pipe_constant_buffer cb;
      memset(&cb, 0, sizeof(cb));
      cb.user_buffer = push_data;
      cb.buffer_size = 128;
      device->pipe->set_constant_buffer(device->pipe, MESA_SHADER_FRAGMENT, 0,
                                        &cb);
   }

   if (pl->unary_map.value_is_float && pl->unary_map.value_bit_size == 32 &&
       pl->unary_map.value_components == 4) {
      if (device->screen->is_format_supported(device->screen,
             PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
             PIPE_BIND_SAMPLER_VIEW) &&
          device->screen->is_format_supported(device->screen,
             PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
             PIPE_BIND_RENDER_TARGET)) {
         ok = r3v_one_in_one_out_dispatch_replay(
            device, pl, dispatch, binds,
            pl->unary_map.input_ssbo_binding,
            pl->unary_map.output_ssbo_binding,
            pl->unary_map.input_ssbo_binding_valid &&
            pl->unary_map.output_ssbo_binding_valid,
            PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
            PIPE_FORMAT_R32G32B32A32_FLOAT);
      }
   } else if (pl->unary_map.value_is_float &&
              pl->unary_map.value_bit_size == 32 &&
              pl->unary_map.value_components == 1) {
      /* Scalar carrier: one float per element.  The input SSBO wraps as an
       * R32_FLOAT sampler view (one texel per element, TEX yields the value
       * in the X channel); the channel-uniform affine FS computes the map on
       * every channel; the FP16x4 RT carries the result and readback gathers
       * the X lane into the 4-byte-stride output buffer. */
      if (device->screen->is_format_supported(device->screen,
             PIPE_FORMAT_R32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
             PIPE_BIND_SAMPLER_VIEW) &&
          device->screen->is_format_supported(device->screen,
             PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
             PIPE_BIND_RENDER_TARGET)) {
         ok = r3v_one_in_one_out_dispatch_replay(
            device, pl, dispatch, binds,
            pl->unary_map.input_ssbo_binding,
            pl->unary_map.output_ssbo_binding,
            pl->unary_map.input_ssbo_binding_valid &&
            pl->unary_map.output_ssbo_binding_valid,
            PIPE_FORMAT_R32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
            PIPE_FORMAT_R32_FLOAT);
      }
   }

   if (uses_push)
      device->pipe->set_constant_buffer(device->pipe, MESA_SHADER_FRAGMENT, 0,
                                        NULL);
   return ok;
}

/* Unary-transcendental replay: out[gid] = f(in[gid]).  The same scalar carrier
 * as the unary_map scalar path -- R32_FLOAT sampler -> FP16x4 RT -> X-lane
 * gather -- with pl->fs_cso holding the synthesized 1-TEX-1-scalar transcendental
 * FS instead of the affine MAD FS.
 * Scalar float32 only; no push window (the op carries no runtime constant).
 * PRECISION: the FP16 render-target carrier bounds the result to ~10-bit
 * mantissa, looser than the fp24 ALU; the readback is approximate, not exact. */
bool
r3v_unary_transcendental_dispatch_replay(
   struct r3v_device *device,
   const struct r3v_pipeline *pl,
   const struct r3v_cmd_dispatch *dispatch,
   const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (pl->unary_transcendental.value_bit_size != 32 ||
       pl->unary_transcendental.value_components != 1)
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW) ||
       !device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r3v_one_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->unary_transcendental.input_ssbo_binding,
      pl->unary_transcendental.output_ssbo_binding,
      pl->unary_transcendental.input_ssbo_binding_valid &&
      pl->unary_transcendental.output_ssbo_binding_valid,
      PIPE_FORMAT_R32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32_FLOAT);
}

/* Logical-shift dispatch replay: out[gid] = a[gid] << k or >> k.  Reuses the
 * 1-in/1-out replay core over the UNORM8 carrier -- the input uint32 wraps as an
 * RGBA8 sampler, the byte-recombination FS writes the shifted bytes to an RGBA8
 * RT, and the raw RT bytes (output_buffer_fmt = NONE) copy straight back to the
 * uint32 output, the same exact UNORM8 round-trip the identity-map verb uses. */
bool
r3v_shift_logical_dispatch_replay(
   struct r3v_device *device,
   const struct r3v_pipeline *pl,
   const struct r3v_cmd_dispatch *dispatch,
   const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!device || !device->screen)
      return false;
   if (pl->shift_logical.value_bit_size != 32 ||
       pl->shift_logical.value_components != 1)
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW) ||
       !device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   const bool captured = (pl->shift_logical.input_ssbo_binding != 0 ||
                          pl->shift_logical.output_ssbo_binding != 0);
   return r3v_one_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->shift_logical.input_ssbo_binding,
      pl->shift_logical.output_ssbo_binding,
      captured,
      PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_R8G8B8A8_UNORM,
      PIPE_FORMAT_NONE);
}

/* Shared 2-in / 1-out compute-as-raster replay core.  Wraps two input SSBOs as
 * sampler views at fragment stages 0 + 1, draws the fullscreen quad with the
 * pipeline's synthesized VS + FS (pl->fs_cso -- the binary-map ALU FS or the
 * DP4 dot FS), and copies the RB3D color export back to the output SSBO.  The
 * caller passes the three SSBO bindings its detector captured plus an optional
 * validity mask; binary-map uses the explicit mask, while legacy wrappers
 * retain their positional fallback until their detectors carry validity
 * metadata. */
static bool
r3v_two_in_one_out_dispatch_replay(struct r3v_device *device,
                                      const struct r3v_pipeline *pl,
                                      const struct r3v_cmd_dispatch *dispatch,
                                      const struct r3v_cmd_bind_descriptor_sets *binds,
                                      uint32_t cap_in_a, uint32_t cap_in_b,
                                      uint32_t cap_out,
                                      bool binding_validity_explicit,
                                      unsigned binding_valid_mask,
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
      IDM_LOG("2in1out early-return null-or-empty-binds");
      return false;
   }
   if (!pl->vs_cso || !pl->fs_cso) {
      IDM_LOG("2in1out early-return no-vs-or-fs-cso");
      return false;
   }
   if (binds->first_set != 0) {
      IDM_LOG("2in1out early-return first_set=%u (only slot 0)",
              binds->first_set);
      return false;
   }
   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("2in1out early-return no-set-or-layout");
      return false;
   }

   /* Binary-map dispatch supplies explicit validity bits.  Complete capture
    * preserves operand order and binding zero; all-opaque capture uses the
    * exact three-role positional contract; partial capture refuses replay.
    * Other legacy two-input patterns still use their historical nonzero
    * capture rule until their detectors carry the same validity metadata. */
   uint32_t in_a_binding = cap_in_a;
   uint32_t in_b_binding = cap_in_b;
   uint32_t out_binding  = cap_out;
   bool detector_captured = false;
   if (binding_validity_explicit) {
      uint32_t bindings[3] = { in_a_binding, in_b_binding, out_binding };
      if (!idm_resolve_binding_roles(device, set, 3, bindings,
                                     binding_valid_mask)) {
         IDM_LOG("2in1out early-return explicit-binding-resolution");
         return false;
      }
      in_a_binding = bindings[0];
      in_b_binding = bindings[1];
      out_binding = bindings[2];
      detector_captured = binding_valid_mask != 0;
   } else {
      detector_captured = (in_a_binding != 0 || in_b_binding != 0 ||
                           out_binding  != 0);
      if (!detector_captured) {
         if (!nth_storage_buffer_binding(set, 0, &in_a_binding) ||
             !nth_storage_buffer_binding(set, 1, &in_b_binding) ||
             !nth_storage_buffer_binding(set, 2, &out_binding)) {
            IDM_LOG("2in1out early-return layout-has-fewer-than-three-storage-buffers");
            return false;
         }
      }
   }
   IDM_LOG("2in1out bindings: in_a=%u in_b=%u out=%u source=%s",
           in_a_binding, in_b_binding, out_binding,
           detector_captured ? "detector" : "positional");

   const struct r3v_descriptor *desc_in_a =
      find_descriptor_by_binding(set, in_a_binding);
   const struct r3v_descriptor *desc_in_b =
      find_descriptor_by_binding(set, in_b_binding);
   const struct r3v_descriptor *desc_out =
      find_descriptor_by_binding(set, out_binding);
   if (!desc_in_a || !desc_in_b || !desc_out) {
      IDM_LOG("2in1out early-return descriptor-walk-miss");
      return false;
   }
   if (!desc_in_a->buf.buffer || !desc_in_b->buf.buffer ||
       !desc_out->buf.buffer) {
      IDM_LOG("2in1out early-return null-vkbuffer-handle");
      return false;
   }
   VK_FROM_HANDLE(r3v_buffer, buf_in_a, desc_in_a->buf.buffer);
   VK_FROM_HANDLE(r3v_buffer, buf_in_b, desc_in_b->buf.buffer);
   VK_FROM_HANDLE(r3v_buffer, buf_out,  desc_out->buf.buffer);
   if (!buf_in_a || !buf_in_b || !buf_out ||
       !buf_in_a->resource || !buf_in_b->resource || !buf_out->resource) {
      IDM_LOG("2in1out early-return null-pipe-resource");
      return false;
   }

   const uint64_t total_invocations =
      r3v_idm_total_invocations(dispatch, pl);
   if (total_invocations == 0 || total_invocations > 2048u * 2048u) {
      IDM_LOG("2in1out early-return total_invocations=%llu out-of-bounds",
              (unsigned long long)total_invocations);
      return false;
   }
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total_invocations, &width, &height);
   if (width > 2048 || height > 2048) {
      IDM_LOG("2in1out early-return extent-exceeds-2048-cap");
      return false;
   }

   /* input_fmt is the per-element sampler-view format (UNORM8 4B for binary-map,
    * R32G32B32A32_FLOAT 16B for the dp4 vec4 inputs); fmt = output_fmt is the
    * RT format.  PIPE_FORMAT_NONE in output_buffer_fmt means the copy-back uses
    * the RT bytes directly. */
   const enum pipe_format fmt = output_fmt;
   struct pipe_sampler_view *sv_a =
      r3v_identity_map_wrap_input_as_sampler_view(device, buf_in_a->resource,
                                                     (unsigned)desc_in_a->buf.offset,
                                                     width, height,
                                                     total_invocations, input_fmt);
   if (!sv_a) {
      IDM_LOG("2in1out early-return wrap-input-a-failed");
      return false;
   }
   struct pipe_sampler_view *sv_b =
      r3v_identity_map_wrap_input_as_sampler_view(device, buf_in_b->resource,
                                                     (unsigned)desc_in_b->buf.offset,
                                                     width, height,
                                                     total_invocations, input_fmt);
   if (!sv_b) {
      pipe_sampler_view_reference(&sv_a, NULL);
      IDM_LOG("2in1out early-return wrap-input-b-failed");
      return false;
   }
   IDM_LOG("2in1out wrap sv_a=%p sv_b=%p", (const void *)sv_a, (const void *)sv_b);

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
      IDM_LOG("2in1out early-return rt-create-failed");
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
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_resource_reference(&rt, NULL);
      pipe_sampler_view_reference(&sv_a, NULL);
      pipe_sampler_view_reference(&sv_b, NULL);
      return false;
   }

   r3v_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_cso.blend,
                                        device->identity_map_cso.rasterizer,
                                        device->identity_map_cso.dsa,
                                        pl->vs_cso, pl->fs_cso, velems_cso);

   /* Bind the two input buffers as the replay core's sampler stages. */
   void *samplers[2] = { device->identity_map_cso.sampler,
                         device->identity_map_cso.sampler };
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
   IDM_LOG("2in1out draw_vbo mode=triangle_strip count=4");
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   pipe->flush(pipe, NULL, 0);
   IDM_LOG("2in1out post-flush, beginning rt->buffer copy");

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
         uint64_t out_byte_count = 0;
         if (r3v_idm_element_byte_count(total_invocations, buf_bs,
                                           &out_byte_count)) {
            out_box.width  = (int)out_byte_count;
            out_box.height = 1;
            out_box.depth  = 1;
            void *out_bytes = pipe->buffer_map(
               pipe, buf_out->resource, 0,
               r3v_idm_buffer_write_flags((unsigned)desc_out->buf.offset,
                                             out_byte_count,
                                             buf_out->resource),
               &out_box, &out_xfer);
            if (out_bytes) {
               copy_ok = r3v_idm_copy_rt_rows_to_buffer(
                  out_bytes, rt_map, rt_xfer->stride, width, height,
                  total_invocations, fmt, buf_fmt, buf_bs);
               pipe->buffer_unmap(pipe, out_xfer);
            }
         }
         pipe->texture_unmap(pipe, rt_xfer);
      }
   }
   IDM_LOG("2in1out rt->buffer copy issued (out=%p, src=%p, box w=%d h=%d)",
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
   IDM_LOG("2in1out orchestrator done copy_ok=%d", (int)copy_ok);
   return copy_ok;
}

/* Binary-map and DP4 share the 2-in / 1-out replay core above; each passes the
 * three ssbo bindings its detector captured.  binary-map's FS reduces the two
 * sampled texels with the detected ALU op; dp4's FS dots them (pl->fs_cso
 * already holds the right synthesized FS). */
bool
r3v_binary_map_dispatch_replay(struct r3v_device *device,
                                  const struct r3v_pipeline *pl,
                                  const struct r3v_cmd_dispatch *dispatch,
                                  const struct r3v_cmd_bind_descriptor_sets *binds)
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
      return r3v_two_in_one_out_dispatch_replay(
         device, pl, dispatch, binds,
         pl->binary_map.input_a_ssbo_binding,
         pl->binary_map.input_b_ssbo_binding,
         pl->binary_map.output_ssbo_binding,
         true,
         (pl->binary_map.input_a_ssbo_binding_valid ? 1u : 0u) |
         (pl->binary_map.input_b_ssbo_binding_valid ? 2u : 0u) |
         (pl->binary_map.output_ssbo_binding_valid ? 4u : 0u),
         PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
         PIPE_FORMAT_R32G32B32A32_FLOAT);
   }
   return r3v_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->binary_map.input_a_ssbo_binding,
      pl->binary_map.input_b_ssbo_binding,
      pl->binary_map.output_ssbo_binding,
      true,
      (pl->binary_map.input_a_ssbo_binding_valid ? 1u : 0u) |
      (pl->binary_map.input_b_ssbo_binding_valid ? 2u : 0u) |
      (pl->binary_map.output_ssbo_binding_valid ? 4u : 0u),
      PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_R8G8B8A8_UNORM,
      PIPE_FORMAT_NONE);
}

/* Binary-transcendental dispatch replay: out[gid] = f(a[gid], b[gid]) for f in
 * {fpow, fdiv}.  Two carriers selected by the detected element width: a scalar
 * kernel rides the R32_FLOAT -> FP16 RT -> X-lane gather path, a vec4 kernel the
 * R32G32B32A32 -> FP16 RT -> RGBA32 unpack path the binary_map float path uses.
 * pl->fs_cso holds the matching componentwise transcendental FS. */
bool
r3v_binary_transcendental_dispatch_replay(
   struct r3v_device *device,
   const struct r3v_pipeline *pl,
   const struct r3v_cmd_dispatch *dispatch,
   const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!device || !device->screen)
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;

   const bool scalar = pl->binary_transcendental.value_components == 1;
   const enum pipe_format in_fmt =
      scalar ? PIPE_FORMAT_R32_FLOAT : PIPE_FORMAT_R32G32B32A32_FLOAT;
   if (!device->screen->is_format_supported(device->screen, in_fmt,
          PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_SAMPLER_VIEW))
      return false;

   return r3v_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->binary_transcendental.input_a_ssbo_binding,
      pl->binary_transcendental.input_b_ssbo_binding,
      pl->binary_transcendental.output_ssbo_binding,
      false, 0,
      in_fmt, PIPE_FORMAT_R16G16B16A16_FLOAT, in_fmt);
}

/* Map the bitwise nir_op to the Gallium logic op the RB3D ROP applies. */
static unsigned
bitwise_pipe_logicop(uint16_t alu_op)
{
   switch ((nir_op)alu_op) {
   case nir_op_iand: return PIPE_LOGICOP_AND;
   case nir_op_ior:  return PIPE_LOGICOP_OR;
   case nir_op_ixor: return PIPE_LOGICOP_XOR;
   default:          return PIPE_LOGICOP_COPY;
   }
}

/* Bitwise-logicop dispatch replay: out[gid] = a[gid] OP b[gid] for OP in
 * {iand, ior, ixor}.  The FP24 ALU cannot do bitwise, so the op rides the RB3D
 * ROP output stage: each uint32 packs as one RGBA8 texel, and the logic op
 * combines source against destination per bit.  Two draws into one RGBA8 RT:
 * draw 1 copies b into the RT (default copy blend), draw 2 draws a with the
 * logic op enabled so the ROP reads dst = b and writes a OP b.  The result is
 * bit-exact (UNORM8 round-trips 0..255 exactly, dithering is off in the
 * substrate's blend state).  pl->vs_cso / pl->fs_cso are the identity-map
 * passthrough VS + copy FS (both draws just sample and export). */
bool
r3v_bitwise_logicop_dispatch_replay(
   struct r3v_device *device,
   const struct r3v_pipeline *pl,
   const struct r3v_cmd_dispatch *dispatch,
   const struct r3v_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device ? device->pipe : NULL;
   struct pipe_screen  *screen = device ? device->screen : NULL;
   if (!pipe || !screen || !pl || !dispatch || !binds || binds->set_count == 0)
      return false;
   if (!pl->vs_cso || !pl->fs_cso)
      return false;
   if (binds->first_set != 0)
      return false;
   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t in_a_binding = pl->bitwise_logicop.input_a_ssbo_binding;
   uint32_t in_b_binding = pl->bitwise_logicop.input_b_ssbo_binding;
   uint32_t out_binding  = pl->bitwise_logicop.output_ssbo_binding;
   if (in_a_binding == 0 && in_b_binding == 0 && out_binding == 0) {
      if (!nth_storage_buffer_binding(set, 0, &in_a_binding) ||
          !nth_storage_buffer_binding(set, 1, &in_b_binding) ||
          !nth_storage_buffer_binding(set, 2, &out_binding))
         return false;
   }

   const struct r3v_descriptor *desc_in_a =
      find_descriptor_by_binding(set, in_a_binding);
   const struct r3v_descriptor *desc_in_b =
      find_descriptor_by_binding(set, in_b_binding);
   const struct r3v_descriptor *desc_out =
      find_descriptor_by_binding(set, out_binding);
   if (!desc_in_a || !desc_in_b || !desc_out)
      return false;
   if (!desc_in_a->buf.buffer || !desc_in_b->buf.buffer || !desc_out->buf.buffer)
      return false;
   VK_FROM_HANDLE(r3v_buffer, buf_in_a, desc_in_a->buf.buffer);
   VK_FROM_HANDLE(r3v_buffer, buf_in_b, desc_in_b->buf.buffer);
   VK_FROM_HANDLE(r3v_buffer, buf_out,  desc_out->buf.buffer);
   if (!buf_in_a || !buf_in_b || !buf_out ||
       !buf_in_a->resource || !buf_in_b->resource || !buf_out->resource)
      return false;

   const uint64_t total_invocations = r3v_idm_total_invocations(dispatch, pl);
   if (total_invocations == 0 || total_invocations > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total_invocations, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   /* RGBA8 carrier: one uint32 per texel, the logic op works per bit. */
   const enum pipe_format fmt = PIPE_FORMAT_R8G8B8A8_UNORM;
   struct pipe_sampler_view *sv_a =
      r3v_identity_map_wrap_input_as_sampler_view(device, buf_in_a->resource,
                                                     (unsigned)desc_in_a->buf.offset,
                                                     width, height,
                                                     total_invocations, fmt);
   if (!sv_a)
      return false;
   struct pipe_sampler_view *sv_b =
      r3v_identity_map_wrap_input_as_sampler_view(device, buf_in_b->resource,
                                                     (unsigned)desc_in_b->buf.offset,
                                                     width, height,
                                                     total_invocations, fmt);
   if (!sv_b) {
      pipe_sampler_view_reference(&sv_a, NULL);
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
      pipe_sampler_view_reference(&sv_b, NULL);
      pipe_sampler_view_reference(&sv_a, NULL);
      return false;
   }
   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = fmt;
   surf_templ.texture = rt;

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_resource_reference(&rt, NULL);
      pipe_sampler_view_reference(&sv_b, NULL);
      pipe_sampler_view_reference(&sv_a, NULL);
      return false;
   }

   /* The logic-op blend state for draw 2.  logicop_enable routes the colour
    * output through R300_RB3D_ROPCNTL; logicop_func is the PIPE_LOGICOP_* the
    * r300 backend emits without translation. */
   struct pipe_blend_state lblend;
   memset(&lblend, 0, sizeof(lblend));
   lblend.rt[0].colormask = PIPE_MASK_RGBA;
   lblend.logicop_enable  = 1;
   lblend.logicop_func    = bitwise_pipe_logicop(pl->bitwise_logicop.alu_op);
   void *logicop_cso = pipe->create_blend_state(pipe, &lblend);
   if (!logicop_cso) {
      pipe->delete_vertex_elements_state(pipe, velems_cso);
      pipe_resource_reference(&vb, NULL);
      pipe_resource_reference(&rt, NULL);
      pipe_sampler_view_reference(&sv_b, NULL);
      pipe_sampler_view_reference(&sv_a, NULL);
      return false;
   }

   void *samp = device->identity_map_cso.sampler;
   struct pipe_vertex_buffer vb_state;
   memset(&vb_state, 0, sizeof(vb_state));
   vb_state.buffer.resource = vb;
   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode           = MESA_PRIM_TRIANGLE_STRIP;
   info.instance_count = 1;
   info.max_index      = 3;
   struct pipe_draw_start_count_bias draw = { .start = 0, .count = 4 };

   /* Draw 1: RT = b, plain copy (default blend, sampler stage 0 = b). */
   r3v_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_cso.blend,
                                        device->identity_map_cso.rasterizer,
                                        device->identity_map_cso.dsa,
                                        pl->vs_cso, pl->fs_cso, velems_cso);
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1, &samp);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 1, 0, &sv_b);
   pipe->set_vertex_buffers(pipe, 1, &vb_state);
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);

   /* Draw 2: RT = a OP b.  The logic-op blend makes the ROP read dst = b (draw
    * 1's result) and write a OP b; sampler stage 0 = a. */
   pipe->bind_blend_state(pipe, logicop_cso);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 1, 0, &sv_a);
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   pipe->flush(pipe, NULL, 0);

   /* Copy the RGBA8 RT straight back into the uint32 output buffer (the element
    * format equals the RT format, so the shared helper does a raw byte copy). */
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
         out_box.x = (unsigned)desc_out->buf.offset;
         const unsigned buf_bs = util_format_get_blocksize(fmt);
         uint64_t out_byte_count = 0;
         if (r3v_idm_element_byte_count(total_invocations, buf_bs,
                                           &out_byte_count)) {
            out_box.width  = (int)out_byte_count;
            out_box.height = 1;
            out_box.depth  = 1;
            void *out_bytes = pipe->buffer_map(
               pipe, buf_out->resource, 0,
               r3v_idm_buffer_write_flags((unsigned)desc_out->buf.offset,
                                             out_byte_count, buf_out->resource),
               &out_box, &out_xfer);
            if (out_bytes) {
               copy_ok = r3v_idm_copy_rt_rows_to_buffer(
                  out_bytes, rt_map, rt_xfer->stride, width, height,
                  total_invocations, fmt, fmt, buf_bs);
               pipe->buffer_unmap(pipe, out_xfer);
            }
         }
         pipe->texture_unmap(pipe, rt_xfer);
      }
   }

   struct pipe_sampler_view *no_view = NULL;
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 1, &no_view);
   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);
   pipe->bind_blend_state(pipe, device->identity_map_cso.blend);
   pipe->delete_blend_state(pipe, logicop_cso);
   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_sampler_view_reference(&sv_b, NULL);
   pipe_sampler_view_reference(&sv_a, NULL);
   pipe_resource_reference(&vb, NULL);
   pipe_resource_reference(&rt, NULL);
   return copy_ok;
}

bool
r3v_dp4_dispatch_replay(struct r3v_device *device,
                           const struct r3v_pipeline *pl,
                           const struct r3v_cmd_dispatch *dispatch,
                           const struct r3v_cmd_bind_descriptor_sets *binds)
{
   /* The dot inputs are copied from the SSBO with the same per-invocation
    * stride the compute load uses.  R32G32_FLOAT preserves fdot2's 8-byte
    * vec2 records; fdot3 and fdot4 stay on the 16-byte carrier because this
    * R300 format table has no R32G32B32_FLOAT sampler target. */
   const enum pipe_format input_fmt = pl->dp4.components == 2
      ? PIPE_FORMAT_R32G32_FLOAT : PIPE_FORMAT_R32G32B32A32_FLOAT;

   /* R300 supports FP32 texture sampling but NOT FP32 render targets, so the
    * dot output is RGBA8 integer-encoded by the FS, not an FP32 RT.  Bail if
    * this variant lacks the required FP32 sampler carrier rather than
    * mis-sample the inputs as bytes. */
   if (!device->screen->is_format_supported(device->screen,
          input_fmt, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   return r3v_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->dp4.input_a_ssbo_binding,
      pl->dp4.input_b_ssbo_binding,
      pl->dp4.output_ssbo_binding,
      false, 0,
      input_fmt, PIPE_FORMAT_R8G8B8A8_UNORM,
      PIPE_FORMAT_NONE);
}

/* QMUL dispatch replay: the quaternion Hamilton product on the compute-as-raster
 * substrate.  Same two-in/one-out skeleton as DP4, but the inputs are the two
 * quaternions sampled as R32G32B32A32_FLOAT and the synthesized Hamilton FS
 * (r3v_build_qmul_fs_nir) writes the four-lane product to an FP16
 * (R16G16B16A16_FLOAT) render target -- R300 samples FP32 but has no FP32 RT,
 * and the substrate's quaternion result is FP16-precise.  The copy-back unpacks
 * the FP16 target into the kernel's vec4 FP32 output buffer.  Bail unless both
 * the FP32 sampler view and the FP16 render target are supported rather than
 * mis-format the pass. */
bool
r3v_qmul_dispatch_replay(struct r3v_device *device,
                            const struct r3v_pipeline *pl,
                            const struct r3v_cmd_dispatch *dispatch,
                            const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r3v_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->qmul.input_a_ssbo_binding,
      pl->qmul.input_b_ssbo_binding,
      pl->qmul.output_ssbo_binding,
      false, 0,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* QDIV dispatch replay: the quaternion quotient a/b on the substrate.  Identical
 * two-in/one-out skeleton to QMUL -- the dividend a and divisor b sampled as
 * R32G32B32A32_FLOAT, the synthesized division FS (r3v_build_qdiv_fs_nir) writes
 * a*inv(b) to an FP16 render target, and the copy-back unpacks it into the kernel's
 * vec4 FP32 output.  Bail unless both the FP32 sampler view and the FP16 render
 * target are supported. */
bool
r3v_qdiv_dispatch_replay(struct r3v_device *device,
                            const struct r3v_pipeline *pl,
                            const struct r3v_cmd_dispatch *dispatch,
                            const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r3v_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->qdiv.input_a_ssbo_binding,
      pl->qdiv.input_b_ssbo_binding,
      pl->qdiv.output_ssbo_binding,
      false, 0,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* QROTATE dispatch replay: rotate v by q on the substrate.  Same two-in/one-out
 * skeleton as QMUL -- inputs are the unit quaternion q and the vector v sampled
 * as R32G32B32A32_FLOAT, the synthesized sandwich FS (r3v_build_qrotate_fs_nir)
 * writes q*embed(v)*conj(q) to an FP16 render target, and the copy-back unpacks
 * it into the kernel's vec4 FP32 output. */
bool
r3v_qrotate_dispatch_replay(struct r3v_device *device,
                               const struct r3v_pipeline *pl,
                               const struct r3v_cmd_dispatch *dispatch,
                               const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r3v_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->qrotate.input_q_ssbo_binding,
      pl->qrotate.input_v_ssbo_binding,
      pl->qrotate.output_ssbo_binding,
      false, 0,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* Shared 1-in / 1-out compute-as-raster replay core.  Same skeleton as
 * r3v_two_in_one_out_dispatch_replay but a single input sampler stage and a
 * two-buffer positional fallback (input = first compute-visible
 * STORAGE_BUFFER, output = second).  The single-lane quaternion ops -- QCONJ
 * (sign flip) and QNORM (self
 * dot) -- sample one FP32 quaternion, render through the synthesized FS to an
 * FP16 target, and unpack into the kernel's vec4 FP32 output, the same FP16-RT /
 * FP32-readback conversion QMUL uses.  The caller passes the input + output
 * bindings its detector captured (0,0 triggers the positional fallback) and the
 * three formats. */
static bool
r3v_one_in_one_out_dispatch_replay(struct r3v_device *device,
                                      const struct r3v_pipeline *pl,
                                      const struct r3v_cmd_dispatch *dispatch,
                                      const struct r3v_cmd_bind_descriptor_sets *binds,
                                      uint32_t cap_in, uint32_t cap_out,
                                      bool detector_captured,
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
   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("1in1out early-return no-set-or-layout");
      return false;
   }

   /* Binding resolution, same priority as the 2-in core: captured constant
    * bindings win; all-zero means the detector saw opaque post-explicit_io
    * handles, so fall back to positional layout iteration (input = 1st
    * compute-visible STORAGE_BUFFER, output = 2nd).  A single-storage-buffer
    * layout is the in-place kernel (d[i] = f(d[i])): the one buffer serves both
    * roles, which is exact because the input is snapshot into the sampler
    * texture before the draw writes the buffer back. */
   uint32_t in_binding  = cap_in;
   uint32_t out_binding = cap_out;
   if (!detector_captured) {
      if (!nth_storage_buffer_binding(set, 0, &in_binding)) {
         IDM_LOG("1in1out early-return layout-has-no-storage-buffer");
         return false;
      }
      if (!nth_storage_buffer_binding(set, 1, &out_binding)) {
         out_binding = in_binding;
         IDM_LOG("1in1out single-buffer layout: in-place binding %u",
                 in_binding);
      }
   }
   IDM_LOG("1in1out bindings: in=%u out=%u source=%s",
           in_binding, out_binding, detector_captured ? "detector" : "positional");

   const struct r3v_descriptor *desc_in =
      find_descriptor_by_binding(set, in_binding);
   const struct r3v_descriptor *desc_out =
      find_descriptor_by_binding(set, out_binding);
   if (!desc_in || !desc_out) {
      IDM_LOG("1in1out early-return descriptor-walk-miss");
      return false;
   }
   if (!desc_in->buf.buffer || !desc_out->buf.buffer) {
      IDM_LOG("1in1out early-return null-vkbuffer-handle");
      return false;
   }
   VK_FROM_HANDLE(r3v_buffer, buf_in,  desc_in->buf.buffer);
   VK_FROM_HANDLE(r3v_buffer, buf_out, desc_out->buf.buffer);
   if (!buf_in || !buf_out || !buf_in->resource || !buf_out->resource) {
      IDM_LOG("1in1out early-return null-pipe-resource");
      return false;
   }

   const uint64_t total_invocations =
      r3v_idm_total_invocations(dispatch, pl);
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
      r3v_identity_map_wrap_input_as_sampler_view(device, buf_in->resource,
                                                     (unsigned)desc_in->buf.offset,
                                                     width, height,
                                                     total_invocations, input_fmt);
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
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_resource_reference(&rt, NULL);
      pipe_sampler_view_reference(&sv, NULL);
      return false;
   }

   r3v_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_cso.blend,
                                        device->identity_map_cso.rasterizer,
                                        device->identity_map_cso.dsa,
                                        pl->vs_cso, pl->fs_cso, velems_cso);
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1,
                             &device->identity_map_cso.sampler);
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
         uint64_t out_byte_count = 0;
         if (r3v_idm_element_byte_count(total_invocations, buf_bs,
                                           &out_byte_count)) {
            out_box.width  = (int)out_byte_count;
            out_box.height = 1;
            out_box.depth  = 1;
            void *out_bytes = pipe->buffer_map(
               pipe, buf_out->resource, 0,
               r3v_idm_buffer_write_flags((unsigned)desc_out->buf.offset,
                                             out_byte_count,
                                             buf_out->resource),
               &out_box, &out_xfer);
            if (out_bytes) {
               copy_ok = r3v_idm_copy_rt_rows_to_buffer(
                  out_bytes, rt_map, rt_xfer->stride, width, height,
                  total_invocations, fmt, buf_fmt, buf_bs);
               pipe->buffer_unmap(pipe, out_xfer);
            }
         }
         pipe->texture_unmap(pipe, rt_xfer);
      }
   }
   IDM_LOG("1in1out rt->buffer copy issued (out=%p, src=%p, box w=%d h=%d)",
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
 * (r3v_build_qconj_fs_nir) writes (a.x,-a.y,-a.z,-a.w) to an FP16 render
 * target, and the copy-back unpacks it into the kernel's vec4 FP32 output. */
bool
r3v_qconj_dispatch_replay(struct r3v_device *device,
                             const struct r3v_pipeline *pl,
                             const struct r3v_cmd_dispatch *dispatch,
                             const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r3v_one_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->qconj.input_ssbo_binding, pl->qconj.output_ssbo_binding,
      pl->qconj.input_ssbo_binding != 0 || pl->qconj.output_ssbo_binding != 0,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* QNORM dispatch replay: the quaternion squared norm on the substrate.  Same
 * one-in/one-out skeleton as QCONJ; the synthesized self-dot FS
 * (r3v_build_qnorm_fs_nir) writes vec4(dot(a,a)) to the FP16 target, unpacked
 * into the kernel's vec4 FP32 output (the kernel reads lane 0). */
bool
r3v_qnorm_dispatch_replay(struct r3v_device *device,
                             const struct r3v_pipeline *pl,
                             const struct r3v_cmd_dispatch *dispatch,
                             const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r3v_one_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->qnorm.input_ssbo_binding, pl->qnorm.output_ssbo_binding,
      pl->qnorm.input_ssbo_binding != 0 || pl->qnorm.output_ssbo_binding != 0,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* QNORMALIZE dispatch: the same one-in/one-out skeleton as QNORM; the synthesized
 * normalize FS (r3v_build_qnormalize_fs_nir) scales the sampled quaternion by
 * the US RSQ of its squared norm, written to the FP16 target and unpacked into the
 * kernel's vec4 FP32 output. */
bool
r3v_qnormalize_dispatch_replay(struct r3v_device *device,
                                  const struct r3v_pipeline *pl,
                                  const struct r3v_cmd_dispatch *dispatch,
                                  const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r3v_one_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->qnormalize.input_ssbo_binding, pl->qnormalize.output_ssbo_binding,
      pl->qnormalize.input_ssbo_binding != 0 ||
      pl->qnormalize.output_ssbo_binding != 0,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

bool
r3v_ieee16_classify_dispatch_replay(struct r3v_device *device,
                                       const struct r3v_pipeline *pl,
                                       const struct r3v_cmd_dispatch *dispatch,
                                       const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r3v_one_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->ieee16_classify.input_ssbo_binding, pl->ieee16_classify.output_ssbo_binding,
      pl->ieee16_classify.input_ssbo_binding != 0 ||
      pl->ieee16_classify.output_ssbo_binding != 0,
      PIPE_FORMAT_R32_FLOAT, PIPE_FORMAT_R8G8B8A8_UNORM,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

bool
r3v_ieee16_mul_dispatch_replay(struct r3v_device *device,
                                  const struct r3v_pipeline *pl,
                                  const struct r3v_cmd_dispatch *dispatch,
                                  const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r3v_one_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->ieee16_mul.input_ssbo_binding, pl->ieee16_mul.output_ssbo_binding,
      pl->ieee16_mul.input_ssbo_binding != 0 ||
      pl->ieee16_mul.output_ssbo_binding != 0,
      PIPE_FORMAT_R32G32_FLOAT, PIPE_FORMAT_R8G8B8A8_UNORM,
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
      uint64_t out_byte_count = 0;
      out_box.x      = out_offset;
      if (r3v_idm_element_byte_count(total, buf_bs, &out_byte_count)) {
         out_box.width  = (int)out_byte_count;
         out_box.height = 1;
         out_box.depth  = 1;
         void *out_bytes = pipe->buffer_map(
            pipe, out_res, 0,
            r3v_idm_buffer_write_flags(out_offset, out_byte_count, out_res),
            &out_box, &out_xfer);
         if (out_bytes) {
            uint8_t *dst = out_bytes;
            const uint8_t *src = rt_map;
            uint64_t remaining = total;
            for (unsigned r = 0; r < height && remaining; r++) {
               unsigned n = remaining < width ? (unsigned)remaining : width;
               util_format_unpack_rgba(PIPE_FORMAT_R16G16B16A16_FLOAT,
                                       dst, src, n);
               dst += (size_t)n * buf_bs;
               src += rt_xfer->stride;
               remaining -= n;
            }
            pipe->buffer_unmap(pipe, out_xfer);
            copy_ok = true;
         }
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
                 struct r3v_device *device,
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

   r3v_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_cso.blend,
                                        device->identity_map_cso.rasterizer,
                                        device->identity_map_cso.dsa,
                                        vs_cso, pass_fs, velems_cso);

   void *samplers[4] = { device->identity_map_cso.sampler,
                         device->identity_map_cso.sampler,
                         device->identity_map_cso.sampler,
                         device->identity_map_cso.sampler };
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
              struct r3v_device *device,
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
                  struct r3v_device *device,
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

   pipe->bind_blend_state(pipe, device->identity_map_cso.blend);
   pipe->bind_rasterizer_state(pipe, device->identity_map_cso.rasterizer);
   pipe->bind_depth_stencil_alpha_state(pipe, device->identity_map_cso.dsa);
   pipe->bind_vs_state(pipe, vs_cso);
   pipe->bind_fs_state(pipe, mrt_fs);
   pipe->bind_vertex_elements_state(pipe, velems_cso);

   void *samplers[4] = { device->identity_map_cso.sampler,
                         device->identity_map_cso.sampler,
                         device->identity_map_cso.sampler,
                         device->identity_map_cso.sampler };
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
r3v_omul_dispatch_replay(struct r3v_device *device,
                            const struct r3v_pipeline *pl,
                            const struct r3v_cmd_dispatch *dispatch,
                            const struct r3v_cmd_bind_descriptor_sets *binds)
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

   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   /* Six bindings: a,b,c,d inputs then o_lo,o_hi outputs.  Captured constants
    * win; all-zero means opaque post-explicit_io handles, so fall back to the
    * first six compute-visible STORAGE_BUFFERs in declaration order. */
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

   const struct r3v_descriptor *desc[6];
   struct r3v_buffer *buf[6];
   for (unsigned i = 0; i < 6; i++) {
      desc[i] = find_descriptor_by_binding(set, bind[i]);
      if (!desc[i] || !desc[i]->buf.buffer)
         return false;
      buf[i] = r3v_buffer_from_handle(desc[i]->buf.buffer);
      if (!buf[i] || !buf[i]->resource)
         return false;
   }

   const uint64_t total = r3v_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   struct pipe_sampler_view *views[4] = { NULL, NULL, NULL, NULL };
   for (unsigned i = 0; i < 4; i++) {
      views[i] = r3v_identity_map_wrap_input_as_sampler_view(
         device, buf[i]->resource, (unsigned)desc[i]->buf.offset,
         width, height, total, PIPE_FORMAT_R32G32B32A32_FLOAT);
      if (!views[i]) {
         for (unsigned k = 0; k < i; k++)
            pipe_sampler_view_reference(&views[k], NULL);
         return false;
      }
   }

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      for (unsigned i = 0; i < 4; i++)
         pipe_sampler_view_reference(&views[i], NULL);
      return false;
   }

   /* Route B (MRT) is preferred when its FS was synthesized; R3V_OMUL_FORCE_2PASS
    * forces the route-A fallback on the same hardware to exercise both paths. */
   bool ok;
   if (pl->fs_cso_mrt && !r3v_getenv_compat("R3V_OMUL_FORCE_2PASS", "R300VK_OMUL_FORCE_2PASS")) {
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
r3v_mat4vec_dispatch_replay(struct r3v_device *device,
                               const struct r3v_pipeline *pl,
                               const struct r3v_cmd_dispatch *dispatch,
                               const struct r3v_cmd_bind_descriptor_sets *binds)
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

   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   /* Three bindings: matrix, vertices, output.  The detector captures the matrix
    * binding (and defaults vertices=1, output=2); if a lookup misses, fall back
    * to the first three compute-visible STORAGE_BUFFERs in declaration order. */
   uint32_t bind[3] = { pl->mat4vec.matrix_ssbo_binding,
                        pl->mat4vec.vertex_ssbo_binding,
                        pl->mat4vec.output_ssbo_binding };
   const struct r3v_descriptor *desc[3];
   struct r3v_buffer *buf[3];
   for (unsigned i = 0; i < 3; i++) {
      desc[i] = find_descriptor_by_binding(set, bind[i]);
      if (!desc[i] || !desc[i]->buf.buffer) {
         if (!nth_storage_buffer_binding(set, i, &bind[i]))
            return false;
         desc[i] = find_descriptor_by_binding(set, bind[i]);
         if (!desc[i] || !desc[i]->buf.buffer)
            return false;
      }
      buf[i] = r3v_buffer_from_handle(desc[i]->buf.buffer);
      if (!buf[i] || !buf[i]->resource)
         return false;
   }

   const uint64_t total = r3v_idm_total_invocations(dispatch, pl);
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
   views[0] = r3v_identity_map_wrap_input_as_sampler_view(
      device, buf[1]->resource, (unsigned)desc[1]->buf.offset,
      width, height, total, PIPE_FORMAT_R32G32B32A32_FLOAT);
   if (!views[0])
      return false;

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
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
 * a sampler.  Three bindings: scalar, quaternions, output.  The detector's
 * binding indices win only when all three binding sources were constants;
 * otherwise the roles are recovered from the first three compute-visible
 * STORAGE_BUFFER declarations in semantic order. */
bool
r3v_qfmul_dispatch_replay(struct r3v_device *device,
                             const struct r3v_pipeline *pl,
                             const struct r3v_cmd_dispatch *dispatch,
                             const struct r3v_cmd_bind_descriptor_sets *binds)
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

   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t bind[3] = { pl->qfmul.scalar_ssbo_binding,
                        pl->qfmul.quat_ssbo_binding,
                        pl->qfmul.output_ssbo_binding };
   const bool detector_captured =
      pl->qfmul.scalar_ssbo_binding_valid &&
      pl->qfmul.quat_ssbo_binding_valid &&
      pl->qfmul.output_ssbo_binding_valid;
   if (!detector_captured) {
      for (unsigned i = 0; i < 3; i++) {
         if (!nth_storage_buffer_binding(set, i, &bind[i]))
            return false;
      }
   }

   const struct r3v_descriptor *desc[3];
   struct r3v_buffer *buf[3];
   for (unsigned i = 0; i < 3; i++) {
      desc[i] = find_descriptor_by_binding(set, bind[i]);
      if (!desc[i] || !desc[i]->buf.buffer)
         return false;
      buf[i] = r3v_buffer_from_handle(desc[i]->buf.buffer);
      if (!buf[i] || !buf[i]->resource)
         return false;
   }

   const uint64_t total = r3v_idm_total_invocations(dispatch, pl);
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
   views[0] = r3v_identity_map_wrap_input_as_sampler_view(
      device, buf[1]->resource, (unsigned)desc[1]->buf.offset,
      width, height, total, PIPE_FORMAT_R32G32B32A32_FLOAT);
   if (!views[0])
      return false;

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
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
 * to the first n compute-visible STORAGE_BUFFERs in declaration order (the
 * inputs precede the outputs in every octonion kernel). */
static bool
octonion_resolve_buffers(const struct r3v_descriptor_set *set, uint32_t *bind,
                         unsigned n, const struct r3v_descriptor **desc,
                         struct r3v_buffer **buf)
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
      buf[i] = r3v_buffer_from_handle(desc[i]->buf.buffer);
      if (!buf[i] || !buf[i]->resource)
         return false;
   }
   return true;
}

/* ONORM dispatch: |(a,b)|^2 = dot(a,a)+dot(b,b).  Two inputs, one output -- the
 * 2-in/1-out core with the synthesized self-dot-sum FS in pl->fs_cso. */
bool
r3v_onorm_dispatch_replay(struct r3v_device *device,
                             const struct r3v_pipeline *pl,
                             const struct r3v_cmd_dispatch *dispatch,
                             const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_SAMPLER_VIEW))
      return false;
   if (!device->screen->is_format_supported(device->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET))
      return false;
   return r3v_two_in_one_out_dispatch_replay(
      device, pl, dispatch, binds,
      pl->onorm.input_a_ssbo_binding, pl->onorm.input_b_ssbo_binding,
      pl->onorm.output_ssbo_binding,
      false, 0,
      PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT);
}

/* OCONJ dispatch: conj((a,b)) = (conj(a), -b) in one MRT pass.  Two inputs a,b
 * sampled at stages 0,1; the MRT FS (pl->fs_cso_mrt) writes conj(a) to o_lo and
 * -b to o_hi. */
bool
r3v_oconj_dispatch_replay(struct r3v_device *device,
                             const struct r3v_pipeline *pl,
                             const struct r3v_cmd_dispatch *dispatch,
                             const struct r3v_cmd_bind_descriptor_sets *binds)
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
   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t bind[4] = { pl->oconj.input_a_ssbo_binding,
                        pl->oconj.input_b_ssbo_binding,
                        pl->oconj.output_lo_ssbo_binding,
                        pl->oconj.output_hi_ssbo_binding };
   const struct r3v_descriptor *desc[4];
   struct r3v_buffer *buf[4];
   if (!octonion_resolve_buffers(set, bind, 4, desc, buf))
      return false;

   const uint64_t total = r3v_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   struct pipe_sampler_view *views[2] = { NULL, NULL };
   for (unsigned i = 0; i < 2; i++) {
      views[i] = r3v_identity_map_wrap_input_as_sampler_view(
         device, buf[i]->resource, (unsigned)desc[i]->buf.offset,
         width, height, total, PIPE_FORMAT_R32G32B32A32_FLOAT);
      if (!views[i]) {
         for (unsigned k = 0; k < i; k++)
            pipe_sampler_view_reference(&views[k], NULL);
         return false;
      }
   }
   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
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
r3v_oaddsub_dispatch_replay(struct r3v_device *device,
                               const struct r3v_pipeline *pl,
                               const struct r3v_cmd_dispatch *dispatch,
                               const struct r3v_cmd_bind_descriptor_sets *binds)
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
   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t bind[6] = { pl->oaddsub.input_a_ssbo_binding,
                        pl->oaddsub.input_b_ssbo_binding,
                        pl->oaddsub.input_c_ssbo_binding,
                        pl->oaddsub.input_d_ssbo_binding,
                        pl->oaddsub.output_lo_ssbo_binding,
                        pl->oaddsub.output_hi_ssbo_binding };
   const struct r3v_descriptor *desc[6];
   struct r3v_buffer *buf[6];
   if (!octonion_resolve_buffers(set, bind, 6, desc, buf))
      return false;

   const uint64_t total = r3v_idm_total_invocations(dispatch, pl);
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
      views[i] = r3v_identity_map_wrap_input_as_sampler_view(
         device, buf[src[i]]->resource, (unsigned)desc[src[i]]->buf.offset,
         width, height, total, PIPE_FORMAT_R32G32B32A32_FLOAT);
      if (!views[i]) {
         for (unsigned k = 0; k < i; k++)
            pipe_sampler_view_reference(&views[k], NULL);
         return false;
      }
   }
   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
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
r3v_odiv_dispatch_replay(struct r3v_device *device,
                            const struct r3v_pipeline *pl,
                            const struct r3v_cmd_dispatch *dispatch,
                            const struct r3v_cmd_bind_descriptor_sets *binds)
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
   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t bind[6] = { pl->odiv.input_xlo_ssbo_binding,
                        pl->odiv.input_xhi_ssbo_binding,
                        pl->odiv.input_ylo_ssbo_binding,
                        pl->odiv.input_yhi_ssbo_binding,
                        pl->odiv.output_lo_ssbo_binding,
                        pl->odiv.output_hi_ssbo_binding };
   const struct r3v_descriptor *desc[6];
   struct r3v_buffer *buf[6];
   if (!octonion_resolve_buffers(set, bind, 6, desc, buf))
      return false;

   const uint64_t total = r3v_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   struct pipe_sampler_view *views[4] = { NULL, NULL, NULL, NULL };
   for (unsigned i = 0; i < 4; i++) {
      views[i] = r3v_identity_map_wrap_input_as_sampler_view(
         device, buf[i]->resource, (unsigned)desc[i]->buf.offset,
         width, height, total, PIPE_FORMAT_R32G32B32A32_FLOAT);
      if (!views[i]) {
         for (unsigned k = 0; k < i; k++)
            pipe_sampler_view_reference(&views[k], NULL);
         return false;
      }
   }
   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
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
r3v_otrans_dispatch_replay(struct r3v_device *device,
                              const struct r3v_pipeline *pl,
                              const struct r3v_cmd_dispatch *dispatch,
                              const struct r3v_cmd_bind_descriptor_sets *binds)
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
   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t bind[6] = { pl->otrans.input_xlo_ssbo_binding,
                        pl->otrans.input_xhi_ssbo_binding,
                        pl->otrans.input_vlo_ssbo_binding,
                        pl->otrans.input_vhi_ssbo_binding,
                        pl->otrans.output_lo_ssbo_binding,
                        pl->otrans.output_hi_ssbo_binding };
   const struct r3v_descriptor *desc[6];
   struct r3v_buffer *buf[6];
   if (!octonion_resolve_buffers(set, bind, 6, desc, buf))
      return false;

   const uint64_t total = r3v_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   /* x and v inputs as FP32 sampler views for pass 1 (OMUL(x,v)). */
   struct pipe_sampler_view *xv[4] = { NULL, NULL, NULL, NULL };
   for (unsigned i = 0; i < 4; i++) {
      xv[i] = r3v_identity_map_wrap_input_as_sampler_view(
         device, buf[i]->resource, (unsigned)desc[i]->buf.offset,
         width, height, total, PIPE_FORMAT_R32G32B32A32_FLOAT);
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
             r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso);

   /* Pass 1: t = x*v to the scratch halves. */
   if (ok)
      ok = omul_run_pass(pipe, screen, device, xv, pl->fs_cso, pl->vs_cso, vb,
                         velems_cso, t_lo, 0, width, height, total) &&
           omul_run_pass(pipe, screen, device, xv, pl->fs_cso2, pl->vs_cso, vb,
                         velems_cso, t_hi, 0, width, height, total);

   /* Pass 2: out = t*conj(x), sampling t at 0,1 and x at 2,3. */
   if (ok) {
      struct pipe_sampler_view *tx[4] = { NULL, NULL, NULL, NULL };
      tx[0] = r3v_identity_map_wrap_input_as_sampler_view(
         device, t_lo, 0, width, height, total,
         PIPE_FORMAT_R32G32B32A32_FLOAT);
      tx[1] = r3v_identity_map_wrap_input_as_sampler_view(
         device, t_hi, 0, width, height, total,
         PIPE_FORMAT_R32G32B32A32_FLOAT);
      tx[2] = r3v_identity_map_wrap_input_as_sampler_view(
         device, buf[0]->resource, (unsigned)desc[0]->buf.offset, width, height,
         total, PIPE_FORMAT_R32G32B32A32_FLOAT);
      tx[3] = r3v_identity_map_wrap_input_as_sampler_view(
         device, buf[1]->resource, (unsigned)desc[1]->buf.offset, width, height,
         total, PIPE_FORMAT_R32G32B32A32_FLOAT);
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
r3v_qfm3_run(struct r3v_device *device, const struct r3v_pipeline *pl,
                const struct r3v_cmd_dispatch *dispatch,
                const struct r3v_cmd_bind_descriptor_sets *binds,
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
   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout)
      return false;

   uint32_t bind[4] = { a_bind, b_bind, c_bind, out_bind };
   const struct r3v_descriptor *desc[4];
   struct r3v_buffer *buf[4];
   if (!octonion_resolve_buffers(set, bind, 4, desc, buf))
      return false;

   const uint64_t total = r3v_idm_total_invocations(dispatch, pl);
   if (total == 0 || total > 2048u * 2048u)
      return false;
   unsigned width = 0, height = 0;
   derive_raster_extent((uint32_t)total, &width, &height);
   if (width > 2048 || height > 2048)
      return false;

   struct pipe_sampler_view *views[4] = { NULL, NULL, NULL, NULL };
   for (unsigned i = 0; i < 3; i++) {
      views[i] = r3v_identity_map_wrap_input_as_sampler_view(
         device, buf[i]->resource, (unsigned)desc[i]->buf.offset,
         width, height, total, PIPE_FORMAT_R32G32B32A32_FLOAT);
      if (!views[i]) {
         for (unsigned k = 0; k < i; k++)
            pipe_sampler_view_reference(&views[k], NULL);
         return false;
      }
   }
   pipe_sampler_view_reference(&views[3], views[0]); /* dummy stage 3, FS ignores */

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   bool ok = r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso);
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
r3v_qfmadd_dispatch_replay(struct r3v_device *device,
                              const struct r3v_pipeline *pl,
                              const struct r3v_cmd_dispatch *dispatch,
                              const struct r3v_cmd_bind_descriptor_sets *binds)
{
   return r3v_qfm3_run(device, pl, dispatch, binds,
                          pl->qfmadd.input_a_ssbo_binding,
                          pl->qfmadd.input_b_ssbo_binding,
                          pl->qfmadd.input_c_ssbo_binding,
                          pl->qfmadd.output_ssbo_binding, pl->fs_cso);
}

/* QFMMUL dispatch: out = a*b*c = (a*b)*c in one pass (pl->fs_cso is the QFMMUL FS). */
bool
r3v_qfmmul_dispatch_replay(struct r3v_device *device,
                              const struct r3v_pipeline *pl,
                              const struct r3v_cmd_dispatch *dispatch,
                              const struct r3v_cmd_bind_descriptor_sets *binds)
{
   return r3v_qfm3_run(device, pl, dispatch, binds,
                          pl->qfmmul.input_a_ssbo_binding,
                          pl->qfmmul.input_b_ssbo_binding,
                          pl->qfmmul.input_c_ssbo_binding,
                          pl->qfmmul.output_ssbo_binding, pl->fs_cso);
}

/* Multi-tap gather orchestrator: identity-map skeleton (one input sampler
 * view, two storage buffers) plus a per-dispatch fragment constant carrying
 * the neighbor texel displacement.  The synthesized FS
 * (r3v_synthesize_multitap_gather_fs) samples the input at three
 * neighborhood offsets and sums them; everything else (RT, VBO, framebuffer,
 * viewport, scissor, draw, copy-back) is the identity-map path. */
bool
r3v_multitap_gather_dispatch_replay(struct r3v_device *device,
                                       const struct r3v_pipeline *pl,
                                       const struct r3v_cmd_dispatch *dispatch,
                                       const struct r3v_cmd_bind_descriptor_sets *binds)
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
   const struct r3v_descriptor_set *set = binds->sets[0];
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
   const struct r3v_descriptor *descs[2] = {0};
   struct r3v_buffer *bufs[2] = {0};
   if (!r3v_idm_resolve_buffers(device, set, 2, bindings, descs, bufs))
      return false;
   const struct r3v_descriptor *in_desc = descs[0];
   const struct r3v_descriptor *out_desc = descs[1];
   struct r3v_buffer *in_buf = bufs[0];
   struct r3v_buffer *out_buf = bufs[1];

   const uint64_t total_invocations =
      r3v_idm_total_invocations(dispatch, pl);
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
      r3v_identity_map_wrap_input_as_sampler_view(device, in_buf->resource,
                                                     (unsigned)in_desc->buf.offset,
                                                     width, height,
                                                     total_invocations, fmt);
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

   r3v_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_cso.blend,
                                        device->identity_map_cso.rasterizer,
                                        device->identity_map_cso.dsa,
                                        pl->vs_cso, pl->fs_cso, velems_cso);

   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1,
                             &device->identity_map_cso.sampler);
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
         const unsigned blocksize = util_format_get_blocksize(fmt);
         uint64_t out_byte_count = 0;
         out_box.x      = (unsigned)out_desc->buf.offset;
         if (r3v_idm_element_byte_count(total_invocations, blocksize,
                                           &out_byte_count)) {
            out_box.width  = (int)out_byte_count;
            out_box.height = 1;
            out_box.depth  = 1;
            void *out_bytes = pipe->buffer_map(
               pipe, out_buf->resource, 0,
               r3v_idm_buffer_write_flags((unsigned)out_desc->buf.offset,
                                             out_byte_count,
                                             out_buf->resource),
               &out_box, &out_xfer);
            if (out_bytes) {
               r3v_identity_map_copy_rows(out_bytes, width * blocksize,
                                             rt_map, rt_xfer->stride,
                                             width, height, blocksize,
                                             total_invocations);
               pipe->buffer_unmap(pipe, out_xfer);
               copy_ok = true;
            }
         }
         pipe->texture_unmap(pipe, rt_xfer);
      }
   }
   IDM_LOG("gather rt->buffer copy issued (out=%p, src=%p, box w=%d h=%d)",
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
r3v_predicated_store_dispatch_replay(struct r3v_device *device,
                                        const struct r3v_pipeline *pl,
                                        const struct r3v_cmd_dispatch *dispatch,
                                        const struct r3v_cmd_bind_descriptor_sets *binds)
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
   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("predstore early-return no-set-or-layout");
      return false;
   }

   /* Positional binding resolution uses binding 0 for the predicate, 1 for
    * the value, and 2 for output when explicit-IO binding sources are not
    * constants. */
   uint32_t pred_binding = 0, val_binding = 0, out_binding = 0;
   if (!nth_storage_buffer_binding(set, 0, &pred_binding) ||
       !nth_storage_buffer_binding(set, 1, &val_binding) ||
       !nth_storage_buffer_binding(set, 2, &out_binding)) {
      IDM_LOG("predstore early-return layout-has-fewer-than-three-storage-buffers");
      return false;
   }
   IDM_LOG("predstore bindings: pred=%u val=%u out=%u",
           pred_binding, val_binding, out_binding);
   const struct r3v_descriptor *pred_desc =
      find_descriptor_by_binding(set, pred_binding);
   const struct r3v_descriptor *val_desc =
      find_descriptor_by_binding(set, val_binding);
   const struct r3v_descriptor *out_desc =
      find_descriptor_by_binding(set, out_binding);
   if (!pred_desc || !val_desc || !out_desc ||
       !pred_desc->buf.buffer || !val_desc->buf.buffer ||
       !out_desc->buf.buffer) {
      IDM_LOG("predstore early-return descriptor-walk-miss");
      return false;
   }
   VK_FROM_HANDLE(r3v_buffer, pred_buf, pred_desc->buf.buffer);
   VK_FROM_HANDLE(r3v_buffer, val_buf,  val_desc->buf.buffer);
   VK_FROM_HANDLE(r3v_buffer, out_buf,  out_desc->buf.buffer);
   if (!pred_buf || !val_buf || !out_buf ||
       !pred_buf->resource || !val_buf->resource || !out_buf->resource) {
      IDM_LOG("predstore early-return null-pipe-resource");
      return false;
   }

   const uint64_t total_invocations =
      r3v_idm_total_invocations(dispatch, pl);
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

   /* Wrap the predicate and value buffers as PIPE_TEXTURE_2D sampler views. */
   struct pipe_sampler_view *sv_pred =
      r3v_identity_map_wrap_input_as_sampler_view(device, pred_buf->resource,
                                                     (unsigned)pred_desc->buf.offset,
                                                     width, height,
                                                     total_invocations, fmt);
   if (!sv_pred) {
      IDM_LOG("predstore early-return pred-wrap-failed");
      return false;
   }
   struct pipe_sampler_view *sv_val =
      r3v_identity_map_wrap_input_as_sampler_view(device, val_buf->resource,
                                                     (unsigned)val_desc->buf.offset,
                                                     width, height,
                                                     total_invocations, fmt);
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
      uint64_t out_byte_count = 0;
      if (!r3v_idm_element_byte_count(total_invocations, bpp,
                                         &out_byte_count)) {
         pipe_resource_reference(&rt, NULL);
         pipe_sampler_view_reference(&sv_val, NULL);
         pipe_sampler_view_reference(&sv_pred, NULL);
         IDM_LOG("predstore early-return out-seed-map-range-overflow");
         return false;
      }
      out_box.x      = (unsigned)out_desc->buf.offset;
      out_box.width  = (int)out_byte_count;
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
      r3v_identity_map_copy_rows(rt_seed, rt_xfer->stride,
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

   r3v_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_cso.blend,
                                        device->identity_map_cso.rasterizer,
                                        device->identity_map_cso.dsa,
                                        pl->vs_cso, pl->fs_cso, velems_cso);
   void *samplers[2] = { device->identity_map_cso.sampler,
                         device->identity_map_cso.sampler };
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
         uint64_t out_byte_count = 0;
         out_box.x      = (unsigned)out_desc->buf.offset;
         if (r3v_idm_element_byte_count(total_invocations, bpp,
                                           &out_byte_count)) {
            out_box.width  = (int)out_byte_count;
            out_box.height = 1; out_box.depth = 1;
            void *out_bytes = pipe->buffer_map(
               pipe, out_buf->resource, 0,
               r3v_idm_buffer_write_flags((unsigned)out_desc->buf.offset,
                                             out_byte_count,
                                             out_buf->resource),
               &out_box, &out_xfer);
            if (out_bytes) {
               r3v_identity_map_copy_rows(out_bytes, width * bpp,
                                             rt_map, rt_xfer->stride,
                                             width, height, bpp,
                                             total_invocations);
               pipe->buffer_unmap(pipe, out_xfer);
               copy_ok = true;
            }
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
 *      blend-disabled identity_map_cso.blend.
 *
 * Other surfaces (rasterizer / dsa / sampler CSOs, framebuffer + viewport
 * + scissor setup, copy-back path) reuse the identity-map orchestrator's
 * shape verbatim. */
bool
r3v_blend_acc_reduction_dispatch_replay(struct r3v_device *device,
                                           const struct r3v_pipeline *pl,
                                           const struct r3v_cmd_dispatch *dispatch,
                                           const struct r3v_cmd_bind_descriptor_sets *binds)
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
   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("blend_acc early-return no-set-or-layout");
      return false;
   }

   /* Complete detector capture is authoritative, including binding zero.  An
    * all-opaque capture uses the exact two-role positional contract; partial
    * capture is refused rather than pairing a known role with a guessed one. */
   uint32_t value_binding  = pl->blend_acc_reduction.value_ssbo_binding;
   uint32_t output_binding = pl->blend_acc_reduction.output_ssbo_binding;
   const unsigned binding_valid_mask =
      (pl->blend_acc_reduction.value_ssbo_binding_valid ? 1u : 0u) |
      (pl->blend_acc_reduction.output_ssbo_binding_valid ? 2u : 0u);
   uint32_t bindings[2] = { value_binding, output_binding };
   if (!idm_resolve_binding_roles(device, set, 2, bindings,
                                  binding_valid_mask)) {
      IDM_LOG("blend_acc early-return binding-resolution");
      return false;
   }
   const struct r3v_descriptor *descs[2] = {0};
   struct r3v_buffer *bufs[2] = {0};
   if (!r3v_idm_resolve_buffers(device, set, 2, bindings, descs, bufs))
      return false;
   const struct r3v_descriptor *in_desc = descs[0];
   const struct r3v_descriptor *out_desc = descs[1];
   struct r3v_buffer *in_buf = bufs[0];
   struct r3v_buffer *out_buf = bufs[1];

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
      r3v_idm_total_invocations(dispatch, pl);
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
   if (!r3v_idm_create_blend_acc_vbo(pipe, in_buf->resource, (unsigned)in_desc->buf.offset, N, M, &vb, &velems_cso)) {
      pipe_resource_reference(&rt, NULL);
      IDM_LOG("blend_acc early-return vbo-create-failed");
      return false;
   }

   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = fmt;
   surf_templ.texture = rt;

   r3v_identity_map_setup_draw_state(pipe, M, 1, &surf_templ,
                                        device->blend_acc_reduction_blend_cso,
                                        device->identity_map_cso.rasterizer,
                                        device->identity_map_cso.dsa,
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
            r3v_identity_map_copy_rows(out_bytes, (unsigned)out_byte_size,
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
 *      fragments; the existing count_out[0] value is read and the u32
 *      atomicAdd increment is applied with wraparound.
 *
 * No blend (only the ZPASS path matters); RT clear unnecessary (RT
 * contents never read).  Other surfaces reuse the identity-map
 * orchestrator's shape verbatim. */
bool
r3v_zpass_reduction_dispatch_replay(struct r3v_device *device,
                                       const struct r3v_pipeline *pl,
                                       const struct r3v_cmd_dispatch *dispatch,
                                       const struct r3v_cmd_bind_descriptor_sets *binds)
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
   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("zpass early-return no-set-or-layout");
      return false;
   }
   uint32_t value_binding  = pl->zpass_reduction.value_ssbo_binding;
   uint32_t output_binding = pl->zpass_reduction.output_ssbo_binding;
   const unsigned binding_valid_mask =
      (pl->zpass_reduction.value_ssbo_binding_valid ? 1u : 0u) |
      (pl->zpass_reduction.output_ssbo_binding_valid ? 2u : 0u);
   uint32_t bindings[2] = { value_binding, output_binding };
   if (!idm_resolve_binding_roles(device, set, 2, bindings,
                                  binding_valid_mask)) {
      IDM_LOG("zpass early-return binding-resolution");
      return false;
   }
   const struct r3v_descriptor *descs[2] = {0};
   struct r3v_buffer *bufs[2] = {0};
   if (!r3v_idm_resolve_buffers(device, set, 2, bindings, descs, bufs))
      return false;
   const struct r3v_descriptor *in_desc = descs[0];
   const struct r3v_descriptor *out_desc = descs[1];
   struct r3v_buffer *in_buf = bufs[0];
   struct r3v_buffer *out_buf = bufs[1];

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
   if (!r3v_idm_compute_raster_grid(device, dispatch, pl, &total_invocations,
                                       &width, &height))
      return false;
   if (total_invocations > 2048u) {
      IDM_LOG("zpass early-return total_invocations=%llu exceeds-1d-axis-cap",
              (unsigned long long)total_invocations);
      return false;
   }
   const uint32_t N = (uint32_t)total_invocations;
   IDM_LOG("dispatch N=%u", N);

   const enum pipe_format fmt = PIPE_FORMAT_R8G8B8A8_UNORM;

   /* N x 1 RT: one rasterization slot per gid so distinct (x, 0) point
    * fragments do not collapse under per-pixel deduplication.  RT
    * contents are unused; only the ZPASS counter matters.  This untiled
    * point form is bounded by the 2048-wide R300 render-target axis cap. */
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
   if (!r3v_idm_create_zpass_vbo(pipe, in_buf->resource, (unsigned)in_desc->buf.offset, N, &vb, &velems_cso)) {
      pipe_resource_reference(&rt, NULL);
      IDM_LOG("zpass early-return vbo-create-failed");
      return false;
   }

   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = fmt;
   surf_templ.texture = rt;

   r3v_identity_map_setup_draw_state(pipe, N, 1, &surf_templ,
                                        device->identity_map_cso.blend,
                                        device->identity_map_cso.rasterizer,
                                        device->identity_map_cso.dsa,
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
   /* The admitted atomic is a 32-bit atomicAdd.  Preserve its existing
    * counter value and apply the query increment with uint32 wraparound. */
   const uint64_t raw_sum = qr.u64;
   const uint32_t increment = (uint32_t)raw_sum;

   uint32_t existing = 0;
   bool existing_ok = false;
   {
      struct pipe_box out_box;
      memset(&out_box, 0, sizeof(out_box));
      out_box.x      = (unsigned)out_desc->buf.offset;
      out_box.width  = (unsigned)sizeof(uint32_t);
      out_box.height = 1; out_box.depth = 1;
      struct pipe_transfer *out_xfer = NULL;
      const void *out_bytes = pipe->buffer_map(pipe, out_buf->resource, 0,
                                                PIPE_MAP_READ, &out_box,
                                                &out_xfer);
      if (out_bytes) {
         memcpy(&existing, out_bytes, sizeof(existing));
         pipe->buffer_unmap(pipe, out_xfer);
         existing_ok = true;
      }
   }
   if (!existing_ok) {
      pipe->set_vertex_buffers(pipe, 0, NULL);
      struct pipe_framebuffer_state empty_fb;
      memset(&empty_fb, 0, sizeof(empty_fb));
      pipe->set_framebuffer_state(pipe, &empty_fb);
      pipe->delete_vertex_elements_state(pipe, velems_cso);
      pipe_resource_reference(&vb, NULL);
      pipe_resource_reference(&rt, NULL);
      IDM_LOG("zpass early-return output-counter-read-failed");
      return false;
   }
   const uint32_t updated = existing + increment;
   IDM_LOG("zpass query u64=%llu increment_u32=%u existing=%u updated=%u",
           (unsigned long long)raw_sum, increment, existing, updated);

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
         memcpy(out_bytes, &updated, sizeof(uint32_t));
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
 *      push-constant plumbing exists (r3v advertises maxPushConstantsSize
 *      but has no R3V_CMD_PUSH_CONSTANTS recording path), so the count
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
r3v_multipass_scan_dispatch_replay(struct r3v_device *device,
                                      const struct r3v_pipeline *pl,
                                      const struct r3v_cmd_dispatch *dispatch,
                                      const struct r3v_cmd_bind_descriptor_sets *binds)
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
   const struct r3v_descriptor_set *set = binds->sets[0];
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
   const struct r3v_descriptor *in_desc =
      find_descriptor_by_binding(set, in_binding);
   const struct r3v_descriptor *out_desc =
      find_descriptor_by_binding(set, out_binding);
   const struct r3v_descriptor *params_desc =
      find_descriptor_by_binding(set, params_binding);
   if (!in_desc || !out_desc || !params_desc ||
       !in_desc->buf.buffer || !out_desc->buf.buffer ||
       !params_desc->buf.buffer) {
      IDM_LOG("multipass early-return descriptor-walk-miss");
      return false;
   }
   VK_FROM_HANDLE(r3v_buffer, in_buf,     in_desc->buf.buffer);
   VK_FROM_HANDLE(r3v_buffer, out_buf,    out_desc->buf.buffer);
   VK_FROM_HANDLE(r3v_buffer, params_buf, params_desc->buf.buffer);
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
      r3v_idm_total_invocations(dispatch, pl);
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
      uint64_t in_byte_count = 0;
      if (!r3v_idm_element_byte_count(total_invocations, bpp,
                                         &in_byte_count)) {
         pipe_resource_reference(&tex[0], NULL);
         pipe_resource_reference(&tex[1], NULL);
         IDM_LOG("multipass early-return in-map-range-overflow");
         return false;
      }
      in_box.x      = (unsigned)in_desc->buf.offset;
      in_box.width  = (int)in_byte_count;
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
      r3v_identity_map_copy_rows(t_map, t_xfer->stride,
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
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_resource_reference(&tex[0], NULL);
      pipe_resource_reference(&tex[1], NULL);
      return false;
   }

   /* State that is constant across all passes: blend off, no cull, depth
    * off, NEAREST sampler, the doubling FS + passthrough VS, the velems,
    * the fullscreen VB, viewport, scissor. */
   pipe->bind_blend_state(pipe, device->identity_map_cso.blend);
   pipe->bind_rasterizer_state(pipe, device->identity_map_cso.rasterizer);
   pipe->bind_depth_stencil_alpha_state(pipe, device->identity_map_cso.dsa);
   pipe->bind_vs_state(pipe, pl->vs_cso);
   pipe->bind_fs_state(pipe, pl->fs_cso);
   pipe->bind_vertex_elements_state(pipe, velems_cso);
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1,
                             &device->identity_map_cso.sampler);
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
   bool copy_ok = r3v_identity_map_readback_rt(pipe, tex[src_idx], out_buf->resource,
                                                  (unsigned)out_desc->buf.offset,
                                                  width, height, fmt,
                                                  width * util_format_get_blocksize(fmt),
                                                  total_invocations);

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

bool
r3v_const_fill_dispatch_replay(struct r3v_device *device,
                                   const struct r3v_pipeline *pl,
                                   const struct r3v_cmd_dispatch *dispatch,
                                   const struct r3v_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe = device->pipe;
   IDM_LOG("constfill entry pl=%p is_const_fill=%d gx=%u gy=%u gz=%u",
           (const void *)pl,
           pl ? (int)pl->const_fill.is_const_fill : -1,
           dispatch ? dispatch->group_count_x : 0,
           dispatch ? dispatch->group_count_y : 0,
           dispatch ? dispatch->group_count_z : 0);
   if (!pipe || !pl || !dispatch || !binds || binds->set_count == 0) {
      IDM_LOG("constfill early-return null-or-empty-binds");
      return false;
   }
   if (binds->first_set != 0) {
      IDM_LOG("constfill early-return first_set=%u (only slot 0 supported)",
              binds->first_set);
      return false;
   }

   /* Only scalar single-component fills are reconstructable from the 4-byte
    * const_value.  Multi-component fills where components differ would need
    * all component bytes, but the pattern struct stores only component 0. */
   if (pl->const_fill.value_components != 1 || pl->const_fill.value_bit_size != 32) {
      IDM_LOG("constfill early-return unsupported components=%u bits=%u",
              pl->const_fill.value_components, pl->const_fill.value_bit_size);
      return false;
   }

   const struct r3v_descriptor_set *set = binds->sets[0];
   if (!set || !set->layout) {
      IDM_LOG("constfill early-return no-set-or-layout");
      return false;
   }

   uint32_t out_binding = pl->const_fill.output_ssbo_binding;
   if (!pl->const_fill.output_ssbo_binding_valid) {
      if (!single_storage_buffer_binding(set, &out_binding)) {
         IDM_LOG("constfill early-return ambiguous-output-binding");
         return false;
      }
      IDM_LOG("constfill recovered binding from layout: out=%u", out_binding);
   }

   const struct r3v_descriptor *out_desc = find_descriptor_by_binding(set, out_binding);
   IDM_LOG("constfill out_binding=%u out_desc=%p", out_binding, (const void *)out_desc);
   if (!out_desc || !out_desc->buf.buffer) {
      IDM_LOG("constfill early-return descriptor-walk-miss");
      return false;
   }

   VK_FROM_HANDLE(r3v_buffer, out_buf, out_desc->buf.buffer);
   if (!out_buf || !out_buf->resource) {
      IDM_LOG("constfill early-return null-pipe-resource");
      return false;
   }

   const uint64_t total_invocations = r3v_idm_total_invocations(dispatch, pl);
   if (total_invocations == 0) {
      IDM_LOG("constfill early-return total_invocations=%llu",
              (unsigned long long)total_invocations);
      return false;
   }

   /* 4 bytes per invocation (1 component x 32 bits). */
   const unsigned element_bytes = 4u;
   if (total_invocations > UINT64_MAX / element_bytes) {
      IDM_LOG("constfill early-return fill_bytes overflow");
      return false;
   }
   const uint64_t fill_bytes = total_invocations * element_bytes;
   if (fill_bytes > INT_MAX) {
      IDM_LOG("constfill early-return fill_bytes map-range-overflow");
      return false;
   }

   struct pipe_box out_box;
   memset(&out_box, 0, sizeof(out_box));
   out_box.x      = (int)out_desc->buf.offset;
   out_box.width  = (int)fill_bytes;
   out_box.height = 1;
   out_box.depth  = 1;

   struct pipe_transfer *xfer = NULL;
   void *ptr = pipe->buffer_map(pipe, out_buf->resource, 0,
                                PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                                &out_box, &xfer);
   if (!ptr) {
      IDM_LOG("constfill early-return buffer-map-failed");
      return false;
   }

   /* Fill: the four const_value bytes are the u32 constant in host byte order.
    * Write them to every element slot without invoking the GPU. */
   uint8_t *dst = ptr;
   for (uint64_t i = 0; i < total_invocations; i++) {
      dst[i * 4 + 0] = pl->const_fill.const_value[0];
      dst[i * 4 + 1] = pl->const_fill.const_value[1];
      dst[i * 4 + 2] = pl->const_fill.const_value[2];
      dst[i * 4 + 3] = pl->const_fill.const_value[3];
   }

   pipe->buffer_unmap(pipe, xfer);
   IDM_LOG("constfill done total_invocations=%llu fill_bytes=%llu",
           (unsigned long long)total_invocations, (unsigned long long)fill_bytes);
   return true;
}

/* Single oversized triangle whose texcoord varying is in TEXEL units: the
 * covered raster interpolates to exactly (x + 0.5, y + 0.5) at each fragment
 * center without any FP24 division by the raster extent.  ONE triangle, not
 * a two-triangle strip: the RS482 affine_iota_index probe showed the strip's
 * second triangle interpolating a hair low (every mismatch sat exactly on the
 * strip diagonal x = W * (1 - y/H)), because the second plane equation
 * anchors at a far vertex and the longer extrapolation drops the sub-ULP the
 * byte-decompose floor cliff needs.  A single triangle anchored at the origin
 * covers every fragment from one plane equation -- the lane the probe measured
 * exact through the full 2^17 ceiling.  The overshoot corners ((3,-1) and
 * (-1,3) in clip space, texel values 2W and 2H) stay inside the FP24
 * exact-integer window and inside the guard band. */
static bool
r3v_idm_create_texel_index_vbo(struct pipe_context *pipe,
                                  unsigned width, unsigned height,
                                  struct pipe_resource **out_vb,
                                  void **out_velems_cso)
{
   struct pipe_screen *screen = pipe->screen;
   const float w = (float)width, h = (float)height;
   const float verts[12] = {
      -1.0f, -1.0f, 0.0f,     0.0f,
       3.0f, -1.0f, 2.0f * w, 0.0f,
      -1.0f,  3.0f, 0.0f,     2.0f * h,
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

bool
r3v_affine_iota_dispatch_replay(struct r3v_device *device,
                                   const struct r3v_pipeline *pl,
                                   const struct r3v_cmd_dispatch *dispatch,
                                   const struct r3v_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device ? device->pipe : NULL;
   struct pipe_screen  *screen = device ? device->screen : NULL;
   const struct r3v_descriptor_set *set = NULL;
   if (!r3v_idm_validate_prologue(device, pl, dispatch, binds, &set))
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R8G8B8A8_UNORM,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET))
      return false;

   /* One binding: the output.  The detector's captured binding wins when the
    * post-explicit_io store binding source was a constant; otherwise the
    * single compute-visible STORAGE_BUFFER in declaration order is the output. */
   uint32_t out_binding = pl->affine_iota.output_ssbo_binding;
   if (!pl->affine_iota.output_ssbo_binding_valid) {
      if (!single_storage_buffer_binding(set, &out_binding)) {
         IDM_LOG("iota early-return ambiguous-output-binding");
         return false;
      }
   }
   const struct r3v_descriptor *out_desc =
      find_descriptor_by_binding(set, out_binding);
   if (!out_desc || !out_desc->buf.buffer) {
      IDM_LOG("iota early-return descriptor-walk-miss");
      return false;
   }
   struct r3v_buffer *out_buf =
      r3v_buffer_from_handle(out_desc->buf.buffer);
   if (!out_buf || !out_buf->resource) {
      IDM_LOG("iota early-return null-pipe-resource");
      return false;
   }

   /* Shape validation.  The fragment program computes
    * v = stride * (y_r * width + x) + offset over the folded raster.  Two
    * admitted shapes make that equal the kernel's stored value:
    *
    *  1D (stride_y == stride_z == 0): the affine rides id.x, which equals
    *  the linear invocation index only for pure-x dispatches; multi-
    *  dimensional grids are refused (id.x is a per-row coordinate there).
    *
    *  3D (canonical flatten): the kernel computes
    *  flat = id.z * (tx * ty) + id.y * tx + id.x scaled by stride, i.e.
    *  ay == ax * tx and az == ax * tx * ty for the dispatch's own per-axis
    *  totals.  Under the interleave row-fold (width = tx,
    *  height = ty * tz, y_r = z * ty + y) the raster linear index
    *  y_r * width + x equals flat exactly, so the silicon-proven 1D
    *  fragment program runs verbatim.  Non-canonical strides are refused:
    *  a wrong fold would write values the kernel never computed. */
   const uint64_t tx = (uint64_t)dispatch->group_count_x *
                       (pl->local_size_x ? pl->local_size_x : 1u);
   const uint64_t ty = (uint64_t)dispatch->group_count_y *
                       (pl->local_size_y ? pl->local_size_y : 1u);
   const uint64_t tz = (uint64_t)dispatch->group_count_z *
                       (pl->local_size_z ? pl->local_size_z : 1u);
   const uint32_t stride = pl->affine_iota.stride;
   const bool is_3d = pl->affine_iota.stride_y || pl->affine_iota.stride_z;
   unsigned width, height;
   if (pl->affine_iota.output_offset_stride != 4 ||
       pl->affine_iota.output_offset_offset != 0) {
      IDM_LOG("iota early-return output-offset-not-gid ax=%u b=%u",
              pl->affine_iota.output_offset_stride,
              pl->affine_iota.output_offset_offset);
      return false;
   }
   if (is_3d) {
      if ((uint64_t)pl->affine_iota.stride_y != (uint64_t)stride * tx ||
          (uint64_t)pl->affine_iota.stride_z != (uint64_t)stride * tx * ty ||
          (uint64_t)pl->affine_iota.output_offset_stride_y != 4ull * tx ||
          (uint64_t)pl->affine_iota.output_offset_stride_z !=
             4ull * tx * ty) {
         IDM_LOG("iota early-return non-canonical flatten ay=%u az=%u "
                 "oay=%u oaz=%u tx=%llu ty=%llu",
                 pl->affine_iota.stride_y, pl->affine_iota.stride_z,
                 pl->affine_iota.output_offset_stride_y,
                 pl->affine_iota.output_offset_stride_z,
                 (unsigned long long)tx, (unsigned long long)ty);
         return false;
      }
      struct r300_grid_fold fold3;
      if (!r300_grid_fold_3d((uint32_t)tx, (uint32_t)ty, (uint32_t)tz,
                             &fold3)) {
         IDM_LOG("iota early-return 3d-fold tx=%llu ty=%llu tz=%llu",
                 (unsigned long long)tx, (unsigned long long)ty,
                 (unsigned long long)tz);
         return false;
      }
      width = fold3.width;
      height = fold3.height;
   } else {
      if (pl->affine_iota.output_offset_stride_y ||
          pl->affine_iota.output_offset_stride_z) {
         IDM_LOG("iota early-return 1d-output-offset-yz ay=%u az=%u",
                 pl->affine_iota.output_offset_stride_y,
                 pl->affine_iota.output_offset_stride_z);
         return false;
      }
      if (ty > 1 || tz > 1) {
         IDM_LOG("iota early-return non-1d dispatch of 1d affine ty=%llu "
                 "tz=%llu", (unsigned long long)ty, (unsigned long long)tz);
         return false;
      }
      struct r300_grid_fold fold;
      if (!r300_grid_fold_1d(tx, &fold)) {
         IDM_LOG("iota early-return fold total=%llu",
                 (unsigned long long)tx);
         return false;
      }
      width = fold.width;
      height = fold.height;
   }

   /* Defence in depth: the queue-level index gate already bounded the
    * materialized value by the FP24 exact-integer ceiling; re-check on the
    * flattened total so a direct caller cannot bypass it. */
   const uint64_t total = tx * ty * tz;
   if (!r300_grid_strided_index_exact(total, stride, pl->affine_iota.offset)) {
      IDM_LOG("iota early-return index-ceiling total=%llu stride=%u offset=%u",
              (unsigned long long)total, stride, pl->affine_iota.offset);
      return false;
   }

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_texel_index_vbo(pipe, width, height, &vb,
                                          &velems_cso))
      return false;

   const enum pipe_format rtfmt = PIPE_FORMAT_R8G8B8A8_UNORM;
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
   if (!rt) {
      pipe->delete_vertex_elements_state(pipe, velems_cso);
      pipe_resource_reference(&vb, NULL);
      return false;
   }

   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = rtfmt;
   surf_templ.texture = rt;
   r3v_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_cso.blend,
                                        device->identity_map_cso.rasterizer,
                                        device->identity_map_cso.dsa,
                                        pl->vs_cso, pl->fs_cso, velems_cso);

   /* CONST[0] = (width, stride, offset, 0): the dispatch-known scalars the
    * FS affine reads.  user_buffer is consumed at the draw below. */
   const float cb_data[4] = { (float)width, (float)stride,
                              (float)pl->affine_iota.offset, 0.0f };
   struct pipe_constant_buffer cb;
   memset(&cb, 0, sizeof(cb));
   cb.user_buffer = cb_data;
   cb.buffer_size = sizeof(cb_data);
   pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);

   struct pipe_vertex_buffer vb_state;
   memset(&vb_state, 0, sizeof(vb_state));
   vb_state.buffer.resource = vb;
   pipe->set_vertex_buffers(pipe, 1, &vb_state);

   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode           = MESA_PRIM_TRIANGLES;
   info.instance_count = 1;
   info.max_index      = 2;
   struct pipe_draw_start_count_bias draw = { .start = 0, .count = 3 };
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   pipe->flush(pipe, NULL, 0);

   /* The RGBA8 texel bytes ARE the little-endian u32 elements; copy raw
    * rows bounded by the kernel's element count. */
   bool copy_ok = false;
   struct pipe_box copy_box;
   memset(&copy_box, 0, sizeof(copy_box));
   copy_box.width = width; copy_box.height = height; copy_box.depth = 1;
   struct pipe_transfer *rt_xfer = NULL;
   const void *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                          &copy_box, &rt_xfer);
   if (rt_map) {
      struct pipe_transfer *out_xfer = NULL;
      struct pipe_box out_box;
      memset(&out_box, 0, sizeof(out_box));
      out_box.x      = (unsigned)out_desc->buf.offset;
      out_box.width  = (unsigned)(total * 4u);
      out_box.height = 1; out_box.depth = 1;
      void *out_bytes = pipe->buffer_map(pipe, out_buf->resource, 0,
                                         PIPE_MAP_WRITE |
                                         PIPE_MAP_DISCARD_RANGE,
                                         &out_box, &out_xfer);
      if (out_bytes) {
         r3v_identity_map_copy_rows(out_bytes, width * 4u, rt_map,
                                       rt_xfer->stride, width, height, 4u,
                                       total);
         pipe->buffer_unmap(pipe, out_xfer);
         copy_ok = true;
      }
      pipe->texture_unmap(pipe, rt_xfer);
   }

   pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, NULL);
   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);
   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_resource_reference(&rt, NULL);
   pipe_resource_reference(&vb, NULL);
   IDM_LOG("iota done total=%llu stride=%u offset=%u copy_ok=%d",
           (unsigned long long)total, stride, pl->affine_iota.offset,
           (int)copy_ok);
   return copy_ok;
}

/* Convolution core shared by the exact-u32-multiply verb and the variable-shift
 * verb.  Multiplies a_res * c_res as 5x7-bit-limb columns on the FP24 ALU (nine
 * column draws accumulated into a full uint64 per element), then stores the
 * 32-bit window (acc >> out_shift) of the exact product.  The multiply verb and
 * the variable LEFT shift keep out_shift = 0 (a*b mod 2^32, and a<<b = low32 of
 * a*2^b); the variable RIGHT shift keeps out_shift = 31 (a>>b = bits[31,62] of
 * a*2^(31-b), the window that fits a uint32 multiplier without a 2^32 corner).
 * a_res/c_res/out_res are the resolved gallium resources at their byte offsets;
 * the caller owns descriptor resolution and any transient c buffer. */
static bool
r3v_multilimb_convolve(struct r3v_device *device,
                          const struct r3v_pipeline *pl,
                          struct pipe_resource *a_res, unsigned a_off,
                          struct pipe_resource *c_res, unsigned c_off,
                          struct pipe_resource *out_res, unsigned out_off,
                          uint64_t total, unsigned out_shift)
{
   struct pipe_context *pipe   = device ? device->pipe : NULL;
   struct pipe_screen  *screen = device ? device->screen : NULL;
   if (!pipe || !screen || !a_res || !c_res || !out_res)
      return false;
   for (unsigned k = 0; k < 9; k++)
      if (!pl->multilimb_fs[k])
         return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R8G8B8A8_UNORM,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET))
      return false;

   struct r300_grid_fold fold;
   if (!r300_grid_fold_1d(total, &fold))
      return false;
   const unsigned width = fold.width, height = fold.height;

   /* Each u32 element is one RGBA8 texel of factor bytes. */
   struct pipe_resource *src_res[2] = { a_res, c_res };
   unsigned src_off[2] = { a_off, c_off };
   struct pipe_sampler_view *views[2] = { NULL, NULL };
   for (unsigned i = 0; i < 2; i++) {
      views[i] = r3v_identity_map_wrap_input_as_sampler_view(
         device, src_res[i], src_off[i],
         width, height, total, PIPE_FORMAT_R8G8B8A8_UNORM);
      if (!views[i]) {
         pipe_sampler_view_reference(&views[0], NULL);
         return false;
      }
   }

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_sampler_view_reference(&views[0], NULL);
      pipe_sampler_view_reference(&views[1], NULL);
      return false;
   }

   uint64_t *acc = calloc(total, sizeof(uint64_t));
   if (!acc) {
      pipe->delete_vertex_elements_state(pipe, velems_cso);
      pipe_sampler_view_reference(&views[0], NULL);
      pipe_sampler_view_reference(&views[1], NULL);
      pipe_resource_reference(&vb, NULL);
      return false;
   }

   bool ok = true;
   for (unsigned k = 0; k < 9 && ok; k++) {
      struct pipe_resource rt_templ;
      memset(&rt_templ, 0, sizeof(rt_templ));
      rt_templ.target     = PIPE_TEXTURE_2D;
      rt_templ.format     = PIPE_FORMAT_R8G8B8A8_UNORM;
      rt_templ.width0     = width;
      rt_templ.height0    = height;
      rt_templ.depth0     = 1;
      rt_templ.array_size = 1;
      rt_templ.usage      = PIPE_USAGE_DEFAULT;
      rt_templ.bind       = PIPE_BIND_RENDER_TARGET;
      struct pipe_resource *rt = screen->resource_create(screen, &rt_templ);
      if (!rt) {
         ok = false;
         break;
      }
      struct pipe_surface surf_templ;
      memset(&surf_templ, 0, sizeof(surf_templ));
      surf_templ.format  = PIPE_FORMAT_R8G8B8A8_UNORM;
      surf_templ.texture = rt;
      r3v_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                           device->identity_map_cso.blend,
                                           device->identity_map_cso.rasterizer,
                                           device->identity_map_cso.dsa,
                                           pl->vs_cso, pl->multilimb_fs[k],
                                           velems_cso);
      void *samplers[2] = { device->identity_map_cso.sampler,
                            device->identity_map_cso.sampler };
      pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 2, samplers);
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
      pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
      pipe->flush(pipe, NULL, 0);

      struct pipe_box copy_box;
      memset(&copy_box, 0, sizeof(copy_box));
      copy_box.width = width; copy_box.height = height; copy_box.depth = 1;
      struct pipe_transfer *rt_xfer = NULL;
      const uint8_t *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                                &copy_box, &rt_xfer);
      if (rt_map) {
         uint64_t idx = 0;
         for (unsigned r = 0; r < height && idx < total; r++) {
            const uint8_t *row = rt_map + (size_t)r * rt_xfer->stride;
            for (unsigned x = 0; x < width && idx < total; x++, idx++) {
               const uint64_t col = (uint64_t)row[x * 4] +
                                    ((uint64_t)row[x * 4 + 1] << 8) +
                                    ((uint64_t)row[x * 4 + 2] << 16);
               acc[idx] += col << (7u * k);
            }
         }
         pipe->texture_unmap(pipe, rt_xfer);
      } else {
         ok = false;
      }
      pipe_resource_reference(&rt, NULL);
   }

   if (ok) {
      struct pipe_transfer *out_xfer = NULL;
      struct pipe_box out_box;
      memset(&out_box, 0, sizeof(out_box));
      out_box.x      = out_off;
      out_box.width  = (unsigned)(total * 4u);
      out_box.height = 1; out_box.depth = 1;
      uint32_t *out_bytes = pipe->buffer_map(pipe, out_res, 0,
                                             PIPE_MAP_WRITE |
                                             PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                             &out_box, &out_xfer);
      if (out_bytes) {
         /* out_shift = 0 stores the low 32 bits of the exact product (the
          * multiply verb and the variable left shift); out_shift = 31 stores
          * bits [31,62] (the variable right shift).  acc is uint64, so the
          * shift then truncates to the windowed 32 bits. */
         for (uint64_t i = 0; i < total; i++)
            out_bytes[i] = (uint32_t)(acc[i] >> out_shift);
         pipe->buffer_unmap(pipe, out_xfer);
      } else {
         ok = false;
      }
   }

   free(acc);
   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);
   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_sampler_view_reference(&views[0], NULL);
   pipe_sampler_view_reference(&views[1], NULL);
   pipe_resource_reference(&vb, NULL);
   return ok;
}

bool
r3v_multilimb_mul_dispatch_replay(struct r3v_device *device,
                                     const struct r3v_pipeline *pl,
                                     const struct r3v_cmd_dispatch *dispatch,
                                     const struct r3v_cmd_bind_descriptor_sets *binds)
{
   const struct r3v_descriptor_set *set = NULL;
   if (!r3v_idm_validate_prologue(device, pl, dispatch, binds, &set))
      return false;

   /* Three bindings: a, b, out.  Captured constants win when all three were
    * constant sources; otherwise the first three compute-visible
    * STORAGE_BUFFERs in declaration order carry the roles. */
   uint32_t bind[3] = { pl->multilimb_mul.input_a_ssbo_binding,
                        pl->multilimb_mul.input_b_ssbo_binding,
                        pl->multilimb_mul.output_ssbo_binding };
   if (bind[0] == bind[1]) {
      for (unsigned i = 0; i < 3; i++)
         if (!nth_storage_buffer_binding(set, i, &bind[i]))
            return false;
   }
   const struct r3v_descriptor *desc[3];
   struct r3v_buffer *buf[3];
   for (unsigned i = 0; i < 3; i++) {
      desc[i] = find_descriptor_by_binding(set, bind[i]);
      if (!desc[i] || !desc[i]->buf.buffer)
         return false;
      buf[i] = r3v_buffer_from_handle(desc[i]->buf.buffer);
      if (!buf[i] || !buf[i]->resource)
         return false;
   }

   const uint64_t total = r3v_idm_total_invocations(dispatch, pl);
   const bool ok = r3v_multilimb_convolve(device, pl,
      buf[0]->resource, (unsigned)desc[0]->buf.offset,
      buf[1]->resource, (unsigned)desc[1]->buf.offset,
      buf[2]->resource, (unsigned)desc[2]->buf.offset,
      total, 0);
   IDM_LOG("multilimb done total=%llu ok=%d",
           (unsigned long long)total, (int)ok);
   return ok;
}

/* Gather pass for the variable-amount shift: render 2^M per element into c_res.
 * The gather FS samples the per-element amount b, indexes the device 2^j lookup
 * (b for left, 31-b for right) with a dependent read, and exports the four bytes
 * of 2^M; the RT is then copied into the transient c buffer the convolution
 * multiplies by.  One draw, no host arithmetic on the amount. */
static bool
r3v_shift_variable_gather(struct r3v_device *device,
                             const struct r3v_pipeline *pl,
                             struct pipe_resource *b_res, unsigned b_off,
                             struct pipe_resource *c_res, uint64_t total)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen || !pl->shift_variable_gather_fs ||
       !device->shift_variable_lut_view)
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R8G8B8A8_UNORM,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET))
      return false;

   struct r300_grid_fold fold;
   if (!r300_grid_fold_1d(total, &fold))
      return false;
   const unsigned width = fold.width, height = fold.height;

   struct pipe_sampler_view *b_view =
      r3v_identity_map_wrap_input_as_sampler_view(
         device, b_res, b_off, width, height, total,
         PIPE_FORMAT_R8G8B8A8_UNORM);
   if (!b_view)
      return false;

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_sampler_view_reference(&b_view, NULL);
      return false;
   }

   bool ok = false;
   struct pipe_resource rt_templ;
   memset(&rt_templ, 0, sizeof(rt_templ));
   rt_templ.target     = PIPE_TEXTURE_2D;
   rt_templ.format     = PIPE_FORMAT_R8G8B8A8_UNORM;
   rt_templ.width0     = width;
   rt_templ.height0    = height;
   rt_templ.depth0     = 1;
   rt_templ.array_size = 1;
   rt_templ.usage      = PIPE_USAGE_DEFAULT;
   rt_templ.bind       = PIPE_BIND_RENDER_TARGET;
   struct pipe_resource *rt = screen->resource_create(screen, &rt_templ);
   if (rt) {
      struct pipe_surface surf_templ;
      memset(&surf_templ, 0, sizeof(surf_templ));
      surf_templ.format  = PIPE_FORMAT_R8G8B8A8_UNORM;
      surf_templ.texture = rt;
      r3v_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                           device->identity_map_cso.blend,
                                           device->identity_map_cso.rasterizer,
                                           device->identity_map_cso.dsa,
                                           pl->vs_cso,
                                           pl->shift_variable_gather_fs,
                                           velems_cso);
      void *samplers[2] = { device->identity_map_cso.sampler,
                            device->identity_map_cso.sampler };
      struct pipe_sampler_view *views[2] = { b_view,
                                             device->shift_variable_lut_view };
      pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 2, samplers);
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
      pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
      pipe->flush(pipe, NULL, 0);

      ok = r3v_identity_map_readback_rt(pipe, rt, c_res, 0, width, height,
                                           PIPE_FORMAT_R8G8B8A8_UNORM,
                                           width * 4u, total);
      pipe_resource_reference(&rt, NULL);
   }

   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);
   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_sampler_view_reference(&b_view, NULL);
   pipe_resource_reference(&vb, NULL);
   return ok;
}

/* Sign-extension pass for variable ishr: out = ushr + sign(a) * fill[b].  Samples
 * the logical ushr result (sampler 0), a for its sign (sampler 1), b for the fill
 * index (sampler 2), and the device fill lookup (sampler 3), then reads the RT
 * back into the output buffer.  The signfill FS does the disjoint per-byte add. */
static bool
r3v_shift_variable_signfill(struct r3v_device *device,
                               const struct r3v_pipeline *pl,
                               struct pipe_resource *ushr_res,
                               struct pipe_resource *a_res, unsigned a_off,
                               struct pipe_resource *b_res, unsigned b_off,
                               struct pipe_resource *out_res, unsigned out_off,
                               uint64_t total)
{
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen || !pl->shift_variable_signfill_fs ||
       !device->shift_variable_fill_lut_view)
      return false;

   struct r300_grid_fold fold;
   if (!r300_grid_fold_1d(total, &fold))
      return false;
   const unsigned width = fold.width, height = fold.height;

   /* sampler 0 = logical ushr, 1 = a (sign), 2 = b (fill index). */
   struct pipe_resource *in_res[3] = { ushr_res, a_res, b_res };
   unsigned in_off[3] = { 0, a_off, b_off };
   struct pipe_sampler_view *views[4] = { NULL, NULL, NULL,
                                          device->shift_variable_fill_lut_view };
   for (unsigned i = 0; i < 3; i++) {
      views[i] = r3v_identity_map_wrap_input_as_sampler_view(
         device, in_res[i], in_off[i], width, height, total,
         PIPE_FORMAT_R8G8B8A8_UNORM);
      if (!views[i]) {
         for (unsigned j = 0; j < i; j++)
            pipe_sampler_view_reference(&views[j], NULL);
         return false;
      }
   }

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      for (unsigned i = 0; i < 3; i++)
         pipe_sampler_view_reference(&views[i], NULL);
      return false;
   }

   bool ok = false;
   struct pipe_resource rt_templ;
   memset(&rt_templ, 0, sizeof(rt_templ));
   rt_templ.target     = PIPE_TEXTURE_2D;
   rt_templ.format     = PIPE_FORMAT_R8G8B8A8_UNORM;
   rt_templ.width0     = width;
   rt_templ.height0    = height;
   rt_templ.depth0     = 1;
   rt_templ.array_size = 1;
   rt_templ.usage      = PIPE_USAGE_DEFAULT;
   rt_templ.bind       = PIPE_BIND_RENDER_TARGET;
   struct pipe_resource *rt = screen->resource_create(screen, &rt_templ);
   if (rt) {
      struct pipe_surface surf_templ;
      memset(&surf_templ, 0, sizeof(surf_templ));
      surf_templ.format  = PIPE_FORMAT_R8G8B8A8_UNORM;
      surf_templ.texture = rt;
      r3v_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                           device->identity_map_cso.blend,
                                           device->identity_map_cso.rasterizer,
                                           device->identity_map_cso.dsa,
                                           pl->vs_cso,
                                           pl->shift_variable_signfill_fs,
                                           velems_cso);
      void *samplers[4] = { device->identity_map_cso.sampler,
                            device->identity_map_cso.sampler,
                            device->identity_map_cso.sampler,
                            device->identity_map_cso.sampler };
      pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 4, samplers);
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 4, 0, views);

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

      ok = r3v_identity_map_readback_rt(pipe, rt, out_res, out_off,
                                           width, height,
                                           PIPE_FORMAT_R8G8B8A8_UNORM,
                                           width * 4u, total);
      pipe_resource_reference(&rt, NULL);
   }

   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);
   pipe->delete_vertex_elements_state(pipe, velems_cso);
   for (unsigned i = 0; i < 3; i++)
      pipe_sampler_view_reference(&views[i], NULL);
   pipe_resource_reference(&vb, NULL);
   return ok;
}

bool
r3v_shift_variable_dispatch_replay(struct r3v_device *device,
                                      const struct r3v_pipeline *pl,
                                      const struct r3v_cmd_dispatch *dispatch,
                                      const struct r3v_cmd_bind_descriptor_sets *binds)
{
   struct pipe_screen *screen = device ? device->screen : NULL;
   const struct r3v_descriptor_set *set = NULL;
   if (!r3v_idm_validate_prologue(device, pl, dispatch, binds, &set))
      return false;
   if (!screen)
      return false;

   /* Bindings: a (value), b (per-element amount), out.  Captured constants win;
    * otherwise the first three compute-visible STORAGE_BUFFERs in declaration
    * order carry the roles. */
   uint32_t bind[3] = { pl->shift_variable.input_a_ssbo_binding,
                        pl->shift_variable.input_b_ssbo_binding,
                        pl->shift_variable.output_ssbo_binding };
   if (bind[0] == bind[1]) {
      for (unsigned i = 0; i < 3; i++)
         if (!nth_storage_buffer_binding(set, i, &bind[i]))
            return false;
   }
   const struct r3v_descriptor *desc[3];
   struct r3v_buffer *buf[3];
   for (unsigned i = 0; i < 3; i++) {
      desc[i] = find_descriptor_by_binding(set, bind[i]);
      if (!desc[i] || !desc[i]->buf.buffer)
         return false;
      buf[i] = r3v_buffer_from_handle(desc[i]->buf.buffer);
      if (!buf[i] || !buf[i]->resource)
         return false;
   }

   const uint64_t total = r3v_idm_total_invocations(dispatch, pl);
   if (total == 0)
      return true;
   /* Both passes fold total into a 2D raster extent; rejecting an unfoldable
    * total here keeps the transient width0 = total*4 inside a uint32 and avoids
    * a doomed multi-gigabyte allocation before the gather's own fold check. */
   struct r300_grid_fold fold_check;
   if (!r300_grid_fold_1d(total, &fold_check))
      return false;

   /* Transient 2^M buffer the gather fills and the convolution multiplies by. */
   struct pipe_resource c_templ;
   memset(&c_templ, 0, sizeof(c_templ));
   c_templ.target     = PIPE_BUFFER;
   c_templ.format     = PIPE_FORMAT_R8_UNORM;
   c_templ.width0     = (unsigned)(total * 4u);
   c_templ.height0    = 1;
   c_templ.depth0     = 1;
   c_templ.array_size = 1;
   c_templ.usage      = PIPE_USAGE_DEFAULT;
   c_templ.bind       = PIPE_BIND_SAMPLER_VIEW;
   struct pipe_resource *c_res = screen->resource_create(screen, &c_templ);
   if (!c_res)
      return false;

   /* ishr convolves into a transient logical-ushr buffer, then the sign-fill pass
    * writes the final output; ishl/ushr convolve straight into the output. */
   const unsigned out_shift = pl->shift_variable.is_left ? 0u : 31u;
   struct pipe_resource *ushr_res = NULL;
   struct pipe_resource *conv_dst = buf[2]->resource;
   unsigned conv_off = (unsigned)desc[2]->buf.offset;
   if (pl->shift_variable.is_arithmetic) {
      ushr_res = screen->resource_create(screen, &c_templ);
      if (!ushr_res) {
         pipe_resource_reference(&c_res, NULL);
         return false;
      }
      conv_dst = ushr_res;
      conv_off = 0;
   }

   bool ok = r3v_shift_variable_gather(device, pl, buf[1]->resource,
                                          (unsigned)desc[1]->buf.offset,
                                          c_res, total);
   if (ok)
      ok = r3v_multilimb_convolve(device, pl,
              buf[0]->resource, (unsigned)desc[0]->buf.offset,
              c_res, 0, conv_dst, conv_off, total, out_shift);
   if (ok && pl->shift_variable.is_arithmetic)
      ok = r3v_shift_variable_signfill(device, pl,
              ushr_res,
              buf[0]->resource, (unsigned)desc[0]->buf.offset,
              buf[1]->resource, (unsigned)desc[1]->buf.offset,
              buf[2]->resource, (unsigned)desc[2]->buf.offset, total);

   pipe_resource_reference(&ushr_res, NULL);
   pipe_resource_reference(&c_res, NULL);
   IDM_LOG("shift_variable done total=%llu left=%d arith=%d ok=%d",
           (unsigned long long)total, (int)pl->shift_variable.is_left,
           (int)pl->shift_variable.is_arithmetic, (int)ok);
   return ok;
}

bool
r3v_cas_dispatch_replay(struct r3v_device *device,
                           const struct r3v_pipeline *pl,
                           const struct r3v_cmd_dispatch *dispatch,
                           const struct r3v_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device ? device->pipe : NULL;
   struct pipe_screen  *screen = device ? device->screen : NULL;
   const struct r3v_descriptor_set *set = NULL;
   if (!r3v_idm_validate_prologue(device, pl, dispatch, binds, &set))
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R8G8B8A8_UNORM,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET))
      return false;

   /* Two bindings: guard, result.  Captured constants win; the positional
    * fallback (guard = first compute-visible STORAGE_BUFFER, result = second)
    * recovers post-explicit_io handles. */
   uint32_t bind[2] = { pl->cas.guard_ssbo_binding,
                        pl->cas.result_ssbo_binding };
   if (!pl->cas.guard_binding_valid || !pl->cas.result_binding_valid) {
      for (unsigned i = 0; i < 2; i++)
         if (!nth_storage_buffer_binding(set, i, &bind[i]))
            return false;
   }
   const struct r3v_descriptor *desc[2];
   struct r3v_buffer *buf[2];
   for (unsigned i = 0; i < 2; i++) {
      desc[i] = find_descriptor_by_binding(set, bind[i]);
      if (!desc[i] || !desc[i]->buf.buffer)
         return false;
      buf[i] = r3v_buffer_from_handle(desc[i]->buf.buffer);
      if (!buf[i] || !buf[i]->resource)
         return false;
   }

   const uint64_t total = r3v_idm_total_invocations(dispatch, pl);
   struct r300_grid_fold fold;
   if (!r300_grid_fold_1d(total, &fold))
      return false;
   const unsigned width = fold.width, height = fold.height;

   /* The returned old IS the guard pre-image: copy it to the result buffer
    * before the swap draw mutates the guard. */
   {
      struct pipe_transfer *gx = NULL, *rx = NULL;
      struct pipe_box gbox, rbox;
      memset(&gbox, 0, sizeof(gbox));
      gbox.x = (int)desc[0]->buf.offset;
      gbox.width = (int)(total * 4u);
      gbox.height = 1; gbox.depth = 1;
      rbox = gbox;
      rbox.x = (int)desc[1]->buf.offset;
      const void *gmap = pipe->buffer_map(pipe, buf[0]->resource, 0,
                                          PIPE_MAP_READ, &gbox, &gx);
      if (!gmap)
         return false;
      void *rmap = pipe->buffer_map(pipe, buf[1]->resource, 0,
                                    PIPE_MAP_WRITE |
                                    PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                    &rbox, &rx);
      if (!rmap) {
         pipe->buffer_unmap(pipe, gx);
         return false;
      }
      memcpy(rmap, gmap, (size_t)(total * 4u));
      pipe->buffer_unmap(pipe, rx);
      pipe->buffer_unmap(pipe, gx);
   }

   struct pipe_sampler_view *view =
      r3v_identity_map_wrap_input_as_sampler_view(
         device, buf[0]->resource, (unsigned)desc[0]->buf.offset,
         width, height, total, PIPE_FORMAT_R8G8B8A8_UNORM);
   if (!view)
      return false;

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_sampler_view_reference(&view, NULL);
      return false;
   }

   struct pipe_resource rt_templ;
   memset(&rt_templ, 0, sizeof(rt_templ));
   rt_templ.target     = PIPE_TEXTURE_2D;
   rt_templ.format     = PIPE_FORMAT_R8G8B8A8_UNORM;
   rt_templ.width0     = width;
   rt_templ.height0    = height;
   rt_templ.depth0     = 1;
   rt_templ.array_size = 1;
   rt_templ.usage      = PIPE_USAGE_DEFAULT;
   rt_templ.bind       = PIPE_BIND_RENDER_TARGET;
   struct pipe_resource *rt = screen->resource_create(screen, &rt_templ);
   if (!rt) {
      pipe->delete_vertex_elements_state(pipe, velems_cso);
      pipe_sampler_view_reference(&view, NULL);
      pipe_resource_reference(&vb, NULL);
      return false;
   }

   struct pipe_surface surf_templ;
   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format  = PIPE_FORMAT_R8G8B8A8_UNORM;
   surf_templ.texture = rt;
   r3v_identity_map_setup_draw_state(pipe, width, height, &surf_templ,
                                        device->identity_map_cso.blend,
                                        device->identity_map_cso.rasterizer,
                                        device->identity_map_cso.dsa,
                                        pl->vs_cso, pl->fs_cso, velems_cso);
   void *samplers[1] = { device->identity_map_cso.sampler };
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1, samplers);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 1, 0, &view);

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

   /* The RGBA8 post-image bytes ARE the swapped little-endian u32 guards. */
   bool copy_ok = false;
   {
      struct pipe_box copy_box;
      memset(&copy_box, 0, sizeof(copy_box));
      copy_box.width = width; copy_box.height = height; copy_box.depth = 1;
      struct pipe_transfer *rt_xfer = NULL;
      const void *rt_map = pipe->texture_map(pipe, rt, 0, PIPE_MAP_READ,
                                             &copy_box, &rt_xfer);
      if (rt_map) {
         struct pipe_transfer *out_xfer = NULL;
         struct pipe_box out_box;
         memset(&out_box, 0, sizeof(out_box));
         out_box.x      = (int)desc[0]->buf.offset;
         out_box.width  = (int)(total * 4u);
         out_box.height = 1; out_box.depth = 1;
         void *out_bytes = pipe->buffer_map(pipe, buf[0]->resource, 0,
                                            PIPE_MAP_WRITE, &out_box,
                                            &out_xfer);
         if (out_bytes) {
            r3v_identity_map_copy_rows(out_bytes, width * 4u, rt_map,
                                          rt_xfer->stride, width, height, 4u,
                                          total);
            pipe->buffer_unmap(pipe, out_xfer);
            copy_ok = true;
         }
         pipe->texture_unmap(pipe, rt_xfer);
      }
   }

   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);
   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_sampler_view_reference(&view, NULL);
   pipe_resource_reference(&rt, NULL);
   pipe_resource_reference(&vb, NULL);
   IDM_LOG("cas done total=%llu expect=0x%08x new=0x%08x copy_ok=%d",
           (unsigned long long)total, pl->cas.expect, pl->cas.value_new,
           (int)copy_ok);
   return copy_ok;
}

bool
r3v_log4_pool_dispatch_replay(struct r3v_device *device,
                                 const struct r3v_pipeline *pl,
                                 const struct r3v_cmd_dispatch *dispatch,
                                 const struct r3v_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe   = device ? device->pipe : NULL;
   struct pipe_screen  *screen = device ? device->screen : NULL;
   const struct r3v_descriptor_set *set = NULL;
   if (!r3v_idm_validate_prologue(device, pl, dispatch, binds, &set))
      return false;
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R8G8B8A8_UNORM,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET))
      return false;

   /* Output grid = dispatch grid (W2 x H2); input = (W, 2 * H2) with the
    * detector-captured row constant W validated against it. */
   const uint64_t tx = (uint64_t)dispatch->group_count_x *
                       (pl->local_size_x ? pl->local_size_x : 1u);
   const uint64_t ty = (uint64_t)dispatch->group_count_y *
                       (pl->local_size_y ? pl->local_size_y : 1u);
   const uint64_t tz = (uint64_t)dispatch->group_count_z *
                       (pl->local_size_z ? pl->local_size_z : 1u);
   const uint32_t W = pl->log4_pool.row_w;
   if (tz != 1 || tx == 0 || ty == 0 || W == 0 || tx * 2 != W ||
       W > 2048 || ty * 2 > 2048) {
      IDM_LOG("log4 early-return grid tx=%llu ty=%llu tz=%llu W=%u",
              (unsigned long long)tx, (unsigned long long)ty,
              (unsigned long long)tz, W);
      return false;
   }
   const unsigned in_w = W, in_h = (unsigned)(ty * 2);
   const unsigned out_w = (unsigned)tx, out_h = (unsigned)ty;
   const uint64_t in_total = (uint64_t)in_w * in_h;
   const uint64_t out_total = (uint64_t)out_w * out_h;

   uint32_t bind[2] = { pl->log4_pool.input_ssbo_binding,
                        pl->log4_pool.output_ssbo_binding };
   const bool input_known = pl->log4_pool.input_binding_valid;
   const bool output_known = pl->log4_pool.output_binding_valid;
   if (!input_known && !output_known) {
      if (!nth_storage_buffer_binding(set, 0, &bind[0]) ||
          !nth_storage_buffer_binding(set, 1, &bind[1]))
         return false;
   } else if (!input_known) {
      if (!single_storage_buffer_binding_excluding(set, bind[1], &bind[0]))
         return false;
   } else if (!output_known) {
      if (!single_storage_buffer_binding_excluding(set, bind[0], &bind[1]))
         return false;
   }
   if (bind[0] == bind[1])
      return false;
   const struct r3v_descriptor *desc[2];
   struct r3v_buffer *buf[2];
   for (unsigned i = 0; i < 2; i++) {
      desc[i] = find_descriptor_by_binding(set, bind[i]);
      if (!desc[i] || !desc[i]->buf.buffer)
         return false;
      buf[i] = r3v_buffer_from_handle(desc[i]->buf.buffer);
      if (!buf[i] || !buf[i]->resource)
         return false;
   }

   /* RUNTIME RANGE ADMISSION: the filter averages the RGBA8 R channel, so
    * any input element >= 256 spills its payload out of the carrier and
    * the carried average diverges from the kernel's integer arithmetic.
    * The bound is data-dependent -- no static admission can prove it --
    * so scan the input during this map and refuse out-of-range dispatches
    * with the explicit no-op contract. */
   {
      struct pipe_transfer *ix = NULL;
      struct pipe_box ibox;
      memset(&ibox, 0, sizeof(ibox));
      ibox.x = (int)desc[0]->buf.offset;
      ibox.width = (int)(in_total * 4u);
      ibox.height = 1;
      ibox.depth = 1;
      const uint32_t *imap = pipe->buffer_map(pipe, buf[0]->resource, 0,
                                              PIPE_MAP_READ, &ibox, &ix);
      if (!imap)
         return false;
      for (uint64_t i = 0; i < in_total; i++) {
         if (imap[i] >= 256u) {
            pipe->buffer_unmap(pipe, ix);
            IDM_LOG("log4 dispatch no-op (range admission): "
                      "element %" PRIu64 " = %u exceeds the RGBA8 carrier",
                      i, imap[i]);
            /* Refused, but the queue stays alive: report success so the
             * caller's no-op contract holds; the output is untouched. */
            return true;
         }
      }
      pipe->buffer_unmap(pipe, ix);
   }

   struct pipe_sampler_view *view =
      r3v_identity_map_wrap_input_as_sampler_view(
         device, buf[0]->resource, (unsigned)desc[0]->buf.offset,
         in_w, in_h, in_total, PIPE_FORMAT_R8G8B8A8_UNORM);
   if (!view)
      return false;

   struct pipe_resource *vb = NULL;
   void *velems_cso = NULL;
   if (!r3v_idm_create_fullscreen_vbo(pipe, &vb, &velems_cso)) {
      pipe_sampler_view_reference(&view, NULL);
      return false;
   }

   /* The LINEAR sampler IS the op: corner taps return the half-up quarter
    * sum.  The device cache holds only the NEAREST CSO; create the LINEAR
    * sibling for this pass. */
   struct pipe_sampler_state samp;
   memset(&samp, 0, sizeof(samp));
   samp.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   samp.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   samp.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   samp.min_img_filter = PIPE_TEX_FILTER_LINEAR;
   samp.mag_img_filter = PIPE_TEX_FILTER_LINEAR;
   void *linear_cso = pipe->create_sampler_state(pipe, &samp);
   if (!linear_cso) {
      pipe->delete_vertex_elements_state(pipe, velems_cso);
      pipe_sampler_view_reference(&view, NULL);
      pipe_resource_reference(&vb, NULL);
      return false;
   }

   struct pipe_resource rt_templ;
   memset(&rt_templ, 0, sizeof(rt_templ));
   rt_templ.target     = PIPE_TEXTURE_2D;
   rt_templ.format     = PIPE_FORMAT_R8G8B8A8_UNORM;
   rt_templ.width0     = out_w;
   rt_templ.height0    = out_h;
   rt_templ.depth0     = 1;
   rt_templ.array_size = 1;
   rt_templ.usage      = PIPE_USAGE_DEFAULT;
   rt_templ.bind       = PIPE_BIND_RENDER_TARGET;
   struct pipe_resource *rt = screen->resource_create(screen, &rt_templ);
   bool copy_ok = false;
   if (rt) {
      struct pipe_surface surf_templ;
      memset(&surf_templ, 0, sizeof(surf_templ));
      surf_templ.format  = PIPE_FORMAT_R8G8B8A8_UNORM;
      surf_templ.texture = rt;
      r3v_identity_map_setup_draw_state(pipe, out_w, out_h, &surf_templ,
                                           device->identity_map_cso.blend,
                                           device->identity_map_cso.rasterizer,
                                           device->identity_map_cso.dsa,
                                           pl->vs_cso, pl->fs_cso, velems_cso);
      void *samplers[1] = { linear_cso };
      pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1, samplers);
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 1, 0, &view);

      /* CONST[0] = (W2, H2, 1/W, 1/H): the FS snaps the output cell and
       * constructs the exact corner coordinate from these. */
      const float cb_data[4] = { (float)out_w, (float)out_h,
                                 1.0f / (float)in_w, 1.0f / (float)in_h };
      struct pipe_constant_buffer cb;
      memset(&cb, 0, sizeof(cb));
      cb.user_buffer = cb_data;
      cb.buffer_size = sizeof(cb_data);
      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);

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
      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, NULL);

      copy_ok = r3v_identity_map_readback_rt(pipe, rt, buf[1]->resource,
                                                (unsigned)desc[1]->buf.offset,
                                                out_w, out_h,
                                                PIPE_FORMAT_R8G8B8A8_UNORM,
                                                out_w * 4u, out_total);
   }

   pipe->set_vertex_buffers(pipe, 0, NULL);
   struct pipe_framebuffer_state empty_fb;
   memset(&empty_fb, 0, sizeof(empty_fb));
   pipe->set_framebuffer_state(pipe, &empty_fb);
   pipe->delete_sampler_state(pipe, linear_cso);
   pipe->delete_vertex_elements_state(pipe, velems_cso);
   pipe_sampler_view_reference(&view, NULL);
   pipe_resource_reference(&rt, NULL);
   pipe_resource_reference(&vb, NULL);
   IDM_LOG("log4 done in=%ux%u out=%ux%u copy_ok=%d", in_w, in_h, out_w,
           out_h, (int)copy_ok);
   return copy_ok;
}
