/*
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_device.h"
#include "r300vk_cmd_buffer.h"
#include "r300vk_pipeline.h"
#include "r300vk_image.h"
#include "r300vk_buffer.h"
#include "r300vk_object.h"
#include "r300vk_descriptor.h"
#include "r300vk_identity_map.h"

#include <stdlib.h>

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
r300vk_replay_bind_descriptor_sets(struct r300vk_device *device,
                                    const struct r300vk_cmd_entry *e,
                                    const struct r300vk_cmd_bind_descriptor_sets **last_bind_dsets)
{
   *last_bind_dsets = &e->bind_dsets;
}

static VkResult
r300vk_replay_dispatch(struct r300vk_device *device,
                        const struct r300vk_cmd_entry *e,
                        const struct r300vk_cmd_bind_descriptor_sets *last_bind_dsets)
{
   const struct r300vk_cmd_dispatch *d = &e->dispatch;
   const struct r300vk_pipeline *pl = d->pipeline;
   bool ok = false;

   if (pl->identity_map.is_identity_map)
      ok = r300vk_identity_map_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->binary_map.is_binary_map)
      ok = r300vk_binary_map_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->blend_acc_reduction.is_blend_acc_reduction)
      ok = r300vk_blend_acc_reduction_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->zpass_reduction.is_zpass_reduction)
      ok = r300vk_zpass_reduction_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->multipass_scan.is_multipass_scan)
      ok = r300vk_multipass_scan_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->predicated_store.is_predicated_store)
      ok = r300vk_predicated_store_dispatch_replay(device, pl, d, last_bind_dsets);
   else if (pl->multitap_gather.is_multitap_gather)
      ok = r300vk_multitap_gather_dispatch_replay(device, pl, d, last_bind_dsets);

   return ok ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

static void
r300vk_replay_end_render_pass(struct r300vk_device *device,
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
r300vk_replay_pipeline_barrier(struct r300vk_device *device,
                                const struct r300vk_cmd_entry *e,
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

/* Bind the first uniform-buffer descriptor in the bound sets to the r300
 * constant file at CONST[0] for both stages, so a graphics shader's
 * load_ubo(0, ...) reads it.  r300 has a single constant file, so only index 0
 * is addressable -- the pipeline-compile guard rejects shaders needing more, and
 * this binds the first UBO found.  r300vk buffers are host-visible Gallium
 * PIPE_BUFFERs, so cb.buffer feeds r300_set_constant_buffer directly (it reads
 * malloced_buffer with no GPU upload), matching the compute identity-map path. */
static void
r300vk_bind_descriptor_ubo(struct r300vk_device *device,
                           const struct r300vk_cmd_bind_descriptor_sets *binds)
{
   struct pipe_context *pipe = device->pipe;
   if (!binds)
      return;

   for (uint32_t s = 0; s < binds->set_count; s++) {
      const struct r300vk_descriptor_set *set = binds->sets[s];
      if (!set || !set->layout)
         continue;
      for (uint32_t b = 0; b < set->layout->binding_count; b++) {
         const struct r300vk_dsl_binding *bnd = &set->layout->bindings[b];
         if (bnd->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
            continue;
         const struct r300vk_descriptor *desc = &set->descriptors[bnd->offset];
         VK_FROM_HANDLE(r300vk_buffer, buf, desc->buf.buffer);
         if (!buf || !buf->resource)
            continue;

         struct pipe_constant_buffer cb;
         memset(&cb, 0, sizeof(cb));
         cb.buffer        = buf->resource;
         cb.buffer_offset = (unsigned)desc->buf.offset;
         cb.buffer_size   = desc->buf.range == VK_WHOLE_SIZE
                            ? (unsigned)(buf->size - desc->buf.offset)
                            : (unsigned)desc->buf.range;
         pipe->set_constant_buffer(pipe, MESA_SHADER_VERTEX, 0, &cb);
         pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);
         return;
      }
   }
}

/* Bind the running push-constant window at CONST[0] for both stages -- the slot a
 * push-constants-only pipeline's lowered load_ubo(0, ...) reads.
 * r300_set_constant_buffer reads cb.user_buffer directly, so the bytes need no
 * GPU upload. */
static void
r300vk_bind_push_constants(struct r300vk_device *device, const uint8_t *data)
{
   struct pipe_context *pipe = device->pipe;
   struct pipe_constant_buffer cb;
   memset(&cb, 0, sizeof(cb));
   cb.user_buffer = data;
   cb.buffer_size = 128;
   pipe->set_constant_buffer(pipe, MESA_SHADER_VERTEX, 0, &cb);
   pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);
}

static void
r300vk_replay_draw(struct r300vk_device *device,
                    const struct r300vk_cmd_entry *e,
                    const struct r300vk_pipeline *bound_pipeline,
                    const struct r300vk_cmd_bind_descriptor_sets *last_bind_dsets,
                    const uint8_t *push_const,
                    struct pipe_vertex_buffer *vb_cache,
                    VkDeviceSize *vb_sizes,
                    uint32_t vb_max_used,
                    bool *vb_dirty,
                    uint32_t tile_origin_x,
                    uint32_t tile_origin_y,
                    uint32_t tile_width,
                    uint32_t tile_height,
                    struct util_dynarray *transient_vbs)
{
   struct pipe_context *pipe = device->pipe;

   /* Unify the direct and indexed draw parameters so the descriptor bind,
    * synthetic VS streams, viewport/scissor, and robustness clamp below apply
    * to both.  An indexed draw's "first/count" are the first index and index
    * count; the index buffer itself is set on pipe_draw_info further down. */
   const bool indexed = (e->type == R300VK_CMD_DRAW_INDEXED);
   const uint32_t draw_first      = indexed ? e->draw_indexed.first_index
                                            : e->draw.first;
   const uint32_t draw_count      = indexed ? e->draw_indexed.index_count
                                            : e->draw.count;
   const uint32_t draw_instances  = indexed ? e->draw_indexed.instances
                                            : e->draw.instances;
   const uint32_t draw_first_inst = indexed ? e->draw_indexed.first_instance
                                            : e->draw.first_instance;
   const VkPrimitiveTopology draw_topology = indexed ? e->draw_indexed.topology
                                                     : e->draw.topology;

   /* A graphics pipeline with no fragment stage (rasterizer discard or
    * depth-only) leaves fs_cso NULL -- a failed fragment compile would instead
    * have failed pipeline creation, so NULL here means no fragment stage.  Such
    * a draw produces no color in r300vk's color-attachment model.  It must be
    * skipped rather than entered: on the RS482 SW-TCL path r300_update_derived_state
    * compiles the hardware vertex shader only when caps.has_tcl is set (it is
    * not), yet r300_update_rs_block unconditionally dereferences r300_vs()->shader
    * and r300_fs()->shader, so a no-fragment draw NULL-derefs in the RS block
    * (dEQP-VK.api.descriptor_set.descriptor_set_layout_lifetime.graphics). */
   if (!bound_pipeline || !bound_pipeline->vs_cso || !bound_pipeline->fs_cso)
      return;

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
      r300vk_scissor_vk_to_tile(&bound_pipeline->static_scissor,
                                tile_origin_x, tile_origin_y,
                                tile_width, tile_height, &sc);
      pipe->set_scissor_states(pipe, 0, 1, &sc);
   }

   struct pipe_vertex_buffer draw_vb_cache[R300VK_MAX_VERTEX_BINDINGS];
   memcpy(draw_vb_cache, vb_cache, sizeof(draw_vb_cache));
   uint32_t draw_vb_max_used = vb_max_used;
   bool draw_vb_dirty = *vb_dirty;
   bool synthetic_streams_ready = true;

   /* Supply the synthetic VS-system-value stream(s) for this draw. */
   if (bound_pipeline && bound_pipeline->needs_vertex_id_stream) {
      synthetic_streams_ready =
         r300vk_bind_synthetic_index_stream(
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
         r300vk_bind_synthetic_index_stream(
            device, pipe, draw_vb_cache,
            bound_pipeline->instance_id_vb_binding,
            draw_first_inst, draw_instances, transient_vbs);
      if (synthetic_streams_ready) {
         if (bound_pipeline->instance_id_vb_binding + 1u >
             draw_vb_max_used)
            draw_vb_max_used =
               bound_pipeline->instance_id_vb_binding + 1u;
         draw_vb_dirty = true;
      }
   }
   /* Flush the draw VB state. */
   if (draw_vb_dirty) {
      pipe->set_vertex_buffers(pipe, draw_vb_max_used, draw_vb_cache);
      *vb_dirty = synthetic_streams_ready &&
                 (bound_pipeline &&
                  (bound_pipeline->needs_vertex_id_stream ||
                   bound_pipeline->needs_instance_id_stream));
   }
   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode           = vk_topology_to_mesa(draw_topology);
   info.instance_count = draw_instances;
   info.start_instance = draw_first_inst;
   struct pipe_draw_start_count_bias draw = { .index_bias = 0 };

   if (indexed) {
      const struct r300vk_cmd_draw_indexed *di = &e->draw_indexed;
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
       * analog of r300vk_robust_vertex_count -- so the index fetch cannot read
       * past the buffer end (the kernel CS validator rejects the same overrun).
       *
       * TODO: gl_VertexIndex on an indexed draw reaches the shader as the
       *       fetched index value + vertexOffset.  RS480/RS482 has no hardware
       *       vertex shader (num_vert_fpus == 0, SW-TCL), so gl_VertexIndex is
       *       supplied through a synthetic vertex-input stream, and
       *       r300vk_bind_synthetic_index_stream fills it with firstVertex + i --
       *       the non-indexed sequence.  Build the stream from di->index_buffer
       *       for the R300VK_CMD_DRAW_INDEXED + needs_vertex_id_stream case. */
      const uint64_t start_elem =
         (uint64_t)di->first_index + di->index_offset / di->index_size;
      const uint64_t end_elem =
         (di->index_offset + di->index_range) / di->index_size;
      const uint64_t usable = end_elem > start_elem ? end_elem - start_elem : 0;
      draw.start = (unsigned)start_elem;
      draw.count = synthetic_streams_ready
                   ? (di->index_count < usable ? di->index_count
                                               : (unsigned)usable)
                   : 0;
      draw.index_bias = di->vertex_offset;
   } else {
      info.index_size = 0;
      draw.start      = draw_first;
      draw.count      = synthetic_streams_ready ?
         r300vk_robust_vertex_count(bound_pipeline, vb_cache,
                                     vb_sizes, draw_first,
                                     draw_count) : 0;
   }
   if (draw.count > 0) {
      /* Push constants and the descriptor UBO are mutually exclusive (the
       * collision is rejected at compile); bind whichever this pipeline uses at
       * CONST[0]. */
      if (bound_pipeline && bound_pipeline->uses_push_constants)
         r300vk_bind_push_constants(device, push_const);
      else
         r300vk_bind_descriptor_ubo(device, last_bind_dsets);
      pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   }
}

static void
r300vk_replay_bind_vertex_buffers(struct r300vk_device *device,
                                   const struct r300vk_cmd_entry *e,
                                   struct pipe_vertex_buffer *vb_cache,
                                   VkDeviceSize *vb_sizes,
                                   uint32_t *vb_max_used,
                                   bool *vb_dirty)
{
   uint32_t first = e->bind_vbufs.first_binding;
   uint32_t count = e->bind_vbufs.binding_count;
   for (uint32_t b = 0; b < count; b++) {
      vb_cache[first + b].is_user_buffer  = false;
      vb_cache[first + b].buffer_offset   = (unsigned)e->bind_vbufs.offsets[b];
      vb_cache[first + b].buffer.resource = e->bind_vbufs.buffers[b]->resource;
      vb_sizes[first + b] = e->bind_vbufs.buffers[b]->size;
      if (first + b + 1 > *vb_max_used)
         *vb_max_used = first + b + 1;
   }
   *vb_dirty = true;
}

static void
r300vk_replay_set_viewport(struct r300vk_device *device,
                            const struct r300vk_cmd_entry *e,
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
r300vk_replay_set_scissor(struct r300vk_device *device,
                           const struct r300vk_cmd_entry *e,
                           uint32_t tile_origin_x,
                           uint32_t tile_origin_y,
                           uint32_t tile_width,
                           uint32_t tile_height)
{
   struct pipe_context *pipe = device->pipe;
   struct pipe_scissor_state sc;
   r300vk_scissor_vk_to_tile(&e->set_sc.scissor, tile_origin_x,
                             tile_origin_y, tile_width, tile_height,
                             &sc);
   pipe->set_scissor_states(pipe, 0, 1, &sc);
}

static void
r300vk_replay_bind_pipeline(struct r300vk_device *device,
                             const struct r300vk_cmd_entry *e,
                             const struct r300vk_pipeline **bound_pipeline,
                             bool *vb_dirty)
{
   struct pipe_context *pipe = device->pipe;
   const struct r300vk_pipeline *pl = e->bind_pipeline.pipeline;
   *bound_pipeline = pl;
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

static void
r300vk_replay_begin_render_pass(struct r300vk_device *device,
                                 const struct r300vk_cmd_entry *e,
                                 unsigned tile_pass,
                                 uint32_t *tile_origin_x,
                                 uint32_t *tile_origin_y,
                                 uint32_t *tile_width,
                                 uint32_t *tile_height,
                                 bool *skip_render_pass)
{
   struct pipe_context *pipe = device->pipe;
   struct pipe_framebuffer_state fb;
   memset(&fb, 0, sizeof(fb));
   fb.width  = e->begin_rp.width;
   fb.height = e->begin_rp.height;
   *skip_render_pass = false;
   if (e->begin_rp.color_image) {
      const struct r300vk_image *img = e->begin_rp.color_image;
      const uint32_t tile_count = r300vk_image_tile_count(img);
      if (tile_pass >= tile_count) {
         *skip_render_pass = true;
         return;
      }

      const uint32_t tile_col = r300vk_image_tile_col(img, tile_pass);
      const uint32_t tile_row = r300vk_image_tile_row(img, tile_pass);
      *tile_origin_x = r300vk_image_tile_origin_x(img, tile_col);
      *tile_origin_y = r300vk_image_tile_origin_y(img, tile_row);
      *tile_width = img->tile_cols ? img->tile_width[tile_col] : fb.width;
      *tile_height = img->tile_rows ? img->tile_height[tile_row] : fb.height;
      fb.width = *tile_width;
      fb.height = *tile_height;

      fb.nr_cbufs = 1;
      fb.cbufs[0].texture     = img->tiles[tile_pass]
                                ? img->tiles[tile_pass]
                                : img->resource;
      fb.cbufs[0].format      = e->begin_rp.color_format;
      fb.cbufs[0].level       = 0;
      fb.cbufs[0].first_layer = 0;
      fb.cbufs[0].last_layer  = 0;
   } else if (tile_pass > 0) {
      *skip_render_pass = true;
      return;
   }
   pipe->set_framebuffer_state(pipe, &fb);

   if (e->begin_rp.color_image &&
       e->begin_rp.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
      union pipe_color_union cv;
      memcpy(cv.f, e->begin_rp.clear_color.float32, sizeof(cv.f));
      pipe->clear(pipe, PIPE_CLEAR_COLOR0, 0xF, 0, NULL, &cv, 0.0, 0);
   }
}

static void
r300vk_replay_clear_attachments(struct r300vk_device *device,
                                const struct r300vk_cmd_entry *render_pass,
                                const struct r300vk_cmd_clear_attachments *clear,
                                unsigned tile_pass,
                                uint32_t tile_origin_x,
                                uint32_t tile_origin_y,
                                uint32_t tile_width,
                                uint32_t tile_height)
{
   if (!render_pass || !render_pass->begin_rp.color_image)
      return;

   const struct r300vk_image *img = render_pass->begin_rp.color_image;
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

   struct pipe_surface surf;
   memset(&surf, 0, sizeof(surf));
   surf.texture     = img->tiles[tile_pass] ? img->tiles[tile_pass] :
                                             img->resource;
   surf.format      = render_pass->begin_rp.color_format;
   surf.level       = 0;
   surf.first_layer = 0;
   surf.last_layer  = 0;

   union pipe_color_union color;
   memset(&color, 0, sizeof(color));
   memcpy(&color, &clear->color, sizeof(clear->color));

   device->pipe->clear_render_target(device->pipe, &surf, &color,
                                     (unsigned)(clip_min_x - tile_min_x),
                                     (unsigned)(clip_min_y - tile_min_y),
                                     (unsigned)(clip_max_x - clip_min_x),
                                     (unsigned)(clip_max_y - clip_min_y),
                                     false);
}

/* Executes one host-side transfer/event command (buffer fill, buffer copy,
 * inline update, image copy/clear, host event signal).  Defined alongside the
 * post-fence CPU pass; forward-declared so the in-submission-order replay can
 * share the exact same execution and stay byte-for-byte consistent with the
 * deferred path. */
static void r300vk_replay_host_entry(struct r300vk_device *device,
                                     const struct r300vk_cmd_entry *e);

/* Drains GPU work flushed since the last drain so a following host transfer
 * observes finished output (a host READ) or is correctly ordered ahead of a
 * later GPU read (a host WRITE).  The flush submits the command stream and the
 * fence_finish blocks until the GPU retires it, mirroring the submit-level
 * drain.  No-op when nothing is pending, so a transfer-only run pays one fence
 * at most per host op that actually follows un-fenced GPU work. */
static void
r300vk_drain_gpu(struct r300vk_device *device, bool *gpu_pending)
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

/* Replays one command buffer's GPU work.  When inorder_host is set, the submit
 * has no multi-tile render pass (every cmd buffer is tile_pass_count == 1), so
 * host transfers run here in submission order -- draining gpu_pending first --
 * instead of in the post-fence CPU pass.  That ordering lets a later in-submit
 * dispatch/draw observe a prior vkCmdFillBuffer/CopyBuffer/UpdateBuffer.  When
 * inorder_host is clear, a multi-tile render pass re-walks this entry list per
 * tile, so host transfers must stay in the post-fence pass and are skipped
 * here.  gpu_pending is threaded across the whole submit so a host transfer in
 * a later cmd buffer drains GPU work emitted by an earlier one. */
static VkResult
r300vk_replay_gpu(struct r300vk_device *device,
                  const struct r300vk_cmd_buffer *cmd,
                  struct util_dynarray *transient_vbs,
                  bool inorder_host,
                  bool *gpu_pending)
{
   struct pipe_context *pipe = device->pipe;
   const uint32_t tile_pass_count = r300vk_cmd_tile_pass_count(cmd);
   VkResult result = VK_SUCCESS;

   for (uint32_t tile_pass = 0; tile_pass < tile_pass_count; tile_pass++) {
      uint32_t tile_origin_x = 0;
      uint32_t tile_origin_y = 0;
      uint32_t tile_width = 0;
      uint32_t tile_height = 0;
      bool skip_render_pass = false;

      struct pipe_vertex_buffer vb_cache[R300VK_MAX_VERTEX_BINDINGS];
      VkDeviceSize vb_sizes[R300VK_MAX_VERTEX_BINDINGS];
      uint32_t vb_max_used = 0;
      bool vb_dirty = false;
      /* Running push-constant window, applied by R300VK_CMD_PUSH_CONSTANTS entries
       * in stream order and bound at CONST[0] for a push-constants-only pipeline.
       * Re-accumulated each tile pass since the entry list is re-walked per tile. */
      uint8_t replay_pc[128] = {0};
      const struct r300vk_pipeline *bound_pipeline = NULL;
      const struct r300vk_cmd_bind_descriptor_sets *last_bind_dsets = NULL;
      const struct r300vk_cmd_entry *current_render_pass = NULL;
      /* The single active r300 occlusion query, if a vkCmdBeginQuery is open.
       * r300 supports one query at a time, and occlusion handling is confined to
       * single-tile submits (tile_pass_count == 1, so this loop runs once). */
      struct pipe_query *active_oq = NULL;
      memset(vb_cache, 0, sizeof(vb_cache));
      memset(vb_sizes, 0, sizeof(vb_sizes));

      for (uint32_t i = 0; i < cmd->entry_count; i++) {
         const struct r300vk_cmd_entry *e = &cmd->entries[i];

         switch (e->type) {
         case R300VK_CMD_BEGIN_RENDER_PASS:
            r300vk_replay_begin_render_pass(device, e, tile_pass,
                                            &tile_origin_x, &tile_origin_y,
                                            &tile_width, &tile_height,
                                            &skip_render_pass);
            current_render_pass = e;
            /* Only loadOp == CLEAR emits a GPU write at begin (the color-image
             * clear) that a later host copy-image-to-buffer could read; a LOAD
             * pass emits nothing here, and its draws set gpu_pending themselves.
             * Gating avoids a needless drain before an in-order host transfer. */
            if (e->begin_rp.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
               *gpu_pending = true;
            break;

         case R300VK_CMD_BIND_PIPELINE:
            if (skip_render_pass) break;
            r300vk_replay_bind_pipeline(device, e, &bound_pipeline, &vb_dirty);
            break;

         case R300VK_CMD_SET_VIEWPORT:
            if (skip_render_pass) break;
            r300vk_replay_set_viewport(device, e, tile_origin_x, tile_origin_y);
            break;

         case R300VK_CMD_SET_SCISSOR:
            if (skip_render_pass) break;
            r300vk_replay_set_scissor(device, e, tile_origin_x, tile_origin_y,
                                      tile_width, tile_height);
            break;

         case R300VK_CMD_BIND_VERTEX_BUFFERS:
            if (skip_render_pass) break;
            r300vk_replay_bind_vertex_buffers(device, e, vb_cache, vb_sizes,
                                              &vb_max_used, &vb_dirty);
            break;

         case R300VK_CMD_DRAW:
         case R300VK_CMD_DRAW_INDEXED:
            if (skip_render_pass) break;
            r300vk_replay_draw(device, e, bound_pipeline, last_bind_dsets,
                               replay_pc, vb_cache, vb_sizes,
                               vb_max_used, &vb_dirty, tile_origin_x,
                               tile_origin_y, tile_width, tile_height,
                               transient_vbs);
            *gpu_pending = true;
            break;

         case R300VK_CMD_PUSH_CONSTANTS: {
            /* Apply the window update unconditionally (pure state, not gated by
             * skip_render_pass) so a later tile pass or draw sees it. */
            const struct r300vk_cmd_push_constants *pc = &e->push_constants;
            if ((uint64_t)pc->offset + pc->size <= sizeof(replay_pc))
               memcpy(replay_pc + pc->offset, pc->data, pc->size);
            break;
         }

         case R300VK_CMD_DRAW_INDIRECT: {
            if (skip_render_pass) break;
            const struct r300vk_cmd_draw_indirect *di = &e->draw_indirect;
            if (!di->buffer || !di->buffer->resource || di->draw_count == 0)
               break;
            /* CPU-read the VkDrawIndirectCommand array (r300vk buffers are
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
            if (di->offset > UINT_MAX || span64 > di->buffer->size ||
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
               struct r300vk_cmd_entry synth;
               synth.type                = R300VK_CMD_DRAW;
               synth.draw.count          = args->vertexCount;
               synth.draw.instances      = args->instanceCount;
               synth.draw.first          = args->firstVertex;
               synth.draw.first_instance = args->firstInstance;
               synth.draw.topology       = di->topology;
               r300vk_replay_draw(device, &synth, bound_pipeline,
                                  last_bind_dsets, replay_pc, vb_cache,
                                  vb_sizes, vb_max_used, &vb_dirty,
                                  tile_origin_x, tile_origin_y, tile_width,
                                  tile_height, transient_vbs);
            }
            pipe_buffer_unmap(pipe, ixfer);
            *gpu_pending = true;
            break;
         }

         case R300VK_CMD_DRAW_INDEXED_INDIRECT: {
            if (skip_render_pass) break;
            const struct r300vk_cmd_draw_indexed_indirect *di =
               &e->draw_indexed_indirect;
            if (!di->buffer || !di->buffer->resource || di->draw_count == 0)
               break;
            /* CPU-read the VkDrawIndexedIndirectCommand array (r300vk buffers are
             * host-visible) and run the indexed draw path per command, exactly as
             * R300VK_CMD_DRAW_INDIRECT does for the non-indexed form. */
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
            if (di->offset > UINT_MAX || span64 > di->buffer->size ||
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
               struct r300vk_cmd_entry synth;
               synth.type                        = R300VK_CMD_DRAW_INDEXED;
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
               r300vk_replay_draw(device, &synth, bound_pipeline,
                                  last_bind_dsets, replay_pc, vb_cache,
                                  vb_sizes, vb_max_used, &vb_dirty,
                                  tile_origin_x, tile_origin_y, tile_width,
                                  tile_height, transient_vbs);
            }
            pipe_buffer_unmap(pipe, ixfer);
            *gpu_pending = true;
            break;
         }

         case R300VK_CMD_END_RENDER_PASS:
            r300vk_replay_end_render_pass(device, &skip_render_pass,
                                          &tile_origin_x, &tile_origin_y,
                                          &tile_width, &tile_height);
            current_render_pass = NULL;
            break;

         case R300VK_CMD_CLEAR_ATTACHMENTS:
            if (skip_render_pass) break;
            r300vk_replay_clear_attachments(device, current_render_pass,
                                            &e->clear_attachments, tile_pass,
                                            tile_origin_x, tile_origin_y,
                                            tile_width, tile_height);
            *gpu_pending = true;
            break;

         case R300VK_CMD_COPY_IMAGE_TO_BUFFER:
         case R300VK_CMD_COPY_BUFFER_TO_IMAGE:
         case R300VK_CMD_COPY_IMAGE:
         case R300VK_CMD_CLEAR_COLOR_IMAGE:
         case R300VK_CMD_FILL_BUFFER:
         case R300VK_CMD_COPY_BUFFER:
         case R300VK_CMD_UPDATE_BUFFER:
         case R300VK_CMD_SET_EVENT:
         case R300VK_CMD_RESET_EVENT:
            /* Without a multi-tile render pass in the submit, run host
             * transfers and event signals in submission order so a later
             * dispatch/draw observes a prior fill/copy/update, draining GPU
             * work first.  With a multi-tile render pass present the entry list
             * is re-walked per tile, so they stay in the post-fence CPU pass. */
            if (!inorder_host)
               break;
            r300vk_drain_gpu(device, gpu_pending);
            r300vk_replay_host_entry(device, e);
            break;

         case R300VK_CMD_PIPELINE_BARRIER:
            r300vk_replay_pipeline_barrier(device, e, skip_render_pass);
            break;

         case R300VK_CMD_BIND_DESCRIPTOR_SETS:
            r300vk_replay_bind_descriptor_sets(device, e, &last_bind_dsets);
            break;

         case R300VK_CMD_DISPATCH:
            result = r300vk_replay_dispatch(device, e, last_bind_dsets);
            if (result != VK_SUCCESS)
               return result;
            pipe->flush(pipe, NULL, 0);
            *gpu_pending = true;
            break;

         case R300VK_CMD_BEGIN_QUERY:
            /* Occlusion only, single-tile only: a multi-tile render pass would
             * re-walk this entry and re-begin, but r300 allows one query at a
             * time.  Map the Vulkan occlusion type to the r300 ZPASS counter and
             * bracket the draws that follow. */
            if (tile_pass_count != 1 || active_oq != NULL ||
                e->query.pool->vk.query_type != VK_QUERY_TYPE_OCCLUSION)
               break;
            active_oq = pipe->create_query(pipe, PIPE_QUERY_OCCLUSION_COUNTER, 0);
            if (active_oq)
               pipe->begin_query(pipe, active_oq);
            break;

         case R300VK_CMD_END_QUERY: {
            if (!active_oq)
               break;
            pipe->end_query(pipe, active_oq);
            /* wait=true flushes and fences, so the sample count is ready to
             * store into the pool slot the host later reads. */
            union pipe_query_result qres;
            memset(&qres, 0, sizeof(qres));
            if (pipe->get_query_result(pipe, active_oq, true, &qres) &&
                e->query.query < e->query.pool->vk.query_count) {
               struct r300vk_query *slot =
                  &e->query.pool->queries[e->query.query];
               slot->result    = qres.u64;
               slot->available = true;
            }
            pipe->destroy_query(pipe, active_oq);
            active_oq = NULL;
            break;
         }

         case R300VK_CMD_RESET_QUERY_POOL: {
            /* Host bookkeeping, applied once per submit (tile-independent):
             * clear availability and result over the reset range. */
            if (tile_pass != 0)
               break;
            struct r300vk_query_pool *qp = e->reset_query_pool.pool;
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
      /* A vkCmdBeginQuery with no matching end before submit is invalid usage;
       * end and destroy the dangling query so the pipe handle is not leaked. */
      if (active_oq) {
         pipe->end_query(pipe, active_oq);
         pipe->destroy_query(pipe, active_oq);
         active_oq = NULL;
      }
      if (result != VK_SUCCESS)
         break;
   }
   return result;
}

static bool
r300vk_linear_region_span(const VkExtent3D *extent,
                          VkDeviceSize buffer_offset,
                          uint32_t buffer_row_length,
                          unsigned bpp,
                          unsigned *row_pitch_out,
                          unsigned *span_out)
{
   if (extent->width == 0 || extent->height == 0 || bpp == 0)
      return false;

   const uint64_t row_texels =
      buffer_row_length ? buffer_row_length : extent->width;
   const uint64_t row_pitch = row_texels * bpp;
   const uint64_t last_row_bytes = (uint64_t)extent->width * bpp;
   const uint64_t span = ((uint64_t)extent->height - 1) * row_pitch +
                         last_row_bytes;

   if (row_pitch > UINT_MAX || span > UINT_MAX ||
       buffer_offset > UINT_MAX ||
       buffer_offset > UINT_MAX - span)
      return false;

   *row_pitch_out = (unsigned)row_pitch;
   *span_out = (unsigned)span;
   return true;
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
   unsigned row_pitch = 0;
   unsigned dst_size = 0;
   if (!r300vk_linear_region_span(&region->imageExtent,
                                  region->bufferOffset,
                                  region->bufferRowLength,
                                  bpp, &row_pitch, &dst_size) ||
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

/* The inverse of r300vk_copy_image_region_to_buffer: a tile-iterated CPU upload.
 * Map the source buffer once, then for every destination image tile touched by
 * the region, map that tile PIPE_MAP_WRITE and copy the matching linear rows.
 * Iterating tiles keeps split images correct instead of writing only tile zero. */
static bool
r300vk_copy_buffer_region_to_image(struct r300vk_device *device,
                                   struct pipe_resource *src,
                                   const struct r300vk_image *dst_img,
                                   const VkBufferImageCopy2 *region)
{
   struct pipe_context *pipe = device->pipe;

   const unsigned bpp = util_format_get_blocksize(dst_img->resource->format);
   unsigned row_pitch = 0;
   unsigned src_size = 0;
   if (!r300vk_linear_region_span(&region->imageExtent,
                                  region->bufferOffset,
                                  region->bufferRowLength,
                                  bpp, &row_pitch, &src_size) ||
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

   const int64_t req_min_x = region->imageOffset.x;
   const int64_t req_min_y = region->imageOffset.y;
   const int64_t req_max_x = req_min_x + region->imageExtent.width;
   const int64_t req_max_y = req_min_y + region->imageExtent.height;

   for (uint32_t tile_row = 0; tile_row < dst_img->tile_rows; tile_row++) {
      const uint32_t tile_origin_y =
         r300vk_image_tile_origin_y(dst_img, tile_row);
      const int64_t tile_min_y = tile_origin_y;
      const int64_t tile_max_y = tile_min_y + dst_img->tile_height[tile_row];
      const int64_t copy_min_y = MAX2(req_min_y, tile_min_y);
      const int64_t copy_max_y = MIN2(req_max_y, tile_max_y);
      if (copy_max_y <= copy_min_y)
         continue;

      for (uint32_t tile_col = 0; tile_col < dst_img->tile_cols; tile_col++) {
         const uint32_t tile_origin_x =
            r300vk_image_tile_origin_x(dst_img, tile_col);
         const int64_t tile_min_x = tile_origin_x;
         const int64_t tile_max_x = tile_min_x + dst_img->tile_width[tile_col];
         const int64_t copy_min_x = MAX2(req_min_x, tile_min_x);
         const int64_t copy_max_x = MIN2(req_max_x, tile_max_x);
         if (copy_max_x <= copy_min_x)
            continue;

         const uint32_t tile_index = tile_row * dst_img->tile_cols + tile_col;
         struct pipe_resource *dst = dst_img->tiles[tile_index];
         if (!dst)
            continue;

         struct pipe_box dst_box;
         u_box_2d((int)(copy_min_x - tile_min_x),
                  (int)(copy_min_y - tile_min_y),
                  (int)(copy_max_x - copy_min_x),
                  (int)(copy_max_y - copy_min_y),
                  &dst_box);

         struct pipe_transfer *dst_xfer = NULL;
         uint8_t *dst_map =
            pipe->texture_map(pipe, dst,
                              region->imageSubresource.mipLevel,
                              PIPE_MAP_WRITE,
                              &dst_box, &dst_xfer);
         if (!dst_map)
            continue;

         const uint8_t *tile_src =
            src_map +
            (copy_min_y - req_min_y) * row_pitch +
            (copy_min_x - req_min_x) * bpp;
         const unsigned copy_width_bytes = (copy_max_x - copy_min_x) * bpp;
         const unsigned copy_height = copy_max_y - copy_min_y;
         for (unsigned row = 0; row < copy_height; row++) {
            memcpy(dst_map + row * dst_xfer->stride,
                   tile_src + row * row_pitch,
                   copy_width_bytes);
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
r300vk_copy_image_region_to_image(struct r300vk_device *device,
                                  const struct r300vk_image *src_img,
                                  const struct r300vk_image *dst_img,
                                  const VkImageCopy2 *region)
{
   struct pipe_context *pipe = device->pipe;
   const unsigned src_bpp = util_format_get_blocksize(src_img->resource->format);
   const unsigned dst_bpp = util_format_get_blocksize(dst_img->resource->format);

   if (src_bpp == 0 || src_bpp != dst_bpp)
      return false;

   unsigned row_pitch = 0;
   unsigned staging_size = 0;
   if (!r300vk_linear_region_span(&region->extent, 0, 0, dst_bpp,
                                  &row_pitch, &staging_size) ||
       row_pitch == 0)
      return false;

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
      .imageExtent       = region->extent,
   };

   const bool ok =
      r300vk_copy_image_region_to_buffer(device, src_img, staging, &download) &&
      r300vk_copy_buffer_region_to_image(device, staging, dst_img, &upload);

   pipe_resource_reference(&staging, NULL);
   return ok;
}

/* vkCmdClearColorImage as a tile-iterated CPU fill.  Pack the clear value to the
 * image format once, then write that texel to every position of each tile. */
static bool
r300vk_clear_color_image(struct r300vk_device *device,
                         const struct r300vk_image *img,
                         const VkClearColorValue *color)
{
   struct pipe_context *pipe = device->pipe;
   const enum pipe_format fmt = img->resource->format;
   const unsigned bpp = util_format_get_blocksize(fmt);

   uint8_t packed[16] = {0};
   if (util_format_is_pure_uint(fmt))
      util_format_pack_rgba(fmt, packed, color->uint32, 1);
   else if (util_format_is_pure_sint(fmt))
      util_format_pack_rgba(fmt, packed, color->int32, 1);
   else
      util_format_pack_rgba(fmt, packed, color->float32, 1);

   for (uint32_t tile_row = 0; tile_row < img->tile_rows; tile_row++) {
      for (uint32_t tile_col = 0; tile_col < img->tile_cols; tile_col++) {
         const uint32_t tile_index = tile_row * img->tile_cols + tile_col;
         struct pipe_resource *tile = img->tiles[tile_index];
         if (!tile)
            continue;
         const unsigned tile_w = img->tile_width[tile_col];
         const unsigned tile_h = img->tile_height[tile_row];

         struct pipe_box box;
         u_box_2d(0, 0, (int)tile_w, (int)tile_h, &box);

         struct pipe_transfer *xfer = NULL;
         uint8_t *map = pipe->texture_map(pipe, tile, 0, PIPE_MAP_WRITE,
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
   return true;
}

/* vkCmdFillBuffer as a CPU map-and-fill: write the repeated 32-bit value over
 * [offset, offset+size) of the buffer, resolving VK_WHOLE_SIZE to the tail and
 * rounding the byte count down to a 32-bit multiple per the spec. */
static void
r300vk_fill_buffer(struct r300vk_device *device,
                   const struct r300vk_cmd_fill_buffer *fb)
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
r300vk_copy_buffer_region(struct r300vk_device *device,
                          const struct r300vk_cmd_copy_buffer *cb)
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
r300vk_update_buffer(struct r300vk_device *device,
                     const struct r300vk_cmd_update_buffer *ub)
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

/* Executes one host-side transfer/event command on the CPU.  Shared by the
 * post-fence readback pass and the in-submission-order replay, so both run the
 * exact same code; the caller is responsible for fencing prior GPU work before
 * a transfer that reads GPU output. */
static void
r300vk_replay_host_entry(struct r300vk_device *device,
                         const struct r300vk_cmd_entry *e)
{
   if (e->type == R300VK_CMD_COPY_IMAGE_TO_BUFFER) {
      const VkBufferImageCopy2 *region = &e->copy_img_buf.region;
      struct pipe_resource     *dst    = e->copy_img_buf.dst->resource;
      r300vk_copy_image_region_to_buffer(device, e->copy_img_buf.src,
                                         dst, region);
   } else if (e->type == R300VK_CMD_COPY_BUFFER_TO_IMAGE) {
      const VkBufferImageCopy2 *region = &e->copy_buf_img.region;
      struct pipe_resource     *src    = e->copy_buf_img.src->resource;
      r300vk_copy_buffer_region_to_image(device, src,
                                         e->copy_buf_img.dst, region);
   } else if (e->type == R300VK_CMD_COPY_IMAGE) {
      const VkImageCopy2 *region = &e->copy_image.region;
      r300vk_copy_image_region_to_image(device, e->copy_image.src,
                                        e->copy_image.dst, region);
   } else if (e->type == R300VK_CMD_CLEAR_COLOR_IMAGE) {
      r300vk_clear_color_image(device, e->clear_color_image.image,
                               &e->clear_color_image.color);
   } else if (e->type == R300VK_CMD_FILL_BUFFER) {
      r300vk_fill_buffer(device, &e->fill_buffer);
   } else if (e->type == R300VK_CMD_COPY_BUFFER) {
      r300vk_copy_buffer_region(device, &e->copy_buffer);
   } else if (e->type == R300VK_CMD_UPDATE_BUFFER) {
      r300vk_update_buffer(device, &e->update_buffer);
   } else if (e->type == R300VK_CMD_SET_EVENT) {
      if (e->event.event)
         e->event.event->status = VK_EVENT_SET;
   } else if (e->type == R300VK_CMD_RESET_EVENT) {
      if (e->event.event)
         e->event.event->status = VK_EVENT_RESET;
   }
}

/* CPU-side buffer/image transfers executed after the GPU fence completes.  Used
 * only when a multi-tile render pass forces the deferred two-pass model; the
 * single-tile path runs these in submission order during r300vk_replay_gpu. */
static void
r300vk_replay_cpu_readback(struct r300vk_device *device,
                            const struct r300vk_cmd_buffer *cmd)
{
   for (uint32_t i = 0; i < cmd->entry_count; i++)
      r300vk_replay_host_entry(device, &cmd->entries[i]);
}

VkResult
r300vk_queue_driver_submit(struct vk_queue *vkq,
                            struct vk_queue_submit *submit)
{
   struct r300vk_queue  *queue  = container_of(vkq, struct r300vk_queue, vk);
   struct r300vk_device *device = container_of(queue->vk.base.device,
                                               struct r300vk_device, vk);
   struct pipe_context  *pipe   = device->pipe;
   VkResult result = VK_SUCCESS;

   /* Synthetic VS-system-value vertex buffers allocated during replay; held
    * until after the submit fence, then released. */
   struct util_dynarray transient_vbs;
   util_dynarray_init(&transient_vbs, NULL);

   /* A multi-tile render pass re-walks its command buffer's entry list once per
    * tile, so host transfers in such a submit must stay in the post-fence CPU
    * pass (running them inside the tiled walk would repeat them per tile).  When
    * no command buffer in the submit needs more than one tile, host transfers
    * run in submission order during replay so a later in-submit dispatch/draw
    * observes a prior fill/copy/update.  gpu_pending is threaded across all
    * command buffers so a host transfer drains GPU work emitted by an earlier
    * one. */
   bool inorder_host = true;
   for (uint32_t ci = 0; ci < submit->command_buffer_count; ci++) {
      struct r300vk_cmd_buffer *cmd =
         container_of(submit->command_buffers[ci],
                      struct r300vk_cmd_buffer, base);
      if (r300vk_cmd_tile_pass_count(cmd) > 1) {
         inorder_host = false;
         break;
      }
   }
   bool gpu_pending = false;

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
      result = r300vk_replay_gpu(device, cmd, &transient_vbs, inorder_host,
                                 &gpu_pending);
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

   /* Deferred host transfers for the tiled path only; the single-tile path
    * already ran them in submission order during replay. */
   if (!inorder_host) {
      for (uint32_t ci = 0; ci < submit->command_buffer_count; ci++) {
         struct r300vk_cmd_buffer *cmd =
            container_of(submit->command_buffers[ci],
                         struct r300vk_cmd_buffer, base);
         r300vk_replay_cpu_readback(device, cmd);
      }
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
