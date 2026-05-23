/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_FRAMEBUFFER_H
#define R300VK_FRAMEBUFFER_H

#include "r300vk_private.h"

#include "vk_object.h"

#include "pipe/p_state.h"  /* PIPE_MAX_COLOR_BUFS */

#ifdef __cplusplus
extern "C" {
#endif

/* r300vk_framebuffer stores the raw VkImageView handles for the attachments
 * alongside the dimensions.  The command recorder resolves the handles to
 * r300vk_image_view pointers and then to pipe_resource pointers at
 * CmdBeginRenderPass time. */
struct r300vk_framebuffer {
   struct vk_object_base  base;
   uint32_t               width;
   uint32_t               height;
   uint32_t               attachment_count;
   VkImageView            attachments[PIPE_MAX_COLOR_BUFS + 1];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_framebuffer, base, VkFramebuffer,
                                VK_OBJECT_TYPE_FRAMEBUFFER)

VkResult r300vk_CreateFramebuffer(VkDevice device,
                                   const VkFramebufferCreateInfo *pCreateInfo,
                                   const VkAllocationCallbacks *pAllocator,
                                   VkFramebuffer *pFramebuffer);

void r300vk_DestroyFramebuffer(VkDevice device,
                                VkFramebuffer framebuffer,
                                const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_FRAMEBUFFER_H */
