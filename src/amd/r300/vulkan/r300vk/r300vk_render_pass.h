/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_RENDER_PASS_H
#define R300VK_RENDER_PASS_H

#include "r300vk_private.h"

#include "vk_object.h"

#include "pipe/p_state.h"  /* PIPE_MAX_COLOR_BUFS */

#ifdef __cplusplus
extern "C" {
#endif

struct r300vk_render_pass_attachment {
   VkFormat            format;
   VkAttachmentLoadOp  load_op;
   VkAttachmentStoreOp store_op;
   VkImageLayout       final_layout;
};

/* r300vk_render_pass stores subpass 0 color attachment descriptions and
 * the index map from subpass slot to attachment array entry.  The command
 * recorder resolves these at CmdBeginRenderPass time against the
 * r300vk_framebuffer's VkImageView handle array. */
/* The most subpasses r300vk replays in one pass.  Two covers the common
 * input-attachment pattern (subpass 0 writes, subpass 1 reads it); the bound is
 * generous so deeper subpass chains record without a special case. */
#define R300VK_MAX_SUBPASSES 8u
#define R300VK_ATTACHMENT_NO_FIRST_USE 0xFFu

/* One subpass: its colour outputs, the input attachments a fragment shader's
 * subpassLoad reads, and the depth/stencil attachment.  Indices reference the
 * render pass attachment array, resolved at CmdBeginRenderPass against the
 * r300vk_framebuffer's VkImageView handles. */
struct r300vk_subpass {
   uint32_t color_attachment_refs[PIPE_MAX_COLOR_BUFS];
   uint32_t color_attachment_count;
   uint32_t input_attachment_refs[PIPE_MAX_COLOR_BUFS];
   uint32_t input_attachment_count;
   uint32_t depth_stencil_attachment_ref;  /* VK_ATTACHMENT_UNUSED if none */
};

struct r300vk_render_pass {
   struct vk_object_base               base;
   uint32_t                            attachment_count;
   struct r300vk_render_pass_attachment attachments[PIPE_MAX_COLOR_BUFS + 1];

   uint32_t                            subpass_count;
   struct r300vk_subpass               subpasses[R300VK_MAX_SUBPASSES];
   /* First subpass index that uses each attachment as a colour or depth/stencil
    * target; R300VK_ATTACHMENT_NO_FIRST_USE when no subpass writes it.  Drives
    * applying each attachment's loadOp at the subpass where it is first used. */
   uint8_t                             first_use_subpass[PIPE_MAX_COLOR_BUFS + 1];

   /* Legacy subpass-0 mirrors (== subpasses[0]), kept for the begin recorder. */
   uint32_t                            color_attachment_refs[PIPE_MAX_COLOR_BUFS];
   uint32_t                            color_attachment_count;
   uint32_t                            input_attachment_refs[PIPE_MAX_COLOR_BUFS];
   uint32_t                            input_attachment_count;
   uint32_t                            depth_stencil_attachment_ref;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_render_pass, base, VkRenderPass,
                                VK_OBJECT_TYPE_RENDER_PASS)

VkResult r300vk_CreateRenderPass(VkDevice device,
                                  const VkRenderPassCreateInfo *pCreateInfo,
                                  const VkAllocationCallbacks *pAllocator,
                                  VkRenderPass *pRenderPass);

VkResult r300vk_CreateRenderPass2(VkDevice device,
                                   const VkRenderPassCreateInfo2 *pCreateInfo,
                                   const VkAllocationCallbacks *pAllocator,
                                   VkRenderPass *pRenderPass);

void r300vk_DestroyRenderPass(VkDevice device,
                               VkRenderPass renderPass,
                               const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_RENDER_PASS_H */
