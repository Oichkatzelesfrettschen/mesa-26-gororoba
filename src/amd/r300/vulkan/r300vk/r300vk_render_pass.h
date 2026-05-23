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
struct r300vk_render_pass {
   struct vk_object_base               base;
   uint32_t                            attachment_count;
   struct r300vk_render_pass_attachment attachments[PIPE_MAX_COLOR_BUFS + 1];
   uint32_t                            color_attachment_refs[PIPE_MAX_COLOR_BUFS];
   uint32_t                            color_attachment_count;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_render_pass, base, VkRenderPass,
                                VK_OBJECT_TYPE_RENDER_PASS)

VkResult r300vk_CreateRenderPass(VkDevice device,
                                  const VkRenderPassCreateInfo *pCreateInfo,
                                  const VkAllocationCallbacks *pAllocator,
                                  VkRenderPass *pRenderPass);

void r300vk_DestroyRenderPass(VkDevice device,
                               VkRenderPass renderPass,
                               const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_RENDER_PASS_H */
