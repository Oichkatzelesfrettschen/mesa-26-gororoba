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

#include "compiler/shader_enums.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "pipe/p_defines.h"
#include "util/u_inlines.h"
#include "util/box.h"
#include "util/format/u_format.h"

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

static void
r300vk_replay_gpu(struct r300vk_device *device,
                  const struct r300vk_cmd_buffer *cmd)
{
   struct pipe_context *pipe = device->pipe;

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
         sc.minx = (unsigned)e->set_sc.scissor.offset.x;
         sc.miny = (unsigned)e->set_sc.scissor.offset.y;
         sc.maxx = sc.minx + e->set_sc.scissor.extent.width;
         sc.maxy = sc.miny + e->set_sc.scissor.extent.height;
         pipe->set_scissor_states(pipe, 0, 1, &sc);
         break;
      }

      case R300VK_CMD_BIND_VERTEX_BUFFERS: {
         uint32_t first = e->bind_vbufs.first_binding;
         uint32_t count = e->bind_vbufs.binding_count;
         struct pipe_vertex_buffer vb[R300VK_MAX_VERTEX_BINDINGS] = {0};
         for (uint32_t b = 0; b < count; b++) {
            vb[first + b].is_user_buffer  = false;
            vb[first + b].buffer_offset   = (unsigned)e->bind_vbufs.offsets[b];
            vb[first + b].buffer.resource = e->bind_vbufs.buffers[b]->resource;
         }
         pipe->set_vertex_buffers(pipe, first + count, vb);
         break;
      }

      case R300VK_CMD_DRAW: {
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
      case R300VK_CMD_PIPELINE_BARRIER:
         /* Handled in the CPU readback pass after flush+fence. */
         break;
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

   return VK_SUCCESS;
}
