/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_device.h"
#include "r3v_format.h"
#include "r3v_cmd_buffer.h"
#include "r3v_pipeline.h"
#include "r3v_image.h"
#include "r3v_buffer.h"
#include "r3v_object.h"
#include "r3v_descriptor.h"
#include "r3v_memory.h"
#include "r3v_identity_map.h"

#include <stdlib.h>

static bool
identity_map_debug_enabled(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *flags = r3v_getenv_compat("R3V_DEBUG", "R300VK_DEBUG");
      cached = (flags && strstr(flags, "identity_map")) ? 1 : 0;
   }
   return cached != 0;
}

#include "vk_queue.h"
#include "vk_sync.h"

#include "compiler/shader_enums.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "pipe/p_defines.h"
#include "util/u_inlines.h"
#include "util/u_dynarray.h"
#include "util/box.h"
#include "util/format/u_format.h"
#include "util/u_pack_color.h"
#include "util/log.h"
#include "util/macros.h"

#include "vulkan/util/vk_util.h"
#include "vk_format.h"

#include "util/os_time.h"

#include <limits.h>
#include <string.h>

/* Convert a VkViewport to Gallium's scale/translate form.
 * Gallium clip space is [0, 1] depth; VkViewport uses [minDepth, maxDepth]. */
static void
viewport_vk_to_gallium(const VkViewport *vp,
                       float tile_origin_x,
                       float tile_origin_y,
                       struct pipe_viewport_state *pv)
{
   pv->scale[0]     = vp->width  * 0.5f;
   pv->scale[1]     = vp->height * 0.5f;
   pv->scale[2]     = vp->maxDepth - vp->minDepth;
   pv->translate[0] = vp->x - tile_origin_x + vp->width  * 0.5f;
   pv->translate[1] = vp->y - tile_origin_y + vp->height * 0.5f;
   pv->translate[2] = vp->minDepth;
   pv->swizzle_x    = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
   pv->swizzle_y    = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
   pv->swizzle_z    = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
   pv->swizzle_w    = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;
}

static void
r3v_clear_synthetic_stream(struct pipe_vertex_buffer *vb_cache,
                              uint8_t binding)
{
   if (binding < R3V_MAX_VERTEX_BINDINGS)
      memset(&vb_cache[binding], 0, sizeof(vb_cache[binding]));
}

static bool
r3v_bind_synthetic_identity_stream(struct r3v_device *device,
                                      struct pipe_context *pipe,
                                      struct pipe_vertex_buffer *vb_cache,
                                      uint8_t binding,
                                      uint32_t total_count,
                                      struct util_dynarray *transient_vbs)
{
   if (total_count == 0)
      return true;

   if (binding >= R3V_MAX_VERTEX_BINDINGS)
      return false;

   if (total_count > UINT32_MAX / sizeof(float)) {
      r3v_clear_synthetic_stream(vb_cache, binding);
      return false;
   }

   struct pipe_resource tmpl = {
      .target     = PIPE_BUFFER,
      .format     = PIPE_FORMAT_R8_UNORM,
      .bind       = PIPE_BIND_VERTEX_BUFFER,
      .usage      = PIPE_USAGE_STREAM,
      .width0     = (uint64_t)total_count * sizeof(float),
      .height0    = 1,
      .depth0     = 1,
      .array_size = 1,
   };
   struct pipe_resource *res =
      device->screen->resource_create(device->screen, &tmpl);
   if (!res) {
      r3v_clear_synthetic_stream(vb_cache, binding);
      return false;
   }

   /* r300_nir_lower_vs_system_values_to_inputs reads this stream through the
    * float-domain VS the SW-TCL draw path compiles (r300_vs_draw.c runs
    * nir_lower_int_to_float over the whole shader).  A PIPE_FORMAT_R32_SINT
    * payload hits pure_integer handling in lp_build_fetch_rgba_aos_array:
    * the LLVM vertex-fetch JIT always requests a float dst_type, so the fetch
    * bitcasts the raw int32 element index into a float register instead of
    * converting it, and a small index like 2 arrives as the subnormal float
    * 2.8e-45 rather than 2.0f.  Store the identity value as a real float and
    * fetch through PIPE_FORMAT_R32_FLOAT (r3v_build_velems_cso) so the fetch
    * takes the floating-point conversion path with no pure_integer bitcast. */
   struct pipe_transfer *xfer = NULL;
   float *map = pipe_buffer_map(pipe, res,
                                PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                &xfer);
   if (!map) {
      r3v_clear_synthetic_stream(vb_cache, binding);
      pipe_resource_reference(&res, NULL);
      return false;
   }

   for (uint32_t i = 0; i < total_count; i++)
      map[i] = (float)i;
   pipe_buffer_unmap(pipe, xfer);

   vb_cache[binding].is_user_buffer  = false;
   vb_cache[binding].buffer_offset   = 0;
   vb_cache[binding].buffer.resource = res;
   util_dynarray_append(transient_vbs, res);
   return true;
}

/* Allocate a transient vertex buffer for a synthetic VS-system-value stream.
 * Gallium applies draw.start and start_instance while fetching vertex elements,
 * so the buffer must be indexable through base + count rather than only count
 * elements.  The owner reference is held in transient_vbs until the submit fence
 * completes; set_vertex_buffers takes its own references. */
static bool
r3v_bind_synthetic_index_stream(struct r3v_device *device,
                                   struct pipe_context *pipe,
                                   struct pipe_vertex_buffer *vb_cache,
                                   uint8_t binding, uint32_t base, uint32_t count,
                                   struct util_dynarray *transient_vbs)
{
   if (count == 0)
      return true;

   if (count > UINT32_MAX - base) {
      r3v_clear_synthetic_stream(vb_cache, binding);
      return false;
   }

   return r3v_bind_synthetic_identity_stream(device, pipe, vb_cache, binding,
                                                base + count, transient_vbs);
}

static uint32_t
r3v_index_value_load(const uint8_t *ptr, uint32_t index_size)
{
   switch (index_size) {
   case 1:
      return *ptr;
   case 2: {
      uint16_t value;
      memcpy(&value, ptr, sizeof(value));
      return value;
   }
   case 4: {
      uint32_t value;
      memcpy(&value, ptr, sizeof(value));
      return value;
   }
   default:
      return 0;
   }
}

static bool
r3v_indexed_draw_map_indices(struct pipe_context *pipe,
                                const struct r3v_cmd_draw_indexed *draw,
                                uint32_t start_elem,
                                uint32_t count,
                                const uint8_t **indices,
                                struct pipe_transfer **index_xfer)
{
   *indices = NULL;
   *index_xfer = NULL;

   if (count == 0)
      return true;

   if (!draw->index_buffer || !draw->index_buffer->resource ||
       draw->index_size == 0)
      return false;

   const uint64_t index_offset64 = (uint64_t)start_elem * draw->index_size;
   const uint64_t index_span64 =
      (uint64_t)(count - 1u) * draw->index_size + draw->index_size;
   if (index_offset64 > UINT_MAX || index_span64 > UINT_MAX ||
       index_span64 > draw->index_buffer->size ||
       index_offset64 > draw->index_buffer->size - index_span64)
      return false;

   *indices = pipe_buffer_map_range(pipe, draw->index_buffer->resource,
                                    (unsigned)index_offset64,
                                    (unsigned)index_span64, PIPE_MAP_READ,
                                    index_xfer);
   return *indices != NULL;
}

/* Indexed SW-TCL fetches the synthetic VertexIndex attribute through the same
 * index buffer as app vertex attributes, so the stream must be an identity
 * table over the fetched index + vertexOffset domain. */
static bool
r3v_indexed_vertex_index_stream_count(
   struct pipe_context *pipe,
   const struct r3v_cmd_draw_indexed *draw,
   uint32_t start_elem,
   uint32_t count,
   uint32_t *total_count)
{
   *total_count = 0;

   if (count == 0)
      return true;

   if (!draw->index_buffer || !draw->index_buffer->resource ||
       draw->index_size == 0)
      return false;

   struct pipe_transfer *index_xfer = NULL;
   const uint8_t *indices = NULL;
   if (!r3v_indexed_draw_map_indices(pipe, draw, start_elem, count,
                                        &indices, &index_xfer))
      return false;

   int64_t max_vertex_index = -1;
   for (uint32_t i = 0; i < count; i++) {
      const uint32_t index =
         r3v_index_value_load(indices + (uint64_t)i * draw->index_size,
                                 draw->index_size);
      const int64_t vertex_index = (int64_t)index + draw->vertex_offset;
      if (vertex_index < 0 ||
          vertex_index > (int64_t)(UINT32_MAX / sizeof(float)) - 1) {
         pipe_buffer_unmap(pipe, index_xfer);
         return false;
      }

      if (vertex_index > max_vertex_index)
         max_vertex_index = vertex_index;
   }
   pipe_buffer_unmap(pipe, index_xfer);

   *total_count = (uint32_t)max_vertex_index + 1u;
   return true;
}

static bool
r3v_binding_index_inside(const struct r3v_pipeline *pl,
                            const struct pipe_vertex_buffer *vb_cache,
                            const VkDeviceSize *vb_sizes,
                            const VkDeviceSize *vb_strides,
                            uint32_t vb_strides_mask,
                            uint32_t binding,
                            uint64_t index)
{
   const VkDeviceSize stride64 = (vb_strides_mask & BITFIELD_BIT(binding))
                                 ? vb_strides[binding]
                                 : pl->vertex_stride[binding];
   if (stride64 > UINT32_MAX)
      return false;

   const uint32_t stride = (uint32_t)stride64;
   const uint32_t extent = pl->vertex_binding_extent[binding];
   if (extent == 0 || !vb_cache[binding].buffer.resource)
      return false;

   const VkDeviceSize offset = vb_cache[binding].buffer_offset;
   const VkDeviceSize size = vb_sizes[binding];
   const VkDeviceSize bytes = size > offset ? size - offset : 0;
   if (bytes < extent)
      return false;

   if (stride == 0)
      return true;

   const VkDeviceSize available = 1 + (bytes - extent) / stride;
   return index < available;
}

static bool
r3v_vertex_index_inside_bindings(const struct r3v_pipeline *pl,
                                    const struct pipe_vertex_buffer *vb_cache,
                                    const VkDeviceSize *vb_sizes,
                                    const VkDeviceSize *vb_strides,
                                    uint32_t vb_strides_mask,
                                    int64_t vertex_index)
{
   if (!pl || pl->vertex_binding_mask == 0)
      return true;
   if (vertex_index < 0)
      return false;

   const uint32_t per_vertex_mask =
      pl->vertex_binding_mask & ~pl->vertex_instance_binding_mask;
   const uint64_t vertex = (uint64_t)vertex_index;
   for (uint32_t b = 0; b < R3V_MAX_VERTEX_BINDINGS; b++) {
      if (!(per_vertex_mask & BITFIELD_BIT(b)))
         continue;

      if (!r3v_binding_index_inside(pl, vb_cache, vb_sizes, vb_strides,
                                       vb_strides_mask, b, vertex))
         return false;
   }

   return true;
}

static uint32_t
r3v_robust_indexed_vertex_count(struct pipe_context *pipe,
                                   const struct r3v_pipeline *pl,
                                   const struct pipe_vertex_buffer *vb_cache,
                                   const VkDeviceSize *vb_sizes,
                                   const VkDeviceSize *vb_strides,
                                   uint32_t vb_strides_mask,
                                   const struct r3v_cmd_draw_indexed *draw,
                                   uint32_t start_elem,
                                   uint32_t count)
{
   if (!pl || pl->vertex_binding_mask == 0 || count == 0)
      return count;

   struct pipe_transfer *index_xfer = NULL;
   const uint8_t *indices = NULL;
   if (!r3v_indexed_draw_map_indices(pipe, draw, start_elem, count,
                                        &indices, &index_xfer))
      return 0;

   uint32_t clamped_count = count;
   for (uint32_t i = 0; i < count; i++) {
      const uint32_t index =
         r3v_index_value_load(indices + (uint64_t)i * draw->index_size,
                                 draw->index_size);
      const int64_t vertex_index = (int64_t)index + draw->vertex_offset;
      if (!r3v_vertex_index_inside_bindings(pl, vb_cache, vb_sizes,
                                               vb_strides, vb_strides_mask,
                                               vertex_index)) {
         clamped_count = i;
         break;
      }
   }

   pipe_buffer_unmap(pipe, index_xfer);
   return clamped_count;
}

static bool
r3v_bind_synthetic_indexed_vertex_index_stream(
   struct r3v_device *device,
   struct pipe_context *pipe,
   struct pipe_vertex_buffer *vb_cache,
   uint8_t binding,
   const struct r3v_cmd_draw_indexed *draw,
   uint32_t start_elem,
   uint32_t count,
   struct util_dynarray *transient_vbs)
{
   uint32_t total_count = 0;
   if (!r3v_indexed_vertex_index_stream_count(pipe, draw, start_elem, count,
                                                 &total_count)) {
      r3v_clear_synthetic_stream(vb_cache, binding);
      return false;
   }

   return r3v_bind_synthetic_identity_stream(device, pipe, vb_cache, binding,
                                                total_count, transient_vbs);
}

static uint32_t
r3v_image_tile_origin_x(const struct r3v_image *img, uint32_t tile_col)
{
   return img && tile_col > 0 ? img->tile_width[0] : 0;
}

static uint32_t
r3v_image_tile_origin_y(const struct r3v_image *img, uint32_t tile_row)
{
   return img && tile_row > 0 ? img->tile_height[0] : 0;
}

static uint32_t
r3v_image_extent_width(const struct r3v_image *img)
{
   if (!img || !img->tile_cols)
      return 0;

   return img->tile_width[0] +
          (img->tile_cols > 1 ? img->tile_width[1] : 0);
}

static uint32_t
r3v_image_extent_height(const struct r3v_image *img)
{
   if (!img || !img->tile_rows)
      return 0;

   return img->tile_height[0] +
          (img->tile_rows > 1 ? img->tile_height[1] : 0);
}

static bool
r3v_image_tile_for_origin(const struct r3v_image *img,
                             uint32_t origin_x,
                             uint32_t origin_y,
                             uint32_t *tile_index,
                             uint32_t *tile_origin_x,
                             uint32_t *tile_origin_y,
                             uint32_t *remaining_width,
                             uint32_t *remaining_height)
{
   if (!img || !img->tile_cols || !img->tile_rows)
      return false;

   if (origin_x >= r3v_image_extent_width(img) ||
       origin_y >= r3v_image_extent_height(img))
      return false;

   const uint32_t col =
      (img->tile_cols > 1 && origin_x >= img->tile_width[0]) ? 1 : 0;
   const uint32_t row =
      (img->tile_rows > 1 && origin_y >= img->tile_height[0]) ? 1 : 0;
   if (col >= img->tile_cols || row >= img->tile_rows)
      return false;

   *tile_index = row * img->tile_cols + col;
   *tile_origin_x = r3v_image_tile_origin_x(img, col);
   *tile_origin_y = r3v_image_tile_origin_y(img, row);
   *remaining_width = img->tile_width[col] - (origin_x - *tile_origin_x);
   *remaining_height = img->tile_height[row] - (origin_y - *tile_origin_y);
   return true;
}

static struct pipe_resource *
r3v_image_tile_resource_for_origin(const struct r3v_image *img,
                                      uint32_t origin_x,
                                      uint32_t origin_y,
                                      uint32_t *tile_origin_x,
                                      uint32_t *tile_origin_y,
                                      uint32_t *remaining_width,
                                      uint32_t *remaining_height)
{
   uint32_t tile_index = 0;
   if (!r3v_image_tile_for_origin(img, origin_x, origin_y, &tile_index,
                                     tile_origin_x, tile_origin_y,
                                     remaining_width, remaining_height))
      return NULL;

   return img->tiles[tile_index] ? img->tiles[tile_index] : img->resource;
}

#define R3V_RENDER_AREA_MAX_CUTS (PIPE_MAX_COLOR_BUFS + 3)

static void
r3v_render_area_insert_cut(uint32_t *cuts, uint32_t *count, uint32_t cut)
{
   for (uint32_t i = 0; i < *count; i++) {
      if (cuts[i] == cut)
         return;
      if (cuts[i] > cut) {
         if (*count >= R3V_RENDER_AREA_MAX_CUTS)
            return;
         memmove(&cuts[i + 1], &cuts[i], (*count - i) * sizeof(cuts[0]));
         cuts[i] = cut;
         (*count)++;
         return;
      }
   }

   if (*count < R3V_RENDER_AREA_MAX_CUTS) {
      cuts[*count] = cut;
      (*count)++;
   }
}

static void
r3v_begin_rp_add_image_tile_cuts(const struct r3v_image *img,
                                    uint32_t area_width,
                                    uint32_t area_height,
                                    uint32_t *x_cuts,
                                    uint32_t *x_count,
                                    uint32_t *y_cuts,
                                    uint32_t *y_count)
{
   if (!img)
      return;

   if (img->tile_cols > 1 && img->tile_width[0] < area_width)
      r3v_render_area_insert_cut(x_cuts, x_count, img->tile_width[0]);
   if (img->tile_rows > 1 && img->tile_height[0] < area_height)
      r3v_render_area_insert_cut(y_cuts, y_count, img->tile_height[0]);
}

static void
r3v_begin_rp_collect_tile_cuts(const struct r3v_cmd_begin_render_pass *rp,
                                  uint32_t *x_cuts,
                                  uint32_t *x_count,
                                  uint32_t *y_cuts,
                                  uint32_t *y_count)
{
   *x_count = 0;
   *y_count = 0;

   if (rp->width == 0 || rp->height == 0)
      return;

   r3v_render_area_insert_cut(x_cuts, x_count, 0);
   r3v_render_area_insert_cut(x_cuts, x_count, rp->width);
   r3v_render_area_insert_cut(y_cuts, y_count, 0);
   r3v_render_area_insert_cut(y_cuts, y_count, rp->height);

   for (uint32_t slot = 0; slot < rp->color_count; slot++)
      r3v_begin_rp_add_image_tile_cuts(rp->color_image[slot], rp->width,
                                          rp->height, x_cuts, x_count,
                                          y_cuts, y_count);
   r3v_begin_rp_add_image_tile_cuts(rp->ds_image, rp->width, rp->height,
                                       x_cuts, x_count, y_cuts, y_count);
}

static bool
r3v_begin_rp_tile_geometry(const struct r3v_cmd_begin_render_pass *rp,
                              uint32_t tile_pass,
                              uint32_t *tile_origin_x,
                              uint32_t *tile_origin_y,
                              uint32_t *tile_width,
                              uint32_t *tile_height)
{
   uint32_t x_cuts[R3V_RENDER_AREA_MAX_CUTS];
   uint32_t y_cuts[R3V_RENDER_AREA_MAX_CUTS];
   uint32_t x_count = 0;
   uint32_t y_count = 0;

   r3v_begin_rp_collect_tile_cuts(rp, x_cuts, &x_count, y_cuts, &y_count);
   if (x_count < 2 || y_count < 2)
      return false;

   const uint32_t tile_cols = x_count - 1;
   const uint32_t tile_rows = y_count - 1;
   if (tile_pass >= tile_cols * tile_rows)
      return false;

   const uint32_t tile_col = tile_pass % tile_cols;
   const uint32_t tile_row = tile_pass / tile_cols;
   *tile_origin_x = x_cuts[tile_col];
   *tile_origin_y = y_cuts[tile_row];
   *tile_width = x_cuts[tile_col + 1] - *tile_origin_x;
   *tile_height = y_cuts[tile_row + 1] - *tile_origin_y;
   return *tile_width != 0 && *tile_height != 0;
}

/* Return a representative attachment for optional diagnostics and quick
 * render-pass presence checks.  Replay tile selection uses the render-area
 * cut grid above, not this image's tile numbering. */
static struct r3v_image *
r3v_begin_rp_ref_image(const struct r3v_cmd_begin_render_pass *rp)
{
   for (uint32_t slot = 0; slot < rp->color_count; slot++)
      if (rp->color_image[slot])
         return rp->color_image[slot];
   return rp->ds_image;
}

static uint32_t
r3v_begin_rp_tile_pass_count(const struct r3v_cmd_begin_render_pass *rp)
{
   uint32_t x_cuts[R3V_RENDER_AREA_MAX_CUTS];
   uint32_t y_cuts[R3V_RENDER_AREA_MAX_CUTS];
   uint32_t x_count = 0;
   uint32_t y_count = 0;

   r3v_begin_rp_collect_tile_cuts(rp, x_cuts, &x_count, y_cuts, &y_count);
   if (x_count < 2 || y_count < 2)
      return 0;

   return (x_count - 1) * (y_count - 1);
}

static uint32_t
r3v_cmd_tile_pass_count(const struct r3v_cmd_buffer *cmd)
{
   uint32_t pass_count = 1;

   for (uint32_t i = 0; i < cmd->entry_count; i++) {
      const struct r3v_cmd_entry *entry = &cmd->entries[i];
      if (entry->type != R3V_CMD_BEGIN_RENDER_PASS &&
          entry->type != R3V_CMD_NEXT_SUBPASS)
         continue;

      pass_count = MAX2(pass_count,
                        r3v_begin_rp_tile_pass_count(&entry->begin_rp));
   }

   return pass_count;
}

static void
r3v_scissor_vk_to_tile(const VkRect2D *rect,
                          uint32_t tile_origin_x,
                          uint32_t tile_origin_y,
                          uint32_t tile_width,
                          uint32_t tile_height,
                          struct pipe_scissor_state *sc)
{
   const int64_t rect_min_x = rect->offset.x;
   const int64_t rect_min_y = rect->offset.y;
   const int64_t rect_max_x = rect_min_x + rect->extent.width;
   const int64_t rect_max_y = rect_min_y + rect->extent.height;
   const int64_t tile_min_x = tile_origin_x;
   const int64_t tile_min_y = tile_origin_y;
   const int64_t tile_max_x = tile_min_x + tile_width;
   const int64_t tile_max_y = tile_min_y + tile_height;

   const int64_t clip_min_x = MAX2(rect_min_x, tile_min_x);
   const int64_t clip_min_y = MAX2(rect_min_y, tile_min_y);
   const int64_t clip_max_x = MIN2(rect_max_x, tile_max_x);
   const int64_t clip_max_y = MIN2(rect_max_y, tile_max_y);

   if (clip_max_x <= clip_min_x || clip_max_y <= clip_min_y) {
      memset(sc, 0, sizeof(*sc));
      return;
   }

   sc->minx = (unsigned)(clip_min_x - tile_min_x);
   sc->miny = (unsigned)(clip_min_y - tile_min_y);
   sc->maxx = (unsigned)(clip_max_x - tile_min_x);
   sc->maxy = (unsigned)(clip_max_y - tile_min_y);
}

static uint32_t
r3v_robust_vertex_count(const struct r3v_pipeline *pl,
                           const struct pipe_vertex_buffer *vb_cache,
                           const VkDeviceSize *vb_sizes,
                           const VkDeviceSize *vb_strides,
                           uint32_t vb_strides_mask,
                           uint32_t first_vertex,
                           uint32_t vertex_count)
{
   if (!pl || pl->vertex_binding_mask == 0)
      return vertex_count;

   const uint32_t per_vertex_mask =
      pl->vertex_binding_mask & ~pl->vertex_instance_binding_mask;
   if (per_vertex_mask == 0)
      return vertex_count;

   uint32_t max_count = vertex_count;
   for (uint32_t b = 0; b < R3V_MAX_VERTEX_BINDINGS; b++) {
      if (!(per_vertex_mask & BITFIELD_BIT(b)))
         continue;

      const VkDeviceSize stride64 = (vb_strides_mask & BITFIELD_BIT(b))
                                    ? vb_strides[b] : pl->vertex_stride[b];
      if (stride64 > UINT32_MAX)
         return 0;

      const uint32_t stride = (uint32_t)stride64;
      const uint32_t extent = pl->vertex_binding_extent[b];
      if (extent == 0 || !vb_cache[b].buffer.resource)
         return 0;

      const VkDeviceSize offset = vb_cache[b].buffer_offset;
      const VkDeviceSize size = vb_sizes[b];
      const VkDeviceSize bytes = size > offset ? size - offset : 0;
      if (bytes < extent)
         return 0;

      if (stride == 0)
         continue;

      const VkDeviceSize vertices = 1 + (bytes - extent) / stride;
      if (first_vertex >= vertices)
         return 0;

      const VkDeviceSize available_vertices = vertices - first_vertex;
      if (available_vertices < max_count)
         max_count = (uint32_t)available_vertices;
   }

   return max_count;
}

static uint32_t
r3v_robust_instance_count(const struct r3v_pipeline *pl,
                             const struct pipe_vertex_buffer *vb_cache,
                             const VkDeviceSize *vb_sizes,
                             const VkDeviceSize *vb_strides,
                             uint32_t vb_strides_mask,
                             uint32_t first_instance,
                             uint32_t instance_count)
{
   if (!pl || pl->vertex_instance_binding_mask == 0)
      return instance_count;

   uint32_t max_count = instance_count;
   for (uint32_t b = 0; b < R3V_MAX_VERTEX_BINDINGS; b++) {
      if (!(pl->vertex_instance_binding_mask & BITFIELD_BIT(b)))
         continue;

      const uint64_t first = first_instance;
      if (!r3v_binding_index_inside(pl, vb_cache, vb_sizes, vb_strides,
                                       vb_strides_mask, b, first))
         return 0;

      const VkDeviceSize stride64 = (vb_strides_mask & BITFIELD_BIT(b))
                                    ? vb_strides[b] : pl->vertex_stride[b];
      const uint32_t stride = (uint32_t)stride64;
      if (stride == 0)
         continue;

      const uint32_t extent = pl->vertex_binding_extent[b];
      const VkDeviceSize offset = vb_cache[b].buffer_offset;
      const VkDeviceSize size = vb_sizes[b];
      const VkDeviceSize bytes = size > offset ? size - offset : 0;
      const VkDeviceSize available = 1 + (bytes - extent) / stride;
      const VkDeviceSize remaining = available - first;
      if (remaining < max_count)
         max_count = (uint32_t)remaining;
   }

   return max_count;
}

/* CCN is proportional to the number of command types dispatched; one case
 * per r3v_cmd_type is the minimum correct structure. */
static void
r3v_replay_bind_descriptor_sets(struct r3v_cmd_bind_descriptor_sets *accum,
                                    const struct r3v_cmd_entry *e)
{
   /* Descriptor-set bindings persist across vkCmdBindDescriptorSets calls: each
    * call replaces only sets [firstSet, firstSet+count), leaving the rest bound.
    * Accumulate per absolute set index so a draw sees every set bound so far --
    * e.g. a UBO in set 0 and a combined image sampler in set 1 bound by two
    * separate calls -- instead of only the most recent call's sets. */
   const struct r3v_cmd_bind_descriptor_sets *b = &e->bind_dsets;
   accum->bind_point      = b->bind_point;
   accum->pipeline_layout = b->pipeline_layout;
   for (uint32_t i = 0; i < b->set_count; i++) {
      const uint32_t abs_set = b->first_set + i;
      if (abs_set >= R3V_MAX_BOUND_DESCRIPTOR_SETS)
         continue;
      accum->sets[abs_set] = b->sets[i];
      if (abs_set + 1 > accum->set_count)
         accum->set_count = abs_set + 1;
   }
   accum->first_set = 0;
   /* Dynamic offsets apply to the dynamic descriptors of the sets in this call;
    * r3v binds only static uniform buffers, so carry the latest call's offsets
    * rather than tracking them per set. */
   accum->dynamic_offset_count = b->dynamic_offset_count;
   for (uint32_t i = 0; i < b->dynamic_offset_count; i++)
      accum->dynamic_offsets[i] = b->dynamic_offsets[i];
}

static VkResult
r3v_replay_dispatch(struct r3v_device *device,
                        const struct r3v_cmd_entry *e,
                        const struct r3v_cmd_bind_descriptor_sets *last_bind_dsets,
                        const uint8_t *push_data)
{
   const struct r3v_cmd_dispatch *d = &e->dispatch;
   const struct r3v_pipeline *pl = d->pipeline;
   bool ok = false;

   if (!pl)
      return VK_SUCCESS;

   if (!pl->admission.admissible) {
      mesa_logw("r3v: dispatch no-op (admission): %s (%s)",
                r300_compute_reject_name(pl->admission.reason),
                pl->admission.detail ? pl->admission.detail : "no detail");
      return VK_SUCCESS;
   }

   /* Index-exactness gate: a dispatch whose invocation count exceeds the
    * honest bound for the kernel's classified index consumption must not
    * draw -- index identity would corrupt silently in FP24.  Like
    * UNKNOWN_SHAPE, the dispatch becomes an explicit logged no-op rather
    * than a poisoned queue. */
   const char *index_reject = NULL;
   if (!r3v_dispatch_index_exact(pl, d, &index_reject)) {
      mesa_logw("r3v: dispatch no-op (index exactness): %s "
                "consumption=%d stride=%u offset=%u gx=%u gy=%u gz=%u",
                index_reject ? index_reject : "unknown",
                pl ? (int)pl->index_consumption.consumption : -1,
                pl ? pl->index_consumption.stride : 0,
                pl ? pl->index_consumption.offset : 0,
                d->group_count_x, d->group_count_y, d->group_count_z);
      return VK_SUCCESS;
   }

   if (pl->identity_map.is_identity_map)
      ok = r3v_identity_map_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->const_fill.is_const_fill)
      ok = r3v_const_fill_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->affine_iota.is_affine_iota)
      ok = r3v_affine_iota_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->multilimb_mul.is_multilimb_mul)
      ok = r3v_multilimb_mul_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->cas.is_cas)
      ok = r3v_cas_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->log4_pool.is_log4_pool)
      ok = r3v_log4_pool_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->binary_map.is_binary_map)
      ok = r3v_binary_map_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->unary_map.is_unary_map)
      ok = r3v_unary_map_dispatch_replay(device, pl, d, last_bind_dsets,
                                            push_data);
   else if (pl->unary_transcendental.is_unary_transcendental)
      ok = r3v_unary_transcendental_dispatch_replay(device, pl, d,
                                                       last_bind_dsets);
   else if (pl->binary_transcendental.is_binary_transcendental)
      ok = r3v_binary_transcendental_dispatch_replay(device, pl, d,
                                                        last_bind_dsets);
   else if (pl->bitwise_logicop.is_bitwise_logicop)
      ok = r3v_bitwise_logicop_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->shift_logical.is_shift_logical)
      ok = r3v_shift_logical_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->shift_variable.is_shift_variable)
      ok = r3v_shift_variable_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->dp4.is_dp4)
      ok = r3v_dp4_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->qmul.is_qmul)
      ok = r3v_qmul_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->qdiv.is_qdiv)
      ok = r3v_qdiv_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->mat4vec.is_mat4vec)
      ok = r3v_mat4vec_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->qfmul.is_qfmul)
      ok = r3v_qfmul_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->qrotate.is_qrotate)
      ok = r3v_qrotate_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->qconj.is_qconj)
      ok = r3v_qconj_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->qnorm.is_qnorm)
      ok = r3v_qnorm_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->qnormalize.is_qnormalize)
      ok = r3v_qnormalize_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->omul.is_omul)
      ok = r3v_omul_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->oaddsub.is_oaddsub)
      ok = r3v_oaddsub_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->oconj.is_oconj)
      ok = r3v_oconj_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->onorm.is_onorm)
      ok = r3v_onorm_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->odiv.is_odiv)
      ok = r3v_odiv_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->otrans.is_otrans)
      ok = r3v_otrans_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->qfmadd.is_qfmadd)
      ok = r3v_qfmadd_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->qfmmul.is_qfmmul)
      ok = r3v_qfmmul_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->blend_acc_reduction.is_blend_acc_reduction)
      ok = r3v_blend_acc_reduction_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->zpass_reduction.is_zpass_reduction)
      ok = r3v_zpass_reduction_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->multipass_scan.is_multipass_scan)
      ok = r3v_multipass_scan_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->predicated_store.is_predicated_store)
      ok = r3v_predicated_store_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->multitap_gather.is_multitap_gather)
      ok = r3v_multitap_gather_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->ieee16_classify.is_ieee16_classify)
      ok = r3v_ieee16_classify_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->ieee16_mul.is_ieee16_mul)
      ok = r3v_ieee16_mul_dispatch_replay(device, pl, d, last_bind_dsets);

   if (ok)
      return VK_SUCCESS;

   /* No raster verb matched at dispatch time.  The kernel was admitted by the
    * classifier (no unsupported construct), but the pattern detectors found no
    * recognized shape.  Log the miss at warning level and return VK_SUCCESS as
    * a no-op: the output buffer is not written, but the queue is not poisoned
    * and subsequent commands on the same queue proceed normally.
    * R300_COMPUTE_REJECT_UNKNOWN_SHAPE is the canonical label for this case. */
   if (pl) {
      mesa_logw("r3v: dispatch no-op (unknown shape): admit=%d "
                "identity=%d binary=%d unary=%d const_fill=%d",
                (int)pl->admission.admissible,
                (int)pl->identity_map.is_identity_map,
                (int)pl->binary_map.is_binary_map,
                (int)pl->unary_map.is_unary_map,
                (int)pl->const_fill.is_const_fill);
   }
   return VK_SUCCESS;
}

/* Map a coordinate through the blit's affine relation, rounded to nearest.
 * Given input range [a0,a1] and output range [b0,b1], returns the output for
 * input x: b0 + (x-a0)*(b1-b0)/(a1-a0).  a1 != a0; either range may descend
 * (a mirrored blit makes the source endpoints descend), so the division is done
 * on magnitudes with the sign restored.  Exact for a 1:1 or integer-ratio blit;
 * a fractional ratio rounds, which is where a tile seam can land one texel off. */
static int32_t
r3v_blit_affine(int32_t x, int32_t a0, int32_t a1, int32_t b0, int32_t b1)
{
   int64_t num = (int64_t)(x - a0) * (b1 - b0);
   int64_t den = (int64_t)a1 - a0;
   if (den < 0) { den = -den; num = -num; }
   int64_t q = num / den;
   int64_t rem = num - q * den;
   if (2 * (rem < 0 ? -rem : rem) >= den)
      q += num < 0 ? -1 : 1;
   return b0 + (int32_t)q;
}

/* The per-axis cut list for the tiled blit: the destination coordinates where a
 * sub-blit must start or end, with the source coordinate the affine map sends
 * each one to.  Cuts land at the destination box edges, the destination tile
 * boundary, and the destination pre-image of the source tile boundary, so every
 * resulting interval lies within one destination tile and maps into one source
 * tile.  An axis is at most two tiles, so at most four cuts (three intervals). */
struct r3v_blit_axis {
   uint32_t count;
   int32_t  dst[4];
   int32_t  src[4];
};

static void
r3v_blit_build_axis(int32_t d_lo, int32_t d_hi,
                       int32_t s_lo, int32_t s_hi,
                       int32_t dst_bound, int32_t src_bound,
                       struct r3v_blit_axis *ax)
{
   int32_t cuts[4];
   uint32_t n = 0;
   cuts[n++] = d_lo;
   cuts[n++] = d_hi;

   /* Destination tile boundary, if the box straddles it. */
   if (dst_bound > d_lo && dst_bound < d_hi)
      cuts[n++] = dst_bound;

   /* Destination coordinate whose source pre-image is the source tile boundary,
    * so the source side of each interval stays inside one source tile. */
   if (src_bound > 0) {
      const int32_t s_min = MIN2(s_lo, s_hi);
      const int32_t s_max = MAX2(s_lo, s_hi);
      if (src_bound > s_min && src_bound < s_max) {
         const int32_t dcut =
            r3v_blit_affine(src_bound, s_lo, s_hi, d_lo, d_hi);
         if (dcut > d_lo && dcut < d_hi)
            cuts[n++] = dcut;
      }
   }

   /* Insertion-sort the (at most four) cuts and drop duplicates. */
   for (uint32_t i = 1; i < n; i++) {
      const int32_t v = cuts[i];
      uint32_t j = i;
      while (j > 0 && cuts[j - 1] > v) { cuts[j] = cuts[j - 1]; j--; }
      cuts[j] = v;
   }

   ax->count = 0;
   for (uint32_t i = 0; i < n; i++) {
      if (i > 0 && cuts[i] == cuts[i - 1])
         continue;
      ax->dst[ax->count] = cuts[i];
      ax->src[ax->count] = r3v_blit_affine(cuts[i], d_lo, d_hi, s_lo, s_hi);
      ax->count++;
   }
}

static unsigned
r3v_blit_aspect_mask(VkImageAspectFlags aspect)
{
   unsigned mask = 0;

   if (aspect & VK_IMAGE_ASPECT_COLOR_BIT)
      mask |= PIPE_MASK_RGBA;
   if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
      mask |= PIPE_MASK_Z;
   if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
      mask |= PIPE_MASK_S;

   return mask;
}

/* Replay one vkCmdBlitImage2 region on the GPU through pipe->blit.  A blit
 * scales and filters, which the CPU tile-copy paths do not, so r300_blit ->
 * util_blitter does the work; r300_blit saves and restores its own pipe state.
 *
 * r300 samples the blit source as a texture and takes TX_WIDTH/TX_HEIGHT from
 * the source resource, so a source resource wider than the sampler cap wraps the
 * 11-bit field and samples garbage.  r3v therefore tiles every optimal image
 * at the sampler cap (see r3v_split_image_axis), and this walks the source
 * and destination tile grids together: each sub-blit reads one source tile and
 * writes one destination tile, both within the cap.  The cut list places every
 * sub-blit's source inside one source tile and destination inside one
 * destination tile.  The destination box is normalized positive per axis; the
 * source endpoint carries the sign so a mirrored blit becomes a negative source
 * width/height, exactly as pipe_blit_info expects.
 *
 * A 1:1 or integer-ratio blit tiles exactly.  A blit that scales by a
 * fractional ratio from a tile-split source has an irreducible seam: the two
 * source tiles are separate textures, so a LINEAR sample cannot interpolate
 * across the boundary and a rounded cut can shift a NEAREST sample one texel.
 * This is a silicon limit (the sampler cannot address past the cap in one view),
 * not a driver defect. */

/* The RS482 TX unit applies the DXT1 endpoint-order rule to DXT3/DXT5 color
 * blocks: when color0 <= color1 it decodes 3-color + black where those formats
 * require always-4-color mode (hardware-confirmed by the BC2/BC3 endpoint-order
 * probe; Mesa's S3TC encoder carries the matching workaround in
 * texcompress_s3tc_tmp.h).  Every such block has an equivalent color0 > color1
 * encoding with an identical specified decode: swap the endpoints and invert
 * the low bit of every 2-bit index (code 0 <-> 1, 2 <-> 3).  An equal-endpoint
 * block decodes to one color everywhere, re-encoded as (color0, 0, all-code-0)
 * -- except color0 == 0, where code-3 black already equals the block's color
 * so the block is left as stored.  The image's stored bytes stay untouched, so
 * buffer/image transfer round-trips remain byte-exact; only the scratch copy
 * the sampler reads is canonicalized. */
static struct pipe_resource *
r3v_dxt35_canonical_source(struct r3v_device *device,
                              struct pipe_resource *sres)
{
   struct pipe_context *pipe = device->pipe;
   struct pipe_resource tmpl = *sres;
   tmpl.bind = PIPE_BIND_SAMPLER_VIEW;
   tmpl.usage = PIPE_USAGE_DEFAULT;
   struct pipe_resource *scratch =
      device->screen->resource_create(device->screen, &tmpl);
   if (!scratch)
      return NULL;

   const unsigned nbx = util_format_get_nblocksx(sres->format, sres->width0);
   const unsigned nby = util_format_get_nblocksy(sres->format, sres->height0);
   struct pipe_box box;
   u_box_2d(0, 0, sres->width0, sres->height0, &box);

   struct pipe_transfer *sxfer = NULL, *dxfer = NULL;
   const uint8_t *smap =
      pipe->texture_map(pipe, sres, 0, PIPE_MAP_READ, &box, &sxfer);
   uint8_t *dmap = smap ? pipe->texture_map(pipe, scratch, 0, PIPE_MAP_WRITE,
                                            &box, &dxfer)
                        : NULL;
   if (!smap || !dmap) {
      if (smap)
         pipe->texture_unmap(pipe, sxfer);
      pipe_resource_reference(&scratch, NULL);
      return NULL;
   }

   for (unsigned by = 0; by < nby; by++) {
      const uint8_t *srow = smap + (size_t)by * sxfer->stride;
      uint8_t *drow = dmap + (size_t)by * dxfer->stride;
      for (unsigned bx = 0; bx < nbx; bx++) {
         uint8_t blk[16];
         memcpy(blk, srow + (size_t)bx * 16, 16);
         /* Bytes 0-7 are the alpha sub-block (mode-free); bytes 8-15 are the
          * color sub-block: two little-endian RGB565 endpoints, then sixteen
          * 2-bit codes.  XOR 0x55 flips the low bit of all four codes in one
          * index byte. */
         const uint16_t c0 = blk[8] | (uint16_t)blk[9] << 8;
         const uint16_t c1 = blk[10] | (uint16_t)blk[11] << 8;
         if (c0 < c1) {
            blk[8] = c1 & 0xff;
            blk[9] = c1 >> 8;
            blk[10] = c0 & 0xff;
            blk[11] = c0 >> 8;
            for (unsigned k = 12; k < 16; k++)
               blk[k] ^= 0x55;
         } else if (c0 == c1 && c0 != 0) {
            blk[10] = 0;
            blk[11] = 0;
            blk[12] = blk[13] = blk[14] = blk[15] = 0;
         }
         memcpy(drow + (size_t)bx * 16, blk, 16);
      }
   }
   pipe->texture_unmap(pipe, sxfer);
   pipe->texture_unmap(pipe, dxfer);
   return scratch;
}

static void
r3v_replay_blit(struct r3v_device *device,
                   const struct r3v_cmd_entry *e)
{
   struct pipe_context *pipe = device->pipe;
   const struct r3v_cmd_blit_image *b = &e->blit_image;
   const struct r3v_image *src = b->src;
   const struct r3v_image *dst = b->dst;

   if (!src->resource || !dst->resource)
      return;

   const enum pipe_format src_fmt = src->resource->format;
   const enum pipe_format dst_fmt = dst->resource->format;
   const unsigned mask =
      r3v_blit_aspect_mask(b->region.srcSubresource.aspectMask);
   if (!mask)
      return;

   /* DXT3/DXT5 sources are sampled through a canonicalized scratch copy so a
    * color0 <= color1 block decodes per the always-4-color rule instead of the
    * silicon's DXT1-mode 3-color + black.  One scratch per source tile, built
    * lazily; DXT1 keeps its stored blocks (punch-through is part of that
    * format's specified decode). */
   const bool canonicalize_dxt35 =
      src_fmt == PIPE_FORMAT_DXT3_RGBA || src_fmt == PIPE_FORMAT_DXT3_SRGBA ||
      src_fmt == PIPE_FORMAT_DXT5_RGBA || src_fmt == PIPE_FORMAT_DXT5_SRGBA;
   struct pipe_resource *canonical_tiles[4] = { NULL, NULL, NULL, NULL };
   const enum pipe_tex_filter filter =
      b->filter == VK_FILTER_NEAREST ? PIPE_TEX_FILTER_NEAREST
                                     : PIPE_TEX_FILTER_LINEAR;

   const VkOffset3D *so = b->region.srcOffsets;
   const VkOffset3D *dof = b->region.dstOffsets;

   /* Normalize each axis: destination ascending, source endpoint following. */
   int32_t dx_lo, dx_hi, sx_lo, sx_hi, dy_lo, dy_hi, sy_lo, sy_hi;
   if (dof[0].x <= dof[1].x) {
      dx_lo = dof[0].x; dx_hi = dof[1].x; sx_lo = so[0].x; sx_hi = so[1].x;
   } else {
      dx_lo = dof[1].x; dx_hi = dof[0].x; sx_lo = so[1].x; sx_hi = so[0].x;
   }
   if (dof[0].y <= dof[1].y) {
      dy_lo = dof[0].y; dy_hi = dof[1].y; sy_lo = so[0].y; sy_hi = so[1].y;
   } else {
      dy_lo = dof[1].y; dy_hi = dof[0].y; sy_lo = so[1].y; sy_hi = so[0].y;
   }

   /* A zero-area destination or source contributes nothing and would divide by
    * zero in the affine map. */
   if (dx_lo == dx_hi || dy_lo == dy_hi || sx_lo == sx_hi || sy_lo == sy_hi)
      return;

   const int32_t src_bound_x = src->tile_cols == 2 ? (int32_t)src->tile_width[0] : 0;
   const int32_t src_bound_y = src->tile_rows == 2 ? (int32_t)src->tile_height[0] : 0;
   const int32_t dst_bound_x = dst->tile_cols == 2 ? (int32_t)dst->tile_width[0] : 0;
   const int32_t dst_bound_y = dst->tile_rows == 2 ? (int32_t)dst->tile_height[0] : 0;

   struct r3v_blit_axis ax, ay;
   r3v_blit_build_axis(dx_lo, dx_hi, sx_lo, sx_hi, dst_bound_x, src_bound_x, &ax);
   r3v_blit_build_axis(dy_lo, dy_hi, sy_lo, sy_hi, dst_bound_y, src_bound_y, &ay);

   /* A cap-sized (2048x2048) blit fills much of the r300 command stream, and
    * the kernel CS parser rejects a submission that batches several of them
    * ("Failed to initialize parser"), silently dropping every sub-blit but the
    * last.  When the blit is tiled, flush after each sub-blit so each one is its
    * own in-limit command stream.  A single-cell (in-cap) blit batches with the
    * surrounding replay and is submitted by the queue flush, unchanged. */
   const uint32_t ncells = (ax.count - 1) * (ay.count - 1);

   for (uint32_t j = 0; j + 1 < ay.count; j++) {
      for (uint32_t i = 0; i + 1 < ax.count; i++) {
         const int32_t cdx0 = ax.dst[i], cdx1 = ax.dst[i + 1];
         const int32_t cdy0 = ay.dst[j], cdy1 = ay.dst[j + 1];
         const int32_t csx0 = ax.src[i], csx1 = ax.src[i + 1];
         const int32_t csy0 = ay.src[j], csy1 = ay.src[j + 1];

         if (cdx0 == cdx1 || cdy0 == cdy1 || csx0 == csx1 || csy0 == csy1)
            continue;

         /* Each cell lies in one destination tile (cuts include the destination
          * boundary) and maps into one source tile (cuts include the source
          * boundary pre-image); pick the tile by the cell's lower corner. */
         const uint32_t dcol = dst->tile_cols == 2 && cdx0 >= dst_bound_x ? 1 : 0;
         const uint32_t drow = dst->tile_rows == 2 && cdy0 >= dst_bound_y ? 1 : 0;
         const uint32_t scol = src->tile_cols == 2 &&
                               MIN2(csx0, csx1) >= src_bound_x ? 1 : 0;
         const uint32_t srow = src->tile_rows == 2 &&
                               MIN2(csy0, csy1) >= src_bound_y ? 1 : 0;

         struct pipe_resource *sres = src->tiles[srow * src->tile_cols + scol];
         struct pipe_resource *dres = dst->tiles[drow * dst->tile_cols + dcol];
         if (!sres || !dres)
            continue;

         if (canonicalize_dxt35) {
            const uint32_t sidx = srow * src->tile_cols + scol;
            if (!canonical_tiles[sidx])
               canonical_tiles[sidx] =
                  r3v_dxt35_canonical_source(device, sres);
            /* A failed scratch (allocation or map) falls back to the stored
             * blocks: color0 <= color1 blocks then take the silicon decode --
             * degraded for those blocks only, never fatal. */
            if (canonical_tiles[sidx])
               sres = canonical_tiles[sidx];
         }

         const int32_t sox = (int32_t)r3v_image_tile_origin_x(src, scol);
         const int32_t soy = (int32_t)r3v_image_tile_origin_y(src, srow);
         const int32_t dox = (int32_t)r3v_image_tile_origin_x(dst, dcol);
         const int32_t doy = (int32_t)r3v_image_tile_origin_y(dst, drow);

         /* The destination cuts are exact, so a cell sits inside its
          * destination tile.  A fractional-ratio source coordinate is rounded,
          * so clamp the source endpoints into the source tile: a 1:1 or
          * integer-ratio blit is unaffected, and a fractional one trims at most
          * the one-texel seam sliver instead of sampling past the tile. */
         const int32_t sxmax = sox + (int32_t)src->tile_width[scol];
         const int32_t symax = soy + (int32_t)src->tile_height[srow];
         const int32_t lsx0 = CLAMP(csx0, sox, sxmax);
         const int32_t lsx1 = CLAMP(csx1, sox, sxmax);
         const int32_t lsy0 = CLAMP(csy0, soy, symax);
         const int32_t lsy1 = CLAMP(csy1, soy, symax);
         if (lsx0 == lsx1 || lsy0 == lsy1)
            continue;

         struct pipe_blit_info info;
         memset(&info, 0, sizeof(info));
         info.src.resource = sres;
         info.dst.resource = dres;
         info.src.format   = src_fmt;
         info.dst.format   = dst_fmt;
         info.mask         = mask;
         info.filter       = filter;
         info.dst.box.x      = cdx0 - dox;
         info.dst.box.width  = cdx1 - cdx0;
         info.dst.box.y      = cdy0 - doy;
         info.dst.box.height = cdy1 - cdy0;
         info.src.box.x      = lsx0 - sox;
         info.src.box.width  = lsx1 - lsx0;
         info.src.box.y      = lsy0 - soy;
         info.src.box.height = lsy1 - lsy0;
         info.src.box.depth  = 1;
         info.dst.box.depth  = 1;

         pipe->blit(pipe, &info);

         if (ncells > 1)
            pipe->flush(pipe, NULL, 0);
      }
   }

   /* The driver holds its own references for commands in flight, so dropping
    * the scratch references after the loop is safe. */
   for (unsigned i = 0; i < 4; i++)
      pipe_resource_reference(&canonical_tiles[i], NULL);
}

static void
r3v_replay_end_render_pass(struct r3v_device *device,
                               bool *skip_render_pass,
                               uint32_t *tile_origin_x,
                               uint32_t *tile_origin_y,
                               uint32_t *tile_width,
                               uint32_t *tile_height)
{
   struct pipe_context *pipe = device->pipe;
   if (*skip_render_pass) {
      *skip_render_pass = false;
      return;
   }
   struct pipe_framebuffer_state empty;
   memset(&empty, 0, sizeof(empty));
   pipe->set_framebuffer_state(pipe, &empty);
   *tile_origin_x = 0;
   *tile_origin_y = 0;
   *tile_width = 0;
   *tile_height = 0;
}

static void
r3v_replay_pipeline_barrier(struct r3v_device *device,
                                const struct r3v_cmd_entry *e,
                                bool skip_render_pass)
{
   struct pipe_context *pipe = device->pipe;
   if (skip_render_pass)
      return;
   pipe->flush(pipe, NULL, 0);
   if (identity_map_debug_enabled()) {
      fprintf(stderr,
              "ident_map: pipeline_barrier flush honored "
              "(dispatch-barrier-dispatch visibility)\n");
   }
   if (e->barrier.image)
      e->barrier.image->resource_state.layout = e->barrier.new_layout;
}

static const uint8_t
r3v_zero_ubo[R3V_VK10_MIN_UNIFORM_BUFFER_RANGE];

/* r300g's set_constant_buffer hook ignores NULL, so a missing descriptor must
 * bind a valid buffer to replace stale CONST[0] state from an earlier draw.  The
 * zero buffer is sized to the advertised Vulkan UBO range; the compiler already
 * rejects offsets the r300 constant file cannot represent. */
static void
r3v_bind_missing_stage_ubo_zero(struct r3v_device *device,
                                   mesa_shader_stage stage)
{
   struct pipe_constant_buffer cb;
   memset(&cb, 0, sizeof(cb));
   cb.user_buffer = r3v_zero_ubo;
   cb.buffer_size = sizeof(r3v_zero_ubo);
   device->pipe->set_constant_buffer(device->pipe, stage, 0, &cb);
}

/* Bind one stage's selected uniform buffer to its r300 constant file at
 * CONST[0], so that stage's load_ubo(0, ...) reads it.  Match the exact
 * (ubo_set, ubo_binding) the shader read: a set may declare several UBO bindings
 * while the shader reads only a later one, and binding the first UBO in layout
 * order would feed CONST[0] the wrong buffer.  The set index in binds->sets[] is
 * relative to binds->first_set, so the absolute Vulkan set number is
 * first_set + s.  r3v buffers are host-visible Gallium PIPE_BUFFERs, so
 * cb.buffer feeds r300_set_constant_buffer directly (it reads malloced_buffer
 * with no GPU upload), matching the compute identity-map path. */
static bool
r3v_try_bind_one_stage_ubo(struct r3v_device *device,
                              const struct r3v_cmd_bind_descriptor_sets *binds,
                              mesa_shader_stage stage,
                              uint32_t ubo_set, uint32_t ubo_binding)
{
   struct pipe_context *pipe = device->pipe;
   for (uint32_t s = 0; s < binds->set_count; s++) {
      if (binds->first_set + s != ubo_set)
         continue;
      const struct r3v_descriptor_set *set = binds->sets[s];
      if (!set || !set->layout)
         continue;
      for (uint32_t b = 0; b < set->layout->binding_count; b++) {
         const struct r3v_dsl_binding *bnd = &set->layout->bindings[b];
         if (bnd->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
             bnd->binding != ubo_binding)
            continue;
         const struct r3v_descriptor *desc = &set->descriptors[bnd->offset];
         VK_FROM_HANDLE(r3v_buffer, buf, desc->buf.buffer);
         if (!buf || !buf->resource)
            return false;

         struct pipe_constant_buffer cb;
         memset(&cb, 0, sizeof(cb));
         cb.buffer        = buf->resource;
         cb.buffer_offset = (unsigned)desc->buf.offset;
         cb.buffer_size   = desc->buf.range == VK_WHOLE_SIZE
                            ? (unsigned)(buf->size - desc->buf.offset)
                            : (unsigned)desc->buf.range;
         if (device->dbg_log_draws && stage == MESA_SHADER_VERTEX) {
            struct pipe_transfer *cx = NULL;
            const float *cf = pipe_buffer_map_range(pipe, buf->resource,
                                                    cb.buffer_offset,
                                                    MIN2(cb.buffer_size, 32),
                                                    PIPE_MAP_READ, &cx);
            if (cf) {
               fprintf(stderr, "r3v vs-ubo: off=%u size=%u "
                       "c0=[%g %g %g %g][%g %g %g %g]\n",
                       cb.buffer_offset, cb.buffer_size,
                       cf[0], cf[1], cf[2], cf[3],
                       cf[4], cf[5], cf[6], cf[7]);
               pipe_buffer_unmap(pipe, cx);
            }
         }
         pipe->set_constant_buffer(pipe, stage, 0, &cb);
         return true;
      }
   }

   return false;
}

static void
r3v_bind_one_stage_ubo(struct r3v_device *device,
                          const struct r3v_cmd_bind_descriptor_sets *binds,
                          mesa_shader_stage stage,
                          uint32_t ubo_set, uint32_t ubo_binding)
{
   if (!r3v_try_bind_one_stage_ubo(device, binds, stage,
                                      ubo_set, ubo_binding))
      r3v_bind_missing_stage_ubo_zero(device, stage);
}

/* r300 has separate vertex and fragment constant files, so bind each stage's
 * selected UBO independently.  The two stages may read different bindings -- even
 * two bindings of the same buffer (dEQP-VK.ubo.link_by_binding) -- so neither is
 * forced onto the other.  A stage that reads no UBO binds nothing. */
static void
r3v_bind_descriptor_ubo(struct r3v_device *device,
                           const struct r3v_pipeline *pipeline,
                           const struct r3v_cmd_bind_descriptor_sets *binds)
{
   if (!binds || !pipeline)
      return;
   if (pipeline->vs_has_ubo)
      r3v_bind_one_stage_ubo(device, binds, MESA_SHADER_VERTEX,
                                pipeline->vs_ubo_set, pipeline->vs_ubo_binding);
   if (pipeline->fs_has_ubo)
      r3v_bind_one_stage_ubo(device, binds, MESA_SHADER_FRAGMENT,
                                pipeline->fs_ubo_set, pipeline->fs_ubo_binding);
}

/* Bind the running push-constant window at CONST[0] for both stages -- the slot a
 * push-constants-only pipeline's lowered load_ubo(0, ...) reads.
 * r300_set_constant_buffer reads cb.user_buffer directly, so the bytes need no
 * GPU upload.
 *
 * r300's constant file is float-only: nir_lower_int_to_float represents an
 * integer by its float value, so each push-constant word the shader reads as an
 * integer (int_word_mask, classified at compile) is converted from its raw int
 * bits to (float)value here.  The conversion is exact for |value| < 2^24, the
 * FP24 integer-exact envelope; int and uint coincide in that range.  Convert
 * into the caller's scratch (valid through draw_vbo) so the recorded window is
 * left intact for the next draw. */
static void
r3v_bind_push_constants(struct r3v_device *device, const uint8_t *data,
                           uint32_t int_word_mask, uint8_t *scratch)
{
   struct pipe_context *pipe = device->pipe;
   const uint8_t *bind_data = data;
   if (int_word_mask) {
      memcpy(scratch, data, R3V_MAX_PUSH_CONSTANTS_SIZE);
      for (unsigned w = 0; w < 32; w++) {
         if (!(int_word_mask & (1u << w)))
            continue;
         int32_t iv;
         memcpy(&iv, scratch + w * 4, sizeof(iv));
         float fv = (float)iv;
         memcpy(scratch + w * 4, &fv, sizeof(fv));
      }
      bind_data = scratch;
   }
   struct pipe_constant_buffer cb;
   memset(&cb, 0, sizeof(cb));
   cb.user_buffer = bind_data;
   cb.buffer_size = R3V_MAX_PUSH_CONSTANTS_SIZE;
   pipe->set_constant_buffer(pipe, MESA_SHADER_VERTEX, 0, &cb);
   pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);
}

/* R3V_MAX_FS_SAMPLER_UNITS (the fixed fragment texture-unit count) is shared
 * with the pipeline via r3v_private.h, which sizes the per-draw bookkeeping
 * arrays here and the (set,binding)->unit map there from the same bound. */

/* Map one resolved VkComponentSwizzle to a Gallium PIPE_SWIZZLE.  vk_image_view
 * pre-resolves VK_COMPONENT_SWIZZLE_IDENTITY to the explicit R/G/B/A, so only the
 * concrete cases appear here; the image view's component remap must reach the
 * sampler or a swizzled view (e.g. an R8 sampled as rrr1) would read the wrong
 * channels. */
static unsigned
vk_swizzle_to_pipe(VkComponentSwizzle s)
{
   switch (s) {
   case VK_COMPONENT_SWIZZLE_G:    return PIPE_SWIZZLE_Y;
   case VK_COMPONENT_SWIZZLE_B:    return PIPE_SWIZZLE_Z;
   case VK_COMPONENT_SWIZZLE_A:    return PIPE_SWIZZLE_W;
   case VK_COMPONENT_SWIZZLE_ZERO: return PIPE_SWIZZLE_0;
   case VK_COMPONENT_SWIZZLE_ONE:  return PIPE_SWIZZLE_1;
   case VK_COMPONENT_SWIZZLE_R:
   default:                        return PIPE_SWIZZLE_X;
   }
}

/* The transient fragment sampler views bound for one draw, recorded so the same
 * draw can release them after draw_vbo returns. */
struct r3v_bound_textures {
   struct pipe_sampler_view *views[R3V_MAX_FS_SAMPLER_UNITS];
   unsigned                  units[R3V_MAX_FS_SAMPLER_UNITS];
   unsigned                  count;
};

/* Build a combined-image-sampler view over a specific backing resource using the
 * descriptor's format, swizzle, mip range, and sampler CSO.  The single-resource
 * bind passes the whole-image resource; the tile-stitch path passes one tile
 * resource of a split image.  Built as PIPE_TEXTURE_2D, so only a plain 2D view
 * is correct. */
static struct pipe_sampler_view *
r3v_create_resource_sampler_view(struct pipe_context *pipe,
                                    const struct r3v_descriptor *desc,
                                    struct pipe_resource *res,
                                    void **sampler_state)
{
   *sampler_state = NULL;
   if (desc->type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || !res)
      return NULL;

   VK_FROM_HANDLE(r3v_image_view, iv, desc->img.image_view);
   if (!iv || !iv->vk.image || iv->vk.view_type != VK_IMAGE_VIEW_TYPE_2D)
      return NULL;

   /* r300 has no stencil texture format: a stencil-aspect view keeps the parent
    * depth/stencil VkFormat (which the per-format query reports sampled for its
    * depth aspect), but r300_create_sampler_view has no hardware mapping for the
    * stencil aspect and asserts hwformat != ~0.  The depth aspect samples fine,
    * so leave only a stencil-aspect (or pure-stencil) view unbound rather than
    * aborting the process. */
   if (iv->vk.aspects == VK_IMAGE_ASPECT_STENCIL_BIT)
      return NULL;

   struct vk_sampler *vks = vk_sampler_from_handle(desc->img.sampler);
   void *samp_cso = vks ? r3v_sampler_from_vk(vks)->pipe_cso : NULL;
   if (!samp_cso)
      return NULL;

   const enum pipe_format fmt =
      r3v_vk_format_to_pipe_format(iv->vk.view_format);
   if (fmt == PIPE_FORMAT_NONE)
      return NULL;

   struct pipe_sampler_view sv_templ;
   memset(&sv_templ, 0, sizeof(sv_templ));
   sv_templ.format    = fmt;
   sv_templ.target    = PIPE_TEXTURE_2D;
   sv_templ.swizzle_r = vk_swizzle_to_pipe(iv->vk.swizzle.r);
   sv_templ.swizzle_g = vk_swizzle_to_pipe(iv->vk.swizzle.g);
   sv_templ.swizzle_b = vk_swizzle_to_pipe(iv->vk.swizzle.b);
   sv_templ.swizzle_a = vk_swizzle_to_pipe(iv->vk.swizzle.a);
   sv_templ.u.tex.first_level = iv->vk.base_mip_level;
   sv_templ.u.tex.last_level  = iv->vk.base_mip_level + iv->vk.level_count - 1;

   struct pipe_sampler_view *view =
      pipe->create_sampler_view(pipe, res, &sv_templ);
   if (view)
      *sampler_state = samp_cso;
   return view;
}

/* The single-resource path samples the whole image (tile 0).  A split image
 * (tile_cols/tile_rows > 1) wraps past the 2048 sampler cap as one resource and
 * gives wrong texels outside one tile, so it is left unbound here unless the
 * experimental tile-stitch path handles it per tile. */
static struct pipe_sampler_view *
r3v_create_descriptor_sampler_view(struct pipe_context *pipe,
                                      const struct r3v_descriptor *desc,
                                      void **sampler_state)
{
   *sampler_state = NULL;
   if (desc->type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
      return NULL;
   VK_FROM_HANDLE(r3v_image_view, iv, desc->img.image_view);
   if (!iv || !iv->vk.image)
      return NULL;
   struct r3v_image *img =
      container_of(iv->vk.image, struct r3v_image, vk);
   if (img->tile_cols > 1 || img->tile_rows > 1 || !img->resource)
      return NULL;
   return r3v_create_resource_sampler_view(pipe, desc, img->resource,
                                              sampler_state);
}

/* Bind the four tile views of a stitched combined-image-sampler to base_unit+0..3
 * and write the per-image affine/split geometry the NIR stitch pass reads from
 * CONST[0]: cu = {W/w0, W/w1, -w0/w1, w0/W}, cv likewise for the rows.  A single-
 * tile or single-axis image fills the missing tiles with the nearest existing
 * tile and sets that axis' threshold to 2.0 so the select collapses to it. */
static void
r3v_stitch_bind_one(struct r3v_device *device,
                       const struct r3v_descriptor *desc, unsigned base_unit,
                       struct pipe_sampler_view **views, void **sampler_states,
                       unsigned *max_unit_plus_one,
                       struct r3v_bound_textures *bound, float geom[8])
{
   struct pipe_context *pipe = device->pipe;
   VK_FROM_HANDLE(r3v_image_view, iv, desc->img.image_view);
   struct r3v_image *img =
      (iv && iv->vk.image) ? container_of(iv->vk.image, struct r3v_image, vk)
                           : NULL;
   if (!img || !img->tile_cols || !img->tile_rows)
      return;

   const float W = (float)(img->tile_width[0] +
                           (img->tile_cols > 1 ? img->tile_width[1] : 0));
   const float H = (float)(img->tile_height[0] +
                           (img->tile_rows > 1 ? img->tile_height[1] : 0));
   const bool multitile = (img->tile_cols > 1 || img->tile_rows > 1);

   struct vk_sampler *vks = vk_sampler_from_handle(desc->img.sampler);
   const struct r3v_sampler *s =
      vks ? r3v_sampler_from_vk(vks) : NULL;

   /* Pick the charts and the per-chart affine/select geometry.  A single-tile
    * image collapses to tile 0 (threshold 2.0) for any sampler.  A split image
    * needs an eligible sampler: a NEAREST point sample uses the disjoint render
    * tiles directly; a LINEAR sample uses the overlapped halo atlas (decision at
    * the logical centre, 0.5) whose duplicated seam keeps the bilinear footprint
    * in one chart; an ineligible sampler is refused (left unbound). */
   struct pipe_resource *const *src = img->tiles;
   unsigned acols = img->tile_cols, arows = img->tile_rows;

   if (multitile && s && s->linear_stitch_eligible) {
      if (!r3v_image_ensure_sampler_atlas(device, img))
         return;
      src = img->sampler_atlas.tiles;
      acols = img->sampler_atlas.cols;
      arows = img->sampler_atlas.rows;
      geom[0] = W / (float)img->sampler_atlas.width[0];
      geom[1] = (acols > 1) ? W / (float)img->sampler_atlas.width[1] : 1.0f;
      geom[2] = (acols > 1)
                ? -((float)img->sampler_atlas.origin_x[1] /
                    (float)img->sampler_atlas.width[1]) : 0.0f;
      geom[3] = (acols > 1) ? 0.5f : 2.0f;
      geom[4] = H / (float)img->sampler_atlas.height[0];
      geom[5] = (arows > 1) ? H / (float)img->sampler_atlas.height[1] : 1.0f;
      geom[6] = (arows > 1)
                ? -((float)img->sampler_atlas.origin_y[1] /
                    (float)img->sampler_atlas.height[1]) : 0.0f;
      geom[7] = (arows > 1) ? 0.5f : 2.0f;
   } else if (multitile && (!s || !s->nearest_stitch_eligible)) {
      return;  /* split image + a sampler the stitch cannot honour exactly */
   } else {
      /* single-tile, or split image + NEAREST: disjoint render-tile partition */
      const float w0 = (float)img->tile_width[0];
      const float w1 = (img->tile_cols > 1) ? (float)img->tile_width[1] : 0.0f;
      const float h0 = (float)img->tile_height[0];
      const float h1 = (img->tile_rows > 1) ? (float)img->tile_height[1] : 0.0f;
      geom[0] = W / w0;
      geom[1] = (img->tile_cols > 1) ? W / w1 : 1.0f;
      geom[2] = (img->tile_cols > 1) ? -(w0 / w1) : 0.0f;
      geom[3] = (img->tile_cols > 1) ? (w0 / W) : 2.0f;
      geom[4] = H / h0;
      geom[5] = (img->tile_rows > 1) ? H / h1 : 1.0f;
      geom[6] = (img->tile_rows > 1) ? -(h0 / h1) : 0.0f;
      geom[7] = (img->tile_rows > 1) ? (h0 / H) : 2.0f;
   }

   for (unsigned ti = 0; ti < R3V_NEAREST_STITCH_TILE_UNITS; ti++) {
      const unsigned unit = base_unit + ti;
      if (unit >= R3V_MAX_FS_SAMPLER_UNITS ||
          bound->count >= R3V_MAX_FS_SAMPLER_UNITS)
         break;
      const unsigned trow = MIN2(ti / 2, arows - 1);
      const unsigned tcol = MIN2(ti % 2, acols - 1);
      struct pipe_resource *res = src[trow * acols + tcol];
      void *samp_cso = NULL;
      struct pipe_sampler_view *view =
         r3v_create_resource_sampler_view(pipe, desc, res, &samp_cso);
      if (!view || views[unit]) {
         if (view)
            pipe_sampler_view_reference(&view, NULL);
         continue;
      }
      sampler_states[unit] = samp_cso;
      views[unit] = view;
      *max_unit_plus_one = MAX2(*max_unit_plus_one, unit + 1);
      bound->units[bound->count] = unit;
      bound->views[bound->count] = view;
      bound->count++;
   }
}

/* Bind every fragment combined-image-sampler in descriptor set 0 to its Gallium
 * texture unit, so an app fragment shader's texture()/texelFetch reads the
 * descriptor's image instead of the unbound-sampler default.  nir_lower_samplers
 * (run inside r300g's nir_to_rc) assigns each sampler the Gallium unit
 * deref->var->data.binding plus a constant array index, so the unit is
 * descriptor binding + array element.  r300g's sampler callbacks require
 * updates to start at unit zero, so this replay gathers all touched units and
 * commits one contiguous [0, max_unit] sampler array.
 *
 * Descriptor sets above zero are skipped until r3v lowers sampler variables
 * onto a flattened set+binding namespace.  Binding them by binding number alone
 * would alias set 1 binding 0 over set 0 binding 0 and render the wrong image.
 * Only the fragment stage is bound: the RS480 SW-TCL vertex path has no sampler
 * and a vertex texture fetch is rejected at pipeline compile.
 *
 * Single-tile images only.  An image wider than the 2048 sampler cap is split
 * into tiles[] (the oversize-blit partition), and a sampler view over either the
 * whole resource (TX_WIDTH wraps past 2048) or one tile (wrong texels outside it)
 * would sample garbage; that case needs the per-tile render composition and is
 * left unbound here, preserving its prior unsampled behavior rather than
 * sampling wrong.  Records the bound (unit, view) pairs for the caller to release. */
static void
r3v_bind_descriptor_textures(struct r3v_device *device,
                                const struct r3v_pipeline *pipeline,
                                const struct r3v_cmd_bind_descriptor_sets *binds,
                                struct r3v_bound_textures *bound,
                                float *stitch_geom)
{
   struct pipe_context *pipe = device->pipe;
   bound->count = 0;
   if (!binds || !pipeline)
      return;

   void *sampler_states[R3V_MAX_FS_SAMPLER_UNITS] = {0};
   struct pipe_sampler_view *views[R3V_MAX_FS_SAMPLER_UNITS] = {0};
   unsigned max_unit_plus_one = 0;
   bool stitched = false;

   for (uint32_t s = 0; s < binds->set_count; s++) {
      const uint32_t descriptor_set = binds->first_set + s;
      const struct r3v_descriptor_set *set = binds->sets[s];
      if (!set || !set->layout)
         continue;
      for (uint32_t b = 0; b < set->layout->binding_count; b++) {
         const struct r3v_dsl_binding *bnd = &set->layout->bindings[b];
         if (bnd->type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            continue;

         /* The flat unit the pipeline assigned this (set, binding) is the unit the
          * fragment shader samples (r3v_nir_remap_sampler_units rewrote the
          * sampler binding to it).  A binding absent from the map is one the
          * pipeline does not sample; skip it. */
         int base_unit = -1;
         for (uint16_t m = 0; m < pipeline->fs_sampler_map_count; m++) {
            if (pipeline->fs_sampler_map[m].set == descriptor_set &&
                pipeline->fs_sampler_map[m].binding == bnd->binding) {
               base_unit = pipeline->fs_sampler_map[m].unit;
               break;
            }
         }
         if (base_unit < 0)
            continue;

         /* Under the stitch gate this sampler reserved a 2x2 tile-unit grid; bind
          * the four tile views and fill the per-image geometry rather than one
          * whole-image view (which would wrap past the 2048 sampler cap). */
         if (pipeline->fs_nearest_stitch && stitch_geom) {
            r3v_stitch_bind_one(device, &set->descriptors[bnd->offset], base_unit,
                                   views, sampler_states, &max_unit_plus_one,
                                   bound, stitch_geom);
            stitched = true;
            continue;
         }

         const uint32_t count =
            MIN2(bnd->count, R3V_MAX_FS_SAMPLER_UNITS - base_unit);
         for (uint32_t elem = 0; elem < count; elem++) {
            const unsigned unit = base_unit + elem;
            if (bound->count >= R3V_MAX_FS_SAMPLER_UNITS)
               break;

            const struct r3v_descriptor *desc =
               &set->descriptors[bnd->offset + elem];
            void *samp_cso = NULL;
            struct pipe_sampler_view *view =
               r3v_create_descriptor_sampler_view(pipe, desc, &samp_cso);
            if (!view)
               continue;

            if (views[unit]) {
               pipe_sampler_view_reference(&view, NULL);
               continue;
            }

            sampler_states[unit] = samp_cso;
            views[unit] = view;
            max_unit_plus_one = MAX2(max_unit_plus_one, unit + 1);

            bound->units[bound->count] = unit;
            bound->views[bound->count] = view;
            bound->count++;
         }
      }
   }

   if (max_unit_plus_one) {
      pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0,
                                max_unit_plus_one, sampler_states);
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, max_unit_plus_one,
                              0, views);
   }

   /* Bind the per-image tile geometry the NIR stitch pass reads from fragment
    * CONST[0].  The stitch + UBO/push/subpass collision is rejected at create, so
    * CONST[0] is free; stitch_geom outlives the draw_vbo in the caller's frame. */
   if (stitched) {
      struct pipe_constant_buffer cb;
      memset(&cb, 0, sizeof(cb));
      cb.user_buffer = stitch_geom;
      cb.buffer_size = R3V_NEAREST_STITCH_CONST_VEC4S * 16;
      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);
   }
}

/* Snapshot the attachment a self-dependent subpass both writes and reads, so
 * the input read samples the copy while the RB3D/ZB pipes keep writing the
 * real surface -- the TX unit never samples the live render target.  The
 * device-cached snapshot is keyed to one attachment tile resource and
 * re-copied while ia_snapshot_stale is raised (render pass begin,
 * next-subpass, and each in-pass pipeline barrier raise it), so reads observe
 * exactly the writes made visible by the last barrier -- the Vulkan
 * self-dependency contract.  Returns NULL when the snapshot cannot be
 * created; the caller skips the draw. */
static struct pipe_resource *
r3v_ia_self_dep_snapshot(struct r3v_device *device,
                            struct pipe_resource *src)
{
   struct pipe_context *pipe = device->pipe;

   if (device->ia_snapshot_src != src || !device->ia_snapshot) {
      pipe_resource_reference(&device->ia_snapshot, NULL);
      pipe_resource_reference(&device->ia_snapshot_src, NULL);

      struct pipe_resource tmpl;
      memset(&tmpl, 0, sizeof(tmpl));
      tmpl.target     = src->target;
      tmpl.format     = src->format;
      tmpl.width0     = src->width0;
      tmpl.height0    = src->height0;
      tmpl.depth0     = 1;
      tmpl.array_size = 1;
      tmpl.usage      = PIPE_USAGE_DEFAULT;
      tmpl.bind       = PIPE_BIND_SAMPLER_VIEW |
                        (util_format_is_depth_or_stencil(src->format) ?
                            PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET);

      device->ia_snapshot =
         pipe->screen->resource_create(pipe->screen, &tmpl);
      if (!device->ia_snapshot)
         return NULL;
      pipe_resource_reference(&device->ia_snapshot_src, src);
      device->ia_snapshot_stale = true;
   }

   if (device->ia_snapshot_stale) {
      /* Resolve pending color/Z writes before the copy samples src -- the
       * same render-to-texture barrier the subpass boundary uses
       * (r300_texture_barrier flushes the RB3D cache and invalidates stale
       * texture lines before the blitter's copy draw). */
      pipe->texture_barrier(pipe, PIPE_TEXTURE_BARRIER_SAMPLER);

      struct pipe_box box;
      u_box_2d(0, 0, src->width0, src->height0, &box);
      pipe->resource_copy_region(pipe, device->ia_snapshot, 0, 0, 0, 0,
                                 src, 0, &box);
      device->ia_snapshot_stale = false;
   }

   return device->ia_snapshot;
}

/* Bind the single input attachment the FS reads via the lowered subpassLoad.
 *
 * The NIR pass r3v_nir_lower_subpass_input rewrites subpassLoad into a
 * normalized texture() read: tex_coord = gl_FragCoord.xy * inv_extent.  Replay
 * translates the viewport into tile-local coordinates, so split images bind the
 * matching tile resource and use that tile's {1/W, 1/H, 0, 0} in FS CONST[0].
 * This function supplies both the sampler view and the constant.
 *
 * inv_extent must outlive the draw_vbo call; pass a float[4] allocated in the
 * caller's frame (r3v_replay_draw), which matches the lifetime rule in
 * r3v_multitap_gather_dispatch_replay: "the float4 is consumed before this
 * stack frame unwinds at the draw below."
 *
 * The collision check at pipeline compile (r3v_compile_shader) ensures the
 * FS cannot combine subpassInput with UBO or push_const, so FS CONST[0] is
 * always free for inv_extent here.  A VS using push_const is still valid --
 * r3v_bind_push_constants set VS+FS CONST[0]; this call overrides FS CONST[0]
 * only.  The identity sampler (NEAREST, CLAMP_TO_EDGE, normalized coords) is the
 * correct choice because the lowered coordinate is already in [0,1].  Identity
 * swizzle (XYZW) is used because subpassLoad returns the raw attachment value
 * without component remapping (Vulkan spec, section "Input Attachment Reads"). */
static bool
r3v_bind_input_attachment(struct r3v_device *device,
                             const struct r3v_pipeline *pipeline,
                             const struct r3v_cmd_bind_descriptor_sets *binds,
                             struct r3v_bound_textures *bound,
                             uint32_t tile_origin_x,
                             uint32_t tile_origin_y,
                             bool input_self_dep,
                             float *inv_extent)
{
   struct pipe_context *pipe = device->pipe;
   if (!binds || !pipeline)
      return false;

   for (uint32_t s = 0; s < binds->set_count; s++) {
      const uint32_t descriptor_set = binds->first_set + s;
      if (descriptor_set != pipeline->fs_input_attachment_set)
         continue;
      const struct r3v_descriptor_set *set = binds->sets[s];
      if (!set || !set->layout)
         continue;
      for (uint32_t b = 0; b < set->layout->binding_count; b++) {
         const struct r3v_dsl_binding *bnd = &set->layout->bindings[b];
         if (bnd->type != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ||
             bnd->binding != pipeline->fs_input_attachment_binding)
            continue;

         const unsigned unit = R3V_INPUT_ATTACHMENT_SAMPLER_UNIT;
         if (bound->count >= R3V_MAX_FS_SAMPLER_UNITS)
            return false;

         const struct r3v_descriptor *desc = &set->descriptors[bnd->offset];
         VK_FROM_HANDLE(r3v_image_view, iv, desc->img.image_view);
         if (!iv || !iv->vk.image)
            return false;
         if (iv->vk.view_type != VK_IMAGE_VIEW_TYPE_2D)
            return false;

         struct r3v_image *img =
            container_of(iv->vk.image, struct r3v_image, vk);
         if (!img->resource)
            return false;

         uint32_t input_tile_origin_x = 0;
         uint32_t input_tile_origin_y = 0;
         uint32_t input_width = 0;
         uint32_t input_height = 0;
         struct pipe_resource *input_resource =
            r3v_image_tile_resource_for_origin(img, tile_origin_x,
                                                  tile_origin_y,
                                                  &input_tile_origin_x,
                                                  &input_tile_origin_y,
                                                  &input_width,
                                                  &input_height);
         if (!input_resource || input_width == 0 || input_height == 0)
            return false;

         /* Self-dependent subpass: the attachment is also this subpass's
          * render target, so sample its snapshot instead. */
         if (input_self_dep) {
            input_resource =
               r3v_ia_self_dep_snapshot(device, input_resource);
            if (!input_resource)
               return false;
         }

         const enum pipe_format fmt =
            r3v_vk_format_to_pipe_format(iv->vk.view_format);
         if (fmt == PIPE_FORMAT_NONE)
            return false;

         struct pipe_sampler_view sv_templ;
         memset(&sv_templ, 0, sizeof(sv_templ));
         sv_templ.format    = fmt;
         sv_templ.target    = PIPE_TEXTURE_2D;
         sv_templ.swizzle_r = PIPE_SWIZZLE_X;
         sv_templ.swizzle_g = PIPE_SWIZZLE_Y;
         sv_templ.swizzle_b = PIPE_SWIZZLE_Z;
         sv_templ.swizzle_a = PIPE_SWIZZLE_W;
         struct pipe_sampler_view *view =
            pipe->create_sampler_view(pipe, input_resource, &sv_templ);
         if (!view)
            return false;

         inv_extent[0] = 1.0f / (float)input_width;
         inv_extent[1] = 1.0f / (float)input_height;
         inv_extent[2] = 0.0f;
         inv_extent[3] = 0.0f;

         struct pipe_constant_buffer cb;
         memset(&cb, 0, sizeof(cb));
         cb.user_buffer = inv_extent;
         cb.buffer_size = 4 * sizeof(float);
         pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);

         pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, unit, 1,
                                   &device->identity_map_cso.sampler);
         pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, unit, 1, 0, &view);

         bound->units[bound->count] = unit;
         bound->views[bound->count] = view;
         bound->count++;
         return true;
      }
   }

   /* The pipeline declares an input attachment (the caller gated on
    * fs_has_input_attachment) but the descriptor set supplied no matching bound
    * view -- or a bind step above failed.  Return false so the caller skips the
    * draw rather than letting the fragment shader sample sampler unit 0's
    * stale/undefined view and render garbage.  Unreachable under valid usage. */
   return false;
}

/* Release the transient sampler views bound for one draw.  Unbind each unit
 * first so the pipe_context drops its internal reference before the view's last
 * reference is dropped -- the identity-map teardown order. */
static void
r3v_unbind_descriptor_textures(struct r3v_device *device,
                                  struct r3v_bound_textures *bound)
{
   struct pipe_context *pipe = device->pipe;
   if (!bound->count)
      return;

   void *sampler_states[R3V_MAX_FS_SAMPLER_UNITS] = {0};
   struct pipe_sampler_view *views[R3V_MAX_FS_SAMPLER_UNITS] = {0};
   unsigned max_unit_plus_one = 0;
   for (unsigned i = 0; i < bound->count; i++)
      max_unit_plus_one = MAX2(max_unit_plus_one, bound->units[i] + 1);

   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, max_unit_plus_one,
                             sampler_states);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0,
                           max_unit_plus_one, views);

   for (unsigned i = 0; i < bound->count; i++)
      pipe_sampler_view_reference(&bound->views[i], NULL);
   bound->count = 0;
}

/* The replay-side dynamic-state overlay.  vkCmdSet* calls are recorded as
 * R3V_CMD_SET_DYNAMIC_STATE entries; the walker merges them in stream
 * order into this shadow and, at each draw, overlays the bound pipeline's
 * rs/dsa templates with the fields that are BOTH set in the command buffer
 * and declared dynamic by that pipeline (Vulkan's static-else-dynamic rule).
 * Stencil per-face fields keep separate front/back values because successive
 * vkCmdSetStencil* calls may target either face mask. */
struct r3v_dyn_face {
   uint32_t    set;          /* R3V_DYN_STENCIL_* bits set on this face */
   VkStencilOp sfail, spass, sdepth_fail;
   VkCompareOp scompare;
   uint32_t    cmp_mask, wr_mask, ref;
};

struct r3v_dyn_overlay {
   uint32_t            flags;  /* accumulated R3V_DYN_* set bits */
   VkCullModeFlags     cull;
   VkFrontFace         front_face;
   VkPrimitiveTopology topology;
   VkBool32            depth_test, depth_write;
   VkCompareOp         depth_op;
   VkBool32            stencil_test;
   struct r3v_dyn_face face[2];   /* 0 = front, 1 = back */
   float               bias_const, bias_clamp, bias_slope;
   VkBool32            bias_enable;
   float               blend_const[4];
   float               line_width;
   uint32_t            stipple_factor;
   uint16_t            stipple_pattern;
   /* Transient CSOs currently bound in place of the pipeline's fixed ones;
    * deleted only after the replacement is bound (create-bind-delete-old). */
   void               *rs_cso;
   void               *dsa_cso;
   void               *velems_cso;
   bool                dirty;
};

#define R3V_DYN_RS_BITS (R3V_DYN_CULL | R3V_DYN_FRONT_FACE | \
                            R3V_DYN_LINE_WIDTH | R3V_DYN_DEPTH_BIAS | \
                            R3V_DYN_DEPTH_BIAS_EN | R3V_DYN_LINE_STIPPLE)
#define R3V_DYN_DSA_BITS (R3V_DYN_DEPTH_TEST | R3V_DYN_DEPTH_WRITE | \
                             R3V_DYN_DEPTH_OP | R3V_DYN_STENCIL_TEST | \
                             R3V_DYN_STENCIL_OP | \
                             R3V_DYN_STENCIL_CMP_MASK | \
                             R3V_DYN_STENCIL_WR_MASK)

static void
r3v_dyn_overlay_merge(struct r3v_dyn_overlay *ov,
                         const struct r3v_cmd_set_dynamic *d)
{
   const uint32_t f = d->flags;
   ov->flags |= f;
   if (f & R3V_DYN_CULL)         ov->cull = d->cull;
   if (f & R3V_DYN_FRONT_FACE)   ov->front_face = d->front;
   if (f & R3V_DYN_TOPOLOGY)     ov->topology = d->topology;
   if (f & R3V_DYN_DEPTH_TEST)   ov->depth_test = d->depth_test;
   if (f & R3V_DYN_DEPTH_WRITE)  ov->depth_write = d->depth_write;
   if (f & R3V_DYN_DEPTH_OP)     ov->depth_op = d->depth_op;
   if (f & R3V_DYN_STENCIL_TEST) ov->stencil_test = d->stencil_test;
   if (f & R3V_DYN_DEPTH_BIAS) {
      ov->bias_const = d->bias_const;
      ov->bias_clamp = d->bias_clamp;
      ov->bias_slope = d->bias_slope;
   }
   if (f & R3V_DYN_DEPTH_BIAS_EN) ov->bias_enable = d->bias_enable;
   if (f & R3V_DYN_BLEND_CONST)
      memcpy(ov->blend_const, d->blend_const, sizeof(ov->blend_const));
   if (f & R3V_DYN_LINE_WIDTH)   ov->line_width = d->line_width;
   if (f & R3V_DYN_LINE_STIPPLE) {
      ov->stipple_factor  = d->stipple_factor;
      ov->stipple_pattern = d->stipple_pattern;
   }

   const uint32_t face_bits = f & (R3V_DYN_STENCIL_OP |
                                   R3V_DYN_STENCIL_CMP_MASK |
                                   R3V_DYN_STENCIL_WR_MASK |
                                   R3V_DYN_STENCIL_REF);
   if (face_bits) {
      for (unsigned i = 0; i < 2; i++) {
         const VkStencilFaceFlags want =
            i == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT;
         if (!(d->face_mask & want))
            continue;
         struct r3v_dyn_face *fc = &ov->face[i];
         fc->set |= face_bits;
         if (f & R3V_DYN_STENCIL_OP) {
            fc->sfail = d->sfail;
            fc->spass = d->spass;
            fc->sdepth_fail = d->sdepth_fail;
            fc->scompare = d->scompare;
         }
         if (f & R3V_DYN_STENCIL_CMP_MASK) fc->cmp_mask = d->cmp_mask;
         if (f & R3V_DYN_STENCIL_WR_MASK)  fc->wr_mask = d->wr_mask;
         if (f & R3V_DYN_STENCIL_REF)      fc->ref = d->ref;
      }
   }
   ov->dirty = true;
}

/* Bind the draw's effective rasterizer/DSA state and the always-set-state
 * pair (stencil ref, blend colour).  Transient CSOs replace the pipeline's
 * fixed ones only when an effective dynamic field differs the template;
 * the old transient is deleted after its replacement is bound, and r300g's
 * delete_rs/dsa_state unbind-if-bound covers the final cleanup. */
static void
r3v_dyn_overlay_apply(struct r3v_device *device,
                         const struct r3v_pipeline *pl,
                         struct r3v_dyn_overlay *ov,
                         bool has_zs,
                         bool has_stencil)
{
   if (!ov->dirty || !pl)
      return;
   if (device->dbg_no_dyn_overlay) {
      struct pipe_context *dbg_pipe = device->pipe;
      dbg_pipe->bind_rasterizer_state(dbg_pipe, pl->rasterizer_cso);
      dbg_pipe->bind_depth_stencil_alpha_state(dbg_pipe, pl->dsa_cso);
      ov->dirty = false;
      return;
   }

   struct pipe_context *pipe = device->pipe;
   const uint32_t eff = ov->flags & pl->dyn_mask;

   void *new_rs_cso = NULL;
   if (eff & R3V_DYN_RS_BITS) {
      struct pipe_rasterizer_state rs = pl->rs_template;
      if (eff & R3V_DYN_CULL)
         rs.cull_face = r3v_cull_mode_to_pipe(ov->cull);
      if (eff & R3V_DYN_FRONT_FACE)
         rs.front_ccw = ov->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE;
      if (eff & R3V_DYN_LINE_WIDTH)
         rs.line_width = ov->line_width != 0.0f ? ov->line_width : 1.0f;
      /* Stipple enable is pipeline-static; the dynamic state carries only
       * factor and pattern. */
      if ((eff & R3V_DYN_LINE_STIPPLE) && rs.line_stipple_enable) {
         rs.line_stipple_factor  = ov->stipple_factor
                                   ? ov->stipple_factor - 1 : 0;
         rs.line_stipple_pattern = ov->stipple_pattern;
      }
      const bool bias = (eff & R3V_DYN_DEPTH_BIAS_EN)
                        ? (bool)ov->bias_enable : pl->rs_template.offset_tri;
      rs.offset_tri = rs.offset_line = rs.offset_point = bias;
      if (eff & R3V_DYN_DEPTH_BIAS) {
         rs.offset_units = ov->bias_const;
         rs.offset_scale = ov->bias_slope;
         rs.offset_clamp = ov->bias_clamp;
      }
      new_rs_cso = pipe->create_rasterizer_state(pipe, &rs);
   }
   pipe->bind_rasterizer_state(pipe, new_rs_cso ? new_rs_cso
                                                : pl->rasterizer_cso);
   if (ov->rs_cso)
      pipe->delete_rasterizer_state(pipe, ov->rs_cso);
   ov->rs_cso = new_rs_cso;

   /* A pass without a depth/stencil attachment runs with both tests
    * disabled: Vulkan's no-attachment semantics, and the only defined r300g
    * behaviour since no zsbuf is bound (depth-testing against nothing would
    * kill every fragment). */
   const bool template_uses_zs = pl->dsa_template.depth_enabled ||
                                 pl->dsa_template.stencil[0].enabled;
   const bool template_uses_stencil = pl->dsa_template.stencil[0].enabled ||
                                      pl->dsa_template.stencil[1].enabled;
   void *new_dsa_cso = NULL;
   if ((eff & R3V_DYN_DSA_BITS) || (!has_zs && template_uses_zs) ||
       (!has_stencil && template_uses_stencil)) {
      struct pipe_depth_stencil_alpha_state dsa = pl->dsa_template;
      if (eff & R3V_DYN_DEPTH_TEST)
         dsa.depth_enabled = ov->depth_test;
      if (eff & R3V_DYN_DEPTH_WRITE)
         dsa.depth_writemask = ov->depth_write;
      if (eff & R3V_DYN_DEPTH_OP)
         dsa.depth_func = r3v_compare_op_to_pipe(ov->depth_op);
      for (unsigned i = 0; i < 2; i++) {
         struct pipe_stencil_state *st = &dsa.stencil[i];
         const struct r3v_dyn_face *fc = &ov->face[i];
         if (eff & R3V_DYN_STENCIL_TEST)
            st->enabled = ov->stencil_test;
         if ((eff & R3V_DYN_STENCIL_OP) &&
             (fc->set & R3V_DYN_STENCIL_OP)) {
            st->func     = r3v_compare_op_to_pipe(fc->scompare);
            st->fail_op  = r3v_stencil_op_to_pipe(fc->sfail);
            st->zpass_op = r3v_stencil_op_to_pipe(fc->spass);
            st->zfail_op = r3v_stencil_op_to_pipe(fc->sdepth_fail);
         }
         if ((eff & R3V_DYN_STENCIL_CMP_MASK) &&
             (fc->set & R3V_DYN_STENCIL_CMP_MASK))
            st->valuemask = (uint8_t)fc->cmp_mask;
         if ((eff & R3V_DYN_STENCIL_WR_MASK) &&
             (fc->set & R3V_DYN_STENCIL_WR_MASK))
            st->writemask = (uint8_t)fc->wr_mask;
      }
      if (!has_zs) {
         dsa.depth_enabled   = false;
         dsa.depth_writemask = false;
         dsa.stencil[0].enabled = false;
         dsa.stencil[1].enabled = false;
      } else if (!has_stencil) {
         /* Depth-only attachment (e.g. D16_UNORM, X8_D24_UNORM_PACK32): there is
          * no stencil aspect to test against, so per Vulkan the stencil test must
          * behave as if it always passes.  Leaving it enabled runs the r300
          * stencil test against a nonexistent stencil buffer, which kills every
          * fragment -- the draw renders black where it should show (the
          * dEQP-VK.pipeline.monolithic.stencil.no_stencil_att.* cluster). */
         dsa.stencil[0].enabled = false;
         dsa.stencil[1].enabled = false;
      }
      new_dsa_cso = pipe->create_depth_stencil_alpha_state(pipe, &dsa);
   }
   pipe->bind_depth_stencil_alpha_state(pipe, new_dsa_cso ? new_dsa_cso
                                                          : pl->dsa_cso);
   if (ov->dsa_cso)
      pipe->delete_depth_stencil_alpha_state(pipe, ov->dsa_cso);
   ov->dsa_cso = new_dsa_cso;

   struct pipe_stencil_ref sref;
   const bool dyn_ref = (eff & R3V_DYN_STENCIL_REF) != 0;
   sref.ref_value[0] = (dyn_ref && (ov->face[0].set & R3V_DYN_STENCIL_REF))
                       ? (uint8_t)ov->face[0].ref
                       : (uint8_t)pl->static_stencil_ref_front;
   sref.ref_value[1] = (dyn_ref && (ov->face[1].set & R3V_DYN_STENCIL_REF))
                       ? (uint8_t)ov->face[1].ref
                       : (uint8_t)pl->static_stencil_ref_back;
   pipe->set_stencil_ref(pipe, sref);

   struct pipe_blend_color bc;
   memcpy(bc.color, (eff & R3V_DYN_BLEND_CONST) ? ov->blend_const
                                                   : pl->static_blend_const,
          sizeof(bc.color));
   pipe->set_blend_color(pipe, &bc);

   ov->dirty = false;
}

/* Delete the transient CSOs at the end of a tile pass.  r300g's
 * delete_rs/dsa_state unbind-if-bound, so deleting a still-bound transient
 * is safe; the next pipeline bind supplies fresh state. */
static void
r3v_dyn_overlay_cleanup(struct r3v_device *device,
                           struct r3v_dyn_overlay *ov)
{
   struct pipe_context *pipe = device->pipe;
   if (ov->rs_cso) {
      pipe->delete_rasterizer_state(pipe, ov->rs_cso);
      ov->rs_cso = NULL;
   }
   if (ov->dsa_cso) {
      pipe->delete_depth_stencil_alpha_state(pipe, ov->dsa_cso);
      ov->dsa_cso = NULL;
   }
   if (ov->velems_cso) {
      pipe->delete_vertex_elements_state(pipe, ov->velems_cso);
      ov->velems_cso = NULL;
   }
}

static void
r3v_replay_draw(struct r3v_device *device,
                    const struct r3v_cmd_entry *e,
                    const struct r3v_pipeline *bound_pipeline,
                    const struct r3v_cmd_bind_descriptor_sets *last_bind_dsets,
                    const uint8_t *push_const,
                    struct pipe_vertex_buffer *vb_cache,
                    VkDeviceSize *vb_sizes,
                    uint32_t vb_max_used,
                    bool *vb_dirty,
                    uint32_t tile_origin_x,
                    uint32_t tile_origin_y,
                    uint32_t tile_width,
                    uint32_t tile_height,
                    struct util_dynarray *transient_vbs,
                    struct r3v_dyn_overlay *dyn,
                    bool render_pass_has_zs,
                    bool render_pass_has_stencil,
                    bool input_self_dep,
                    const VkDeviceSize *vb_strides,
                    uint32_t vb_strides_mask)
{
   struct pipe_context *pipe = device->pipe;

   /* Unify the direct and indexed draw parameters so the descriptor bind,
    * synthetic VS streams, viewport/scissor, and robustness clamp below apply
    * to both.  An indexed draw's "first/count" are the first index and index
    * count; the index buffer itself is set on pipe_draw_info further down. */
   const bool indexed = (e->type == R3V_CMD_DRAW_INDEXED);
   const uint32_t draw_first      = indexed ? e->draw_indexed.first_index
                                            : e->draw.first;
   const uint32_t draw_count      = indexed ? e->draw_indexed.index_count
                                            : e->draw.count;
   const uint32_t draw_instances  = indexed ? e->draw_indexed.instances
                                            : e->draw.instances;
   const uint32_t draw_first_inst = indexed ? e->draw_indexed.first_instance
                                            : e->draw.first_instance;
   /* The recorded per-draw topology is the pipeline's static value; a
    * vkCmdSetPrimitiveTopology in effect overrides it when the pipeline
    * declared topology dynamic. */
   const bool dyn_topology =
      bound_pipeline && dyn && !device->dbg_no_topo_override &&
      (dyn->flags & bound_pipeline->dyn_mask & R3V_DYN_TOPOLOGY);
   const VkPrimitiveTopology draw_topology =
      dyn_topology ? dyn->topology
                   : (indexed ? e->draw_indexed.topology : e->draw.topology);

   /* A graphics pipeline with no fragment stage (rasterizer discard or
    * depth-only) leaves fs_cso NULL -- a failed fragment compile would instead
    * have failed pipeline creation, so NULL here means no fragment stage.  Such
    * a draw produces no color in r3v's color-attachment model.  It must be
    * skipped rather than entered: on the RS482 SW-TCL path r300_update_derived_state
    * compiles the hardware vertex shader only when caps.has_tcl is set (it is
    * not), yet r300_update_rs_block unconditionally dereferences r300_vs()->shader
    * and r300_fs()->shader, so a no-fragment draw NULL-derefs in the RS block
    * (dEQP-VK.api.descriptor_set.descriptor_set_layout_lifetime.graphics). */
   if (!bound_pipeline || !bound_pipeline->vs_cso || !bound_pipeline->fs_cso)
      return;

   /* Overlay the merged vkCmdSet* shadow onto this pipeline's rs/dsa state
    * before any draw-time binds. */
   if (dyn)
      r3v_dyn_overlay_apply(device, bound_pipeline, dyn,
                               render_pass_has_zs, render_pass_has_stencil);

   /* Dynamic vertex strides: when a vkCmdBindVertexBuffers2 stride differs
    * from the vertex-input description's, rebuild the element CSO with the
    * bind-time strides patched in (the dynamic-stride state makes them
    * authoritative).  Synthetic VS-system-value bindings sit on reserved
    * driver bindings no app bind reaches, so they never patch. */
   if (dyn && bound_pipeline && bound_pipeline->velems_count) {
      bool patch = false;
      for (uint32_t i = 0; i < bound_pipeline->velems_count; i++) {
         const uint8_t b = bound_pipeline->velems_template[i].vertex_buffer_index;
         if (b < R3V_MAX_VERTEX_BINDINGS &&
             (vb_strides_mask & BITFIELD_BIT(b)) &&
             bound_pipeline->velems_template[i].src_stride !=
             (uint16_t)vb_strides[b]) {
            patch = true;
            break;
         }
      }
      if (patch) {
         struct pipe_vertex_element ve[PIPE_MAX_ATTRIBS];
         memcpy(ve, bound_pipeline->velems_template,
                sizeof(ve[0]) * bound_pipeline->velems_count);
         for (uint32_t i = 0; i < bound_pipeline->velems_count; i++) {
            const uint8_t b = ve[i].vertex_buffer_index;
            if (b < R3V_MAX_VERTEX_BINDINGS &&
                (vb_strides_mask & BITFIELD_BIT(b)))
               ve[i].src_stride = (uint16_t)vb_strides[b];
         }
         void *new_velems =
            pipe->create_vertex_elements_state(pipe,
                                               bound_pipeline->velems_count, ve);
         if (new_velems) {
            pipe->bind_vertex_elements_state(pipe, new_velems);
            if (dyn->velems_cso)
               pipe->delete_vertex_elements_state(pipe, dyn->velems_cso);
            dyn->velems_cso = new_velems;
            *vb_dirty = true;
         }
      } else if (dyn->velems_cso) {
         pipe->bind_vertex_elements_state(pipe, bound_pipeline->velems_cso);
         pipe->delete_vertex_elements_state(pipe, dyn->velems_cso);
         dyn->velems_cso = NULL;
         *vb_dirty = true;
      }
   }

   /* Resolve pipeline-static viewport/scissor before the draw. */
   if (bound_pipeline && bound_pipeline->has_static_viewport) {
      struct pipe_viewport_state pv;
      viewport_vk_to_gallium(&bound_pipeline->static_viewport,
                             (float)tile_origin_x, (float)tile_origin_y,
                             &pv);
      pipe->set_viewport_states(pipe, 0, 1, &pv);
   }
   if (bound_pipeline && bound_pipeline->has_static_scissor) {
      struct pipe_scissor_state sc;
      r3v_scissor_vk_to_tile(&bound_pipeline->static_scissor,
                                tile_origin_x, tile_origin_y,
                                tile_width, tile_height, &sc);
      pipe->set_scissor_states(pipe, 0, 1, &sc);
   }

   const uint32_t robust_instances =
      r3v_robust_instance_count(bound_pipeline, vb_cache, vb_sizes,
                                   vb_strides, vb_strides_mask,
                                   draw_first_inst, draw_instances);

   struct pipe_vertex_buffer draw_vb_cache[R3V_MAX_VERTEX_BINDINGS];
   memcpy(draw_vb_cache, vb_cache, sizeof(draw_vb_cache));
   uint32_t draw_vb_max_used = vb_max_used;
   bool draw_vb_dirty = *vb_dirty;
   bool synthetic_streams_ready = true;

   /* Supply the synthetic VS-system-value stream(s) for this draw. */
   if (!indexed && bound_pipeline && bound_pipeline->needs_vertex_id_stream) {
      synthetic_streams_ready =
         r3v_bind_synthetic_index_stream(
            device, pipe, draw_vb_cache,
            bound_pipeline->vertex_id_vb_binding, draw_first,
            draw_count, transient_vbs);
      if (synthetic_streams_ready) {
         if (bound_pipeline->vertex_id_vb_binding + 1u > draw_vb_max_used)
            draw_vb_max_used = bound_pipeline->vertex_id_vb_binding + 1u;
         draw_vb_dirty = true;
      }
   }
   if (synthetic_streams_ready && bound_pipeline &&
       bound_pipeline->needs_instance_id_stream) {
      synthetic_streams_ready =
         r3v_bind_synthetic_index_stream(
            device, pipe, draw_vb_cache,
            bound_pipeline->instance_id_vb_binding,
            draw_first_inst, robust_instances, transient_vbs);
      if (synthetic_streams_ready) {
         if (bound_pipeline->instance_id_vb_binding + 1u >
             draw_vb_max_used)
            draw_vb_max_used =
               bound_pipeline->instance_id_vb_binding + 1u;
         draw_vb_dirty = true;
      }
   }
   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode           = vk_topology_to_mesa(draw_topology);
   info.instance_count = robust_instances;
   info.start_instance = draw_first_inst;
   struct pipe_draw_start_count_bias draw = { .index_bias = 0 };

   if (indexed) {
      const struct r3v_cmd_draw_indexed *di = &e->draw_indexed;
      if (!di->index_buffer || !di->index_buffer->resource ||
          di->index_size == 0)
         return;
      info.index_size       = di->index_size;
      info.index.resource   = di->index_buffer->resource;
      info.has_user_indices = false;
      /* pipe_draw_start_count_bias.start is in index elements; the
       * vkCmdBindIndexBuffer byte offset is guaranteed a multiple of the index
       * size, so fold it into start.  index_bias is the vertexOffset added to
       * every fetched index.  Clamp the count to the bound range -- the indexed
       * analog of r3v_robust_vertex_count -- so the index fetch cannot read
       * past the buffer end (the kernel CS validator rejects the same overrun). */
      const uint64_t start_elem =
         (uint64_t)di->first_index + di->index_offset / di->index_size;
      const uint64_t end_elem =
         (di->index_offset + di->index_range) / di->index_size;
      const uint64_t usable = end_elem > start_elem ? end_elem - start_elem : 0;
      if (start_elem > UINT_MAX)
         return;
      draw.start = (unsigned)start_elem;
      draw.count = synthetic_streams_ready
                   ? (di->index_count < usable ? di->index_count
                                               : (unsigned)usable)
                   : 0;
      draw.index_bias = di->vertex_offset;
      draw.count = r3v_robust_indexed_vertex_count(
         pipe, bound_pipeline, vb_cache, vb_sizes, vb_strides, vb_strides_mask,
         di, draw.start, draw.count);
      if (synthetic_streams_ready && bound_pipeline &&
          bound_pipeline->needs_vertex_id_stream) {
         synthetic_streams_ready =
            r3v_bind_synthetic_indexed_vertex_index_stream(
               device, pipe, draw_vb_cache,
               bound_pipeline->vertex_id_vb_binding, di, draw.start,
               draw.count, transient_vbs);
         if (synthetic_streams_ready) {
            if (bound_pipeline->vertex_id_vb_binding + 1u > draw_vb_max_used)
               draw_vb_max_used = bound_pipeline->vertex_id_vb_binding + 1u;
            draw_vb_dirty = true;
         } else {
            draw.count = 0;
         }
      }
   } else {
      info.index_size = 0;
      draw.start      = draw_first;
      draw.count      = synthetic_streams_ready ?
         r3v_robust_vertex_count(bound_pipeline, vb_cache,
                                     vb_sizes, vb_strides, vb_strides_mask,
                                     draw_first, draw_count) : 0;
   }
   if (draw.count > 0 && draw_vb_dirty) {
      pipe->set_vertex_buffers(pipe, draw_vb_max_used, draw_vb_cache);
      *vb_dirty = synthetic_streams_ready &&
                 (bound_pipeline &&
                  (bound_pipeline->needs_vertex_id_stream ||
                   bound_pipeline->needs_instance_id_stream));
   }
   if (draw.count > 0) {
      /* Push constants and the descriptor UBO are mutually exclusive (the
       * collision is rejected at compile); bind whichever this pipeline uses at
       * CONST[0].  The descriptor UBO bind takes the pipeline so it can select
       * the shader-chosen (set, binding) rather than the first UBO in layout. */
      /* pc_scratch holds the int->float-converted push window; it must outlive
       * the draw_vbo below (set_constant_buffer reads user_buffer directly). */
      uint8_t pc_scratch[R3V_MAX_PUSH_CONSTANTS_SIZE];
      if (bound_pipeline && bound_pipeline->uses_push_constants)
         r3v_bind_push_constants(device, push_const,
                                    bound_pipeline->push_const_int_word_mask,
                                    pc_scratch);
      else
         r3v_bind_descriptor_ubo(device, bound_pipeline, last_bind_dsets);

      /* Bind fragment textures for this draw, then release them after: the
       * sampler views are transient (created over the descriptor's image at
       * draw time), so they must not outlive the draw that referenced them.
       * stitch_geom holds the per-image tile geometry a stitched sampler binds to
       * FS CONST[0]; it lives in this frame, valid through the draw_vbo below (the
       * same user_buffer lifetime contract as ia_inv_extent). */
      struct r3v_bound_textures bound_tex;
      float stitch_geom[8] = {0};
      r3v_bind_descriptor_textures(device, bound_pipeline, last_bind_dsets,
                                      &bound_tex, stitch_geom);

      /* Input attachment: override FS CONST[0] with inv_extent and bind the
       * subpass color image.  Done after r3v_bind_descriptor_textures so
       * it can append to bound_tex without that function resetting bound->count.
       * ia_inv_extent lives in this frame, valid through draw_vbo below -- the
       * same lifetime contract as r3v_multitap_gather_dispatch_replay. */
      float ia_inv_extent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      bool ia_bind_ok = true;
      if (bound_pipeline && bound_pipeline->fs_has_input_attachment)
         ia_bind_ok = r3v_bind_input_attachment(device, bound_pipeline,
                                      last_bind_dsets, &bound_tex,
                                      tile_origin_x, tile_origin_y,
                                      input_self_dep, ia_inv_extent);

      if (device->dbg_log_draws) {
         fprintf(stderr,
                 "r3v draw: mode=%u count=%u start=%u inst=%u topo=%d "
                 "dyn_topo=%d dyn_mask=0x%x dyn_flags=0x%x zs=%d\n",
                 (unsigned)info.mode, draw.count, draw.start, draw_instances,
                 (int)draw_topology, dyn_topology ? 1 : 0,
                 bound_pipeline ? bound_pipeline->dyn_mask : 0,
                 dyn ? dyn->flags : 0, render_pass_has_zs ? 1 : 0);
         /* Per-element fetch parameters plus the first two vertices each
          * element would fetch, so a flat-output draw can be split into
          * "zink uploaded constant data" vs "the fetch offset/stride is
          * wrong".  Reads through the same pipe mapping the draw uses. */
         for (uint32_t i = 0;
              bound_pipeline && i < bound_pipeline->velems_count; i++) {
            const struct pipe_vertex_element *ve =
               &bound_pipeline->velems_template[i];
            const uint8_t b = ve->vertex_buffer_index;
            if (b >= R3V_MAX_VERTEX_BINDINGS ||
                !draw_vb_cache[b].buffer.resource)
               continue;
            const uint32_t eff_stride =
               (vb_strides_mask & BITFIELD_BIT(b))
               ? (uint32_t)vb_strides[b] : ve->src_stride;
            const uint64_t fetch_off =
               (uint64_t)draw_vb_cache[b].buffer_offset + ve->src_offset +
               (uint64_t)(indexed ? 0 : draw.start) * eff_stride;
            fprintf(stderr,
                    "r3v ve[%u]: bind=%u fmt=%u src_off=%u tmpl_stride=%u "
                    "eff_stride=%u vb_off=%u vb_size=%llu fetch_off=%llu res=%p",
                    i, b, (unsigned)ve->src_format, ve->src_offset,
                    ve->src_stride, eff_stride,
                    draw_vb_cache[b].buffer_offset,
                    (unsigned long long)vb_sizes[b],
                    (unsigned long long)fetch_off,
                    (void *)draw_vb_cache[b].buffer.resource);
            struct pipe_transfer *vx = NULL;
            const float *vf = pipe_buffer_map_range(
               pipe, draw_vb_cache[b].buffer.resource, (unsigned)fetch_off,
               eff_stride ? eff_stride + 16 : 16, PIPE_MAP_READ, &vx);
            if (vf) {
               fprintf(stderr, " v0=[%g %g %g %g] v1=[%g %g %g %g]",
                       vf[0], vf[1], vf[2], vf[3],
                       eff_stride ? vf[eff_stride / 4 + 0] : vf[0],
                       eff_stride ? vf[eff_stride / 4 + 1] : vf[1],
                       eff_stride ? vf[eff_stride / 4 + 2] : vf[2],
                       eff_stride ? vf[eff_stride / 4 + 3] : vf[3]);
               pipe_buffer_unmap(pipe, vx);
            }
            fprintf(stderr, "\n");
         }
      }
      /* Skip the draw when an expected input attachment could not be bound: the
       * FS samples sampler unit 0, which would otherwise hold a stale/undefined
       * view -- a defined no-op beats rendering garbage.  ia_bind_ok is always
       * true under valid usage, so a conformant draw is never skipped. */
      if (ia_bind_ok)
         pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
      r3v_unbind_descriptor_textures(device, &bound_tex);
   }
}

static void
r3v_replay_bind_vertex_buffers(struct r3v_device *device,
                                   const struct r3v_cmd_entry *e,
                                   struct pipe_vertex_buffer *vb_cache,
                                   VkDeviceSize *vb_sizes,
                                   VkDeviceSize *vb_strides,
                                   uint32_t *vb_strides_mask,
                                   uint32_t *vb_max_used,
                                   bool *vb_dirty)
{
   uint32_t first = e->bind_vbufs.first_binding;
   uint32_t count = e->bind_vbufs.binding_count;
   for (uint32_t b = 0; b < count; b++) {
      vb_cache[first + b].is_user_buffer  = false;
      vb_cache[first + b].buffer_offset   = (unsigned)e->bind_vbufs.offsets[b];
      vb_cache[first + b].buffer.resource = e->bind_vbufs.buffers[b]->resource;
      /* pSizes narrows the bound range below the whole buffer; the robust
       * clamp consumes (size - offset), so fold the range end in here. */
      const VkDeviceSize whole = e->bind_vbufs.buffers[b]->size;
      const VkDeviceSize range = e->bind_vbufs.sizes[b];
      vb_sizes[first + b] = (range != VK_WHOLE_SIZE &&
                             e->bind_vbufs.offsets[b] + range < whole)
                            ? e->bind_vbufs.offsets[b] + range : whole;
      if (e->bind_vbufs.has_strides) {
         vb_strides[first + b] = e->bind_vbufs.strides[b];
         *vb_strides_mask |= BITFIELD_BIT(first + b);
      }
      if (first + b + 1 > *vb_max_used)
         *vb_max_used = first + b + 1;
   }
   *vb_dirty = true;
}

static void
r3v_replay_set_viewport(struct r3v_device *device,
                            const struct r3v_cmd_entry *e,
                            uint32_t tile_origin_x,
                            uint32_t tile_origin_y)
{
   struct pipe_context *pipe = device->pipe;
   struct pipe_viewport_state pv;
   viewport_vk_to_gallium(&e->set_vp.vp, (float)tile_origin_x,
                          (float)tile_origin_y, &pv);
   pipe->set_viewport_states(pipe, 0, 1, &pv);
}

static void
r3v_replay_set_scissor(struct r3v_device *device,
                           const struct r3v_cmd_entry *e,
                           uint32_t tile_origin_x,
                           uint32_t tile_origin_y,
                           uint32_t tile_width,
                           uint32_t tile_height)
{
   struct pipe_context *pipe = device->pipe;
   struct pipe_scissor_state sc;
   r3v_scissor_vk_to_tile(&e->set_sc.scissor, tile_origin_x,
                             tile_origin_y, tile_width, tile_height,
                             &sc);
   pipe->set_scissor_states(pipe, 0, 1, &sc);
}

static void
r3v_replay_bind_pipeline(struct r3v_device *device,
                             const struct r3v_cmd_entry *e,
                             const struct r3v_pipeline **bound_pipeline,
                             bool *vb_dirty)
{
   struct pipe_context *pipe = device->pipe;
   const struct r3v_pipeline *pl = e->bind_pipeline.pipeline;
   *bound_pipeline = pl;
   /* The spec requires a non-null pipeline handle
    * (VUID-vkCmdBindPipeline-pipeline-parameter), but an application that
    * ignores a failed vkCreateGraphicsPipelines and binds VK_NULL_HANDLE must
    * not crash the GPU replay.  Record the null binding and bind no CSOs; the
    * draw replay already skips any draw whose bound pipeline (or its vs_cso /
    * fs_cso) is null, so the offending draw is dropped instead of dereferencing
    * a null pipeline here. */
   if (!pl) {
      *vb_dirty = true;
      return;
   }
   pipe->bind_blend_state(pipe, pl->blend_cso);
   pipe->bind_rasterizer_state(pipe, pl->rasterizer_cso);
   pipe->bind_depth_stencil_alpha_state(pipe, pl->dsa_cso);
   pipe->bind_vs_state(pipe, pl->vs_cso);
   pipe->bind_fs_state(pipe, pl->fs_cso);
   pipe->bind_vertex_elements_state(pipe, pl->velems_cso);
   /* Changing vertex elements requires a subsequent set_vertex_buffers
    * before the next draw per p_context.h.  Vulkan allows CmdBindPipeline
    * without a follow-up CmdBindVertexBuffers, so force a VB flush. */
   *vb_dirty = true;
}

static bool
r3v_begin_rp_clip_render_area(const struct r3v_cmd_begin_render_pass *brp,
                                 uint32_t tile_origin_x,
                                 uint32_t tile_origin_y,
                                 uint32_t tile_width,
                                 uint32_t tile_height,
                                 int64_t *clip_min_x,
                                 int64_t *clip_min_y,
                                 int64_t *clip_max_x,
                                 int64_t *clip_max_y)
{
   const int64_t req_min_x = brp->render_area_offset_x;
   const int64_t req_min_y = brp->render_area_offset_y;
   const int64_t req_max_x = brp->width;
   const int64_t req_max_y = brp->height;
   const int64_t tile_min_x = tile_origin_x;
   const int64_t tile_min_y = tile_origin_y;
   const int64_t tile_max_x = tile_min_x + tile_width;
   const int64_t tile_max_y = tile_min_y + tile_height;

   *clip_min_x = MAX2(req_min_x, tile_min_x);
   *clip_min_y = MAX2(req_min_y, tile_min_y);
   *clip_max_x = MIN2(req_max_x, tile_max_x);
   *clip_max_y = MIN2(req_max_y, tile_max_y);

   return *clip_max_x > *clip_min_x && *clip_max_y > *clip_min_y;
}

/* Apply the begin load-op clears for a freshly bound render pass.  pipe->clear
 * paints one colour across every bound cbuf, so distinct per-attachment clear
 * colours go through clear_render_target per slot; depth/stencil uses
 * clear_depth_stencil for clipped rectangles.  The common single-attachment
 * full-cell case keeps the combined colour+ds clear so its CMASK fast-fill path
 * is unchanged. */
static void
r3v_replay_begin_clears(struct pipe_context *pipe,
                           struct pipe_framebuffer_state *fb,
                           const struct r3v_cmd_begin_render_pass *brp,
                           uint32_t tile_origin_x,
                           uint32_t tile_origin_y,
                           uint32_t tile_width,
                           uint32_t tile_height)
{
   int64_t clip_min_x = 0;
   int64_t clip_min_y = 0;
   int64_t clip_max_x = 0;
   int64_t clip_max_y = 0;
   if (!r3v_begin_rp_clip_render_area(brp, tile_origin_x, tile_origin_y,
                                         tile_width, tile_height,
                                         &clip_min_x, &clip_min_y,
                                         &clip_max_x, &clip_max_y))
      return;

   unsigned ds_bits = 0;
   if (fb->zsbuf.texture && brp->ds_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
      ds_bits |= PIPE_CLEAR_DEPTH;
      if (util_format_has_stencil(util_format_description(brp->ds_format)))
         ds_bits |= PIPE_CLEAR_STENCIL;
   }

   const bool single_color =
      fb->nr_cbufs == 1 && fb->cbufs[0].texture &&
      brp->load_op[0] == VK_ATTACHMENT_LOAD_OP_CLEAR;
   const bool full_framebuffer =
      clip_min_x == (int64_t)tile_origin_x &&
      clip_min_y == (int64_t)tile_origin_y &&
      clip_max_x == (int64_t)tile_origin_x + fb->width &&
      clip_max_y == (int64_t)tile_origin_y + fb->height;

   if (single_color && full_framebuffer) {
      union pipe_color_union cv;
      memcpy(cv.f, brp->clear_color[0].float32, sizeof(cv.f));
      pipe->clear(pipe, PIPE_CLEAR_COLOR0 | ds_bits, 0xF, 0, NULL, &cv,
                  brp->clear_depth, brp->clear_stencil);
      return;
   }

   for (uint32_t slot = 0; slot < brp->color_count; slot++) {
      const struct r3v_image *img = brp->color_image[slot];
      if (!img || !fb->cbufs[slot].texture ||
          brp->load_op[slot] != VK_ATTACHMENT_LOAD_OP_CLEAR)
         continue;

      uint32_t attachment_tile_origin_x = 0;
      uint32_t attachment_tile_origin_y = 0;
      uint32_t attachment_remaining_width = 0;
      uint32_t attachment_remaining_height = 0;
      struct pipe_resource *tile_resource =
         r3v_image_tile_resource_for_origin(img, tile_origin_x,
                                               tile_origin_y,
                                               &attachment_tile_origin_x,
                                               &attachment_tile_origin_y,
                                               &attachment_remaining_width,
                                               &attachment_remaining_height);
      if (!tile_resource || tile_resource != fb->cbufs[slot].texture ||
          clip_min_x < (int64_t)attachment_tile_origin_x ||
          clip_min_y < (int64_t)attachment_tile_origin_y)
         continue;

      union pipe_color_union cv;
      memcpy(cv.f, brp->clear_color[slot].float32, sizeof(cv.f));
      pipe->clear_render_target(pipe, &fb->cbufs[slot], &cv,
                                (unsigned)(clip_min_x -
                                           attachment_tile_origin_x),
                                (unsigned)(clip_min_y -
                                           attachment_tile_origin_y),
                                (unsigned)(clip_max_x - clip_min_x),
                                (unsigned)(clip_max_y - clip_min_y),
                                false);
   }
   if (ds_bits && brp->ds_image) {
      uint32_t ds_tile_origin_x = 0;
      uint32_t ds_tile_origin_y = 0;
      uint32_t ds_remaining_width = 0;
      uint32_t ds_remaining_height = 0;
      struct pipe_resource *ds_resource =
         r3v_image_tile_resource_for_origin(brp->ds_image, tile_origin_x,
                                               tile_origin_y,
                                               &ds_tile_origin_x,
                                               &ds_tile_origin_y,
                                               &ds_remaining_width,
                                               &ds_remaining_height);
      if (ds_resource && ds_resource == fb->zsbuf.texture &&
          clip_min_x >= (int64_t)ds_tile_origin_x &&
          clip_min_y >= (int64_t)ds_tile_origin_y) {
         pipe->clear_depth_stencil(pipe, &fb->zsbuf, ds_bits,
                                   brp->clear_depth, brp->clear_stencil,
                                   (unsigned)(clip_min_x - ds_tile_origin_x),
                                   (unsigned)(clip_min_y - ds_tile_origin_y),
                                   (unsigned)(clip_max_x - clip_min_x),
                                   (unsigned)(clip_max_y - clip_min_y),
                                   false);
      }
   }
}

static void
r3v_replay_begin_render_pass(struct r3v_device *device,
                                 const struct r3v_cmd_entry *e,
                                 unsigned tile_pass,
                                 uint32_t *tile_origin_x,
                                 uint32_t *tile_origin_y,
                                 uint32_t *tile_width,
                                 uint32_t *tile_height,
                                 bool *skip_render_pass)
{
   struct pipe_context *pipe = device->pipe;
   struct pipe_framebuffer_state fb;
   struct pipe_resource *dummy_cbufs[PIPE_MAX_COLOR_BUFS] = {0};
   memset(&fb, 0, sizeof(fb));
   *skip_render_pass = false;

   if (!r3v_begin_rp_tile_geometry(&e->begin_rp, tile_pass,
                                      tile_origin_x, tile_origin_y,
                                      tile_width, tile_height)) {
      *skip_render_pass = true;
      return;
   }

   if (*tile_width > R3V_R3XX_MAX_RENDER_DIMENSION ||
       *tile_height > R3V_R3XX_MAX_RENDER_DIMENSION) {
      *skip_render_pass = true;
      return;
   }
   fb.width = *tile_width;
   fb.height = *tile_height;

   for (uint32_t slot = 0; slot < e->begin_rp.color_count; slot++) {
      const struct r3v_image *img = e->begin_rp.color_image[slot];
      if (!img)
         continue;

      uint32_t attachment_tile_index = 0;
      uint32_t attachment_tile_origin_x = 0;
      uint32_t attachment_tile_origin_y = 0;
      uint32_t attachment_remaining_width = 0;
      uint32_t attachment_remaining_height = 0;
      if (!r3v_image_tile_for_origin(img, *tile_origin_x, *tile_origin_y,
                                        &attachment_tile_index,
                                        &attachment_tile_origin_x,
                                        &attachment_tile_origin_y,
                                        &attachment_remaining_width,
                                        &attachment_remaining_height)) {
         *skip_render_pass = true;
         return;
      }
      /* One replay cell has one viewport and scissor translation.  Binding an
       * attachment tile with a different local origin would route writes to the
       * wrong pixels inside that attachment. */
      if (attachment_tile_origin_x != *tile_origin_x ||
          attachment_tile_origin_y != *tile_origin_y) {
         *skip_render_pass = true;
         return;
      }
      *tile_width = MIN2(*tile_width, attachment_remaining_width);
      *tile_height = MIN2(*tile_height, attachment_remaining_height);
   }

   if (e->begin_rp.ds_image) {
      uint32_t ds_tile_index = 0;
      uint32_t ds_tile_origin_x = 0;
      uint32_t ds_tile_origin_y = 0;
      uint32_t ds_remaining_width = 0;
      uint32_t ds_remaining_height = 0;
      if (!r3v_image_tile_for_origin(e->begin_rp.ds_image,
                                        *tile_origin_x, *tile_origin_y,
                                        &ds_tile_index, &ds_tile_origin_x,
                                        &ds_tile_origin_y,
                                        &ds_remaining_width,
                                        &ds_remaining_height)) {
         *skip_render_pass = true;
         return;
      }
      if (ds_tile_origin_x != *tile_origin_x ||
          ds_tile_origin_y != *tile_origin_y) {
         *skip_render_pass = true;
         return;
      }
      *tile_width = MIN2(*tile_width, ds_remaining_width);
      *tile_height = MIN2(*tile_height, ds_remaining_height);
   }

   if (*tile_width == 0 || *tile_height == 0) {
      *skip_render_pass = true;
      return;
   }
   fb.width = *tile_width;
   fb.height = *tile_height;

   /* Bind each color attachment at its own slot so fragment output i lands
    * on attachment i (R300 ROP order COLOROFFSET0+4*i).  r300g substitutes
    * another bound cbuf for NULL holes in r300_get_nonnull_cb(), so unused
    * MRT slots before a later attachment bind throwaway render targets. */
   for (uint32_t slot = 0; slot < e->begin_rp.color_count; slot++)
      if (e->begin_rp.color_image[slot])
         fb.nr_cbufs = slot + 1;

   enum pipe_format dummy_format = PIPE_FORMAT_R8G8B8A8_UNORM;
   for (uint32_t slot = 0; slot < fb.nr_cbufs; slot++) {
      if (e->begin_rp.color_format[slot] != PIPE_FORMAT_NONE) {
         dummy_format = e->begin_rp.color_format[slot];
         break;
      }
   }

   for (uint32_t slot = 0; slot < fb.nr_cbufs; slot++) {
      const struct r3v_image *img = e->begin_rp.color_image[slot];
      if (!img) {
         struct pipe_resource tmpl = {
            .target     = PIPE_TEXTURE_2D,
            .format     = dummy_format,
            .bind       = PIPE_BIND_RENDER_TARGET,
            .usage      = PIPE_USAGE_DEFAULT,
            .width0     = fb.width,
            .height0    = fb.height,
            .depth0     = 1,
            .array_size = 1,
         };
         dummy_cbufs[slot] = device->screen->resource_create(device->screen,
                                                             &tmpl);
         if (!dummy_cbufs[slot]) {
            *skip_render_pass = true;
            goto out_release_dummy_cbufs;
         }
         fb.cbufs[slot].texture = dummy_cbufs[slot];
         fb.cbufs[slot].format = dummy_format;
         fb.cbufs[slot].level = 0;
         fb.cbufs[slot].first_layer = 0;
         fb.cbufs[slot].last_layer = 0;
         continue;
      }

      uint32_t attachment_tile_origin_x = 0;
      uint32_t attachment_tile_origin_y = 0;
      uint32_t attachment_remaining_width = 0;
      uint32_t attachment_remaining_height = 0;
      fb.cbufs[slot].texture =
         r3v_image_tile_resource_for_origin(img, *tile_origin_x,
                                               *tile_origin_y,
                                               &attachment_tile_origin_x,
                                               &attachment_tile_origin_y,
                                               &attachment_remaining_width,
                                               &attachment_remaining_height);
      if (!fb.cbufs[slot].texture) {
         *skip_render_pass = true;
         goto out_release_dummy_cbufs;
      }
      if (attachment_tile_origin_x != *tile_origin_x ||
          attachment_tile_origin_y != *tile_origin_y) {
         *skip_render_pass = true;
         goto out_release_dummy_cbufs;
      }
      fb.cbufs[slot].format      = e->begin_rp.color_format[slot];
      fb.cbufs[slot].level       = 0;
      fb.cbufs[slot].first_layer = 0;
      fb.cbufs[slot].last_layer  = 0;
   }

   /* Bind the depth/stencil attachment as the zsbuf at the same pass origin. */
   if (e->begin_rp.ds_image) {
      const struct r3v_image *ds = e->begin_rp.ds_image;
      uint32_t ds_tile_origin_x = 0;
      uint32_t ds_tile_origin_y = 0;
      uint32_t ds_remaining_width = 0;
      uint32_t ds_remaining_height = 0;
      fb.zsbuf.texture =
         r3v_image_tile_resource_for_origin(ds, *tile_origin_x,
                                               *tile_origin_y,
                                               &ds_tile_origin_x,
                                               &ds_tile_origin_y,
                                               &ds_remaining_width,
                                               &ds_remaining_height);
      if (fb.zsbuf.texture) {
         if (ds_tile_origin_x != *tile_origin_x ||
             ds_tile_origin_y != *tile_origin_y) {
            *skip_render_pass = true;
            goto out_release_dummy_cbufs;
         }
         fb.zsbuf.format      = e->begin_rp.ds_format;
         fb.zsbuf.level       = 0;
         fb.zsbuf.first_layer = 0;
         fb.zsbuf.last_layer  = 0;
      }
   }
   pipe->set_framebuffer_state(pipe, &fb);
   r3v_replay_begin_clears(pipe, &fb, &e->begin_rp,
                              *tile_origin_x, *tile_origin_y,
                              *tile_width, *tile_height);

out_release_dummy_cbufs:
   for (uint32_t slot = 0; slot < ARRAY_SIZE(dummy_cbufs); slot++)
      pipe_resource_reference(&dummy_cbufs[slot], NULL);
}

static void
r3v_replay_clear_attachments(struct r3v_device *device,
                                const struct r3v_cmd_entry *render_pass,
                                const struct r3v_cmd_clear_attachments *clear,
                                uint32_t tile_origin_x,
                                uint32_t tile_origin_y,
                                uint32_t tile_width,
                                uint32_t tile_height)
{
   if (!render_pass ||
       (!r3v_begin_rp_ref_image(&render_pass->begin_rp) &&
        !render_pass->begin_rp.ds_image))
      return;

   /* A colour clear names its target subpass slot; select that attachment.
    * Depth/stencil clears ignore the slot. */
   const uint32_t color_slot = clear->color_attachment;
   const struct r3v_image *img =
      color_slot < render_pass->begin_rp.color_count
         ? render_pass->begin_rp.color_image[color_slot]
         : NULL;
   const int64_t req_min_x = clear->rect.offset.x;
   const int64_t req_min_y = clear->rect.offset.y;
   const int64_t req_max_x = req_min_x + clear->rect.extent.width;
   const int64_t req_max_y = req_min_y + clear->rect.extent.height;
   const int64_t tile_min_x = tile_origin_x;
   const int64_t tile_min_y = tile_origin_y;
   const int64_t tile_max_x = tile_min_x + tile_width;
   const int64_t tile_max_y = tile_min_y + tile_height;
   const int64_t clip_min_x = MAX2(req_min_x, tile_min_x);
   const int64_t clip_min_y = MAX2(req_min_y, tile_min_y);
   const int64_t clip_max_x = MIN2(req_max_x, tile_max_x);
   const int64_t clip_max_y = MIN2(req_max_y, tile_max_y);

   if (clip_max_x <= clip_min_x || clip_max_y <= clip_min_y)
      return;

   if ((clear->aspect & VK_IMAGE_ASPECT_COLOR_BIT) && img) {
      uint32_t attachment_tile_origin_x = 0;
      uint32_t attachment_tile_origin_y = 0;
      uint32_t attachment_remaining_width = 0;
      uint32_t attachment_remaining_height = 0;
      struct pipe_resource *tile_resource =
         r3v_image_tile_resource_for_origin(img, tile_origin_x,
                                               tile_origin_y,
                                               &attachment_tile_origin_x,
                                               &attachment_tile_origin_y,
                                               &attachment_remaining_width,
                                               &attachment_remaining_height);
      if (!tile_resource)
         return;

      struct pipe_surface surf;
      memset(&surf, 0, sizeof(surf));
      surf.texture     = tile_resource;
      surf.format      = render_pass->begin_rp.color_format[color_slot];
      surf.level       = 0;
      surf.first_layer = 0;
      surf.last_layer  = 0;

      union pipe_color_union color;
      memset(&color, 0, sizeof(color));
      memcpy(&color, &clear->color, sizeof(clear->color));

      device->pipe->clear_render_target(device->pipe, &surf, &color,
                                        (unsigned)(clip_min_x -
                                                   attachment_tile_origin_x),
                                        (unsigned)(clip_min_y -
                                                   attachment_tile_origin_y),
                                        (unsigned)(clip_max_x - clip_min_x),
                                        (unsigned)(clip_max_y - clip_min_y),
                                        false);
   }

   /* Depth/stencil aspect: clear the matching zsbuf tile region.  zink
    * defers GL clears and applies them as vkCmdClearAttachments once draws
    * arrive, so a missing depth clear leaves the fresh tile at ~0.0 and a
    * LESS test kills every fragment. */
   const struct r3v_image *ds = render_pass->begin_rp.ds_image;
   if ((clear->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT |
                         VK_IMAGE_ASPECT_STENCIL_BIT)) && ds) {
      uint32_t ds_tile_origin_x = 0;
      uint32_t ds_tile_origin_y = 0;
      uint32_t ds_remaining_width = 0;
      uint32_t ds_remaining_height = 0;
      struct pipe_resource *ds_resource =
         r3v_image_tile_resource_for_origin(ds, tile_origin_x,
                                               tile_origin_y,
                                               &ds_tile_origin_x,
                                               &ds_tile_origin_y,
                                               &ds_remaining_width,
                                               &ds_remaining_height);
      if (!ds_resource)
         return;

      struct pipe_surface zsurf;
      memset(&zsurf, 0, sizeof(zsurf));
      zsurf.texture     = ds_resource;
      zsurf.format      = render_pass->begin_rp.ds_format;
      zsurf.level       = 0;
      zsurf.first_layer = 0;
      zsurf.last_layer  = 0;

      unsigned zs_flags = 0;
      if (clear->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
         zs_flags |= PIPE_CLEAR_DEPTH;
      if (clear->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
         zs_flags |= PIPE_CLEAR_STENCIL;
      device->pipe->clear_depth_stencil(device->pipe, &zsurf, zs_flags,
                                        clear->depth, clear->stencil,
                                        (unsigned)(clip_min_x -
                                                   ds_tile_origin_x),
                                        (unsigned)(clip_min_y -
                                                   ds_tile_origin_y),
                                        (unsigned)(clip_max_x - clip_min_x),
                                        (unsigned)(clip_max_y - clip_min_y),
                                        false);
   }
}

/* Executes one host-side transfer/event command (buffer fill, buffer copy,
 * inline update, image copy/clear, host event signal).  The replay walker calls
 * this only outside render-pass segments and drains pending GPU work first, so
 * host-visible writes stay ordered ahead of later GPU reads. */
static void r3v_replay_host_entry(struct r3v_device *device,
                                     const struct r3v_cmd_entry *e);

/* Drains GPU work flushed since the last drain so a following host transfer
 * observes finished output (a host READ) or is correctly ordered ahead of a
 * later GPU read (a host WRITE).  The flush submits the command stream and the
 * fence_finish blocks until the GPU retires it, mirroring the submit-level
 * drain.  No-op when nothing is pending, so a transfer-only run pays one fence
 * at most per host op that actually follows un-fenced GPU work. */
static void
r3v_drain_gpu(struct r3v_device *device, bool *gpu_pending)
{
   if (!*gpu_pending)
      return;

   struct pipe_fence_handle *fence = NULL;
   device->pipe->flush(device->pipe, &fence, 0);
   if (fence) {
      device->screen->fence_finish(device->screen, NULL, fence,
                                   OS_TIMEOUT_INFINITE);
      device->screen->fence_reference(device->screen, &fence, NULL);
   }
   *gpu_pending = false;
}

struct r3v_replay_state {
   struct pipe_vertex_buffer vb_cache[R3V_MAX_VERTEX_BINDINGS];
   VkDeviceSize vb_sizes[R3V_MAX_VERTEX_BINDINGS];
   VkDeviceSize vb_strides[R3V_MAX_VERTEX_BINDINGS];
   uint32_t vb_strides_mask;
   uint32_t vb_max_used;
   bool vb_dirty;
   uint8_t replay_pc[128];
   const struct r3v_pipeline *bound_pipeline;
   struct r3v_dyn_overlay dyn_ov;
   const struct r3v_cmd_bind_descriptor_sets *last_bind_dsets;
   /* Per-set accumulation of the descriptor sets bound so far, so a draw sees
    * every set even when separate vkCmdBindDescriptorSets calls bind different
    * sets.  last_bind_dsets points here once any set is bound. */
   struct r3v_cmd_bind_descriptor_sets accum_dsets;
   const struct r3v_cmd_entry *last_viewport;
   const struct r3v_cmd_entry *last_scissor;
   struct pipe_query *active_oq;
};

static void
r3v_replay_state_cleanup(struct r3v_device *device,
                            struct r3v_replay_state *state)
{
   struct pipe_context *pipe = device->pipe;

   /* A vkCmdBeginQuery with no matching end before submit is invalid usage; end
    * and destroy the dangling query so the pipe handle is not leaked. */
   if (state->active_oq) {
      pipe->end_query(pipe, state->active_oq);
      pipe->destroy_query(pipe, state->active_oq);
      state->active_oq = NULL;
   }
   r3v_dyn_overlay_cleanup(device, &state->dyn_ov);
}

static void
r3v_replay_state_rebind(struct r3v_device *device,
                           struct r3v_replay_state *state)
{
   /* last_bind_dsets references this state's own accum_dsets.  A per-tile replay
    * copies the whole state by value, which leaves the pointer aimed at the
    * source state's accum_dsets; repoint it here, where every segment and tile
    * re-enters, so a draw reads the descriptor sets accumulated in this state. */
   if (state->last_bind_dsets)
      state->last_bind_dsets = &state->accum_dsets;

   if (!state->bound_pipeline)
      return;

   struct pipe_context *pipe = device->pipe;
   const struct r3v_pipeline *pl = state->bound_pipeline;
   pipe->bind_blend_state(pipe, pl->blend_cso);
   pipe->bind_rasterizer_state(pipe, pl->rasterizer_cso);
   pipe->bind_depth_stencil_alpha_state(pipe, pl->dsa_cso);
   pipe->bind_vs_state(pipe, pl->vs_cso);
   pipe->bind_fs_state(pipe, pl->fs_cso);
   pipe->bind_vertex_elements_state(pipe, pl->velems_cso);
   state->vb_dirty = true;
   state->dyn_ov.dirty = true;
}

/* Sample the colour attachment after the pass's GPU work flushes: a grid of
 * 16 texels plus a count of texels differing from the corner.  Distinguishes
 * "geometry never landed" (uniform clear colour) from "present path loses
 * content" (varied pixels here, black on screen). */
static void
r3v_dbg_log_attachment_pixels(struct r3v_device *device,
                                 const struct r3v_cmd_entry *rp,
                                 unsigned tile_pass,
                                 uint32_t tile_origin_x,
                                 uint32_t tile_origin_y,
                                 uint32_t tile_width,
                                 uint32_t tile_height)
{
   struct pipe_context *pipe = device->pipe;
   const struct r3v_image *img = r3v_begin_rp_ref_image(&rp->begin_rp);
   if (!img)
      return;

   uint32_t attachment_tile_origin_x = 0;
   uint32_t attachment_tile_origin_y = 0;
   uint32_t attachment_remaining_width = 0;
   uint32_t attachment_remaining_height = 0;
   struct pipe_resource *tex =
      r3v_image_tile_resource_for_origin(img, tile_origin_x, tile_origin_y,
                                            &attachment_tile_origin_x,
                                            &attachment_tile_origin_y,
                                            &attachment_remaining_width,
                                            &attachment_remaining_height);
   if (!tex || !tile_width || !tile_height)
      return;

   pipe->flush(pipe, NULL, 0);

   struct pipe_box box;
   u_box_2d(0, 0, tile_width, tile_height, &box);
   struct pipe_transfer *xfer = NULL;
   const uint8_t *map = pipe->texture_map(pipe, tex, 0, PIPE_MAP_READ,
                                          &box, &xfer);
   if (!map)
      return;

   const unsigned bpp = util_format_get_blocksize(tex->format);
   uint32_t corner = 0;
   memcpy(&corner, map, MIN2(bpp, 4));
   unsigned diff = 0;
   uint32_t samples[16];
   for (unsigned i = 0; i < 16; i++) {
      const unsigned sx = (tile_width  * (i % 4)) / 4 + tile_width / 8;
      const unsigned sy = (tile_height * (i / 4)) / 4 + tile_height / 8;
      uint32_t px = 0;
      memcpy(&px, map + sy * xfer->stride + sx * bpp, MIN2(bpp, 4));
      samples[i] = px;
      if (px != corner)
         diff++;
   }
   pipe->texture_unmap(pipe, xfer);

   fprintf(stderr, "r3v pixels: tile=%u %ux%u corner=%08x diff=%u "
           "s=[%08x %08x %08x %08x ...]\n",
           tile_pass, tile_width, tile_height, corner, diff,
           samples[0], samples[5], samples[10], samples[15]);
}

static bool
r3v_entry_begins_render_pass(const struct r3v_cmd_entry *entry)
{
   return entry->type == R3V_CMD_BEGIN_RENDER_PASS;
}

static uint32_t
r3v_render_pass_segment_end(const struct r3v_cmd_buffer *cmd,
                               uint32_t begin_entry)
{
   for (uint32_t i = begin_entry; i < cmd->entry_count; i++)
      if (cmd->entries[i].type == R3V_CMD_END_RENDER_PASS)
         return i + 1;

   return cmd->entry_count;
}

static uint32_t
r3v_render_pass_segment_tile_pass_count(const struct r3v_cmd_buffer *cmd,
                                           uint32_t begin_entry,
                                           uint32_t end_entry)
{
   uint32_t pass_count = 1;

   for (uint32_t i = begin_entry; i < end_entry; i++) {
      const struct r3v_cmd_entry *entry = &cmd->entries[i];
      if (entry->type != R3V_CMD_BEGIN_RENDER_PASS &&
          entry->type != R3V_CMD_NEXT_SUBPASS)
         continue;

      pass_count = MAX2(pass_count,
                        r3v_begin_rp_tile_pass_count(&entry->begin_rp));
   }

   return pass_count;
}

static VkResult
r3v_replay_gpu_range(struct r3v_device *device,
                        const struct r3v_cmd_buffer *cmd,
                        uint32_t first_entry,
                        uint32_t end_entry,
                        uint32_t tile_pass,
                        uint32_t cmd_tile_pass_count,
                        struct r3v_replay_state *state,
                        struct util_dynarray *transient_vbs,
                        bool *gpu_pending)
{
   struct pipe_context *pipe = device->pipe;
   uint32_t tile_origin_x = 0;
   uint32_t tile_origin_y = 0;
   uint32_t tile_width = 0;
   uint32_t tile_height = 0;
   bool skip_render_pass = false;
   const struct r3v_cmd_entry *current_render_pass = NULL;

   for (uint32_t i = first_entry; i < end_entry; i++) {
      const struct r3v_cmd_entry *e = &cmd->entries[i];

      switch (e->type) {
      case R3V_CMD_BEGIN_RENDER_PASS:
         r3v_replay_begin_render_pass(device, e, tile_pass,
                                         &tile_origin_x, &tile_origin_y,
                                         &tile_width, &tile_height,
                                         &skip_render_pass);
         current_render_pass = e;
         /* A new subpass is a self-dependency visibility point: the next
          * self-dependent input bind re-copies its snapshot. */
         device->ia_snapshot_stale = true;
         /* The pass boundary can change zsbuf presence, which feeds the
          * depth/stencil clamp; re-overlay at the next draw. */
         state->dyn_ov.dirty = true;
         /* Re-apply state recorded before the pass began: its tile
          * translation ran against zero tile dimensions (empty scissor). */
         if (!skip_render_pass) {
            if (state->last_viewport)
               r3v_replay_set_viewport(device, state->last_viewport,
                                          tile_origin_x, tile_origin_y);
            if (state->last_scissor)
               r3v_replay_set_scissor(device, state->last_scissor,
                                         tile_origin_x, tile_origin_y,
                                         tile_width, tile_height);
         }
         /* Only loadOp == CLEAR emits a GPU write at begin (a color-image
          * clear on any attachment) that a later host copy-image-to-buffer
          * could read; a LOAD pass emits nothing here, and its draws set
          * gpu_pending themselves.  Gating avoids a needless drain before an
          * in-order host transfer. */
         for (uint32_t slot = 0; slot < e->begin_rp.color_count; slot++)
            if (e->begin_rp.load_op[slot] == VK_ATTACHMENT_LOAD_OP_CLEAR) {
               *gpu_pending = true;
               break;
            }
         break;

      case R3V_CMD_NEXT_SUBPASS:
         /* Render-to-texture barrier so this subpass samples the prior
          * subpass's output coherently as an input attachment.
          * PIPE_TEXTURE_BARRIER_SAMPLER marks r300's gpu_flush and
          * texture_cache_inval atoms dirty (r300_texture_barrier); the
          * gpu_flush atom flushes the RB3D color cache to memory and the
          * texcache-invalidate atom drops stale texture lines, both emitted
          * before this subpass's first draw -- so the sample reads the prior
          * subpass's writes.  The RB3D cache is physical, not framebuffer-
          * scoped, so the flush resolves the prior color writes even though
          * the framebuffer is rebound below.  Then bind this subpass's
          * framebuffer at the same pass origin and re-apply the in-flight
          * viewport/scissor, mirroring the begin path. */
         device->pipe->texture_barrier(device->pipe,
                                       PIPE_TEXTURE_BARRIER_SAMPLER);
         r3v_replay_begin_render_pass(device, e, tile_pass,
                                         &tile_origin_x, &tile_origin_y,
                                         &tile_width, &tile_height,
                                         &skip_render_pass);
         current_render_pass = e;
         device->ia_snapshot_stale = true;
         state->dyn_ov.dirty = true;
         if (!skip_render_pass) {
            if (state->last_viewport)
               r3v_replay_set_viewport(device, state->last_viewport,
                                          tile_origin_x, tile_origin_y);
            if (state->last_scissor)
               r3v_replay_set_scissor(device, state->last_scissor,
                                         tile_origin_x, tile_origin_y,
                                         tile_width, tile_height);
         }
         for (uint32_t slot = 0; slot < e->begin_rp.color_count; slot++)
            if (e->begin_rp.load_op[slot] == VK_ATTACHMENT_LOAD_OP_CLEAR) {
               *gpu_pending = true;
               break;
            }
         break;

      case R3V_CMD_BIND_PIPELINE:
         if (skip_render_pass) break;
         r3v_replay_bind_pipeline(device, e, &state->bound_pipeline,
                                     &state->vb_dirty);
         /* The bind installed the pipeline's fixed CSOs; re-overlay the
          * merged shadow (and the static stencil ref / blend color) at
          * the next draw. */
         state->dyn_ov.dirty = true;
         break;

      case R3V_CMD_SET_DYNAMIC_STATE:
         /* Pure state, not gated by skip_render_pass: the shadow must be
          * current when a later tile pass or render pass draws. */
         r3v_dyn_overlay_merge(&state->dyn_ov, &e->set_dyn);
         break;

      case R3V_CMD_SET_VIEWPORT:
         state->last_viewport = e;
         if (skip_render_pass) break;
         r3v_replay_set_viewport(device, e, tile_origin_x, tile_origin_y);
         break;

      case R3V_CMD_SET_SCISSOR:
         state->last_scissor = e;
         if (skip_render_pass) break;
         r3v_replay_set_scissor(device, e, tile_origin_x, tile_origin_y,
                                   tile_width, tile_height);
         break;

      case R3V_CMD_BIND_VERTEX_BUFFERS:
         if (skip_render_pass) break;
         r3v_replay_bind_vertex_buffers(device, e, state->vb_cache,
                                           state->vb_sizes, state->vb_strides,
                                           &state->vb_strides_mask,
                                           &state->vb_max_used,
                                           &state->vb_dirty);
         break;

      case R3V_CMD_DRAW:
      case R3V_CMD_DRAW_INDEXED:
         if (skip_render_pass) break;
         r3v_replay_draw(device, e, state->bound_pipeline,
                            state->last_bind_dsets, state->replay_pc,
                            state->vb_cache, state->vb_sizes,
                            state->vb_max_used, &state->vb_dirty,
                            tile_origin_x, tile_origin_y, tile_width,
                            tile_height, transient_vbs, &state->dyn_ov,
                            current_render_pass &&
                            current_render_pass->begin_rp.ds_image,
                            current_render_pass &&
                            current_render_pass->begin_rp.ds_image &&
                            util_format_has_stencil(util_format_description(
                               current_render_pass->begin_rp.ds_format)),
                            current_render_pass &&
                            current_render_pass->begin_rp.input_self_dep,
                            state->vb_strides, state->vb_strides_mask);
         *gpu_pending = true;
         break;

      case R3V_CMD_PUSH_CONSTANTS: {
         /* Apply the window update unconditionally (pure state, not gated by
          * skip_render_pass) so a later tile pass or draw sees it. */
         const struct r3v_cmd_push_constants *pc = &e->push_constants;
         if ((uint64_t)pc->offset + pc->size <= sizeof(state->replay_pc))
            memcpy(state->replay_pc + pc->offset, pc->data, pc->size);
         break;
      }

      case R3V_CMD_DRAW_INDIRECT: {
         if (skip_render_pass) break;
         const struct r3v_cmd_draw_indirect *di = &e->draw_indirect;
         if (!di->buffer || !di->buffer->resource || di->draw_count == 0)
            break;
         /* CPU-read the VkDrawIndirectCommand array (r3v buffers are
          * host-visible) and run the normal draw path per command. */
         const unsigned stride =
            di->stride ? di->stride : (unsigned)sizeof(VkDrawIndirectCommand);
         /* Bound the mapped extent against the buffer before the unsigned
          * cast: an app may record an offset/draw_count that runs past the
          * BO, and pipe_buffer_map_range would otherwise map past the
          * resource (the kernel CS validator rejects the same overflow in
          * evergreen_cs.c evergreen_packet3_check PACKET3_DRAW_INDIRECT).
          * Compute span in 64-bit; the subtract form avoids add overflow. */
         const uint64_t span64 = (uint64_t)(di->draw_count - 1u) * stride +
                                 sizeof(VkDrawIndirectCommand);
         if (di->offset > UINT_MAX || span64 > UINT_MAX ||
             span64 > di->buffer->size ||
             di->offset > di->buffer->size - span64)
            break;
         const unsigned span = (unsigned)span64;
         struct pipe_transfer *ixfer = NULL;
         const uint8_t *imap =
            pipe_buffer_map_range(pipe, di->buffer->resource,
                                  (unsigned)di->offset, span,
                                  PIPE_MAP_READ, &ixfer);
         if (!imap) break;
         for (uint32_t d = 0; d < di->draw_count; d++) {
            const VkDrawIndirectCommand *args =
               (const VkDrawIndirectCommand *)(imap + (size_t)d * stride);
            struct r3v_cmd_entry synth;
            synth.type                = R3V_CMD_DRAW;
            synth.draw.count          = args->vertexCount;
            synth.draw.instances      = args->instanceCount;
            synth.draw.first          = args->firstVertex;
            synth.draw.first_instance = args->firstInstance;
            synth.draw.topology       = di->topology;
            r3v_replay_draw(device, &synth, state->bound_pipeline,
                               state->last_bind_dsets, state->replay_pc,
                               state->vb_cache, state->vb_sizes,
                               state->vb_max_used, &state->vb_dirty,
                               tile_origin_x, tile_origin_y, tile_width,
                               tile_height, transient_vbs, &state->dyn_ov,
                               current_render_pass &&
                               current_render_pass->begin_rp.ds_image,
                               current_render_pass &&
                               current_render_pass->begin_rp.ds_image &&
                               util_format_has_stencil(util_format_description(
                                  current_render_pass->begin_rp.ds_format)),
                               current_render_pass &&
                               current_render_pass->begin_rp.input_self_dep,
                               state->vb_strides, state->vb_strides_mask);
         }
         pipe_buffer_unmap(pipe, ixfer);
         *gpu_pending = true;
         break;
      }

      case R3V_CMD_DRAW_INDEXED_INDIRECT: {
         if (skip_render_pass) break;
         const struct r3v_cmd_draw_indexed_indirect *di =
            &e->draw_indexed_indirect;
         if (!di->buffer || !di->buffer->resource || di->draw_count == 0)
            break;
         /* CPU-read the VkDrawIndexedIndirectCommand array (r3v buffers are
          * host-visible) and run the indexed draw path per command, exactly as
          * R3V_CMD_DRAW_INDIRECT does for the non-indexed form. */
         const unsigned stride =
            di->stride ? di->stride
                       : (unsigned)sizeof(VkDrawIndexedIndirectCommand);
         /* Bound the mapped extent against the buffer before the unsigned cast;
          * the kernel CS validator rejects the same overrun in evergreen_cs.c
          * evergreen_packet3_check.  Compute span in 64-bit; the subtract form
          * avoids add overflow. */
         const uint64_t span64 =
            (uint64_t)(di->draw_count - 1u) * stride +
            sizeof(VkDrawIndexedIndirectCommand);
         if (di->offset > UINT_MAX || span64 > UINT_MAX ||
             span64 > di->buffer->size ||
             di->offset > di->buffer->size - span64)
            break;
         const unsigned span = (unsigned)span64;
         struct pipe_transfer *ixfer = NULL;
         const uint8_t *imap =
            pipe_buffer_map_range(pipe, di->buffer->resource,
                                  (unsigned)di->offset, span,
                                  PIPE_MAP_READ, &ixfer);
         if (!imap) break;
         for (uint32_t d = 0; d < di->draw_count; d++) {
            const VkDrawIndexedIndirectCommand *args =
               (const VkDrawIndexedIndirectCommand *)(imap +
                                                      (size_t)d * stride);
            struct r3v_cmd_entry synth;
            synth.type                        = R3V_CMD_DRAW_INDEXED;
            synth.draw_indexed.index_buffer   = di->index_buffer;
            synth.draw_indexed.index_offset   = di->index_offset;
            synth.draw_indexed.index_range    = di->index_range;
            synth.draw_indexed.index_size     = di->index_size;
            synth.draw_indexed.index_count    = args->indexCount;
            synth.draw_indexed.first_index    = args->firstIndex;
            synth.draw_indexed.vertex_offset  = args->vertexOffset;
            synth.draw_indexed.instances      = args->instanceCount;
            synth.draw_indexed.first_instance = args->firstInstance;
            synth.draw_indexed.topology       = di->topology;
            r3v_replay_draw(device, &synth, state->bound_pipeline,
                               state->last_bind_dsets, state->replay_pc,
                               state->vb_cache, state->vb_sizes,
                               state->vb_max_used, &state->vb_dirty,
                               tile_origin_x, tile_origin_y, tile_width,
                               tile_height, transient_vbs, &state->dyn_ov,
                               current_render_pass &&
                               current_render_pass->begin_rp.ds_image,
                               current_render_pass &&
                               current_render_pass->begin_rp.ds_image &&
                               util_format_has_stencil(util_format_description(
                                  current_render_pass->begin_rp.ds_format)),
                               current_render_pass &&
                               current_render_pass->begin_rp.input_self_dep,
                               state->vb_strides, state->vb_strides_mask);
         }
         pipe_buffer_unmap(pipe, ixfer);
         *gpu_pending = true;
         break;
      }

      case R3V_CMD_END_RENDER_PASS:
         if (device->dbg_log_pixels && !skip_render_pass &&
             current_render_pass &&
             r3v_begin_rp_ref_image(&current_render_pass->begin_rp))
            r3v_dbg_log_attachment_pixels(device,
                                             current_render_pass,
                                             tile_pass,
                                             tile_origin_x,
                                             tile_origin_y,
                                             tile_width, tile_height);
         r3v_replay_end_render_pass(device, &skip_render_pass,
                                       &tile_origin_x, &tile_origin_y,
                                       &tile_width, &tile_height);
         current_render_pass = NULL;
         break;

      case R3V_CMD_CLEAR_ATTACHMENTS:
         if (skip_render_pass) break;
         r3v_replay_clear_attachments(device, current_render_pass,
                                         &e->clear_attachments,
                                         tile_origin_x, tile_origin_y,
                                         tile_width, tile_height);
         *gpu_pending = true;
         break;

      case R3V_CMD_COPY_IMAGE_TO_BUFFER:
      case R3V_CMD_COPY_BUFFER_TO_IMAGE:
      case R3V_CMD_COPY_IMAGE:
      case R3V_CMD_CLEAR_COLOR_IMAGE:
      case R3V_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
      case R3V_CMD_FILL_BUFFER:
      case R3V_CMD_COPY_BUFFER:
      case R3V_CMD_UPDATE_BUFFER:
      case R3V_CMD_COPY_QUERY_POOL_RESULTS:
      case R3V_CMD_SET_EVENT:
      case R3V_CMD_RESET_EVENT:
         if (current_render_pass)
            break;
         r3v_drain_gpu(device, gpu_pending);
         r3v_replay_host_entry(device, e);
         break;

      case R3V_CMD_PIPELINE_BARRIER:
         r3v_replay_pipeline_barrier(device, e, skip_render_pass);
         /* An in-pass barrier is the self-dependency visibility point: the
          * next self-dependent input bind re-copies its snapshot so the read
          * sees the writes this barrier makes visible. */
         if (current_render_pass)
            device->ia_snapshot_stale = true;
         break;

      case R3V_CMD_BIND_DESCRIPTOR_SETS:
         r3v_replay_bind_descriptor_sets(&state->accum_dsets, e);
         state->last_bind_dsets = &state->accum_dsets;
         break;

      case R3V_CMD_DISPATCH: {
         if (current_render_pass)
            break;
         VkResult result =
            r3v_replay_dispatch(device, e, state->last_bind_dsets,
                                   state->replay_pc);
         if (result != VK_SUCCESS)
            return result;
         pipe->flush(pipe, NULL, 0);
         *gpu_pending = true;
         break;
      }

      case R3V_CMD_BLIT_IMAGE:
         if (current_render_pass)
            break;
         r3v_replay_blit(device, e);
         pipe->flush(pipe, NULL, 0);
         *gpu_pending = true;
         break;

      case R3V_CMD_BEGIN_QUERY:
         /* Occlusion only, single-tile command buffers only: a multi-tile render
          * pass would re-walk the query entries, but r300 allows one query at a
          * time.  Map the Vulkan occlusion type to the r300 ZPASS counter and
          * bracket the draws that follow. */
         if (cmd_tile_pass_count != 1 || state->active_oq != NULL ||
             e->query.pool->vk.query_type != VK_QUERY_TYPE_OCCLUSION)
            break;
         state->active_oq =
            pipe->create_query(pipe, PIPE_QUERY_OCCLUSION_COUNTER, 0);
         if (state->active_oq)
            pipe->begin_query(pipe, state->active_oq);
         break;

      case R3V_CMD_END_QUERY: {
         if (!state->active_oq)
            break;
         pipe->end_query(pipe, state->active_oq);
         /* wait=true flushes and fences, so the sample count is ready to
          * store into the pool slot the host later reads. */
         union pipe_query_result qres;
         memset(&qres, 0, sizeof(qres));
         if (pipe->get_query_result(pipe, state->active_oq, true, &qres) &&
             e->query.query < e->query.pool->vk.query_count) {
            struct r3v_query *slot =
               &e->query.pool->queries[e->query.query];
            slot->result    = qres.u64;
            slot->available = true;
         }
         pipe->destroy_query(pipe, state->active_oq);
         state->active_oq = NULL;
         break;
      }

      case R3V_CMD_RESET_QUERY_POOL: {
         /* Host bookkeeping, applied once per submit (tile-independent):
          * clear availability and result over the reset range. */
         if (current_render_pass)
            break;
         struct r3v_query_pool *qp = e->reset_query_pool.pool;
         const uint32_t first = e->reset_query_pool.first_query;
         uint32_t n = e->reset_query_pool.query_count;
         if (first < qp->vk.query_count) {
            if (n > qp->vk.query_count - first)
               n = qp->vk.query_count - first;
            for (uint32_t k = 0; k < n; k++) {
               qp->queries[first + k].result    = 0;
               qp->queries[first + k].available = false;
            }
         }
         break;
      }

      default: break;
      }
   }

   return VK_SUCCESS;
}

/* Replays one command buffer's GPU work.  Host transfers run in submission order
 * by splitting the stream around render-pass segments: outside-pass commands run
 * once when reached, while each render-pass segment alone is re-walked per tile.
 * gpu_pending is threaded across the whole submit so a host transfer in a later
 * command buffer drains GPU work emitted by an earlier one. */
static VkResult
r3v_replay_gpu(struct r3v_device *device,
                  const struct r3v_cmd_buffer *cmd,
                  struct util_dynarray *transient_vbs,
                  bool *gpu_pending)
{
   VkResult result = VK_SUCCESS;
   const uint32_t cmd_tile_pass_count = r3v_cmd_tile_pass_count(cmd);
   struct r3v_replay_state state;
   memset(&state, 0, sizeof(state));

   for (uint32_t i = 0; i < cmd->entry_count;) {
      const uint32_t outside_begin = i;
      while (i < cmd->entry_count &&
             !r3v_entry_begins_render_pass(&cmd->entries[i]))
         i++;

      if (outside_begin < i) {
         result = r3v_replay_gpu_range(device, cmd, outside_begin, i, 0,
                                          cmd_tile_pass_count, &state,
                                          transient_vbs, gpu_pending);
         r3v_dyn_overlay_cleanup(device, &state.dyn_ov);
         if (result != VK_SUCCESS)
            break;
      }

      if (i >= cmd->entry_count)
         break;

      const uint32_t segment_begin = i;
      const uint32_t segment_end =
         r3v_render_pass_segment_end(cmd, segment_begin);
      const uint32_t segment_tile_pass_count =
         r3v_render_pass_segment_tile_pass_count(cmd, segment_begin,
                                                    segment_end);

      if (segment_tile_pass_count == 1) {
         r3v_replay_state_rebind(device, &state);
         result = r3v_replay_gpu_range(device, cmd, segment_begin,
                                          segment_end, 0, cmd_tile_pass_count,
                                          &state,
                                          transient_vbs, gpu_pending);
         r3v_dyn_overlay_cleanup(device, &state.dyn_ov);
         if (result != VK_SUCCESS)
            break;
      } else {
         for (uint32_t tile_pass = 0; tile_pass < segment_tile_pass_count;
              tile_pass++) {
            struct r3v_replay_state tile_state = state;
            r3v_replay_state_rebind(device, &tile_state);
            result = r3v_replay_gpu_range(device, cmd, segment_begin,
                                             segment_end, tile_pass,
                                             cmd_tile_pass_count, &tile_state,
                                             transient_vbs, gpu_pending);
            r3v_dyn_overlay_cleanup(device, &tile_state.dyn_ov);
            if (result != VK_SUCCESS)
               break;
            if (tile_pass + 1 == segment_tile_pass_count)
               state = tile_state;
         }
         if (result != VK_SUCCESS)
            break;
      }

      i = segment_end;
   }
   r3v_replay_state_cleanup(device, &state);
   return result;
}

static bool
r3v_linear_region_span(const VkExtent3D *extent,
                          VkDeviceSize buffer_offset,
                          uint32_t buffer_row_length,
                          unsigned bpp,
                          unsigned block_w,
                          unsigned block_h,
                          unsigned *row_pitch_out,
                          unsigned *span_out)
{
   if (extent->width == 0 || extent->height == 0 || bpp == 0)
      return false;

   /* bpp is bytes per format block; extents and bufferRowLength are texels
    * (Vulkan buffer-image addressing), so a block-compressed format walks
    * DIV_ROUND_UP(texels, block) block columns and rows.  Plain formats have
    * 1x1 blocks and reduce to the texel arithmetic. */
   const uint64_t row_texels =
      buffer_row_length ? buffer_row_length : extent->width;
   const uint64_t row_pitch = DIV_ROUND_UP(row_texels, block_w) * bpp;
   const uint64_t last_row_bytes =
      (uint64_t)DIV_ROUND_UP(extent->width, block_w) * bpp;
   const uint64_t block_rows = DIV_ROUND_UP(extent->height, block_h);
   const uint64_t span = (block_rows - 1) * row_pitch + last_row_bytes;

   if (row_pitch > UINT_MAX || span > UINT_MAX ||
       buffer_offset > UINT_MAX ||
       buffer_offset > UINT_MAX - span)
      return false;

   *row_pitch_out = (unsigned)row_pitch;
   *span_out = (unsigned)span;
   return true;
}

/* Depth/stencil buffer<->image transfers need a per-texel repack because r300
 * backs VK_FORMAT_D24_UNORM_S8_UINT / VK_FORMAT_X8_D24_UNORM_PACK32 with the
 * depth-high twins (S8_UINT_Z24_UNORM / X8Z24_UNORM): the r300 image word stores
 * depth in bits [31:8] and stencil/pad in [7:0] (util_pack_z shifts depth << 8),
 * while Vulkan's depth-aspect buffer element holds the 24-bit depth in bits
 * [23:0] and the stencil aspect is a single byte.  This descriptor gives the two
 * element sizes -- img_bpp is the r300 word, buf_bpp is the Vulkan buffer element
 * for the copied aspect -- and the repack kind.  Z16_UNORM and the depth-low
 * Z24_UNORM_S8 twin need no repack (PLAIN); colour formats are always PLAIN with
 * buf_bpp == img_bpp, so the existing memcpy path is unchanged. */
enum r3v_zs_copy_kind {
   R3V_ZS_PLAIN = 0,
   R3V_ZS_DEPTH,
   R3V_ZS_STENCIL,
};
struct r3v_zs_copy {
   unsigned img_bpp;
   unsigned buf_bpp;
   enum r3v_zs_copy_kind kind;
};

static struct r3v_zs_copy
r3v_zs_copy_for(enum pipe_format img_fmt, VkImageAspectFlags aspect)
{
   const unsigned blk = util_format_get_blocksize(img_fmt);
   struct r3v_zs_copy z = { .img_bpp = blk, .buf_bpp = blk,
                               .kind = R3V_ZS_PLAIN };
   if (img_fmt == PIPE_FORMAT_S8_UINT_Z24_UNORM) {
      if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
         z.buf_bpp = 1;            /* Vulkan stencil aspect is one byte */
         z.kind = R3V_ZS_STENCIL;
      } else {
         z.kind = R3V_ZS_DEPTH; /* depth aspect is 32-bit, buf_bpp == 4 */
      }
   } else if (img_fmt == PIPE_FORMAT_X8Z24_UNORM) {
      z.kind = R3V_ZS_DEPTH;    /* depth-only twin, no stencil aspect */
   }
   return z;
}

/* image word -> Vulkan buffer element (readback). */
static inline void
r3v_zs_unpack_texel(enum r3v_zs_copy_kind kind,
                       const uint8_t *img, uint8_t *buf)
{
   uint32_t w;
   memcpy(&w, img, sizeof(w));
   if (kind == R3V_ZS_DEPTH) {
      const uint32_t depth = (w >> 8) & 0x00FFFFFFu;
      memcpy(buf, &depth, sizeof(depth));
   } else { /* R3V_ZS_STENCIL */
      buf[0] = (uint8_t)(w & 0xFFu);
   }
}

/* Vulkan buffer element -> image word (upload), read-modify-write so the aspect
 * not being copied keeps its bits. */
static inline void
r3v_zs_pack_texel(enum r3v_zs_copy_kind kind,
                     const uint8_t *buf, uint8_t *img)
{
   uint32_t w;
   memcpy(&w, img, sizeof(w));
   if (kind == R3V_ZS_DEPTH) {
      uint32_t depth;
      memcpy(&depth, buf, sizeof(depth));
      w = (w & 0x000000FFu) | ((depth & 0x00FFFFFFu) << 8);
   } else { /* R3V_ZS_STENCIL */
      w = (w & 0xFFFFFF00u) | buf[0];
   }
   memcpy(img, &w, sizeof(w));
}

static bool
r3v_copy_image_region_to_buffer(struct r3v_device *device,
                                   const struct r3v_image *src_img,
                                   struct pipe_resource *dst,
                                   const VkBufferImageCopy2 *region)
{
   struct pipe_context *pipe = device->pipe;

   /* Row pitch drives both the mapping size and the destination stride.
    * Use the actual format block size rather than assuming 4 bpp so that
    * non-RGBA8 formats (R8, RG16, RGBA16F) map and copy correctly.
    * bufferRowLength == 0 means tightly packed per the Vulkan spec. */
   /* For a depth/stencil aspect the Vulkan buffer element (buf_bpp) differs from
    * the r300 image word (img_bpp) and the texels need repacking; for colour
    * formats this is a plain copy with buf_bpp == img_bpp. */
   const struct r3v_zs_copy zs =
      r3v_zs_copy_for(src_img->resource->format,
                         region->imageSubresource.aspectMask);
   const unsigned bpp = zs.buf_bpp;
   const unsigned bw = util_format_get_blockwidth(src_img->resource->format);
   const unsigned bh = util_format_get_blockheight(src_img->resource->format);
   unsigned row_pitch = 0;
   unsigned dst_size = 0;
   if (!r3v_linear_region_span(&region->imageExtent,
                                  region->bufferOffset,
                                  region->bufferRowLength,
                                  bpp, bw, bh, &row_pitch, &dst_size) ||
       (uint64_t)region->bufferOffset + dst_size > dst->width0)
      return false;

   struct pipe_transfer *dst_xfer = NULL;
   uint8_t *dst_map = pipe_buffer_map_range(pipe, dst,
                                            (unsigned)region->bufferOffset,
                                            dst_size,
                                            PIPE_MAP_WRITE,
                                            &dst_xfer);
   if (!dst_map)
      return false;

   /* All rect arithmetic below runs in BLOCK space: offsets are block-aligned
    * per the Vulkan compressed-copy rules, extents block-ceil at image edges,
    * and plain formats reduce to texels through their 1x1 blocks. */
   const int64_t req_min_x = region->imageOffset.x / bw;
   const int64_t req_min_y = region->imageOffset.y / bh;
   const int64_t req_max_x =
      req_min_x + DIV_ROUND_UP(region->imageExtent.width, bw);
   const int64_t req_max_y =
      req_min_y + DIV_ROUND_UP(region->imageExtent.height, bh);

   /* Mip chains exist only on single-tile images (origin 0), so scaling the
    * tile dimensions by the level keeps the walk correct on every level. */
   const uint32_t src_mip = region->imageSubresource.mipLevel;

   for (uint32_t tile_row = 0; tile_row < src_img->tile_rows; tile_row++) {
      const uint32_t tile_origin_y =
         r3v_image_tile_origin_y(src_img, tile_row);
      const uint32_t tile_h_eff =
         MAX2(src_img->tile_height[tile_row] >> src_mip, 1u);
      const int64_t tile_min_y = tile_origin_y / bh;
      const int64_t tile_max_y =
         tile_min_y + DIV_ROUND_UP(tile_h_eff, bh);
      const int64_t copy_min_y = MAX2(req_min_y, tile_min_y);
      const int64_t copy_max_y = MIN2(req_max_y, tile_max_y);
      if (copy_max_y <= copy_min_y)
         continue;

      for (uint32_t tile_col = 0; tile_col < src_img->tile_cols; tile_col++) {
         const uint32_t tile_origin_x =
            r3v_image_tile_origin_x(src_img, tile_col);
         const uint32_t tile_w_eff =
            MAX2(src_img->tile_width[tile_col] >> src_mip, 1u);
         const int64_t tile_min_x = tile_origin_x / bw;
         const int64_t tile_max_x =
            tile_min_x + DIV_ROUND_UP(tile_w_eff, bw);
         const int64_t copy_min_x = MAX2(req_min_x, tile_min_x);
         const int64_t copy_max_x = MIN2(req_max_x, tile_max_x);
         if (copy_max_x <= copy_min_x)
            continue;

         const uint32_t tile_index = tile_row * src_img->tile_cols + tile_col;
         struct pipe_resource *src = src_img->tiles[tile_index];
         if (!src)
            continue;

         /* The gallium box is texel-addressed; block-ceil extents clamp to
          * the tile's texel size at image edges. */
         struct pipe_box src_box;
         u_box_2d((int)((copy_min_x - tile_min_x) * bw),
                  (int)((copy_min_y - tile_min_y) * bh),
                  (int)MIN2((copy_max_x - copy_min_x) * bw,
                            tile_w_eff - (copy_min_x - tile_min_x) * bw),
                  (int)MIN2((copy_max_y - copy_min_y) * bh,
                            tile_h_eff - (copy_min_y - tile_min_y) * bh),
                  &src_box);

         struct pipe_transfer *src_xfer = NULL;
         const uint8_t *src_map =
            pipe->texture_map(pipe, src,
                              region->imageSubresource.mipLevel,
                              PIPE_MAP_READ,
                              &src_box, &src_xfer);
         if (!src_map)
            continue;

         uint8_t *tile_dst =
            dst_map +
            (copy_min_y - req_min_y) * row_pitch +
            (copy_min_x - req_min_x) * bpp;
         const unsigned copy_width_texels = copy_max_x - copy_min_x;
         const unsigned copy_width_bytes = copy_width_texels * bpp;
         const unsigned copy_height = copy_max_y - copy_min_y;
         for (unsigned row = 0; row < copy_height; row++) {
            uint8_t *drow = tile_dst + row * row_pitch;
            const uint8_t *srow = src_map + row * src_xfer->stride;
            if (zs.kind == R3V_ZS_PLAIN) {
               memcpy(drow, srow, copy_width_bytes);
            } else {
               for (unsigned c = 0; c < copy_width_texels; c++)
                  r3v_zs_unpack_texel(zs.kind, srow + c * zs.img_bpp,
                                         drow + c * zs.buf_bpp);
            }
         }

         pipe->texture_unmap(pipe, src_xfer);
      }
   }

   pipe->buffer_unmap(pipe, dst_xfer);
   return true;
}

/* The inverse of r3v_copy_image_region_to_buffer: a tile-iterated CPU upload.
 * Map the source buffer once, then for every destination image tile touched by
 * the region, map that tile PIPE_MAP_WRITE and copy the matching linear rows.
 * Iterating tiles keeps split images correct instead of writing only tile zero. */
static bool
r3v_copy_buffer_region_to_image(struct r3v_device *device,
                                   struct pipe_resource *src,
                                   const struct r3v_image *dst_img,
                                   const VkBufferImageCopy2 *region)
{
   struct pipe_context *pipe = device->pipe;
   r3v_image_mark_written(dst_img);

   /* Depth/stencil aspect uploads repack per texel (see r3v_zs_copy_for) and
    * are read-modify-write so the aspect not being copied keeps its bits; the
    * tile must therefore be mapped readable.  Colour formats stay a plain,
    * write-only copy with buf_bpp == img_bpp. */
   const struct r3v_zs_copy zs =
      r3v_zs_copy_for(dst_img->resource->format,
                         region->imageSubresource.aspectMask);
   const unsigned bpp = zs.buf_bpp;
   const unsigned bw = util_format_get_blockwidth(dst_img->resource->format);
   const unsigned bh = util_format_get_blockheight(dst_img->resource->format);
   const unsigned dst_map_flags =
      zs.kind == R3V_ZS_PLAIN ? PIPE_MAP_WRITE : PIPE_MAP_READ_WRITE;
   unsigned row_pitch = 0;
   unsigned src_size = 0;
   if (!r3v_linear_region_span(&region->imageExtent,
                                  region->bufferOffset,
                                  region->bufferRowLength,
                                  bpp, bw, bh, &row_pitch, &src_size) ||
       (uint64_t)region->bufferOffset + src_size > src->width0)
      return false;

   struct pipe_transfer *src_xfer = NULL;
   const uint8_t *src_map = pipe_buffer_map_range(pipe, src,
                                                  (unsigned)region->bufferOffset,
                                                  src_size,
                                                  PIPE_MAP_READ,
                                                  &src_xfer);
   if (!src_map)
      return false;

   /* BLOCK-space rect arithmetic, mirroring the image -> buffer walker. */
   const int64_t req_min_x = region->imageOffset.x / bw;
   const int64_t req_min_y = region->imageOffset.y / bh;
   const int64_t req_max_x =
      req_min_x + DIV_ROUND_UP(region->imageExtent.width, bw);
   const int64_t req_max_y =
      req_min_y + DIV_ROUND_UP(region->imageExtent.height, bh);

   const uint32_t dst_mip = region->imageSubresource.mipLevel;

   for (uint32_t tile_row = 0; tile_row < dst_img->tile_rows; tile_row++) {
      const uint32_t tile_origin_y =
         r3v_image_tile_origin_y(dst_img, tile_row);
      const uint32_t tile_h_eff =
         MAX2(dst_img->tile_height[tile_row] >> dst_mip, 1u);
      const int64_t tile_min_y = tile_origin_y / bh;
      const int64_t tile_max_y =
         tile_min_y + DIV_ROUND_UP(tile_h_eff, bh);
      const int64_t copy_min_y = MAX2(req_min_y, tile_min_y);
      const int64_t copy_max_y = MIN2(req_max_y, tile_max_y);
      if (copy_max_y <= copy_min_y)
         continue;

      for (uint32_t tile_col = 0; tile_col < dst_img->tile_cols; tile_col++) {
         const uint32_t tile_origin_x =
            r3v_image_tile_origin_x(dst_img, tile_col);
         const uint32_t tile_w_eff =
            MAX2(dst_img->tile_width[tile_col] >> dst_mip, 1u);
         const int64_t tile_min_x = tile_origin_x / bw;
         const int64_t tile_max_x =
            tile_min_x + DIV_ROUND_UP(tile_w_eff, bw);
         const int64_t copy_min_x = MAX2(req_min_x, tile_min_x);
         const int64_t copy_max_x = MIN2(req_max_x, tile_max_x);
         if (copy_max_x <= copy_min_x)
            continue;

         const uint32_t tile_index = tile_row * dst_img->tile_cols + tile_col;
         struct pipe_resource *dst = dst_img->tiles[tile_index];
         if (!dst)
            continue;

         struct pipe_box dst_box;
         u_box_2d((int)((copy_min_x - tile_min_x) * bw),
                  (int)((copy_min_y - tile_min_y) * bh),
                  (int)MIN2((copy_max_x - copy_min_x) * bw,
                            tile_w_eff - (copy_min_x - tile_min_x) * bw),
                  (int)MIN2((copy_max_y - copy_min_y) * bh,
                            tile_h_eff - (copy_min_y - tile_min_y) * bh),
                  &dst_box);

         struct pipe_transfer *dst_xfer = NULL;
         uint8_t *dst_map =
            pipe->texture_map(pipe, dst,
                              region->imageSubresource.mipLevel,
                              dst_map_flags,
                              &dst_box, &dst_xfer);
         if (!dst_map)
            continue;

         const uint8_t *tile_src =
            src_map +
            (copy_min_y - req_min_y) * row_pitch +
            (copy_min_x - req_min_x) * bpp;
         const unsigned copy_width_texels = copy_max_x - copy_min_x;
         const unsigned copy_width_bytes = copy_width_texels * bpp;
         const unsigned copy_height = copy_max_y - copy_min_y;
         for (unsigned row = 0; row < copy_height; row++) {
            const uint8_t *srow = tile_src + row * row_pitch;
            uint8_t *drow = dst_map + row * dst_xfer->stride;
            if (zs.kind == R3V_ZS_PLAIN) {
               memcpy(drow, srow, copy_width_bytes);
            } else {
               for (unsigned c = 0; c < copy_width_texels; c++)
                  r3v_zs_pack_texel(zs.kind, srow + c * zs.buf_bpp,
                                       drow + c * zs.img_bpp);
            }
         }

         pipe->texture_unmap(pipe, dst_xfer);
      }
   }

   pipe->buffer_unmap(pipe, src_xfer);
   return true;
}

/* vkCmdCopyImage2 as image -> linear staging buffer -> image.  The staging
 * buffer lets the existing source and destination tile walks run independently,
 * avoiding a fragile source-tile x destination-tile cross product. */
static bool
r3v_copy_image_region_to_image(struct r3v_device *device,
                                  const struct r3v_image *src_img,
                                  const struct r3v_image *dst_img,
                                  const VkImageCopy2 *region)
{
   struct pipe_context *pipe = device->pipe;
   /* zink can translate glCopyPixels on an unacquired swapchain into a copy whose
    * source is a VK_NULL_HANDLE image (the swapchain image was never acquired, so
    * its backing resource is null).  A null VkImage in vkCmdCopyImage is invalid
    * per the spec; skip the copy rather than dereferencing the null image. */
   if (!src_img || !dst_img || !src_img->resource || !dst_img->resource)
      return false;
   r3v_image_mark_written(dst_img);
   const unsigned src_bpp = util_format_get_blocksize(src_img->resource->format);
   const unsigned dst_bpp = util_format_get_blocksize(dst_img->resource->format);
   const unsigned src_bw = util_format_get_blockwidth(src_img->resource->format);
   const unsigned src_bh = util_format_get_blockheight(src_img->resource->format);
   const unsigned dst_bw = util_format_get_blockwidth(dst_img->resource->format);
   const unsigned dst_bh = util_format_get_blockheight(dst_img->resource->format);

   /* Vulkan only permits image copies between formats of equal block byte
    * size; one block of the compressed side corresponds to one texel of the
    * uncompressed side. */
   if (src_bpp == 0 || src_bpp != dst_bpp)
      return false;

   unsigned row_pitch = 0;
   unsigned staging_size = 0;
   if (!r3v_linear_region_span(&region->extent, 0, 0, src_bpp,
                                  src_bw, src_bh,
                                  &row_pitch, &staging_size) ||
       row_pitch == 0)
      return false;

   /* The copy extent is in source texels; the destination consumes the same
    * BLOCK count, so its texel extent scales by the block-dimension ratio
    * (compressed -> plain shrinks by the source block, plain -> compressed
    * grows by the destination block). */
   const VkExtent3D dst_extent = {
      .width  = DIV_ROUND_UP(region->extent.width, src_bw) * dst_bw,
      .height = DIV_ROUND_UP(region->extent.height, src_bh) * dst_bh,
      .depth  = region->extent.depth,
   };

   struct pipe_resource tmpl;
   memset(&tmpl, 0, sizeof(tmpl));
   tmpl.target     = PIPE_BUFFER;
   tmpl.format     = PIPE_FORMAT_R8_UNORM;
   tmpl.width0     = staging_size;
   tmpl.height0    = 1;
   tmpl.depth0     = 1;
   tmpl.array_size = 1;
   tmpl.usage      = PIPE_USAGE_STAGING;

   struct pipe_resource *staging =
      pipe->screen->resource_create(pipe->screen, &tmpl);
   if (!staging)
      return false;

   const VkBufferImageCopy2 download = {
      .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
      .bufferOffset      = 0,
      .bufferRowLength   = 0,
      .bufferImageHeight = 0,
      .imageSubresource  = region->srcSubresource,
      .imageOffset       = region->srcOffset,
      .imageExtent       = region->extent,
   };
   const VkBufferImageCopy2 upload = {
      .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
      .bufferOffset      = 0,
      .bufferRowLength   = 0,
      .bufferImageHeight = 0,
      .imageSubresource  = region->dstSubresource,
      .imageOffset       = region->dstOffset,
      .imageExtent       = dst_extent,
   };

   const bool ok =
      r3v_copy_image_region_to_buffer(device, src_img, staging, &download) &&
      r3v_copy_buffer_region_to_image(device, staging, dst_img, &upload);

   pipe_resource_reference(&staging, NULL);
   return ok;
}

/* vkCmdClearColorImage as a tile-iterated CPU fill.  Pack the clear value to the
 * image format once, then write that texel to every position of each tile. */
static bool
r3v_clear_color_image(struct r3v_device *device,
                         const struct r3v_image *img,
                         const VkClearColorValue *color,
                         const VkImageSubresourceRange *range)
{
   struct pipe_context *pipe = device->pipe;
   r3v_image_mark_written(img);
   const enum pipe_format fmt = img->resource->format;
   const unsigned bpp = util_format_get_blocksize(fmt);

   uint8_t packed[16] = {0};
   if (util_format_is_pure_uint(fmt))
      util_format_pack_rgba(fmt, packed, color->uint32, 1);
   else if (util_format_is_pure_sint(fmt))
      util_format_pack_rgba(fmt, packed, color->int32, 1);
   else
      util_format_pack_rgba(fmt, packed, color->float32, 1);

   const uint32_t base_mip = range ? range->baseMipLevel : 0;
   const uint32_t mip_count =
      (range && range->levelCount != VK_REMAINING_MIP_LEVELS)
      ? range->levelCount : img->vk.mip_levels - base_mip;

   for (uint32_t tile_row = 0; tile_row < img->tile_rows; tile_row++) {
      for (uint32_t tile_col = 0; tile_col < img->tile_cols; tile_col++) {
         const uint32_t tile_index = tile_row * img->tile_cols + tile_col;
         struct pipe_resource *tile = img->tiles[tile_index];
         if (!tile)
            continue;
         for (uint32_t m = base_mip; m < base_mip + mip_count; m++) {
            const unsigned tile_w = MAX2(img->tile_width[tile_col] >> m, 1u);
            const unsigned tile_h = MAX2(img->tile_height[tile_row] >> m, 1u);

            struct pipe_box box;
            u_box_2d(0, 0, (int)tile_w, (int)tile_h, &box);

            struct pipe_transfer *xfer = NULL;
            uint8_t *map = pipe->texture_map(pipe, tile, m, PIPE_MAP_WRITE,
                                             &box, &xfer);
            if (!map)
               continue;
            for (unsigned y = 0; y < tile_h; y++) {
               uint8_t *row = map + y * xfer->stride;
               for (unsigned x = 0; x < tile_w; x++)
                  memcpy(row + x * bpp, packed, bpp);
            }
            pipe->texture_unmap(pipe, xfer);
         }
      }
   }
   return true;
}

/* vkCmdClearDepthStencilImage as a tile-iterated CPU fill.  r300 backs the
 * depth/stencil formats with one interleaved word (depth in bits [31:8],
 * stencil in [7:0] for S8_UINT_Z24; depth-only for X8Z24/Z16), so the clear
 * packs depth+stencil with util_pack_z_stencil and masks the word to the aspects
 * actually being cleared -- a depth-only clear keeps stencil and vice versa, so
 * those need a read-modify-write (mapped READ_WRITE); clearing the whole word is
 * a plain fill. */
static bool
r3v_clear_depth_stencil_image(struct r3v_device *device,
                                 const struct r3v_image *img,
                                 const VkClearDepthStencilValue *ds,
                                 VkImageAspectFlags aspect,
                                 const VkImageSubresourceRange *range)
{
   struct pipe_context *pipe = device->pipe;
   r3v_image_mark_written(img);
   const enum pipe_format fmt = img->resource->format;
   const unsigned bpp = util_format_get_blocksize(fmt);
   if (bpp == 0 || bpp > 4)
      return false;

   const uint32_t packed =
      util_pack_z_stencil(fmt, ds->depth, (uint8_t)ds->stencil);

   /* Which bits each aspect occupies in the r300 word. */
   uint32_t depth_mask = 0, stencil_mask = 0;
   if (fmt == PIPE_FORMAT_S8_UINT_Z24_UNORM) {
      depth_mask = 0xFFFFFF00u; stencil_mask = 0x000000FFu;
   } else if (fmt == PIPE_FORMAT_X8Z24_UNORM) {
      depth_mask = 0xFFFFFF00u;          /* X8 low byte is don't-care */
   } else if (fmt == PIPE_FORMAT_Z16_UNORM) {
      depth_mask = 0x0000FFFFu;
   } else {
      return false;                      /* no float depth on this silicon */
   }
   const uint32_t write_mask =
      ((aspect & VK_IMAGE_ASPECT_DEPTH_BIT)   ? depth_mask   : 0) |
      ((aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? stencil_mask : 0);
   if (write_mask == 0)
      return true;
   const uint32_t word_mask =
      (bpp >= 4) ? 0xFFFFFFFFu : ((1u << (bpp * 8)) - 1u);
   const bool full_word = (write_mask & word_mask) == word_mask;
   const unsigned map_flags = full_word ? PIPE_MAP_WRITE : PIPE_MAP_READ_WRITE;

   const uint32_t base_mip = range ? range->baseMipLevel : 0;
   const uint32_t mip_count =
      (range && range->levelCount != VK_REMAINING_MIP_LEVELS)
      ? range->levelCount : img->vk.mip_levels - base_mip;

   for (uint32_t tile_row = 0; tile_row < img->tile_rows; tile_row++) {
      for (uint32_t tile_col = 0; tile_col < img->tile_cols; tile_col++) {
         const uint32_t tile_index = tile_row * img->tile_cols + tile_col;
         struct pipe_resource *tile = img->tiles[tile_index];
         if (!tile)
            continue;
         for (uint32_t m = base_mip; m < base_mip + mip_count; m++) {
            const unsigned tile_w = MAX2(img->tile_width[tile_col] >> m, 1u);
            const unsigned tile_h = MAX2(img->tile_height[tile_row] >> m, 1u);

            struct pipe_box box;
            u_box_2d(0, 0, (int)tile_w, (int)tile_h, &box);

            struct pipe_transfer *xfer = NULL;
            uint8_t *map = pipe->texture_map(pipe, tile, m, map_flags,
                                             &box, &xfer);
            if (!map)
               continue;
            for (unsigned y = 0; y < tile_h; y++) {
               uint8_t *row = map + y * xfer->stride;
               for (unsigned x = 0; x < tile_w; x++) {
                  uint8_t *texel = row + x * bpp;
                  if (full_word) {
                     memcpy(texel, &packed, bpp);
                  } else {
                     uint32_t w = 0;
                     memcpy(&w, texel, bpp);
                     w = (w & ~write_mask) | (packed & write_mask);
                     memcpy(texel, &w, bpp);
                  }
               }
            }
            pipe->texture_unmap(pipe, xfer);
         }
      }
   }
   return true;
}

/* vkCmdFillBuffer as a CPU map-and-fill: write the repeated 32-bit value over
 * [offset, offset+size) of the buffer, resolving VK_WHOLE_SIZE to the tail and
 * rounding the byte count down to a 32-bit multiple per the spec. */
static void
r3v_fill_buffer(struct r3v_device *device,
                   const struct r3v_cmd_fill_buffer *fb)
{
   struct pipe_context  *pipe = device->pipe;
   struct pipe_resource *buf  = fb->buffer ? fb->buffer->resource : NULL;
   if (!buf)
      return;

   const uint64_t total = fb->buffer->size;
   const uint64_t offset = fb->offset;
   if (offset >= total)
      return;
   uint64_t size = (fb->size == VK_WHOLE_SIZE) ? (total - offset) : fb->size;
   size = MIN2(size, total - offset) & ~(uint64_t)3;
   if (size == 0)
      return;
   /* pipe_buffer_map_range takes unsigned offsets; size is already clamped to
    * the buffer, but guard the offset cast and the vkCmdFillBuffer 4-byte
    * dstOffset alignment so a >4 GiB descriptor or a misaligned offset cannot
    * map at a truncated or unaligned address. */
   if (offset > UINT_MAX || size > UINT_MAX || (offset & 3u) != 0)
      return;

   struct pipe_transfer *xfer = NULL;
   uint32_t *map = pipe_buffer_map_range(pipe, buf, (unsigned)offset,
                                         (unsigned)size, PIPE_MAP_WRITE, &xfer);
   if (!map)
      return;
   for (uint64_t i = 0; i < size / 4; i++)
      map[i] = fb->data;
   pipe_buffer_unmap(pipe, xfer);
}

/* vkCmdCopyBuffer2 region as a CPU memcpy.  Distinct buffers map src READ and
 * dst WRITE and memcpy; an aliasing src==dst copy maps the union of both ranges
 * once and memmoves so overlap is well defined. */
static void
r3v_copy_buffer_region(struct r3v_device *device,
                          const struct r3v_cmd_copy_buffer *cb)
{
   struct pipe_context  *pipe = device->pipe;
   struct pipe_resource *src  = cb->src ? cb->src->resource : NULL;
   struct pipe_resource *dst  = cb->dst ? cb->dst->resource : NULL;
   const unsigned size = (unsigned)cb->size;
   if (!src || !dst || size == 0)
      return;
   /* Reject a region running past either buffer; pipe_buffer_map_range takes
    * unsigned offsets, so validate the 64-bit extent before the cast (the
    * buffer-to-image copies bounds-check the same way).  The subtract form
    * avoids a 64-bit add overflow on a malformed offset/size. */
   if (cb->size > cb->src->size || cb->src_offset > cb->src->size - cb->size ||
       cb->size > cb->dst->size || cb->dst_offset > cb->dst->size - cb->size ||
       cb->src_offset > UINT_MAX || cb->dst_offset > UINT_MAX)
      return;

   if (src == dst) {
      const uint64_t lo = MIN2(cb->src_offset, cb->dst_offset);
      const uint64_t hi = MAX2(cb->src_offset, cb->dst_offset) + size;
      struct pipe_transfer *xfer = NULL;
      uint8_t *map = pipe_buffer_map_range(pipe, dst, (unsigned)lo,
                                           (unsigned)(hi - lo),
                                           PIPE_MAP_READ | PIPE_MAP_WRITE, &xfer);
      if (!map)
         return;
      memmove(map + (cb->dst_offset - lo), map + (cb->src_offset - lo), size);
      pipe_buffer_unmap(pipe, xfer);
      return;
   }

   struct pipe_transfer *sxfer = NULL, *dxfer = NULL;
   const uint8_t *smap = pipe_buffer_map_range(pipe, src, (unsigned)cb->src_offset,
                                               size, PIPE_MAP_READ, &sxfer);
   uint8_t *dmap = pipe_buffer_map_range(pipe, dst, (unsigned)cb->dst_offset,
                                         size, PIPE_MAP_WRITE, &dxfer);
   if (smap && dmap)
      memcpy(dmap, smap, size);
   if (smap)
      pipe_buffer_unmap(pipe, sxfer);
   if (dmap)
      pipe_buffer_unmap(pipe, dxfer);
}

/* vkCmdUpdateBuffer as a CPU memcpy of the recorded inline data into the buffer. */
static void
r3v_update_buffer(struct r3v_device *device,
                     const struct r3v_cmd_update_buffer *ub)
{
   struct pipe_context  *pipe = device->pipe;
   struct pipe_resource *buf  = ub->buffer ? ub->buffer->resource : NULL;
   const unsigned size = (unsigned)ub->size;
   if (!buf || !ub->data || size == 0)
      return;
   /* Reject an update running past the buffer; validate in 64-bit before the
    * unsigned offset cast. */
   if (ub->size > ub->buffer->size ||
       ub->offset > ub->buffer->size - ub->size || ub->offset > UINT_MAX)
      return;

   struct pipe_transfer *xfer = NULL;
   uint8_t *map = pipe_buffer_map_range(pipe, buf, (unsigned)ub->offset, size,
                                        PIPE_MAP_WRITE, &xfer);
   if (!map)
      return;
   memcpy(map, ub->data, size);
   pipe_buffer_unmap(pipe, xfer);
}

static void
r3v_write_query_result_field(struct pipe_context *pipe,
                                struct pipe_resource *buf,
                                unsigned offset,
                                unsigned size,
                                uint64_t value)
{
   struct pipe_transfer *xfer = NULL;
   uint8_t *map = pipe_buffer_map_range(pipe, buf, offset, size,
                                        PIPE_MAP_WRITE, &xfer);
   if (!map)
      return;

   if (size == sizeof(uint64_t)) {
      const uint64_t v = value;
      memcpy(map, &v, sizeof(v));
   } else {
      const uint32_t v = (uint32_t)value;
      memcpy(map, &v, sizeof(v));
   }

   pipe_buffer_unmap(pipe, xfer);
}

static bool
r3v_query_result_layout_in_bounds(
   const struct r3v_cmd_copy_query_pool_results *cq,
   uint32_t query_index, unsigned result_size, unsigned per_query,
   bool write_availability, uint64_t total, uint64_t *byte_offset)
{
   if (cq->stride && query_index > UINT64_MAX / cq->stride)
      return false;

   const uint64_t stride_offset = (uint64_t)query_index * cq->stride;
   if (cq->dst_offset > UINT64_MAX - stride_offset)
      return false;

   const uint64_t offset = cq->dst_offset + stride_offset;
   const uint64_t mapped_span = write_availability ? per_query : result_size;
   /* pipe_buffer_map_range takes unsigned offsets and sizes.  Bound the full
    * written layout before any field map so offset + per_query cannot overflow
    * or wrap after the unsigned offset cast. */
   if (mapped_span > UINT_MAX || mapped_span > total ||
       offset > total - mapped_span || offset > UINT_MAX - mapped_span)
      return false;

   *byte_offset = offset;
   return true;
}

static void
r3v_write_query_result_word(struct pipe_context *pipe,
                               struct pipe_resource *buf,
                               uint64_t byte_offset,
                               unsigned result_size,
                               bool write_result,
                               const struct r3v_query *query)
{
   if (!write_result)
      return;

   r3v_write_query_result_field(pipe, buf, (unsigned)byte_offset,
                                   result_size,
                                   query->available ? query->result : 0);
}

/* vkCmdCopyQueryPoolResults on the host: write each query's result word (and an
 * availability word when VK_QUERY_RESULT_WITH_AVAILABILITY_BIT is set) into the
 * destination buffer at dst_offset + i*stride, mirroring
 * r3v_GetQueryPoolResults.  The serialized CPU replay has stored the
 * end-query results into the pool slots before this host entry runs, so
 * VK_QUERY_RESULT_WAIT never blocks; an unavailable slot is one reset but never
 * ended.  Each written field maps only its own destination word, so the stride
 * gap between entries and any unavailable result word are left unmodified. */
static void
r3v_copy_query_pool_results(struct r3v_device *device,
                               const struct r3v_cmd_copy_query_pool_results *cq)
{
   struct pipe_context      *pipe = device->pipe;
   struct r3v_query_pool *pool = cq->pool;
   struct pipe_resource     *buf  = cq->dst ? cq->dst->resource : NULL;
   if (!pool || !buf)
      return;

   const bool b64        = (cq->flags & VK_QUERY_RESULT_64_BIT) != 0;
   const bool want_avail = (cq->flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
   const bool force_result = (cq->flags & (VK_QUERY_RESULT_PARTIAL_BIT |
                                           VK_QUERY_RESULT_WAIT_BIT)) != 0;
   const unsigned rsize  = b64 ? sizeof(uint64_t) : sizeof(uint32_t);
   const unsigned per_query = rsize * (want_avail ? 2u : 1u);
   const uint64_t total  = cq->dst->size;

   for (uint32_t i = 0; i < cq->query_count; i++) {
      if (cq->first_query + i >= pool->vk.query_count)
         break;

      const struct r3v_query *q = &pool->queries[cq->first_query + i];
      const bool available = q->available;
      const bool write_result = available || force_result;
      if (!write_result && !want_avail)
         continue;

      uint64_t byte_off;
      if (!r3v_query_result_layout_in_bounds(cq, i, rsize, per_query,
                                                want_avail, total, &byte_off))
         break;

      r3v_write_query_result_word(pipe, buf, byte_off, rsize, write_result,
                                     q);
      if (want_avail) {
         const uint64_t avail_off = byte_off + rsize;
         r3v_write_query_result_field(pipe, buf, (unsigned)avail_off, rsize,
                                         available ? 1 : 0);
      }
   }
}

/* Executes one host-side transfer/event command on the CPU.  The caller is
 * responsible for fencing prior GPU work before a transfer that reads GPU
 * output. */
static void
r3v_replay_host_entry(struct r3v_device *device,
                         const struct r3v_cmd_entry *e)
{
   if (e->type == R3V_CMD_COPY_IMAGE_TO_BUFFER) {
      const VkBufferImageCopy2 *region = &e->copy_img_buf.region;
      struct pipe_resource     *dst    = e->copy_img_buf.dst->resource;
      r3v_copy_image_region_to_buffer(device, e->copy_img_buf.src,
                                         dst, region);
   } else if (e->type == R3V_CMD_COPY_BUFFER_TO_IMAGE) {
      const VkBufferImageCopy2 *region = &e->copy_buf_img.region;
      struct pipe_resource     *src    = e->copy_buf_img.src->resource;
      r3v_copy_buffer_region_to_image(device, src,
                                         e->copy_buf_img.dst, region);
   } else if (e->type == R3V_CMD_COPY_IMAGE) {
      const VkImageCopy2 *region = &e->copy_image.region;
      r3v_copy_image_region_to_image(device, e->copy_image.src,
                                        e->copy_image.dst, region);
   } else if (e->type == R3V_CMD_CLEAR_COLOR_IMAGE) {
      r3v_clear_color_image(device, e->clear_color_image.image,
                               &e->clear_color_image.color,
                               &e->clear_color_image.range);
   } else if (e->type == R3V_CMD_CLEAR_DEPTH_STENCIL_IMAGE) {
      r3v_clear_depth_stencil_image(
         device, e->clear_depth_stencil_image.image,
         &e->clear_depth_stencil_image.value,
         e->clear_depth_stencil_image.range.aspectMask,
         &e->clear_depth_stencil_image.range);
   } else if (e->type == R3V_CMD_FILL_BUFFER) {
      r3v_fill_buffer(device, &e->fill_buffer);
   } else if (e->type == R3V_CMD_COPY_BUFFER) {
      r3v_copy_buffer_region(device, &e->copy_buffer);
   } else if (e->type == R3V_CMD_UPDATE_BUFFER) {
      r3v_update_buffer(device, &e->update_buffer);
   } else if (e->type == R3V_CMD_COPY_QUERY_POOL_RESULTS) {
      r3v_copy_query_pool_results(device, &e->copy_query_pool_results);
   } else if (e->type == R3V_CMD_SET_EVENT) {
      if (e->event.event)
         e->event.event->status = VK_EVENT_SET;
   } else if (e->type == R3V_CMD_RESET_EVENT) {
      if (e->event.event)
         e->event.event->status = VK_EVENT_RESET;
   }
}

VkResult
r3v_queue_driver_submit(struct vk_queue *vkq,
                            struct vk_queue_submit *submit)
{
   struct r3v_queue  *queue  = container_of(vkq, struct r3v_queue, vk);
   struct r3v_device *device = container_of(queue->vk.base.device,
                                               struct r3v_device, vk);
   struct pipe_context  *pipe   = device->pipe;
   VkResult result =
      vk_sync_wait_many(&device->vk, submit->wait_count, submit->waits,
                        VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
   if (result != VK_SUCCESS)
      return result;

   /* Submit-boundary coherence, entry half: push every owns_buffer host map
    * into its bound resource so the replay reads the app's latest writes.
    * HOST_COHERENT memory promises visibility without an explicit flush, and
    * on this device all GPU access happens inside this synchronous submit, so
    * the submit boundary is exactly where that promise is kept. */
   r3v_device_memory_sync_bound(device, true /* host -> buffer */);

   /* Synthetic VS-system-value vertex buffers allocated during replay; held
    * until after the submit fence, then released. */
   struct util_dynarray transient_vbs;
   util_dynarray_init(&transient_vbs, NULL);

   /* gpu_pending is threaded across all command buffers so a host transfer in a
    * later command buffer drains GPU work emitted by an earlier one. */
   bool gpu_pending = false;

   for (uint32_t ci = 0; ci < submit->command_buffer_count; ci++) {
      struct r3v_cmd_buffer *cmd =
         container_of(submit->command_buffers[ci],
                      struct r3v_cmd_buffer, base);

      /* Submit backend selection.  device->use_cs_backend selects the cs-direct
       * path (native PM4 via radeon_winsys) when the hazard gate is accepted.
       * That path is not implemented and is not separately validatable on
       * RS482/RS485: r300g's emit functions are coupled to the private
       * struct r300_context and its populated dirty-atom state machine, so a
       * standalone PM4 emitter would either duplicate the pipe_context replay
       * with worse coupling or re-derive r300_emit.c with no register-level
       * oracle to check it against (the curated safe-register set carries no
       * 3D-engine config registers).  Honor the flag by reporting the gap
       * once, then run the pipe_context replay path. */
      if (device->use_cs_backend)
         mesa_logw_once("r3v: cs-direct-emit backend requested via "
                        "R3V_CS_DIRECT_BACKEND_HAZARD_ACCEPTED but not "
                        "implemented; using pipe_context replay backend");
      result = r3v_replay_gpu(device, cmd, &transient_vbs, &gpu_pending);
      if (result != VK_SUCCESS)
         break;
   }

   if (result == VK_SUCCESS) {
      struct pipe_fence_handle *fence = NULL;
      pipe->flush(pipe, &fence, 0);
      if (fence) {
         device->screen->fence_finish(device->screen, NULL, fence,
                                      OS_TIMEOUT_INFINITE);
         device->screen->fence_reference(device->screen, &fence, NULL);
      }
   }

   /* GPU is done with the draws; release the synthetic VS-system-value streams. */
   util_dynarray_foreach(&transient_vbs, struct pipe_resource *, pres)
      pipe_resource_reference(pres, NULL);
   util_dynarray_fini(&transient_vbs);

   if (result != VK_SUCCESS)
      return result;

   /* Submit-boundary coherence, exit half: the GPU fence retired above, so
    * pull every bound resource back into its owns_buffer host map -- a
    * coherent-memory reader polls the map after the fence without calling
    * vkInvalidateMappedMemoryRanges. */
   r3v_device_memory_sync_bound(device, false /* buffer -> host */);

   /* In IMMEDIATE submit mode (VK_DEVICE_TIMELINE_MODE_NONE), the vk_queue
    * runtime calls vk_sync_signal_unwrap before driver_submit, which strips
    * timeline wrappers but does NOT call .signal on binary syncs.  After
    * driver_submit returns, only timeline signal_points are processed by the
    * runtime.  Binary syncs in submit->signals are the driver's
    * responsibility.  Signal them here so vkWaitForFences unblocks. */
   return vk_sync_signal_many(&device->vk, submit->signal_count,
                              submit->signals);
}
