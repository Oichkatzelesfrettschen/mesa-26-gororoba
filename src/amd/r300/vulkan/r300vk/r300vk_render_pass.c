/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_render_pass.h"
#include "r300vk_device.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"

/* Tear down a render pass that failed validation partway through creation.
 * Every reject path below allocated rp and ran vk_object_base_init, so the
 * cleanup is identical; factoring it keeps each reject site to the error it
 * reports. */
static void
r300vk_render_pass_destroy_partial(struct r300vk_device *device,
                                   const VkAllocationCallbacks *pAllocator,
                                   struct r300vk_render_pass *rp)
{
   vk_object_base_finish(&rp->base);
   vk_free2(&device->vk.alloc, pAllocator, rp);
}

/* Reject a subpass 0 input attachment that also serves as a colour attachment
 * in the same subpass.  r300 renders a single subpass in one pass and binds the
 * input attachments as fragment textures; reading an attachment the same draw
 * is writing is a feedback loop the hardware cannot resolve coherently, so it is
 * rejected rather than rendered with stale texels.  Returns the aliasing
 * attachment index, or VK_ATTACHMENT_UNUSED when no overlap exists. */
static uint32_t
r300vk_render_pass_self_dependency(const struct r300vk_render_pass *rp)
{
   for (uint32_t i = 0; i < rp->input_attachment_count; i++) {
      if (rp->input_attachment_refs[i] == VK_ATTACHMENT_UNUSED)
         continue;
      for (uint32_t j = 0; j < rp->color_attachment_count; j++) {
         if (rp->color_attachment_refs[j] == rp->input_attachment_refs[i])
            return rp->input_attachment_refs[i];
      }
   }
   return VK_ATTACHMENT_UNUSED;
}

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
      r300vk_render_pass_destroy_partial(device, pAllocator, rp);
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

   /* r300vk replays only subpass 0.  A later subpass that reads input
    * attachments would need those reads honoured against subpass-0 output, which
    * this single-subpass path cannot do, so reject rather than silently drop the
    * usage.  Later subpasses without input attachments stay out of the way of
    * the subpass-0 replay. */
   for (uint32_t s = 1; s < pCreateInfo->subpassCount; s++) {
      if (pCreateInfo->pSubpasses[s].inputAttachmentCount > 0) {
         r300vk_render_pass_destroy_partial(device, pAllocator, rp);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "r300vk: subpass %u reads input attachments; r300vk "
                          "replays only subpass 0", s);
      }
   }

   /* Parse subpass 0 color attachment references. */
   if (pCreateInfo->subpassCount > 0) {
      const VkSubpassDescription *sp = &pCreateInfo->pSubpasses[0];
      if (sp->colorAttachmentCount > PIPE_MAX_COLOR_BUFS) {
         r300vk_render_pass_destroy_partial(device, pAllocator, rp);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "r300vk: subpass colorAttachmentCount %u exceeds r300 fixed storage %u",
                          sp->colorAttachmentCount, PIPE_MAX_COLOR_BUFS);
      }
      rp->color_attachment_count = sp->colorAttachmentCount;
      for (uint32_t i = 0; i < sp->colorAttachmentCount; i++)
         rp->color_attachment_refs[i] = sp->pColorAttachments[i].attachment;

      /* Parse subpass 0 input attachments.  subpassLoad reads these images at
       * the fragment's own coordinate; the replay binds them as fragment
       * textures.  Reject an inputAttachmentCount past the fixed storage rather
       * than clamping it -- a silent clamp would drop the trailing references
       * and leave those subpassLoads reading whatever was bound, matching the
       * colorAttachment overflow reject above. */
      if (sp->inputAttachmentCount > PIPE_MAX_COLOR_BUFS) {
         r300vk_render_pass_destroy_partial(device, pAllocator, rp);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "r300vk: subpass inputAttachmentCount %u exceeds r300 fixed storage %u",
                          sp->inputAttachmentCount, PIPE_MAX_COLOR_BUFS);
      }
      rp->input_attachment_count = sp->inputAttachmentCount;
      for (uint32_t i = 0; i < rp->input_attachment_count; i++)
         rp->input_attachment_refs[i] = sp->pInputAttachments[i].attachment;
      rp->depth_stencil_attachment_ref =
         sp->pDepthStencilAttachment ? sp->pDepthStencilAttachment->attachment
                                     : VK_ATTACHMENT_UNUSED;

      uint32_t self_dep = r300vk_render_pass_self_dependency(rp);
      if (self_dep != VK_ATTACHMENT_UNUSED) {
         r300vk_render_pass_destroy_partial(device, pAllocator, rp);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "r300vk: attachment %u is both a colour and an input "
                          "attachment in subpass 0; r300 cannot read an "
                          "attachment it is concurrently writing", self_dep);
      }
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
      r300vk_render_pass_destroy_partial(device, pAllocator, rp);
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

   for (uint32_t s = 1; s < pCreateInfo->subpassCount; s++) {
      if (pCreateInfo->pSubpasses[s].inputAttachmentCount > 0) {
         r300vk_render_pass_destroy_partial(device, pAllocator, rp);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "r300vk: subpass %u reads input attachments; r300vk "
                          "replays only subpass 0", s);
      }
   }

   /* Parse subpass 0 color and input attachment references. */
   if (pCreateInfo->subpassCount > 0) {
      const VkSubpassDescription2 *sp = &pCreateInfo->pSubpasses[0];
      if (sp->colorAttachmentCount > PIPE_MAX_COLOR_BUFS) {
         r300vk_render_pass_destroy_partial(device, pAllocator, rp);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "r300vk: subpass colorAttachmentCount %u exceeds r300 fixed storage %u",
                          sp->colorAttachmentCount, PIPE_MAX_COLOR_BUFS);
      }
      rp->color_attachment_count = sp->colorAttachmentCount;
      for (uint32_t i = 0; i < sp->colorAttachmentCount; i++)
         rp->color_attachment_refs[i] = sp->pColorAttachments[i].attachment;

      if (sp->inputAttachmentCount > PIPE_MAX_COLOR_BUFS) {
         r300vk_render_pass_destroy_partial(device, pAllocator, rp);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "r300vk: subpass inputAttachmentCount %u exceeds r300 fixed storage %u",
                          sp->inputAttachmentCount, PIPE_MAX_COLOR_BUFS);
      }
      rp->input_attachment_count = sp->inputAttachmentCount;
      for (uint32_t i = 0; i < rp->input_attachment_count; i++)
         rp->input_attachment_refs[i] = sp->pInputAttachments[i].attachment;
      rp->depth_stencil_attachment_ref =
         sp->pDepthStencilAttachment ? sp->pDepthStencilAttachment->attachment
                                     : VK_ATTACHMENT_UNUSED;

      uint32_t self_dep = r300vk_render_pass_self_dependency(rp);
      if (self_dep != VK_ATTACHMENT_UNUSED) {
         r300vk_render_pass_destroy_partial(device, pAllocator, rp);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "r300vk: attachment %u is both a colour and an input "
                          "attachment in subpass 0; r300 cannot read an "
                          "attachment it is concurrently writing", self_dep);
      }
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
