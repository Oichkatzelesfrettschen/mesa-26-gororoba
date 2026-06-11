/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_render_pass.h"
#include "r300vk_device.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"

VkResult
r300vk_CreateRenderPass(VkDevice _device,
                         const VkRenderPassCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkRenderPass *pRenderPass)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   struct r300vk_render_pass *rp;

   rp = vk_zalloc2(&device->vk.alloc, pAllocator,
                   sizeof(*rp), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!rp)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &rp->base, VK_OBJECT_TYPE_RENDER_PASS);
   rp->depth_stencil_attachment_ref = VK_ATTACHMENT_UNUSED;

   if (pCreateInfo->attachmentCount > PIPE_MAX_COLOR_BUFS + 1) {
      vk_object_base_finish(&rp->base);
      vk_free2(&device->vk.alloc, pAllocator, rp);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r300vk: attachmentCount %u exceeds r300 fixed storage %u",
                       pCreateInfo->attachmentCount, PIPE_MAX_COLOR_BUFS + 1);
   }

   rp->attachment_count = pCreateInfo->attachmentCount;
   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      rp->attachments[i].format       = pCreateInfo->pAttachments[i].format;
      rp->attachments[i].load_op      = pCreateInfo->pAttachments[i].loadOp;
      rp->attachments[i].store_op     = pCreateInfo->pAttachments[i].storeOp;
      rp->attachments[i].final_layout = pCreateInfo->pAttachments[i].finalLayout;
   }

   /* Parse subpass 0 color attachment references. */
   if (pCreateInfo->subpassCount > 0) {
      const VkSubpassDescription *sp = &pCreateInfo->pSubpasses[0];
      if (sp->colorAttachmentCount > PIPE_MAX_COLOR_BUFS) {
         vk_object_base_finish(&rp->base);
         vk_free2(&device->vk.alloc, pAllocator, rp);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "r300vk: subpass colorAttachmentCount %u exceeds r300 fixed storage %u",
                          sp->colorAttachmentCount, PIPE_MAX_COLOR_BUFS);
      }
      rp->color_attachment_count = sp->colorAttachmentCount;
      for (uint32_t i = 0; i < sp->colorAttachmentCount; i++)
         rp->color_attachment_refs[i] = sp->pColorAttachments[i].attachment;

      /* Parse subpass 0 input attachments.  subpassLoad reads these images at
       * the fragment's own coordinate; the replay binds them as fragment
       * textures.  Excess input attachments beyond the fixed storage are simply
       * not recorded -- the lowering binds only the slots it can resolve, and an
       * unresolved subpassLoad keeps its prior (rejected) behavior. */
      rp->input_attachment_count = MIN2(sp->inputAttachmentCount,
                                        PIPE_MAX_COLOR_BUFS);
      for (uint32_t i = 0; i < rp->input_attachment_count; i++)
         rp->input_attachment_refs[i] = sp->pInputAttachments[i].attachment;
      rp->depth_stencil_attachment_ref =
         sp->pDepthStencilAttachment ? sp->pDepthStencilAttachment->attachment
                                     : VK_ATTACHMENT_UNUSED;
   }

   *pRenderPass = r300vk_render_pass_to_handle(rp);
   return VK_SUCCESS;
}

/* VK_KHR_create_renderpass2.  The 2.0 create info chains sType onto the
 * attachment, subpass, and attachment-reference structs but exposes the same
 * fields r300 consumes (format, load/store op, final layout; subpass 0 colour
 * and input references), so this mirrors r300vk_CreateRenderPass onto the 2.0
 * structures and produces the identical r300vk_render_pass.  A bespoke 2.0
 * entry point on r300vk's own object keeps the common-runtime emulation, which
 * would read the handle as a vk_render_pass, out of the path. */
VkResult
r300vk_CreateRenderPass2(VkDevice _device,
                          const VkRenderPassCreateInfo2 *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkRenderPass *pRenderPass)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   struct r300vk_render_pass *rp;

   rp = vk_zalloc2(&device->vk.alloc, pAllocator,
                   sizeof(*rp), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!rp)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &rp->base, VK_OBJECT_TYPE_RENDER_PASS);
   rp->depth_stencil_attachment_ref = VK_ATTACHMENT_UNUSED;

   if (pCreateInfo->attachmentCount > PIPE_MAX_COLOR_BUFS + 1) {
      vk_object_base_finish(&rp->base);
      vk_free2(&device->vk.alloc, pAllocator, rp);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r300vk: attachmentCount %u exceeds r300 fixed storage %u",
                       pCreateInfo->attachmentCount, PIPE_MAX_COLOR_BUFS + 1);
   }

   rp->attachment_count = pCreateInfo->attachmentCount;
   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      rp->attachments[i].format       = pCreateInfo->pAttachments[i].format;
      rp->attachments[i].load_op      = pCreateInfo->pAttachments[i].loadOp;
      rp->attachments[i].store_op     = pCreateInfo->pAttachments[i].storeOp;
      rp->attachments[i].final_layout = pCreateInfo->pAttachments[i].finalLayout;
   }

   /* Parse subpass 0 color and input attachment references. */
   if (pCreateInfo->subpassCount > 0) {
      const VkSubpassDescription2 *sp = &pCreateInfo->pSubpasses[0];
      if (sp->colorAttachmentCount > PIPE_MAX_COLOR_BUFS) {
         vk_object_base_finish(&rp->base);
         vk_free2(&device->vk.alloc, pAllocator, rp);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "r300vk: subpass colorAttachmentCount %u exceeds r300 fixed storage %u",
                          sp->colorAttachmentCount, PIPE_MAX_COLOR_BUFS);
      }
      rp->color_attachment_count = sp->colorAttachmentCount;
      for (uint32_t i = 0; i < sp->colorAttachmentCount; i++)
         rp->color_attachment_refs[i] = sp->pColorAttachments[i].attachment;

      rp->input_attachment_count = MIN2(sp->inputAttachmentCount,
                                        PIPE_MAX_COLOR_BUFS);
      for (uint32_t i = 0; i < rp->input_attachment_count; i++)
         rp->input_attachment_refs[i] = sp->pInputAttachments[i].attachment;
      rp->depth_stencil_attachment_ref =
         sp->pDepthStencilAttachment ? sp->pDepthStencilAttachment->attachment
                                     : VK_ATTACHMENT_UNUSED;
   }

   *pRenderPass = r300vk_render_pass_to_handle(rp);
   return VK_SUCCESS;
}

void
r300vk_DestroyRenderPass(VkDevice _device,
                          VkRenderPass _rp,
                          const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_render_pass, rp, _rp);
   if (!rp)
      return;

   vk_object_base_finish(&rp->base);
   vk_free2(&device->vk.alloc, pAllocator, rp);
}
