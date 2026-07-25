/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_FRAMEBUFFER_H
#define R3V_FRAMEBUFFER_H

#include "r3v_private.h"

#include "vk_object.h"

#include "pipe/p_state.h"  /* PIPE_MAX_COLOR_BUFS */

#ifdef __cplusplus
extern "C" {
#endif

/* r3v_framebuffer stores the raw VkImageView handles for the attachments
 * alongside the dimensions.  The command recorder resolves the handles to
 * r3v_image_view pointers and then to pipe_resource pointers at
 * CmdBeginRenderPass time.  An imageless framebuffer (VK_KHR_imageless_
 * framebuffer) carries no views: attachments[] stays empty and the actual
 * views arrive at begin time in a VkRenderPassAttachmentBeginInfo. */
struct r3v_framebuffer {
   struct vk_object_base  base;
   uint32_t               width;
   uint32_t               height;
   uint32_t               attachment_count;
   bool                   imageless;
   VkImageView            attachments[PIPE_MAX_COLOR_BUFS + 1];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_framebuffer, base, VkFramebuffer,
                                VK_OBJECT_TYPE_FRAMEBUFFER)

VkResult r3v_CreateFramebuffer(VkDevice device,
                                   const VkFramebufferCreateInfo *pCreateInfo,
                                   const VkAllocationCallbacks *pAllocator,
                                   VkFramebuffer *pFramebuffer);

void r3v_DestroyFramebuffer(VkDevice device,
                                VkFramebuffer framebuffer,
                                const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

#endif /* R3V_FRAMEBUFFER_H */
