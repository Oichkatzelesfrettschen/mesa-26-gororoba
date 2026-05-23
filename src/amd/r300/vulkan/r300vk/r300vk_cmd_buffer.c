/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_cmd_buffer.h"
#include "r300vk_device.h"
#include "r300vk_entrypoints.h"
#include "r300vk_framebuffer.h"
#include "r300vk_image.h"
#include "r300vk_pipeline.h"
#include "r300vk_render_pass.h"
#include "r300vk_buffer.h"

#include "vk_alloc.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_object.h"

#include <string.h>
#include <stdlib.h>

#define R300VK_CMD_INITIAL_CAP 64

static struct r300vk_cmd_entry *
r300vk_cmd_append(struct r300vk_cmd_buffer *cmd)
{
   if (cmd->entry_count >= cmd->entry_cap) {
      uint32_t new_cap = cmd->entry_cap ? cmd->entry_cap * 2 : R300VK_CMD_INITIAL_CAP;
      struct r300vk_cmd_entry *new_entries =
         realloc(cmd->entries, new_cap * sizeof(*cmd->entries));
      if (!new_entries) {
         vk_command_buffer_set_error(&cmd->base, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }
      cmd->entries  = new_entries;
      cmd->entry_cap = new_cap;
   }
   return &cmd->entries[cmd->entry_count++];
}

static VkResult
r300vk_cmd_buffer_create(struct vk_command_pool *pool,
                          VkCommandBufferLevel level,
                          struct vk_command_buffer **cmd_buffer_out)
{
   struct r300vk_cmd_buffer *cmd =
      vk_zalloc(&pool->alloc, sizeof(*cmd), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return vk_error(pool->base.device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = vk_command_buffer_init(pool, &cmd->base,
                                             &r300vk_cmd_buffer_ops, level);
   if (result != VK_SUCCESS) {
      vk_free(&pool->alloc, cmd);
      return result;
   }

   *cmd_buffer_out = &cmd->base;
   return VK_SUCCESS;
}

static void
r300vk_cmd_buffer_reset(struct vk_command_buffer *base,
                         VkCommandBufferResetFlags flags)
{
   struct r300vk_cmd_buffer *cmd =
      container_of(base, struct r300vk_cmd_buffer, base);
   cmd->entry_count        = 0;
   cmd->bound_pipeline     = NULL;
   cmd->current_color_image = NULL;
   vk_command_buffer_reset(base);
}

static void
r300vk_cmd_buffer_destroy(struct vk_command_buffer *base)
{
   struct r300vk_cmd_buffer *cmd =
      container_of(base, struct r300vk_cmd_buffer, base);
   free(cmd->entries);
   vk_command_buffer_finish(base);
   vk_free(&cmd->base.pool->alloc, cmd);
}

const struct vk_command_buffer_ops r300vk_cmd_buffer_ops = {
   .create  = r300vk_cmd_buffer_create,
   .reset   = r300vk_cmd_buffer_reset,
   .destroy = r300vk_cmd_buffer_destroy,
};

VkResult
r300vk_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                           const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(r300vk_cmd_buffer, cmd, commandBuffer);
   cmd->entry_count         = 0;
   cmd->bound_pipeline      = NULL;
   cmd->current_color_image = NULL;
   vk_command_buffer_begin(&cmd->base, pBeginInfo);
   return VK_SUCCESS;
}

VkResult
r300vk_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r300vk_cmd_buffer, cmd, commandBuffer);
   return vk_command_buffer_end(&cmd->base);
}

void
r300vk_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                           const VkRenderPassBeginInfo *pRenderPassBegin,
                           VkSubpassContents contents)
{
   VK_FROM_HANDLE(r300vk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r300vk_render_pass, rp,
                  pRenderPassBegin->renderPass);
   VK_FROM_HANDLE(r300vk_framebuffer, fb,
                  pRenderPassBegin->framebuffer);

   /* Resolve the first color attachment to the underlying pipe_resource
    * and its pipe_format for framebuffer setup at replay time.  Skip the
    * slot if the subpass uses VK_ATTACHMENT_UNUSED. */
   struct r300vk_image *color_image = NULL;
   enum pipe_format color_format = PIPE_FORMAT_NONE;
   VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   VkClearColorValue clear_color = {0};

   if (rp->color_attachment_count > 0) {
      uint32_t att_idx = rp->color_attachment_refs[0];
      if (att_idx != VK_ATTACHMENT_UNUSED &&
          att_idx < fb->attachment_count &&
          att_idx < rp->attachment_count) {
         VK_FROM_HANDLE(r300vk_image_view, iv, fb->attachments[att_idx]);
         color_image  = container_of(iv->vk.image, struct r300vk_image, vk);
         color_format = vk_format_to_pipe_format(rp->attachments[att_idx].format);
         load_op      = rp->attachments[att_idx].load_op;
         if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR &&
             pRenderPassBegin->clearValueCount > att_idx)
            clear_color = pRenderPassBegin->pClearValues[att_idx].color;
      }
   }

   struct r300vk_cmd_entry *e = r300vk_cmd_append(cmd);
   if (!e) return;

   e->type = R300VK_CMD_BEGIN_RENDER_PASS;
   e->begin_rp.color_image  = color_image;
   e->begin_rp.color_format = color_format;
   e->begin_rp.width        = fb->width;
   e->begin_rp.height       = fb->height;
   e->begin_rp.load_op      = load_op;
   e->begin_rp.clear_color  = clear_color;

   cmd->current_color_image = color_image;
}

void
r300vk_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r300vk_cmd_buffer, cmd, commandBuffer);
   struct r300vk_cmd_entry *e = r300vk_cmd_append(cmd);
   if (!e) return;
   e->type = R300VK_CMD_END_RENDER_PASS;
   cmd->current_color_image = NULL;
}

void
r300vk_CmdBindPipeline(VkCommandBuffer commandBuffer,
                        VkPipelineBindPoint pipelineBindPoint,
                        VkPipeline pipeline)
{
   if (pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS)
      return;
   VK_FROM_HANDLE(r300vk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r300vk_pipeline, pl, pipeline);
   struct r300vk_cmd_entry *e = r300vk_cmd_append(cmd);
   if (!e) return;
   e->type              = R300VK_CMD_BIND_PIPELINE;
   e->bind_pipeline.pipeline = pl;
   cmd->bound_pipeline  = pl;
}

void
r300vk_CmdSetViewport(VkCommandBuffer commandBuffer,
                       uint32_t firstViewport,
                       uint32_t viewportCount,
                       const VkViewport *pViewports)
{
   /* RS482/RS485 has a single viewport slot; only viewport 0 is meaningful. */
   if (firstViewport > 0 || viewportCount == 0)
      return;
   VK_FROM_HANDLE(r300vk_cmd_buffer, cmd, commandBuffer);
   struct r300vk_cmd_entry *e = r300vk_cmd_append(cmd);
   if (!e) return;
   e->type      = R300VK_CMD_SET_VIEWPORT;
   e->set_vp.vp = pViewports[0];
}

void
r300vk_CmdSetScissor(VkCommandBuffer commandBuffer,
                      uint32_t firstScissor,
                      uint32_t scissorCount,
                      const VkRect2D *pScissors)
{
   /* RS482/RS485 has a single scissor slot; only scissor 0 is meaningful. */
   if (firstScissor > 0 || scissorCount == 0)
      return;
   VK_FROM_HANDLE(r300vk_cmd_buffer, cmd, commandBuffer);
   struct r300vk_cmd_entry *e = r300vk_cmd_append(cmd);
   if (!e) return;
   e->type           = R300VK_CMD_SET_SCISSOR;
   e->set_sc.scissor = pScissors[0];
}

void
r300vk_CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                             uint32_t firstBinding,
                             uint32_t bindingCount,
                             const VkBuffer *pBuffers,
                             const VkDeviceSize *pOffsets)
{
   VK_FROM_HANDLE(r300vk_cmd_buffer, cmd, commandBuffer);
   struct r300vk_cmd_entry *e = r300vk_cmd_append(cmd);
   if (!e) return;

   uint32_t first = firstBinding < R300VK_MAX_VERTEX_BINDINGS
                    ? firstBinding : R300VK_MAX_VERTEX_BINDINGS;
   uint32_t avail = R300VK_MAX_VERTEX_BINDINGS - first;
   uint32_t count = bindingCount < avail ? bindingCount : avail;
   e->type                      = R300VK_CMD_BIND_VERTEX_BUFFERS;
   e->bind_vbufs.first_binding  = first;
   e->bind_vbufs.binding_count  = count;
   for (uint32_t i = 0; i < count; i++) {
      VK_FROM_HANDLE(r300vk_buffer, buf, pBuffers[i]);
      e->bind_vbufs.buffers[i] = buf;
      e->bind_vbufs.offsets[i] = pOffsets[i];
   }
}

void
r300vk_CmdDraw(VkCommandBuffer commandBuffer,
                uint32_t vertexCount,
                uint32_t instanceCount,
                uint32_t firstVertex,
                uint32_t firstInstance)
{
   VK_FROM_HANDLE(r300vk_cmd_buffer, cmd, commandBuffer);
   struct r300vk_cmd_entry *e = r300vk_cmd_append(cmd);
   if (!e) return;
   e->type                = R300VK_CMD_DRAW;
   e->draw.count          = vertexCount;
   e->draw.first          = firstVertex;
   e->draw.instances      = instanceCount;
   e->draw.first_instance = firstInstance;
   /* Snapshot topology so replay is correct even if a different pipeline
    * is bound before this draw is executed. */
   e->draw.topology = cmd->bound_pipeline
                      ? cmd->bound_pipeline->topology
                      : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

void
r300vk_CmdCopyImageToBuffer2(VkCommandBuffer commandBuffer,
                              const VkCopyImageToBufferInfo2 *pCopyImageToBufferInfo)
{
   VK_FROM_HANDLE(r300vk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r300vk_image, src, pCopyImageToBufferInfo->srcImage);
   VK_FROM_HANDLE(r300vk_buffer, dst, pCopyImageToBufferInfo->dstBuffer);

   for (uint32_t i = 0; i < pCopyImageToBufferInfo->regionCount; i++) {
      struct r300vk_cmd_entry *e = r300vk_cmd_append(cmd);
      if (!e) return;
      e->type               = R300VK_CMD_COPY_IMAGE_TO_BUFFER;
      e->copy_img_buf.src    = src;
      e->copy_img_buf.dst    = dst;
      e->copy_img_buf.region = pCopyImageToBufferInfo->pRegions[i];
   }
}

void
r300vk_CmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                            const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(r300vk_cmd_buffer, cmd, commandBuffer);
   /* Replay issues a flush at submit boundary; no per-barrier action needed. */
   struct r300vk_cmd_entry *e = r300vk_cmd_append(cmd);
   if (!e) return;
   e->type = R300VK_CMD_PIPELINE_BARRIER;
}
