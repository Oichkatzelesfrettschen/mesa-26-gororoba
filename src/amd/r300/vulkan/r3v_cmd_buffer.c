/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_cmd_buffer.h"
#include "r3v_format.h"
#include "r3v_descriptor.h"
#include "r3v_device.h"
#include "r3v_entrypoints.h"
#include "r3v_object.h"
#include "r3v_framebuffer.h"
#include "r3v_image.h"
#include "r3v_pipeline.h"
#include "r3v_render_pass.h"
#include "r3v_buffer.h"

#include "vk_alloc.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_pipeline_layout.h"
#include "vk_util.h"

#include <stdint.h>
#include <string.h>

#define R3V_CMD_INITIAL_CAP 64

static bool
r3v_push_constants_stage_range_supported(const struct vk_pipeline_layout *layout,
                                            VkShaderStageFlags stage_flags,
                                            uint32_t offset,
                                            uint32_t size)
{
   if (!layout || layout->push_range_count != 1)
      return false;

   const VkPushConstantRange *range = &layout->push_ranges[0];
   const uint64_t update_start = offset;
   const uint64_t update_end = update_start + size;
   const uint64_t range_start = range->offset;
   const uint64_t range_end = range_start + range->size;

   if (update_start < range_start || update_end > range_end)
      return false;

   return stage_flags == range->stageFlags;
}

static struct r3v_cmd_entry *
r3v_cmd_append(struct r3v_cmd_buffer *cmd)
{
   if (cmd->entry_count >= cmd->entry_cap) {
      uint32_t new_cap = R3V_CMD_INITIAL_CAP;
      if (cmd->entry_cap) {
         if (cmd->entry_cap > UINT32_MAX / 2) {
            vk_command_buffer_set_error(&cmd->base,
                                        VK_ERROR_OUT_OF_HOST_MEMORY);
            return NULL;
         }
         new_cap = cmd->entry_cap * 2;
      }
#if SIZE_MAX <= UINT32_MAX
      if ((size_t)new_cap > SIZE_MAX / sizeof(*cmd->entries)) {
         vk_command_buffer_set_error(&cmd->base, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }
#endif
      struct r3v_cmd_entry *new_entries =
         vk_realloc(&cmd->base.pool->alloc, cmd->entries,
                    new_cap * sizeof(*cmd->entries), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!new_entries) {
         vk_command_buffer_set_error(&cmd->base, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }
      cmd->entries  = new_entries;
      cmd->entry_cap = new_cap;
   }
   return &cmd->entries[cmd->entry_count++];
}

/* Free the per-entry heap data the recorder owns (vkCmdUpdateBuffer's inline
 * source copy) before the entries are discarded or the array is freed. */
static void
r3v_cmd_buffer_free_entry_data(struct r3v_cmd_buffer *cmd)
{
   for (uint32_t i = 0; i < cmd->entry_count; i++) {
      if (cmd->entries[i].type == R3V_CMD_UPDATE_BUFFER)
         vk_free(&cmd->base.pool->alloc, cmd->entries[i].update_buffer.data);
   }
}

static void
r3v_cmd_buffer_reset_recording_state(struct r3v_cmd_buffer *cmd)
{
   r3v_cmd_buffer_free_entry_data(cmd);
   cmd->entry_count             = 0;
   cmd->bound_pipeline          = NULL;
   cmd->bound_compute_pipeline  = NULL;
   cmd->current_color_image     = NULL;

   /* The bound index buffer holds a pointer to an r3v_buffer that the app
    * may destroy before re-recording.  vkResetCommandBuffer / vkBeginCommandBuffer
    * return the buffer to the initial state where that binding is undefined, so
    * drop it here.  A re-recorded vkCmdDrawIndexed without a fresh
    * vkCmdBindIndexBuffer then snapshots a NULL buffer with index_size 0, which
    * the replay's index guard skips, rather than dereferencing a freed buffer. */
   cmd->bound_index_buffer      = NULL;
   cmd->bound_index_offset      = 0;
   cmd->bound_index_size        = 0;
   cmd->bound_index_range       = 0;
}

static VkResult
r3v_cmd_buffer_create(struct vk_command_pool *pool,
                          VkCommandBufferLevel level,
                          struct vk_command_buffer **cmd_buffer_out)
{
   struct r3v_cmd_buffer *cmd =
      vk_zalloc(&pool->alloc, sizeof(*cmd), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return vk_error(pool->base.device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* r3v records into its own entry stream (r3v_cmd_append) for a
    * primary command buffer, replayed directly at submit.  A secondary
    * command buffer's commands are not tied to a submission until
    * vkCmdExecuteCommands merges them into a primary buffer, so the
    * vk_common entrypoint generator routes secondary-buffer recording
    * through vk_cmd_enqueue_unless_primary_* into cmd_queue instead of
    * the driver's own entrypoints, then vkCmdExecuteCommands replays that
    * queue (vk_cmd_queue_execute in vk_command_buffer.c) into the primary
    * buffer's real entry stream.  vk_command_buffer_init leaves cmd_queue
    * uninitialized (ctx stays NULL) unless needs_cmd_queue is requested;
    * request it exactly when a secondary buffer will need that queue. */
   VkResult result = vk_command_buffer_init_with_params(
      &cmd->base,
      &(struct vk_command_buffer_init_params){
         .pool = pool,
         .ops = &r3v_cmd_buffer_ops,
         .level = level,
         .needs_cmd_queue = level == VK_COMMAND_BUFFER_LEVEL_SECONDARY,
      });
   if (result != VK_SUCCESS) {
      vk_free(&pool->alloc, cmd);
      return result;
   }

   *cmd_buffer_out = &cmd->base;
   return VK_SUCCESS;
}

static void
r3v_cmd_buffer_reset(struct vk_command_buffer *base,
                         VkCommandBufferResetFlags flags)
{
   struct r3v_cmd_buffer *cmd =
      container_of(base, struct r3v_cmd_buffer, base);
   r3v_cmd_buffer_reset_recording_state(cmd);
   if (flags & VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT) {
      vk_free(&cmd->base.pool->alloc, cmd->entries);
      cmd->entries = NULL;
      cmd->entry_cap = 0;
   }
   vk_command_buffer_reset(base);
}

static void
r3v_cmd_buffer_destroy(struct vk_command_buffer *base)
{
   struct r3v_cmd_buffer *cmd =
      container_of(base, struct r3v_cmd_buffer, base);
   r3v_cmd_buffer_free_entry_data(cmd);
   vk_free(&cmd->base.pool->alloc, cmd->entries);
   vk_command_buffer_finish(base);
   vk_free(&cmd->base.pool->alloc, cmd);
}

const struct vk_command_buffer_ops r3v_cmd_buffer_ops = {
   .create  = r3v_cmd_buffer_create,
   .reset   = r3v_cmd_buffer_reset,
   .destroy = r3v_cmd_buffer_destroy,
};

VkResult
r3v_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                           const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   r3v_cmd_buffer_reset_recording_state(cmd);
   vk_command_buffer_begin(&cmd->base, pBeginInfo);
   return vk_command_buffer_get_record_result(&cmd->base);
}

VkResult
r3v_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   return vk_command_buffer_end(&cmd->base);
}

/* Resolve render-pass attachment att_idx to its backing image and pipe format
 * through the framebuffer (or the imageless begin-info), plus its loadOp and
 * clear value.  Leaves image NULL for an out-of-range index or a null view. */
static void
r3v_resolve_attachment(const struct r3v_render_pass *rp,
                          const struct r3v_framebuffer *fb,
                          const VkRenderPassAttachmentBeginInfo *attach_begin,
                          const VkRenderPassBeginInfo *pRenderPassBegin,
                          uint32_t att_idx,
                          struct r3v_resolved_attachment *out)
{
   memset(out, 0, sizeof(*out));
   out->format  = PIPE_FORMAT_NONE;
   out->load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   if (att_idx >= fb->attachment_count || att_idx >= rp->attachment_count)
      return;

   VkImageView view = fb->imageless
      ? ((attach_begin && att_idx < attach_begin->attachmentCount)
            ? attach_begin->pAttachments[att_idx] : VK_NULL_HANDLE)
      : fb->attachments[att_idx];
   if (view == VK_NULL_HANDLE)
      return;

   VK_FROM_HANDLE(r3v_image_view, iv, view);
   out->image   = container_of(iv->vk.image, struct r3v_image, vk);
   out->format  = r3v_vk_format_to_pipe_format(rp->attachments[att_idx].format);
   out->load_op = rp->attachments[att_idx].load_op;
   if (out->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR &&
       pRenderPassBegin->clearValueCount > att_idx)
      out->clear = pRenderPassBegin->pClearValues[att_idx];
}

/* Fill a begin_rp event with subpass subpass_idx's framebuffer -- its colour
 * outputs and depth/stencil target taken from the cmd buffer's resolved
 * attachment table.  An attachment's loadOp clear is applied only at the subpass
 * where it is first used (rp->first_use_subpass); a later subpass that re-binds
 * an already-used attachment LOADs it.  Returns the first bound color image
 * for draw debug sampling.  Shared by CmdBeginRenderPass (subpass 0) and
 * CmdNextSubpass. */
static struct r3v_image *
r3v_record_subpass_framebuffer(struct r3v_cmd_buffer *cmd,
                                  struct r3v_cmd_entry *e,
                                  uint32_t subpass_idx)
{
   const struct r3v_render_pass *rp = cmd->current_render_pass;
   const struct r3v_subpass *sp = &rp->subpasses[subpass_idx];

   memset(&e->begin_rp, 0, sizeof(e->begin_rp));
   e->begin_rp.render_area_offset_x = cmd->current_rp_offset_x;
   e->begin_rp.render_area_offset_y = cmd->current_rp_offset_y;
   e->begin_rp.width  = cmd->current_rp_width;
   e->begin_rp.height = cmd->current_rp_height;
   e->begin_rp.input_self_dep =
      sp->self_dep_attachment != VK_ATTACHMENT_UNUSED;

   struct r3v_image *ref_color = NULL;
   const uint32_t color_count =
      MIN2(sp->color_attachment_count, (uint32_t)PIPE_MAX_COLOR_BUFS);
   e->begin_rp.color_count = color_count;
   for (uint32_t slot = 0; slot < color_count; slot++) {
      e->begin_rp.color_format[slot] = PIPE_FORMAT_NONE;
      e->begin_rp.load_op[slot]      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

      const uint32_t ref = sp->color_attachment_refs[slot];
      if (ref == VK_ATTACHMENT_UNUSED || ref >= rp->attachment_count)
         continue;
      const struct r3v_resolved_attachment *a = &cmd->current_attachments[ref];
      if (!a->image)
         continue;
      e->begin_rp.color_image[slot]  = a->image;
      e->begin_rp.color_format[slot] = a->format;
      if (rp->first_use_subpass[ref] == subpass_idx &&
          a->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         e->begin_rp.load_op[slot]     = VK_ATTACHMENT_LOAD_OP_CLEAR;
         e->begin_rp.clear_color[slot] = a->clear.color;
      }
      if (!ref_color)
         ref_color = a->image;
   }

   const uint32_t ds = sp->depth_stencil_attachment_ref;
   if (ds != VK_ATTACHMENT_UNUSED && ds < rp->attachment_count &&
       cmd->current_attachments[ds].image) {
      const struct r3v_resolved_attachment *a = &cmd->current_attachments[ds];
      e->begin_rp.ds_image  = a->image;
      e->begin_rp.ds_format = a->format;
      if (rp->first_use_subpass[ds] == subpass_idx &&
          a->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         e->begin_rp.ds_load_op    = VK_ATTACHMENT_LOAD_OP_CLEAR;
         e->begin_rp.clear_depth   = a->clear.depthStencil.depth;
         e->begin_rp.clear_stencil = a->clear.depthStencil.stencil;
      }
   }
   return ref_color;
}

/* Shared body for the VkRenderPass begin entry points (1.0 and
 * VK_KHR_create_renderpass2).  Resolves every render-pass attachment once into
 * the cmd buffer's table, then binds subpass 0's framebuffer; CmdNextSubpass
 * advances through the rest. */
static void
r3v_record_begin_render_pass(struct r3v_cmd_buffer *cmd,
                                const VkRenderPassBeginInfo *pRenderPassBegin)
{
   VK_FROM_HANDLE(r3v_render_pass, rp, pRenderPassBegin->renderPass);
   VK_FROM_HANDLE(r3v_framebuffer, fb, pRenderPassBegin->framebuffer);

   /* A normal framebuffer holds the views; an imageless one
    * (VK_KHR_imageless_framebuffer) supplies them at begin time in a
    * VkRenderPassAttachmentBeginInfo chained on pRenderPassBegin. */
   const VkRenderPassAttachmentBeginInfo *attach_begin =
      fb->imageless ? vk_find_struct_const(pRenderPassBegin,
                                           RENDER_PASS_ATTACHMENT_BEGIN_INFO)
                    : NULL;

   cmd->current_render_pass = rp;
   cmd->current_subpass     = 0;
   cmd->current_rp_offset_x = pRenderPassBegin->renderArea.offset.x;
   cmd->current_rp_offset_y = pRenderPassBegin->renderArea.offset.y;
   cmd->current_rp_width =
      pRenderPassBegin->renderArea.offset.x +
      pRenderPassBegin->renderArea.extent.width;
   cmd->current_rp_height =
      pRenderPassBegin->renderArea.offset.y +
      pRenderPassBegin->renderArea.extent.height;
   const uint32_t att_count =
      MIN2(rp->attachment_count, (uint32_t)(PIPE_MAX_COLOR_BUFS + 1));
   for (uint32_t a = 0; a < PIPE_MAX_COLOR_BUFS + 1; a++)
      memset(&cmd->current_attachments[a], 0,
             sizeof(cmd->current_attachments[a]));
   for (uint32_t a = 0; a < att_count; a++)
      r3v_resolve_attachment(rp, fb, attach_begin, pRenderPassBegin, a,
                                &cmd->current_attachments[a]);

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type = R3V_CMD_BEGIN_RENDER_PASS;
   cmd->current_color_image = r3v_record_subpass_framebuffer(cmd, e, 0);
}

/* Advance to the next subpass: record a NEXT_SUBPASS entry carrying that
 * subpass's framebuffer.  The replay flushes the prior subpass's writes and
 * rebinds, so a draw in subpass N reads subpass M<N's output as an input
 * attachment.  Outside a classic render pass (dynamic rendering) it is a
 * no-op. */
static void
r3v_record_next_subpass(struct r3v_cmd_buffer *cmd)
{
   const struct r3v_render_pass *rp = cmd->current_render_pass;
   if (!rp || cmd->current_subpass + 1 >= rp->subpass_count)
      return;

   cmd->current_subpass++;
   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type = R3V_CMD_NEXT_SUBPASS;
   cmd->current_color_image =
      r3v_record_subpass_framebuffer(cmd, e, cmd->current_subpass);
}

static void
r3v_record_end_render_pass(struct r3v_cmd_buffer *cmd)
{
   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type = R3V_CMD_END_RENDER_PASS;
   cmd->current_color_image = NULL;
   cmd->current_render_pass = NULL;
   cmd->current_rp_offset_x = 0;
   cmd->current_rp_offset_y = 0;
   cmd->current_rp_width = 0;
   cmd->current_rp_height = 0;
}

void
r3v_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                           const VkRenderPassBeginInfo *pRenderPassBegin,
                           VkSubpassContents contents)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   (void)contents;
   r3v_record_begin_render_pass(cmd, pRenderPassBegin);
}

void
r3v_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   r3v_record_end_render_pass(cmd);
}

/* VK_KHR_create_renderpass2 begin/end.  r3v renders a single subpass, so the
 * VkSubpassBeginInfo/VkSubpassEndInfo carry nothing the replay needs.  These
 * reuse the same record helpers as the 1.0 entry points and operate on r3v's
 * own r3v_render_pass / r3v_framebuffer objects, so the common-runtime
 * render-pass emulation (which reads vk_render_pass / vk_framebuffer, not these
 * bespoke types) is never reached. */
void
r3v_CmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                            const VkRenderPassBeginInfo *pRenderPassBegin,
                            const VkSubpassBeginInfo *pSubpassBeginInfo)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   (void)pSubpassBeginInfo;
   r3v_record_begin_render_pass(cmd, pRenderPassBegin);
}

void
r3v_CmdEndRenderPass2(VkCommandBuffer commandBuffer,
                          const VkSubpassEndInfo *pSubpassEndInfo)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   (void)pSubpassEndInfo;
   r3v_record_end_render_pass(cmd);
}

/* Advance to the next subpass, recording that subpass's framebuffer so the
 * replay flushes the prior subpass and rebinds (a draw in the new subpass reads
 * the prior subpass's output as an input attachment).  These overrides also keep
 * the call off the common-runtime vk_common_CmdNextSubpass2, which would
 * dereference vk_render_pass state r3v's bespoke CmdBeginRenderPass never
 * populates (a NULL-pass SIGSEGV in end_subpass). */
void
r3v_CmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   (void)contents;
   r3v_record_next_subpass(cmd);
}

void
r3v_CmdNextSubpass2(VkCommandBuffer commandBuffer,
                        const VkSubpassBeginInfo *pSubpassBeginInfo,
                        const VkSubpassEndInfo *pSubpassEndInfo)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   (void)pSubpassBeginInfo;
   (void)pSubpassEndInfo;
   r3v_record_next_subpass(cmd);
}

/* VK_KHR_dynamic_rendering names the color attachment as a VkImageView
 * directly on VkRenderingInfo, with no VkFramebuffer or VkRenderPass object.
 * The framebuffer setup it needs is identical to the render-pass path, so this
 * records the same R3V_CMD_BEGIN_RENDER_PASS entry that
 * r3v_replay_begin_render_pass consumes: color attachment 0 resolved to its
 * pipe_resource and pipe_format, the render-area far corner as the replay
 * extent, and the load-op clear value.  Each color attachment is bound at its
 * own slot so a fragment-shader output at location i targets attachment i;
 * depth/stencil is the single zsbuf the replay's framebuffer state carries. */
void
r3v_CmdBeginRendering(VkCommandBuffer commandBuffer,
                          const VkRenderingInfo *pRenderingInfo)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);

   /* Resolve every color attachment into its slot.  A null view leaves the
    * slot NULL here; replay fills holes before later bound attachments with
    * throwaway cbufs to preserve slot order without letting r300g duplicate a
    * real attachment into the hole. */
   struct r3v_image *color_image[PIPE_MAX_COLOR_BUFS] = {0};
   enum pipe_format color_format[PIPE_MAX_COLOR_BUFS];
   VkAttachmentLoadOp load_op[PIPE_MAX_COLOR_BUFS];
   VkClearColorValue clear_color[PIPE_MAX_COLOR_BUFS] = {0};
   struct r3v_image *ref_color = NULL;
   uint32_t color_count =
      MIN2(pRenderingInfo->colorAttachmentCount, (uint32_t)PIPE_MAX_COLOR_BUFS);
   for (uint32_t slot = 0; slot < color_count; slot++) {
      color_format[slot] = PIPE_FORMAT_NONE;
      load_op[slot]      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

      const VkRenderingAttachmentInfo *att =
         &pRenderingInfo->pColorAttachments[slot];
      if (att->imageView == VK_NULL_HANDLE)
         continue;

      VK_FROM_HANDLE(r3v_image_view, iv, att->imageView);
      color_image[slot]  = container_of(iv->vk.image, struct r3v_image, vk);
      color_format[slot] = r3v_vk_format_to_pipe_format(iv->vk.format);
      load_op[slot]      = att->loadOp;
      if (load_op[slot] == VK_ATTACHMENT_LOAD_OP_CLEAR)
         clear_color[slot] = att->clearValue.color;
      if (!ref_color)
         ref_color = color_image[slot];
   }

   /* VkRenderingInfo names the depth attachment directly.  A combined
    * depth/stencil image arrives on pDepthAttachment (and pStencilAttachment
    * references the same image); the depth attachment's view format carries
    * the combined pipe format after the r300 stencil-low twin remap. */
   struct r3v_image *ds_image = NULL;
   enum pipe_format ds_format = PIPE_FORMAT_NONE;
   VkAttachmentLoadOp ds_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   float clear_depth = 0.0f;
   uint32_t clear_stencil = 0;
   const VkRenderingAttachmentInfo *ds_att = pRenderingInfo->pDepthAttachment
                                             ? pRenderingInfo->pDepthAttachment
                                             : pRenderingInfo->pStencilAttachment;
   if (ds_att && ds_att->imageView != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(r3v_image_view, ds_iv, ds_att->imageView);
      ds_image   = container_of(ds_iv->vk.image, struct r3v_image, vk);
      ds_format  = r3v_vk_format_to_pipe_format(ds_iv->vk.format);
      ds_load_op = ds_att->loadOp;
      if (ds_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         clear_depth   = ds_att->clearValue.depthStencil.depth;
         clear_stencil = ds_att->clearValue.depthStencil.stencil;
      }
   }

   const VkRect2D *area = &pRenderingInfo->renderArea;

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;

   e->type = R3V_CMD_BEGIN_RENDER_PASS;
   memset(&e->begin_rp, 0, sizeof(e->begin_rp));
   e->begin_rp.color_count = color_count;
   for (uint32_t slot = 0; slot < color_count; slot++) {
      e->begin_rp.color_image[slot]  = color_image[slot];
      e->begin_rp.color_format[slot] = color_format[slot];
      e->begin_rp.load_op[slot]      = load_op[slot];
      e->begin_rp.clear_color[slot]  = clear_color[slot];
   }
   e->begin_rp.render_area_offset_x = area->offset.x;
   e->begin_rp.render_area_offset_y = area->offset.y;
   e->begin_rp.width        = area->offset.x + area->extent.width;
   e->begin_rp.height       = area->offset.y + area->extent.height;
   e->begin_rp.ds_image      = ds_image;
   e->begin_rp.ds_format     = ds_format;
   e->begin_rp.ds_load_op    = ds_load_op;
   e->begin_rp.clear_depth   = clear_depth;
   e->begin_rp.clear_stencil = clear_stencil;

   cmd->current_color_image = ref_color;
   /* Dynamic rendering has no subpasses; a stray CmdNextSubpass must not bind a
    * stale classic render pass. */
   cmd->current_render_pass = NULL;
}

void
r3v_CmdEndRendering(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type = R3V_CMD_END_RENDER_PASS;
   cmd->current_color_image = NULL;
}

void
r3v_CmdBindPipeline(VkCommandBuffer commandBuffer,
                        VkPipelineBindPoint pipelineBindPoint,
                        VkPipeline pipeline)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_pipeline, pl, pipeline);

   /* GRAPHICS and COMPUTE are independent bind points, so a compute bind must
    * not clobber graphics state.  The graphics bind records an entry because
    * replay binds the pipeline's CSOs; a compute pipeline carries no CSOs (its
    * no-op kernel emits no GPU work), so its bind only tracks the pipeline that
    * the next CmdDispatch validates against. */
   if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
      cmd->bound_compute_pipeline = pl;
      return;
   }
   if (pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS)
      return;

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type              = R3V_CMD_BIND_PIPELINE;
   e->bind_pipeline.pipeline = pl;
   cmd->bound_pipeline  = pl;
}


/* Dynamic-state recording.  Each vkCmdSet* appends one R3V_CMD_SET_DYNAMIC_
 * STATE entry naming its field in flags; the replay walker merges them in
 * order.  VK_EXT_extended_dynamic_state is required in practice: zink loads
 * vkCmdBindVertexBuffers2 unconditionally (its binding-stride flag is
 * hardwired) and drives cull/front-face/topology/depth/stencil dynamically. */
static struct r3v_cmd_set_dynamic *
r3v_cmd_append_dyn(struct r3v_cmd_buffer *cmd, uint32_t flag)
{
   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return NULL;
   memset(&e->set_dyn, 0, sizeof(e->set_dyn));
   e->type = R3V_CMD_SET_DYNAMIC_STATE;
   e->set_dyn.flags = flag;
   return &e->set_dyn;
}

#define R3V_DYN_RECORD(flag) \
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer); \
   struct r3v_cmd_set_dynamic *d = r3v_cmd_append_dyn(cmd, flag); \
   if (!d) return

static void
r3v_record_bind_vertex_buffers(struct r3v_cmd_buffer *cmd,
                                  uint32_t firstBinding,
                                  uint32_t bindingCount,
                                  const VkBuffer *pBuffers,
                                  const VkDeviceSize *pOffsets,
                                  const VkDeviceSize *pSizes,
                                  const VkDeviceSize *pStrides);

void
r3v_CmdSetLineStipple(VkCommandBuffer commandBuffer,
                         uint32_t lineStippleFactor,
                         uint16_t lineStipplePattern)
{
   R3V_DYN_RECORD(R3V_DYN_LINE_STIPPLE);
   d->stipple_factor  = lineStippleFactor;
   d->stipple_pattern = lineStipplePattern;
}

void
r3v_CmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode)
{
   R3V_DYN_RECORD(R3V_DYN_CULL);
   d->cull = cullMode;
}

void
r3v_CmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace)
{
   R3V_DYN_RECORD(R3V_DYN_FRONT_FACE);
   d->front = frontFace;
}

void
r3v_CmdSetPrimitiveTopology(VkCommandBuffer commandBuffer,
                               VkPrimitiveTopology primitiveTopology)
{
   R3V_DYN_RECORD(R3V_DYN_TOPOLOGY);
   d->topology = primitiveTopology;
}

void
r3v_CmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable)
{
   R3V_DYN_RECORD(R3V_DYN_DEPTH_TEST);
   d->depth_test = depthTestEnable;
}

void
r3v_CmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable)
{
   R3V_DYN_RECORD(R3V_DYN_DEPTH_WRITE);
   d->depth_write = depthWriteEnable;
}

void
r3v_CmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp)
{
   R3V_DYN_RECORD(R3V_DYN_DEPTH_OP);
   d->depth_op = depthCompareOp;
}

void
r3v_CmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer,
                                   VkBool32 depthBoundsTestEnable)
{
   R3V_DYN_RECORD(R3V_DYN_DEPTH_BOUNDS);
   d->depth_bounds = depthBoundsTestEnable;
}

void
r3v_CmdSetStencilTestEnable(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable)
{
   R3V_DYN_RECORD(R3V_DYN_STENCIL_TEST);
   d->stencil_test = stencilTestEnable;
}

void
r3v_CmdSetStencilOp(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                       VkStencilOp failOp, VkStencilOp passOp,
                       VkStencilOp depthFailOp, VkCompareOp compareOp)
{
   R3V_DYN_RECORD(R3V_DYN_STENCIL_OP);
   d->face_mask = faceMask;
   d->sfail = failOp; d->spass = passOp;
   d->sdepth_fail = depthFailOp; d->scompare = compareOp;
}

void
r3v_CmdSetStencilCompareMask(VkCommandBuffer commandBuffer,
                                VkStencilFaceFlags faceMask, uint32_t compareMask)
{
   R3V_DYN_RECORD(R3V_DYN_STENCIL_CMP_MASK);
   d->face_mask = faceMask;
   d->cmp_mask = compareMask;
}

void
r3v_CmdSetStencilWriteMask(VkCommandBuffer commandBuffer,
                              VkStencilFaceFlags faceMask, uint32_t writeMask)
{
   R3V_DYN_RECORD(R3V_DYN_STENCIL_WR_MASK);
   d->face_mask = faceMask;
   d->wr_mask = writeMask;
}

void
r3v_CmdSetStencilReference(VkCommandBuffer commandBuffer,
                              VkStencilFaceFlags faceMask, uint32_t reference)
{
   R3V_DYN_RECORD(R3V_DYN_STENCIL_REF);
   d->face_mask = faceMask;
   d->ref = reference;
}

void
r3v_CmdSetDepthBias(VkCommandBuffer commandBuffer, float depthBiasConstantFactor,
                       float depthBiasClamp, float depthBiasSlopeFactor)
{
   R3V_DYN_RECORD(R3V_DYN_DEPTH_BIAS);
   d->bias_const = depthBiasConstantFactor;
   d->bias_clamp = depthBiasClamp;
   d->bias_slope = depthBiasSlopeFactor;
}

void
r3v_CmdSetDepthBiasEnable(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable)
{
   R3V_DYN_RECORD(R3V_DYN_DEPTH_BIAS_EN);
   d->bias_enable = depthBiasEnable;
}

void
r3v_CmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4])
{
   R3V_DYN_RECORD(R3V_DYN_BLEND_CONST);
   memcpy(d->blend_const, blendConstants, sizeof(d->blend_const));
}

void
r3v_CmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
   R3V_DYN_RECORD(R3V_DYN_LINE_WIDTH);
   d->line_width = lineWidth;
}

/* The WithCount forms carry the same single-slot payload as the 1.0 calls. */
void
r3v_CmdSetViewportWithCount(VkCommandBuffer commandBuffer,
                               uint32_t viewportCount, const VkViewport *pViewports)
{
   r3v_CmdSetViewport(commandBuffer, 0, viewportCount, pViewports);
}

void
r3v_CmdSetScissorWithCount(VkCommandBuffer commandBuffer,
                              uint32_t scissorCount, const VkRect2D *pScissors)
{
   r3v_CmdSetScissor(commandBuffer, 0, scissorCount, pScissors);
}

/* The sizes are robustness ranges the replay's buffer-size snapshot already
 * bounds, and under zink's ZINK_DYNAMIC_STATE template the strides equal the
 * bound pipeline's vertex-element strides, which the velems CSO carries; the
 * 1.0 recording therefore captures everything the replay consumes. */
void
r3v_CmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding,
                             uint32_t bindingCount, const VkBuffer *pBuffers,
                             const VkDeviceSize *pOffsets, const VkDeviceSize *pSizes,
                             const VkDeviceSize *pStrides)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   r3v_record_bind_vertex_buffers(cmd, firstBinding, bindingCount,
                                     pBuffers, pOffsets, pSizes, pStrides);
}

void
r3v_CmdSetViewport(VkCommandBuffer commandBuffer,
                       uint32_t firstViewport,
                       uint32_t viewportCount,
                       const VkViewport *pViewports)
{
   /* RS482/RS485 has a single viewport slot; only viewport 0 is meaningful. */
   if (firstViewport > 0 || viewportCount == 0)
      return;
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type      = R3V_CMD_SET_VIEWPORT;
   e->set_vp.vp = pViewports[0];
}

void
r3v_CmdSetScissor(VkCommandBuffer commandBuffer,
                      uint32_t firstScissor,
                      uint32_t scissorCount,
                      const VkRect2D *pScissors)
{
   /* RS482/RS485 has a single scissor slot; only scissor 0 is meaningful. */
   if (firstScissor > 0 || scissorCount == 0)
      return;
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type           = R3V_CMD_SET_SCISSOR;
   e->set_sc.scissor = pScissors[0];
}

static void
r3v_record_bind_vertex_buffers(struct r3v_cmd_buffer *cmd,
                                  uint32_t firstBinding,
                                  uint32_t bindingCount,
                                  const VkBuffer *pBuffers,
                                  const VkDeviceSize *pOffsets,
                                  const VkDeviceSize *pSizes,
                                  const VkDeviceSize *pStrides)
{
   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;

   uint32_t first = firstBinding < R3V_MAX_VERTEX_BINDINGS
                    ? firstBinding : R3V_MAX_VERTEX_BINDINGS;
   uint32_t avail = R3V_MAX_VERTEX_BINDINGS - first;
   uint32_t count = bindingCount < avail ? bindingCount : avail;
   e->type                      = R3V_CMD_BIND_VERTEX_BUFFERS;
   e->bind_vbufs.first_binding  = first;
   e->bind_vbufs.binding_count  = count;
   e->bind_vbufs.has_strides    = pStrides != NULL;
   for (uint32_t i = 0; i < count; i++) {
      VK_FROM_HANDLE(r3v_buffer, buf, pBuffers[i]);
      e->bind_vbufs.buffers[i] = buf;
      e->bind_vbufs.offsets[i] = pOffsets[i];
      e->bind_vbufs.strides[i] = pStrides ? pStrides[i] : 0;
      e->bind_vbufs.sizes[i]   = pSizes ? pSizes[i] : VK_WHOLE_SIZE;
   }
}

void
r3v_CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                             uint32_t firstBinding,
                             uint32_t bindingCount,
                             const VkBuffer *pBuffers,
                             const VkDeviceSize *pOffsets)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   r3v_record_bind_vertex_buffers(cmd, firstBinding, bindingCount,
                                     pBuffers, pOffsets, NULL, NULL);
}

void
r3v_CmdDraw(VkCommandBuffer commandBuffer,
                uint32_t vertexCount,
                uint32_t instanceCount,
                uint32_t firstVertex,
                uint32_t firstInstance)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type                = R3V_CMD_DRAW;
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
r3v_CmdDrawIndirect(VkCommandBuffer commandBuffer,
                       VkBuffer _buffer,
                       VkDeviceSize offset,
                       uint32_t drawCount,
                       uint32_t stride)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_buffer, buffer, _buffer);

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type                     = R3V_CMD_DRAW_INDIRECT;
   e->draw_indirect.buffer     = buffer;
   e->draw_indirect.offset     = offset;
   e->draw_indirect.draw_count = drawCount;
   e->draw_indirect.stride     = stride;
   e->draw_indirect.topology   = cmd->bound_pipeline
                                 ? cmd->bound_pipeline->topology
                                 : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

static uint32_t
r3v_index_type_bytes(VkIndexType type)
{
   switch (type) {
   case VK_INDEX_TYPE_UINT8:  return 1;
   case VK_INDEX_TYPE_UINT16: return 2;
   case VK_INDEX_TYPE_UINT32: return 4;
   /* Fail closed on VK_INDEX_TYPE_NONE_KHR and any unrecognized value: a 0
    * stride sets bound_index_size to 0, and the indexed-draw replay guard
    * (di->index_size == 0) skips the draw rather than fetch with a guessed
    * stride.  Returning 2 here would silently reinterpret the indices. */
   default:                   return 0;
   }
}

/* vkCmdBindIndexBuffer[2] only update the recording cmd-buffer's bound-index
 * state; each following vkCmdDrawIndexed snapshots it into its command entry.
 * vk_common provides CmdBindIndexBuffer2KHR by forwarding to the base
 * CmdBindIndexBuffer, so both are implemented to cover either routing. */
void
r3v_CmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                          VkBuffer _buffer,
                          VkDeviceSize offset,
                          VkIndexType indexType)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_buffer, buffer, _buffer);
   cmd->bound_index_buffer = buffer;
   cmd->bound_index_offset = offset;
   cmd->bound_index_size   = r3v_index_type_bytes(indexType);
   cmd->bound_index_range  = (buffer && buffer->size > offset)
                             ? buffer->size - offset : 0;
}

void
r3v_CmdBindIndexBuffer2(VkCommandBuffer commandBuffer,
                           VkBuffer _buffer,
                           VkDeviceSize offset,
                           VkDeviceSize size,
                           VkIndexType indexType)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_buffer, buffer, _buffer);
   cmd->bound_index_buffer = buffer;
   cmd->bound_index_offset = offset;
   cmd->bound_index_size   = r3v_index_type_bytes(indexType);
   VkDeviceSize avail = (buffer && buffer->size > offset)
                        ? buffer->size - offset : 0;
   cmd->bound_index_range  = (size == VK_WHOLE_SIZE || size > avail)
                             ? avail : size;
}

void
r3v_CmdDrawIndexed(VkCommandBuffer commandBuffer,
                      uint32_t indexCount,
                      uint32_t instanceCount,
                      uint32_t firstIndex,
                      int32_t vertexOffset,
                      uint32_t firstInstance)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type                        = R3V_CMD_DRAW_INDEXED;
   e->draw_indexed.index_buffer   = cmd->bound_index_buffer;
   e->draw_indexed.index_offset   = cmd->bound_index_offset;
   e->draw_indexed.index_range    = cmd->bound_index_range;
   e->draw_indexed.index_size     = cmd->bound_index_size;
   e->draw_indexed.index_count    = indexCount;
   e->draw_indexed.first_index    = firstIndex;
   e->draw_indexed.vertex_offset  = vertexOffset;
   e->draw_indexed.instances      = instanceCount;
   e->draw_indexed.first_instance = firstInstance;
   e->draw_indexed.topology       = cmd->bound_pipeline
                                    ? cmd->bound_pipeline->topology
                                    : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

/* vkCmdDrawIndexedIndirect snapshots the indirect-args buffer and the index
 * buffer bound by vkCmdBindIndexBuffer[2] (like vkCmdDrawIndexed); replay reads
 * each VkDrawIndexedIndirectCommand and runs the indexed draw path per command. */
void
r3v_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                              VkBuffer _buffer,
                              VkDeviceSize offset,
                              uint32_t drawCount,
                              uint32_t stride)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_buffer, buffer, _buffer);

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type                                = R3V_CMD_DRAW_INDEXED_INDIRECT;
   e->draw_indexed_indirect.buffer        = buffer;
   e->draw_indexed_indirect.offset        = offset;
   e->draw_indexed_indirect.draw_count    = drawCount;
   e->draw_indexed_indirect.stride        = stride;
   e->draw_indexed_indirect.index_buffer  = cmd->bound_index_buffer;
   e->draw_indexed_indirect.index_offset  = cmd->bound_index_offset;
   e->draw_indexed_indirect.index_range   = cmd->bound_index_range;
   e->draw_indexed_indirect.index_size    = cmd->bound_index_size;
   e->draw_indexed_indirect.topology      = cmd->bound_pipeline
                                            ? cmd->bound_pipeline->topology
                                            : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

/* Record a push-constant window update into the entry stream.  vk_common forwards
 * CmdPushConstants to the r3v base entrypoint, so leaving it unimplemented
 * makes the call jump to a NULL dispatch slot (SIGSEGV).  Replay applies these in
 * order into the running maxPushConstantsSize buffer that a push-constants-only
 * pipeline binds at CONST[0] (r3v_bind_push_constants); a pipeline that reads
 * both push constants and a UBO is rejected at compile, since r300's single
 * constant file cannot host both.  r3v accepts one push-constant range and
 * records only updates whose stage flags match that range, so replay never
 * broadens a subset-stage update across the shared window. */
void
r3v_CmdPushConstants(VkCommandBuffer commandBuffer,
                        VkPipelineLayout layout,
                        VkShaderStageFlags stageFlags,
                        uint32_t offset,
                        uint32_t size,
                        const void *pValues)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   /* A zero-size update may pass pValues == NULL, and memcpy(dst, NULL, 0) is
    * undefined behavior; guard it.  Ignore an out-of-window write rather than
    * overflow the fixed entry payload. */
   if (size == 0 || pValues == NULL)
      return;
   if ((uint64_t)offset + size > R3V_MAX_PUSH_CONSTANTS_SIZE)
      return;

   VK_FROM_HANDLE(vk_pipeline_layout, pc_layout, layout);
   if (!r3v_push_constants_stage_range_supported(pc_layout, stageFlags,
                                                    offset, size))
      return;

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type                  = R3V_CMD_PUSH_CONSTANTS;
   e->push_constants.offset = offset;
   e->push_constants.size   = size;
   memcpy(e->push_constants.data, pValues, size);
}

void
r3v_CmdDispatch(VkCommandBuffer commandBuffer,
                   uint32_t groupCountX,
                   uint32_t groupCountY,
                   uint32_t groupCountZ)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);

   /* Dispatch without a bound compute pipeline is undefined; a compute pipeline
    * is created only under the experimental hybrid-compute gate, so a NULL here
    * also covers the ungated case where compute is not exposed.  Surface it as a
    * command-buffer error rather than recording a dispatch with no kernel. */
   if (!cmd->bound_compute_pipeline) {
      vk_command_buffer_set_error(&cmd->base, VK_ERROR_FEATURE_NOT_PRESENT);
      return;
   }

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type                  = R3V_CMD_DISPATCH;
   e->dispatch.group_count_x = groupCountX;
   e->dispatch.group_count_y = groupCountY;
   e->dispatch.group_count_z = groupCountZ;
   /* Snapshot the compute pipeline so the queue replay can branch on
    * identity_map.is_identity_map without re-walking the cmd-buffer for
    * the most recent CmdBindPipeline. */
   e->dispatch.pipeline = cmd->bound_compute_pipeline;
}

void
r3v_CmdBindDescriptorSets2KHR(VkCommandBuffer commandBuffer,
                                  const VkBindDescriptorSetsInfoKHR *info)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   if (!info || info->descriptorSetCount == 0)
      return;

   if (info->descriptorSetCount > R3V_MAX_BOUND_DESCRIPTOR_SETS ||
       info->dynamicOffsetCount > R3V_MAX_DYNAMIC_OFFSETS) {
      vk_command_buffer_set_error(&cmd->base, VK_ERROR_FEATURE_NOT_PRESENT);
      return;
   }

   /* The legacy shim emits one bind-point stage mask, while the 2KHR entrypoint
    * can name graphics and compute stages together.  Replay records the mask
    * and applies the descriptor update to every named bind-point state. */
   const uint32_t targets = r3v_descriptor_bind_targets(info->stageFlags);
   if (targets == 0) {
      vk_command_buffer_set_error(&cmd->base, VK_ERROR_FEATURE_NOT_PRESENT);
      return;
   }
   const VkPipelineBindPoint bp =
      (targets & R3V_DESCRIPTOR_BIND_COMPUTE) &&
      !(targets & R3V_DESCRIPTOR_BIND_GRAPHICS)
      ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;

   uint32_t sets_n = info->descriptorSetCount;
   uint32_t doff_n = info->dynamicOffsetCount;

   e->type                          = R3V_CMD_BIND_DESCRIPTOR_SETS;
   e->bind_dsets.bind_point         = bp;
   e->bind_dsets.stage_flags        = info->stageFlags;
   e->bind_dsets.pipeline_layout    = info->layout;
   e->bind_dsets.first_set          = info->firstSet;
   e->bind_dsets.set_count          = sets_n;
   e->bind_dsets.dynamic_offset_count = doff_n;
   for (uint32_t i = 0; i < sets_n; i++) {
      VK_FROM_HANDLE(r3v_descriptor_set, dset, info->pDescriptorSets[i]);
      e->bind_dsets.sets[i] = dset;
   }
   for (uint32_t i = 0; i < doff_n; i++)
      e->bind_dsets.dynamic_offsets[i] = info->pDynamicOffsets[i];
}

void
r3v_CmdDispatchBase(VkCommandBuffer commandBuffer,
                       uint32_t baseGroupX,
                       uint32_t baseGroupY,
                       uint32_t baseGroupZ,
                       uint32_t groupCountX,
                       uint32_t groupCountY,
                       uint32_t groupCountZ)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   (void)baseGroupX;
   (void)baseGroupY;
   (void)baseGroupZ;
   (void)groupCountX;
   (void)groupCountY;
   (void)groupCountZ;

   vk_command_buffer_set_error(&cmd->base, VK_ERROR_FEATURE_NOT_PRESENT);
}

void
r3v_CmdDispatchIndirect(VkCommandBuffer commandBuffer,
                           VkBuffer buffer,
                           VkDeviceSize offset)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   (void)buffer;
   (void)offset;

   vk_command_buffer_set_error(&cmd->base, VK_ERROR_FEATURE_NOT_PRESENT);
}

void
r3v_CmdCopyImageToBuffer2(VkCommandBuffer commandBuffer,
                              const VkCopyImageToBufferInfo2 *pCopyImageToBufferInfo)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_image, src, pCopyImageToBufferInfo->srcImage);
   VK_FROM_HANDLE(r3v_buffer, dst, pCopyImageToBufferInfo->dstBuffer);

   for (uint32_t i = 0; i < pCopyImageToBufferInfo->regionCount; i++) {
      struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
      if (!e) return;
      e->type               = R3V_CMD_COPY_IMAGE_TO_BUFFER;
      e->copy_img_buf.src    = src;
      e->copy_img_buf.dst    = dst;
      e->copy_img_buf.region = pCopyImageToBufferInfo->pRegions[i];
   }
}

void
r3v_CmdCopyBufferToImage2(VkCommandBuffer commandBuffer,
                             const VkCopyBufferToImageInfo2 *pCopyBufferToImageInfo)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_buffer, src, pCopyBufferToImageInfo->srcBuffer);
   VK_FROM_HANDLE(r3v_image, dst, pCopyBufferToImageInfo->dstImage);

   for (uint32_t i = 0; i < pCopyBufferToImageInfo->regionCount; i++) {
      struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
      if (!e) return;
      e->type                = R3V_CMD_COPY_BUFFER_TO_IMAGE;
      e->copy_buf_img.src    = src;
      e->copy_buf_img.dst    = dst;
      e->copy_buf_img.region = pCopyBufferToImageInfo->pRegions[i];
   }
}

void
r3v_CmdCopyImage2(VkCommandBuffer commandBuffer,
                     const VkCopyImageInfo2 *pCopyImageInfo)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_image, src, pCopyImageInfo->srcImage);
   VK_FROM_HANDLE(r3v_image, dst, pCopyImageInfo->dstImage);

   for (uint32_t i = 0; i < pCopyImageInfo->regionCount; i++) {
      struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
      if (!e) return;
      e->type               = R3V_CMD_COPY_IMAGE;
      e->copy_image.src     = src;
      e->copy_image.dst     = dst;
      e->copy_image.region  = pCopyImageInfo->pRegions[i];
   }
}

void
r3v_CmdBlitImage2(VkCommandBuffer commandBuffer,
                     const VkBlitImageInfo2 *pBlitImageInfo)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_image, src, pBlitImageInfo->srcImage);
   VK_FROM_HANDLE(r3v_image, dst, pBlitImageInfo->dstImage);

   /* One entry per region; the filter is per-call, so it is copied into each
    * entry.  The GPU blit and the sampler-cap fallback decision happen at
    * replay, where the pipe_screen is reachable. */
   for (uint32_t i = 0; i < pBlitImageInfo->regionCount; i++) {
      struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
      if (!e) return;
      e->type               = R3V_CMD_BLIT_IMAGE;
      e->blit_image.src     = src;
      e->blit_image.dst     = dst;
      e->blit_image.region  = pBlitImageInfo->pRegions[i];
      e->blit_image.filter  = pBlitImageInfo->filter;
   }
}

void
r3v_CmdClearColorImage(VkCommandBuffer commandBuffer,
                          VkImage image,
                          VkImageLayout imageLayout,
                          const VkClearColorValue *pColor,
                          uint32_t rangeCount,
                          const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_image, img, image);
   (void)imageLayout;

   for (uint32_t i = 0; i < rangeCount; i++) {
      struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
      if (!e) return;
      e->type                    = R3V_CMD_CLEAR_COLOR_IMAGE;
      e->clear_color_image.image = img;
      e->clear_color_image.color = *pColor;
      e->clear_color_image.range = pRanges[i];
   }
}

void
r3v_CmdClearDepthStencilImage(VkCommandBuffer commandBuffer,
                                 VkImage image,
                                 VkImageLayout imageLayout,
                                 const VkClearDepthStencilValue *pDepthStencil,
                                 uint32_t rangeCount,
                                 const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_image, img, image);
   (void)imageLayout;

   for (uint32_t i = 0; i < rangeCount; i++) {
      struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
      if (!e) return;
      e->type                            = R3V_CMD_CLEAR_DEPTH_STENCIL_IMAGE;
      e->clear_depth_stencil_image.image = img;
      e->clear_depth_stencil_image.value = *pDepthStencil;
      e->clear_depth_stencil_image.range = pRanges[i];
   }
}

void
r3v_CmdClearAttachments(VkCommandBuffer commandBuffer,
                           uint32_t attachmentCount,
                           const VkClearAttachment *pAttachments,
                           uint32_t rectCount,
                           const VkClearRect *pRects)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);

   for (uint32_t attachment = 0; attachment < attachmentCount; attachment++) {
      const VkImageAspectFlags aspect = pAttachments[attachment].aspectMask;
      if (!(aspect & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT |
                      VK_IMAGE_ASPECT_STENCIL_BIT)))
         continue;

      for (uint32_t rect = 0; rect < rectCount; rect++) {
         struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
         if (!e)
            return;

         e->type                      = R3V_CMD_CLEAR_ATTACHMENTS;
         e->clear_attachments.aspect  = aspect;
         e->clear_attachments.color_attachment =
            pAttachments[attachment].colorAttachment;
         e->clear_attachments.color   = pAttachments[attachment].clearValue.color;
         e->clear_attachments.depth   =
            pAttachments[attachment].clearValue.depthStencil.depth;
         e->clear_attachments.stencil =
            pAttachments[attachment].clearValue.depthStencil.stencil;
         e->clear_attachments.rect    = pRects[rect].rect;
      }
   }
}

void
r3v_CmdFillBuffer(VkCommandBuffer commandBuffer,
                     VkBuffer dstBuffer,
                     VkDeviceSize dstOffset,
                     VkDeviceSize size,
                     uint32_t data)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_buffer, buf, dstBuffer);

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type               = R3V_CMD_FILL_BUFFER;
   e->fill_buffer.buffer = buf;
   e->fill_buffer.offset = dstOffset;
   e->fill_buffer.size   = size;
   e->fill_buffer.data   = data;
}

/* Occlusion query begin/end.  r300 supports one occlusion query at a time; the
 * replay brackets the spanned draws with an r300 PIPE_QUERY_OCCLUSION_COUNTER.
 * The VkQueryControlFlags (PRECISE) refinement is a no-op: r300's ZPASS counter
 * is always exact, so a precise result is what the non-precise path returns. */
void
r3v_CmdBeginQuery(VkCommandBuffer commandBuffer,
                     VkQueryPool queryPool,
                     uint32_t query,
                     VkQueryControlFlags flags)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(vk_query_pool, vk_pool, queryPool);
   (void)flags;   /* PRECISE is a no-op: r300's ZPASS counter is always exact. */

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type        = R3V_CMD_BEGIN_QUERY;
   e->query.pool  = r3v_query_pool(vk_pool);
   e->query.query = query;
}

void
r3v_CmdEndQuery(VkCommandBuffer commandBuffer,
                   VkQueryPool queryPool,
                   uint32_t query)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(vk_query_pool, vk_pool, queryPool);

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type        = R3V_CMD_END_QUERY;
   e->query.pool  = r3v_query_pool(vk_pool);
   e->query.query = query;
}

void
r3v_CmdResetQueryPool(VkCommandBuffer commandBuffer,
                         VkQueryPool queryPool,
                         uint32_t firstQuery,
                         uint32_t queryCount)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(vk_query_pool, vk_pool, queryPool);

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type                         = R3V_CMD_RESET_QUERY_POOL;
   e->reset_query_pool.pool        = r3v_query_pool(vk_pool);
   e->reset_query_pool.first_query = firstQuery;
   e->reset_query_pool.query_count = queryCount;
}

/* r3v has no buffer device address, so the runtime's vk_common
 * CmdCopyQueryPoolResults (which resolves dstBuffer via vk_buffer_address and
 * asserts on a zero device address) cannot run.  Record the copy and resolve it
 * on the host at replay, after the end-query store, from the per-slot storage. */
void
r3v_CmdCopyQueryPoolResults(VkCommandBuffer commandBuffer,
                               VkQueryPool queryPool,
                               uint32_t firstQuery,
                               uint32_t queryCount,
                               VkBuffer dstBuffer,
                               VkDeviceSize dstOffset,
                               VkDeviceSize stride,
                               VkQueryResultFlags flags)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(vk_query_pool, vk_pool, queryPool);
   VK_FROM_HANDLE(r3v_buffer, dst, dstBuffer);

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type                              = R3V_CMD_COPY_QUERY_POOL_RESULTS;
   e->copy_query_pool_results.pool        = r3v_query_pool(vk_pool);
   e->copy_query_pool_results.dst         = dst;
   e->copy_query_pool_results.first_query = firstQuery;
   e->copy_query_pool_results.query_count = queryCount;
   e->copy_query_pool_results.dst_offset  = dstOffset;
   e->copy_query_pool_results.stride      = stride;
   e->copy_query_pool_results.flags       = flags;
}

void
r3v_CmdCopyBuffer2(VkCommandBuffer commandBuffer,
                      const VkCopyBufferInfo2 *pCopyBufferInfo)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_buffer, src, pCopyBufferInfo->srcBuffer);
   VK_FROM_HANDLE(r3v_buffer, dst, pCopyBufferInfo->dstBuffer);

   for (uint32_t i = 0; i < pCopyBufferInfo->regionCount; i++) {
      const VkBufferCopy2 *r = &pCopyBufferInfo->pRegions[i];
      struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
      if (!e) return;
      e->type                  = R3V_CMD_COPY_BUFFER;
      e->copy_buffer.src        = src;
      e->copy_buffer.dst        = dst;
      e->copy_buffer.src_offset = r->srcOffset;
      e->copy_buffer.dst_offset = r->dstOffset;
      e->copy_buffer.size       = r->size;
   }
}

void
r3v_CmdUpdateBuffer(VkCommandBuffer commandBuffer,
                       VkBuffer dstBuffer,
                       VkDeviceSize dstOffset,
                       VkDeviceSize dataSize,
                       const void *pData)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_buffer, buf, dstBuffer);

   if (dataSize == 0)
      return;

   if (dataSize > SIZE_MAX) {
      vk_command_buffer_set_error(&cmd->base, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   /* pData is caller-owned only for this call, so copy it now; the command-pool
    * allocator owns the copy until reset/destroy. */
   void *copy = vk_alloc(&cmd->base.pool->alloc, (size_t)dataSize, 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!copy) {
      vk_command_buffer_set_error(&cmd->base, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   memcpy(copy, pData, dataSize);

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) {
      vk_free(&cmd->base.pool->alloc, copy);
      return;
   }
   e->type                 = R3V_CMD_UPDATE_BUFFER;
   e->update_buffer.buffer = buf;
   e->update_buffer.offset = dstOffset;
   e->update_buffer.size   = dataSize;
   e->update_buffer.data   = copy;
}

void
r3v_CmdSetEvent2(VkCommandBuffer commandBuffer,
                    VkEvent _event,
                    const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_event, event, _event);

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type        = R3V_CMD_SET_EVENT;
   e->event.event = event;
}

void
r3v_CmdResetEvent2(VkCommandBuffer commandBuffer,
                      VkEvent _event,
                      VkPipelineStageFlags2 stageMask)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(r3v_event, event, _event);

   struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
   if (!e) return;
   e->type        = R3V_CMD_RESET_EVENT;
   e->event.event = event;
}

void
r3v_CmdWaitEvents2(VkCommandBuffer commandBuffer,
                      uint32_t eventCount,
                      const VkEvent *pEvents,
                      const VkDependencyInfo *pDependencyInfos)
{
   /* The post-fence CPU pass replays entries in recorded order, so any event a
    * prior CmdSetEvent2 in this submit signalled is already applied by the time
    * a wait would run; the wait is a no-op on the single-queue serialized model.
    * (Cross-submit GPU-side event gating of later GPU work is not modelled --
    * GPU draws replay before the CPU pass; the host event contract is honoured.) */
}

void
r3v_CmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                            const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(r3v_cmd_buffer, cmd, commandBuffer);

   uint32_t image_count = pDependencyInfo ?
      pDependencyInfo->imageMemoryBarrierCount : 0;

   /* No image barrier: still record one entry so the replay flush creates the
    * submit boundary the execution/memory barrier requires. */
   if (image_count == 0) {
      struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
      if (!e) return;
      e->type               = R3V_CMD_PIPELINE_BARRIER;
      e->barrier.image      = NULL;
      e->barrier.new_layout = VK_IMAGE_LAYOUT_UNDEFINED;
      return;
   }

   /* One ledger update per image barrier.  Every image in the dependency must
    * reach the resource-state ledger at replay, not only the first; recording
    * a single entry left the other images with stale layouts.  Each entry
    * also acts as an independent flush boundary, which is conservative but
    * correct for the RS482/RS485 UMA no-aux-surface model. */
   for (uint32_t i = 0; i < image_count; i++) {
      const VkImageMemoryBarrier2 *ib = &pDependencyInfo->pImageMemoryBarriers[i];
      struct r3v_cmd_entry *e = r3v_cmd_append(cmd);
      if (!e) return;
      VK_FROM_HANDLE(r3v_image, img, ib->image);
      e->type               = R3V_CMD_PIPELINE_BARRIER;
      e->barrier.image      = img;
      e->barrier.new_layout = ib->newLayout;
   }
}
