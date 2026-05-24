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
#include "util/box.h"
#include "util/format/u_format.h"
#include "util/log.h"

#include "vulkan/util/vk_util.h"

#include "util/os_time.h"

#include <string.h>

/* Convert a VkViewport to Gallium's scale/translate form.
 * Gallium clip space is [0, 1] depth; VkViewport uses [minDepth, maxDepth]. */
static void
viewport_vk_to_gallium(const VkViewport *vp, struct pipe_viewport_state *pv)
{
   pv->scale[0]     = vp->width  * 0.5f;
   pv->scale[1]     = vp->height * 0.5f;
   pv->scale[2]     = vp->maxDepth - vp->minDepth;
   pv->translate[0] = vp->x + vp->width  * 0.5f;
   pv->translate[1] = vp->y + vp->height * 0.5f;
   pv->translate[2] = vp->minDepth;
   pv->swizzle_x    = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
   pv->swizzle_y    = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
   pv->swizzle_z    = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
   pv->swizzle_w    = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;
}

/* CCN is proportional to the number of command types dispatched; one case
 * per r300vk_cmd_type is the minimum correct structure. */
static void
r300vk_replay_gpu(struct r300vk_device *device,
                  const struct r300vk_cmd_buffer *cmd)
{
   struct pipe_context *pipe = device->pipe;

   /* VB cache: defers set_vertex_buffers to DRAW time so that it always
    * follows bind_vertex_elements_state (Gallium p_context.h line 417).
    * Accumulating here also preserves lower binding slots when firstBinding > 0
    * without clobbering slots outside the current CmdBindVertexBuffers range. */
   struct pipe_vertex_buffer vb_cache[R300VK_MAX_VERTEX_BINDINGS];
   uint32_t vb_max_used = 0;
   bool vb_dirty = false;
   memset(vb_cache, 0, sizeof(vb_cache));

   for (uint32_t i = 0; i < cmd->entry_count; i++) {
      const struct r300vk_cmd_entry *e = &cmd->entries[i];

      switch (e->type) {

      case R300VK_CMD_BEGIN_RENDER_PASS: {
         struct pipe_framebuffer_state fb;
         memset(&fb, 0, sizeof(fb));
         fb.width  = e->begin_rp.width;
         fb.height = e->begin_rp.height;
         if (e->begin_rp.color_image) {
            fb.nr_cbufs = 1;
            fb.cbufs[0].texture     = e->begin_rp.color_image->resource;
            fb.cbufs[0].format      = e->begin_rp.color_format;
            fb.cbufs[0].level       = 0;
            fb.cbufs[0].first_layer = 0;
            fb.cbufs[0].last_layer  = 0;
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
         const struct r300vk_pipeline *pl = e->bind_pipeline.pipeline;
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
         struct pipe_viewport_state pv;
         viewport_vk_to_gallium(&e->set_vp.vp, &pv);
         pipe->set_viewport_states(pipe, 0, 1, &pv);
         break;
      }

      case R300VK_CMD_SET_SCISSOR: {
         struct pipe_scissor_state sc;
         /* VkRect2D offset is signed; clamp to zero before converting to
          * unsigned Gallium coordinates so negative origins do not wrap. */
         int32_t ox = e->set_sc.scissor.offset.x;
         int32_t oy = e->set_sc.scissor.offset.y;
         sc.minx = (unsigned)(ox > 0 ? ox : 0);
         sc.miny = (unsigned)(oy > 0 ? oy : 0);
         sc.maxx = sc.minx + e->set_sc.scissor.extent.width;
         sc.maxy = sc.miny + e->set_sc.scissor.extent.height;
         pipe->set_scissor_states(pipe, 0, 1, &sc);
         break;
      }

      case R300VK_CMD_BIND_VERTEX_BUFFERS: {
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
            if (first + b + 1 > vb_max_used)
               vb_max_used = first + b + 1;
         }
         vb_dirty = true;
         break;
      }

      case R300VK_CMD_DRAW: {
         /* Flush the VB cache after bind_vertex_elements_state (set by
          * BIND_PIPELINE above in the stream) so the Gallium ordering holds.
          * vb_max_used tracks the highest slot index written so only the live
          * range is submitted, and lower slots from earlier binds are preserved. */
         if (vb_dirty) {
            pipe->set_vertex_buffers(pipe, vb_max_used, vb_cache);
            vb_dirty = false;
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
            .count      = e->draw.count,
            .index_bias = 0,
         };
         pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
         break;
      }

      case R300VK_CMD_END_RENDER_PASS: {
         struct pipe_framebuffer_state empty;
         memset(&empty, 0, sizeof(empty));
         pipe->set_framebuffer_state(pipe, &empty);
         break;
      }

      case R300VK_CMD_COPY_IMAGE_TO_BUFFER:
         /* Handled in the CPU readback pass after flush+fence. */
         break;

      case R300VK_CMD_PIPELINE_BARRIER: {
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
      }
   }
}

/* CPU-side image-to-buffer copies executed after the GPU fence completes. */
static void
r300vk_replay_cpu_readback(struct r300vk_device *device,
                            const struct r300vk_cmd_buffer *cmd)
{
   struct pipe_context *pipe = device->pipe;

   for (uint32_t i = 0; i < cmd->entry_count; i++) {
      const struct r300vk_cmd_entry *e = &cmd->entries[i];
      if (e->type != R300VK_CMD_COPY_IMAGE_TO_BUFFER)
         continue;

      const VkBufferImageCopy2 *region = &e->copy_img_buf.region;
      struct pipe_resource     *src    = e->copy_img_buf.src->resource;
      struct pipe_resource     *dst    = e->copy_img_buf.dst->resource;

      struct pipe_box src_box;
      u_box_2d(region->imageOffset.x, region->imageOffset.y,
               (int)region->imageExtent.width,
               (int)region->imageExtent.height,
               &src_box);

      struct pipe_transfer *src_xfer = NULL;
      const uint8_t *src_map = pipe->texture_map(pipe, src,
                                                   region->imageSubresource.mipLevel,
                                                   PIPE_MAP_READ,
                                                   &src_box, &src_xfer);
      if (!src_map)
         continue;

      /* Row pitch drives both the mapping size and the destination stride.
       * Use the actual format block size rather than assuming 4 bpp so that
       * non-RGBA8 formats (R8, RG16, RGBA16F) map and copy correctly.
       * bufferRowLength == 0 means tightly packed per the Vulkan spec. */
      unsigned bpp = util_format_get_blocksize(src->format);
      unsigned row_pitch = (region->bufferRowLength
                            ? region->bufferRowLength
                            : region->imageExtent.width) * bpp;
      unsigned dst_size  = row_pitch * region->imageExtent.height;

      struct pipe_transfer *dst_xfer = NULL;
      uint8_t *dst_map = pipe_buffer_map_range(pipe, dst,
                                               (unsigned)region->bufferOffset,
                                               dst_size,
                                               PIPE_MAP_WRITE,
                                               &dst_xfer);
      if (!dst_map) {
         pipe->texture_unmap(pipe, src_xfer);
         continue;
      }

      for (unsigned row = 0; row < region->imageExtent.height; row++) {
         memcpy(dst_map + row * row_pitch,
                src_map + row * src_xfer->stride,
                region->imageExtent.width * bpp);
      }

      pipe->buffer_unmap(pipe, dst_xfer);
      pipe->texture_unmap(pipe, src_xfer);
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
      r300vk_replay_gpu(device, cmd);
   }

   struct pipe_fence_handle *fence = NULL;
   pipe->flush(pipe, &fence, 0);
   if (fence) {
      device->screen->fence_finish(device->screen, NULL, fence,
                                   OS_TIMEOUT_INFINITE);
      device->screen->fence_reference(device->screen, &fence, NULL);
   }

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
