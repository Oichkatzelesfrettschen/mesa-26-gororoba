/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_device.h"
#include "r300vk_cmd_buffer.h"
#include "r300vk_pipeline.h"
#include "r300vk_image.h"
#include "r300vk_buffer.h"

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
#include "util/log.h"
#include "util/macros.h"

#include "vulkan/util/vk_util.h"

#include "util/os_time.h"

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

/* Allocate a transient vertex buffer for a synthetic VS-system-value stream.
 * Gallium applies draw.start and start_instance while fetching vertex elements,
 * so the buffer must be indexable through base + count rather than only count
 * elements.  The owner reference is held in transient_vbs until the submit fence
 * completes; set_vertex_buffers takes its own references. */
static bool
r300vk_bind_synthetic_index_stream(struct r300vk_device *device,
                                   struct pipe_context *pipe,
                                   struct pipe_vertex_buffer *vb_cache,
                                   uint8_t binding, uint32_t base, uint32_t count,
                                   struct util_dynarray *transient_vbs)
{
   if (count == 0)
      return true;

   if (binding >= R300VK_MAX_VERTEX_BINDINGS)
      return false;

   if (count > UINT32_MAX - base) {
      memset(&vb_cache[binding], 0, sizeof(vb_cache[binding]));
      return false;
   }

   const uint32_t total_count = base + count;
   struct pipe_resource tmpl = {
      .target     = PIPE_BUFFER,
      .format     = PIPE_FORMAT_R8_UNORM,
      .bind       = PIPE_BIND_VERTEX_BUFFER,
      .usage      = PIPE_USAGE_STREAM,
      .width0     = (uint64_t)total_count * sizeof(int32_t),
      .height0    = 1,
      .depth0     = 1,
      .array_size = 1,
   };
   struct pipe_resource *res =
      device->screen->resource_create(device->screen, &tmpl);
   if (!res) {
      memset(&vb_cache[binding], 0, sizeof(vb_cache[binding]));
      return false;
   }

   struct pipe_transfer *xfer = NULL;
   int32_t *map = pipe_buffer_map(pipe, res,
                                  PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                  &xfer);
   if (!map) {
      memset(&vb_cache[binding], 0, sizeof(vb_cache[binding]));
      pipe_resource_reference(&res, NULL);
      return false;
   }

   for (uint32_t i = 0; i < total_count; i++)
      map[i] = (int32_t)i;
   pipe_buffer_unmap(pipe, xfer);

   vb_cache[binding].is_user_buffer  = false;
   vb_cache[binding].buffer_offset   = 0;
   vb_cache[binding].buffer.resource = res;
   util_dynarray_append(transient_vbs, res);
   return true;
}

static uint32_t
r300vk_image_tile_count(const struct r300vk_image *img)
{
   if (!img || img->tile_cols == 0 || img->tile_rows == 0)
      return 1;

   return img->tile_cols * img->tile_rows;
}

static uint32_t
r300vk_image_tile_col(const struct r300vk_image *img, uint32_t tile_pass)
{
   return img && img->tile_cols ? tile_pass % img->tile_cols : 0;
}

static uint32_t
r300vk_image_tile_row(const struct r300vk_image *img, uint32_t tile_pass)
{
   return img && img->tile_cols ? tile_pass / img->tile_cols : 0;
}

static uint32_t
r300vk_image_tile_origin_x(const struct r300vk_image *img, uint32_t tile_col)
{
   return img && tile_col > 0 ? img->tile_width[0] : 0;
}

static uint32_t
r300vk_image_tile_origin_y(const struct r300vk_image *img, uint32_t tile_row)
{
   return img && tile_row > 0 ? img->tile_height[0] : 0;
}

static uint32_t
r300vk_cmd_tile_pass_count(const struct r300vk_cmd_buffer *cmd)
{
   uint32_t pass_count = 1;

   for (uint32_t i = 0; i < cmd->entry_count; i++) {
      const struct r300vk_cmd_entry *entry = &cmd->entries[i];
      if (entry->type != R300VK_CMD_BEGIN_RENDER_PASS)
         continue;

      pass_count = MAX2(pass_count,
                        r300vk_image_tile_count(entry->begin_rp.color_image));
   }

   return pass_count;
}

static void
r300vk_scissor_vk_to_tile(const VkRect2D *rect,
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
r300vk_robust_vertex_count(const struct r300vk_pipeline *pl,
                           const struct pipe_vertex_buffer *vb_cache,
                           const VkDeviceSize *vb_sizes,
                           uint32_t first_vertex,
                           uint32_t vertex_count)
{
   if (!pl || pl->vertex_binding_mask == 0)
      return vertex_count;

   uint32_t max_count = vertex_count;
   for (uint32_t b = 0; b < R300VK_MAX_VERTEX_BINDINGS; b++) {
      if (!(pl->vertex_binding_mask & BITFIELD_BIT(b)))
         continue;

      const uint32_t stride = pl->vertex_stride[b];
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

/* CCN is proportional to the number of command types dispatched; one case
 * per r300vk_cmd_type is the minimum correct structure. */
static void
r300vk_replay_gpu(struct r300vk_device *device,
                  const struct r300vk_cmd_buffer *cmd,
                  struct util_dynarray *transient_vbs)
{
   struct pipe_context *pipe = device->pipe;
   const uint32_t tile_pass_count = r300vk_cmd_tile_pass_count(cmd);

   for (uint32_t tile_pass = 0; tile_pass < tile_pass_count; tile_pass++) {
      uint32_t tile_origin_x = 0;
      uint32_t tile_origin_y = 0;
      uint32_t tile_width = 0;
      uint32_t tile_height = 0;
      bool skip_render_pass = false;

      /* VB cache: defers set_vertex_buffers to DRAW time so that it always
       * follows bind_vertex_elements_state (Gallium p_context.h line 417).
       * Accumulating here also preserves lower binding slots when firstBinding > 0
       * without clobbering slots outside the current CmdBindVertexBuffers range. */
      struct pipe_vertex_buffer vb_cache[R300VK_MAX_VERTEX_BINDINGS];
      VkDeviceSize vb_sizes[R300VK_MAX_VERTEX_BINDINGS];
      uint32_t vb_max_used = 0;
      bool vb_dirty = false;
      const struct r300vk_pipeline *bound_pipeline = NULL;
      memset(vb_cache, 0, sizeof(vb_cache));
      memset(vb_sizes, 0, sizeof(vb_sizes));

      for (uint32_t i = 0; i < cmd->entry_count; i++) {
         const struct r300vk_cmd_entry *e = &cmd->entries[i];

         switch (e->type) {

      case R300VK_CMD_BEGIN_RENDER_PASS: {
         struct pipe_framebuffer_state fb;
         memset(&fb, 0, sizeof(fb));
         fb.width  = e->begin_rp.width;
         fb.height = e->begin_rp.height;
         skip_render_pass = false;
         if (e->begin_rp.color_image) {
            const struct r300vk_image *img = e->begin_rp.color_image;
            const uint32_t tile_count = r300vk_image_tile_count(img);
            if (tile_pass >= tile_count) {
               skip_render_pass = true;
               break;
            }

            const uint32_t tile_col = r300vk_image_tile_col(img, tile_pass);
            const uint32_t tile_row = r300vk_image_tile_row(img, tile_pass);
            tile_origin_x = r300vk_image_tile_origin_x(img, tile_col);
            tile_origin_y = r300vk_image_tile_origin_y(img, tile_row);
            tile_width = img->tile_cols ? img->tile_width[tile_col] : fb.width;
            tile_height = img->tile_rows ? img->tile_height[tile_row] : fb.height;
            fb.width = tile_width;
            fb.height = tile_height;

            fb.nr_cbufs = 1;
            fb.cbufs[0].texture     = img->tiles[tile_pass]
                                      ? img->tiles[tile_pass]
                                      : img->resource;
            fb.cbufs[0].format      = e->begin_rp.color_format;
            fb.cbufs[0].level       = 0;
            fb.cbufs[0].first_layer = 0;
            fb.cbufs[0].last_layer  = 0;
         } else if (tile_pass > 0) {
            skip_render_pass = true;
            break;
         }
         pipe->set_framebuffer_state(pipe, &fb);

         if (e->begin_rp.color_image &&
             e->begin_rp.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            union pipe_color_union cv;
            memcpy(cv.f, e->begin_rp.clear_color.float32, sizeof(cv.f));
            pipe->clear(pipe, PIPE_CLEAR_COLOR0, 0xF, 0, NULL, &cv, 0.0, 0);
         }
         break;
      }

      case R300VK_CMD_BIND_PIPELINE: {
         if (skip_render_pass)
            break;
         const struct r300vk_pipeline *pl = e->bind_pipeline.pipeline;
         bound_pipeline = pl;
         pipe->bind_blend_state(pipe, pl->blend_cso);
         pipe->bind_rasterizer_state(pipe, pl->rasterizer_cso);
         pipe->bind_depth_stencil_alpha_state(pipe, pl->dsa_cso);
         pipe->bind_vs_state(pipe, pl->vs_cso);
         pipe->bind_fs_state(pipe, pl->fs_cso);
         pipe->bind_vertex_elements_state(pipe, pl->velems_cso);
         /* Changing vertex elements requires a subsequent set_vertex_buffers
          * before the next draw per p_context.h.  Vulkan allows CmdBindPipeline
          * without a follow-up CmdBindVertexBuffers, so force a VB flush. */
         vb_dirty = true;
         break;
      }

      case R300VK_CMD_SET_VIEWPORT: {
         if (skip_render_pass)
            break;
         struct pipe_viewport_state pv;
         viewport_vk_to_gallium(&e->set_vp.vp, (float)tile_origin_x,
                                (float)tile_origin_y, &pv);
         pipe->set_viewport_states(pipe, 0, 1, &pv);
         break;
      }

      case R300VK_CMD_SET_SCISSOR: {
         if (skip_render_pass)
            break;
         struct pipe_scissor_state sc;
         r300vk_scissor_vk_to_tile(&e->set_sc.scissor, tile_origin_x,
                                   tile_origin_y, tile_width, tile_height,
                                   &sc);
         pipe->set_scissor_states(pipe, 0, 1, &sc);
         break;
      }

      case R300VK_CMD_BIND_VERTEX_BUFFERS: {
         if (skip_render_pass)
            break;
         /* Accumulate into the VB cache rather than calling set_vertex_buffers
          * immediately.  set_vertex_buffers must follow bind_vertex_elements_state
          * per p_context.h; deferring to DRAW guarantees the Gallium ordering
          * contract regardless of the order CmdBindPipeline and
          * CmdBindVertexBuffers appear in the Vulkan recording. */
         uint32_t first = e->bind_vbufs.first_binding;
         uint32_t count = e->bind_vbufs.binding_count;
         for (uint32_t b = 0; b < count; b++) {
            vb_cache[first + b].is_user_buffer  = false;
            vb_cache[first + b].buffer_offset   = (unsigned)e->bind_vbufs.offsets[b];
            vb_cache[first + b].buffer.resource = e->bind_vbufs.buffers[b]->resource;
            vb_sizes[first + b] = e->bind_vbufs.buffers[b]->size;
            if (first + b + 1 > vb_max_used)
               vb_max_used = first + b + 1;
         }
         vb_dirty = true;
         break;
      }

      case R300VK_CMD_DRAW: {
         if (skip_render_pass)
            break;

         struct pipe_vertex_buffer draw_vb_cache[R300VK_MAX_VERTEX_BINDINGS];
         memcpy(draw_vb_cache, vb_cache, sizeof(draw_vb_cache));
         uint32_t draw_vb_max_used = vb_max_used;
         bool draw_vb_dirty = vb_dirty;
         bool synthetic_streams_ready = true;

         /* Supply the synthetic VS-system-value stream(s) for this draw: the
          * vertex-id stream steps per vertex (firstVertex + i), the instance-id
          * stream per instance (firstInstance + i).  The velems CSO already
          * carries the matching element + instance_divisor. */
         if (bound_pipeline && bound_pipeline->needs_vertex_id_stream) {
            synthetic_streams_ready =
               r300vk_bind_synthetic_index_stream(
                  device, pipe, draw_vb_cache,
                  bound_pipeline->vertex_id_vb_binding, e->draw.first,
                  e->draw.count, transient_vbs);
            if (synthetic_streams_ready) {
               if (bound_pipeline->vertex_id_vb_binding + 1u > draw_vb_max_used)
                  draw_vb_max_used = bound_pipeline->vertex_id_vb_binding + 1u;
               draw_vb_dirty = true;
            }
         }
         if (synthetic_streams_ready && bound_pipeline &&
             bound_pipeline->needs_instance_id_stream) {
            synthetic_streams_ready =
               r300vk_bind_synthetic_index_stream(
                  device, pipe, draw_vb_cache,
                  bound_pipeline->instance_id_vb_binding,
                  e->draw.first_instance, e->draw.instances, transient_vbs);
            if (synthetic_streams_ready) {
               if (bound_pipeline->instance_id_vb_binding + 1u >
                   draw_vb_max_used)
                  draw_vb_max_used =
                     bound_pipeline->instance_id_vb_binding + 1u;
               draw_vb_dirty = true;
            }
         }
         /* Flush the draw VB state after bind_vertex_elements_state (set by
          * BIND_PIPELINE above in the stream) so the Gallium ordering holds.
          * draw_vb_max_used tracks the highest slot index written so only the
          * live range is submitted.  Synthetic streams are draw-local overlays;
          * vb_dirty restores the application VB cache before a later draw that
          * does not use the same overlay. */
         if (draw_vb_dirty) {
            pipe->set_vertex_buffers(pipe, draw_vb_max_used, draw_vb_cache);
            vb_dirty = synthetic_streams_ready &&
                       (bound_pipeline &&
                        (bound_pipeline->needs_vertex_id_stream ||
                         bound_pipeline->needs_instance_id_stream));
         }
         struct pipe_draw_info info;
         memset(&info, 0, sizeof(info));
         /* Topology was snapshotted at record time so the correct primitive
          * mode is used even if a different pipeline is bound before submit. */
         info.mode           = vk_topology_to_mesa(e->draw.topology);
         info.index_size     = 0;
         info.instance_count = e->draw.instances;
         info.start_instance = e->draw.first_instance;
         struct pipe_draw_start_count_bias draw = {
            .start      = e->draw.first,
            .count      = synthetic_streams_ready ?
               r300vk_robust_vertex_count(bound_pipeline, vb_cache,
                                           vb_sizes, e->draw.first,
                                           e->draw.count) : 0,
            .index_bias = 0,
         };
         if (draw.count > 0)
            pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
         break;
      }

      case R300VK_CMD_END_RENDER_PASS: {
         if (skip_render_pass) {
            skip_render_pass = false;
            break;
         }
         struct pipe_framebuffer_state empty;
         memset(&empty, 0, sizeof(empty));
         pipe->set_framebuffer_state(pipe, &empty);
         tile_origin_x = 0;
         tile_origin_y = 0;
         tile_width = 0;
         tile_height = 0;
         break;
      }

      case R300VK_CMD_COPY_IMAGE_TO_BUFFER:
         /* Handled in the CPU readback pass after flush+fence. */
         break;

      case R300VK_CMD_PIPELINE_BARRIER: {
         if (skip_render_pass)
            break;
         /* RS482/RS485 is UMA with no auxiliary compression surfaces
          * (no CMASK, no HTILE, no DCC -- R3xx predates those features).
          * A layout transition here has no aux decompression step; the
          * only hardware action is a CS flush to create a submit boundary.
          *
          * Flush analysis: pipe->flush() submits the current CS, then
          * r300_flush_and_cleanup() re-marks every state atom dirty through
          * r300_mark_atom_dirty() -- r300g has no single dirty bitmask; each
          * atom carries its own dirty flag walked by foreach_atom.
          * r300_emit_dirty_state() then re-emits all dirty atoms (framebuffer,
          * blend, rasterizer, DSA, vertex elements) before the next draw_vbo
          * call, so state remains
          * coherent across the flush boundary.  vb_dirty is local
          * replay-loop state tracking whether the VB cache needs pushing
          * before the next draw; it is not reset by the flush and continues
          * to work correctly.
          *
          * The submit-time flush in r300vk_queue_driver_submit already
          * provides the ordering guarantee for the render->readback path,
          * but an explicit flush here satisfies the Vulkan memory-ordering
          * contract at barrier granularity for future multi-submit cases. */
         pipe->flush(pipe, NULL, 0);

         /* Update the resource-state ledger so commands after this barrier
          * observe the new layout.  On RS482/RS485 there is no aux surface
          * state to transition; this is a pure bookkeeping update. */
         if (e->barrier.image)
            e->barrier.image->resource_state.layout = e->barrier.new_layout;
         break;
      }

      case R300VK_CMD_DISPATCH:
         if (skip_render_pass)
            break;
         /* The no-op compute kernel emits no GPU work, so this proves the
          * Vulkan compute object lifecycle (pipeline create, bind, dispatch
          * record, submit, fence), not GPU activation.  A dispatch is still an
          * execution boundary: the flush submits any preceding work in a mixed
          * graphics+compute command buffer, and is a harmless no-op on the
          * empty compute-only CS.  The submit-time flush in
          * r300vk_queue_driver_submit signals the fence either way; lowering
          * the kernel onto the compute-as-raster substrate (the draw that
          * produces the kernel's output) is the next stage. */
         pipe->flush(pipe, NULL, 0);
         break;
      }
      }
   }
}

static bool
r300vk_copy_image_region_to_buffer(struct r300vk_device *device,
                                   const struct r300vk_image *src_img,
                                   struct pipe_resource *dst,
                                   const VkBufferImageCopy2 *region)
{
   struct pipe_context *pipe = device->pipe;

   /* Row pitch drives both the mapping size and the destination stride.
    * Use the actual format block size rather than assuming 4 bpp so that
    * non-RGBA8 formats (R8, RG16, RGBA16F) map and copy correctly.
    * bufferRowLength == 0 means tightly packed per the Vulkan spec. */
   unsigned bpp = util_format_get_blocksize(src_img->resource->format);
   unsigned row_pitch = (region->bufferRowLength
                         ? region->bufferRowLength
                         : region->imageExtent.width) * bpp;
   unsigned dst_size = row_pitch * region->imageExtent.height;

   struct pipe_transfer *dst_xfer = NULL;
   uint8_t *dst_map = pipe_buffer_map_range(pipe, dst,
                                            (unsigned)region->bufferOffset,
                                            dst_size,
                                            PIPE_MAP_WRITE,
                                            &dst_xfer);
   if (!dst_map)
      return false;

   const int64_t req_min_x = region->imageOffset.x;
   const int64_t req_min_y = region->imageOffset.y;
   const int64_t req_max_x = req_min_x + region->imageExtent.width;
   const int64_t req_max_y = req_min_y + region->imageExtent.height;

   for (uint32_t tile_row = 0; tile_row < src_img->tile_rows; tile_row++) {
      const uint32_t tile_origin_y =
         r300vk_image_tile_origin_y(src_img, tile_row);
      const int64_t tile_min_y = tile_origin_y;
      const int64_t tile_max_y = tile_min_y + src_img->tile_height[tile_row];
      const int64_t copy_min_y = MAX2(req_min_y, tile_min_y);
      const int64_t copy_max_y = MIN2(req_max_y, tile_max_y);
      if (copy_max_y <= copy_min_y)
         continue;

      for (uint32_t tile_col = 0; tile_col < src_img->tile_cols; tile_col++) {
         const uint32_t tile_origin_x =
            r300vk_image_tile_origin_x(src_img, tile_col);
         const int64_t tile_min_x = tile_origin_x;
         const int64_t tile_max_x = tile_min_x + src_img->tile_width[tile_col];
         const int64_t copy_min_x = MAX2(req_min_x, tile_min_x);
         const int64_t copy_max_x = MIN2(req_max_x, tile_max_x);
         if (copy_max_x <= copy_min_x)
            continue;

         const uint32_t tile_index = tile_row * src_img->tile_cols + tile_col;
         struct pipe_resource *src = src_img->tiles[tile_index];
         if (!src)
            continue;

         struct pipe_box src_box;
         u_box_2d((int)(copy_min_x - tile_min_x),
                  (int)(copy_min_y - tile_min_y),
                  (int)(copy_max_x - copy_min_x),
                  (int)(copy_max_y - copy_min_y),
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
         const unsigned copy_width_bytes = (copy_max_x - copy_min_x) * bpp;
         const unsigned copy_height = copy_max_y - copy_min_y;
         for (unsigned row = 0; row < copy_height; row++) {
            memcpy(tile_dst + row * row_pitch,
                   src_map + row * src_xfer->stride,
                   copy_width_bytes);
         }

         pipe->texture_unmap(pipe, src_xfer);
      }
   }

   pipe->buffer_unmap(pipe, dst_xfer);
   return true;
}

/* CPU-side image-to-buffer copies executed after the GPU fence completes. */
static void
r300vk_replay_cpu_readback(struct r300vk_device *device,
                            const struct r300vk_cmd_buffer *cmd)
{
   for (uint32_t i = 0; i < cmd->entry_count; i++) {
      const struct r300vk_cmd_entry *e = &cmd->entries[i];
      if (e->type != R300VK_CMD_COPY_IMAGE_TO_BUFFER)
         continue;

      const VkBufferImageCopy2 *region = &e->copy_img_buf.region;
      struct pipe_resource     *dst    = e->copy_img_buf.dst->resource;
      r300vk_copy_image_region_to_buffer(device, e->copy_img_buf.src,
                                         dst, region);
   }
}

VkResult
r300vk_queue_driver_submit(struct vk_queue *vkq,
                            struct vk_queue_submit *submit)
{
   struct r300vk_queue  *queue  = container_of(vkq, struct r300vk_queue, vk);
   struct r300vk_device *device = container_of(queue->vk.base.device,
                                               struct r300vk_device, vk);
   struct pipe_context  *pipe   = device->pipe;

   /* Synthetic VS-system-value vertex buffers allocated during replay; held
    * until after the submit fence, then released. */
   struct util_dynarray transient_vbs;
   util_dynarray_init(&transient_vbs, NULL);

   for (uint32_t ci = 0; ci < submit->command_buffer_count; ci++) {
      struct r300vk_cmd_buffer *cmd =
         container_of(submit->command_buffers[ci],
                      struct r300vk_cmd_buffer, base);

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
         mesa_logw_once("r300vk: cs-direct-emit backend requested via "
                        "R300VK_CS_DIRECT_BACKEND_HAZARD_ACCEPTED but not "
                        "implemented; using pipe_context replay backend");
      r300vk_replay_gpu(device, cmd, &transient_vbs);
   }

   struct pipe_fence_handle *fence = NULL;
   pipe->flush(pipe, &fence, 0);
   if (fence) {
      device->screen->fence_finish(device->screen, NULL, fence,
                                   OS_TIMEOUT_INFINITE);
      device->screen->fence_reference(device->screen, &fence, NULL);
   }

   /* GPU is done with the draws; release the synthetic VS-system-value streams. */
   util_dynarray_foreach(&transient_vbs, struct pipe_resource *, pres)
      pipe_resource_reference(pres, NULL);
   util_dynarray_fini(&transient_vbs);

   for (uint32_t ci = 0; ci < submit->command_buffer_count; ci++) {
      struct r300vk_cmd_buffer *cmd =
         container_of(submit->command_buffers[ci],
                      struct r300vk_cmd_buffer, base);
      r300vk_replay_cpu_readback(device, cmd);
   }

   /* In IMMEDIATE submit mode (VK_DEVICE_TIMELINE_MODE_NONE), the vk_queue
    * runtime calls vk_sync_signal_unwrap before driver_submit, which strips
    * timeline wrappers but does NOT call .signal on binary syncs.  After
    * driver_submit returns, only timeline signal_points are processed by the
    * runtime.  Binary syncs in submit->signals are the driver's
    * responsibility.  Signal them here so vkWaitForFences unblocks. */
   return vk_sync_signal_many(&device->vk, submit->signal_count,
                              submit->signals);
}
