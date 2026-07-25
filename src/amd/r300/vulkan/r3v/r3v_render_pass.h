/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_RENDER_PASS_H
#define R3V_RENDER_PASS_H

#include "r3v_private.h"

#include "vk_object.h"

#include "pipe/p_state.h"  /* PIPE_MAX_COLOR_BUFS */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct r3v_render_pass_attachment {
   VkFormat            format;
   VkAttachmentLoadOp  load_op;
   VkAttachmentStoreOp store_op;
   VkImageLayout       final_layout;
};

/* r3v_render_pass stores each subpass's attachment reference map.  The
 * command recorder resolves those references at CmdBeginRenderPass time against
 * the r3v_framebuffer's VkImageView handle array. */
#define R3V_ATTACHMENT_NO_FIRST_USE UINT32_MAX

/* One subpass: its colour outputs, the input attachments a fragment shader's
 * subpassLoad reads, and the depth/stencil attachment.  Indices reference the
 * render pass attachment array, resolved at CmdBeginRenderPass against the
 * r3v_framebuffer's VkImageView handles. */
struct r3v_subpass {
   uint32_t color_attachment_refs[PIPE_MAX_COLOR_BUFS];
   uint32_t color_attachment_count;
   uint32_t input_attachment_refs[PIPE_MAX_COLOR_BUFS];
   VkImageAspectFlags input_attachment_aspects[PIPE_MAX_COLOR_BUFS];
   uint32_t input_attachment_count;
   uint32_t depth_stencil_attachment_ref;  /* VK_ATTACHMENT_UNUSED if none */
   VkImageLayout depth_stencil_attachment_layout;
   VkImageLayout depth_stencil_stencil_layout;
   /* Attachment this subpass both reads as input and writes as color or
    * depth/stencil (VK_ATTACHMENT_UNUSED if none).  The replay reads it
    * through a snapshot copy so the TX unit never samples the live render
    * target. */
   uint32_t self_dep_attachment;
};

struct r3v_render_pass {
   struct vk_object_base               base;
   uint32_t                            attachment_count;
   struct r3v_render_pass_attachment attachments[PIPE_MAX_COLOR_BUFS + 1];

   uint32_t                            subpass_count;
   struct r3v_subpass              *subpasses;
   /* First subpass index that uses each attachment as a color or depth/stencil
    * target; R3V_ATTACHMENT_NO_FIRST_USE when no subpass writes it.  Drives
    * applying each attachment's loadOp at the subpass where it is first used. */
   uint32_t                            first_use_subpass[PIPE_MAX_COLOR_BUFS + 1];

   /* Legacy subpass-0 mirrors (== subpasses[0]), kept for the begin recorder. */
   uint32_t                            color_attachment_refs[PIPE_MAX_COLOR_BUFS];
   uint32_t                            color_attachment_count;
   uint32_t                            input_attachment_refs[PIPE_MAX_COLOR_BUFS];
   uint32_t                            input_attachment_count;
   uint32_t                            depth_stencil_attachment_ref;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_render_pass, base, VkRenderPass,
                                VK_OBJECT_TYPE_RENDER_PASS)

VkResult r3v_CreateRenderPass(VkDevice device,
                                  const VkRenderPassCreateInfo *pCreateInfo,
                                  const VkAllocationCallbacks *pAllocator,
                                  VkRenderPass *pRenderPass);

VkResult r3v_CreateRenderPass2(VkDevice device,
                                   const VkRenderPassCreateInfo2 *pCreateInfo,
                                   const VkAllocationCallbacks *pAllocator,
                                   VkRenderPass *pRenderPass);

void r3v_DestroyRenderPass(VkDevice device,
                               VkRenderPass renderPass,
                               const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

#endif /* R3V_RENDER_PASS_H */
