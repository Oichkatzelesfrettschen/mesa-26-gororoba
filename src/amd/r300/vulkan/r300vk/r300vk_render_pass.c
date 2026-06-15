/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_render_pass.h"
#include "r300vk_device.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"

#include <string.h>

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
   vk_free2(&device->vk.alloc, pAllocator, rp->subpasses);
   vk_free2(&device->vk.alloc, pAllocator, rp);
}

static VkResult
r300vk_render_pass_alloc_subpasses(struct r300vk_device *device,
                                   const VkAllocationCallbacks *pAllocator,
                                   struct r300vk_render_pass *rp,
                                   uint32_t subpass_count)
{
   if (subpass_count == 0)
      return VK_SUCCESS;

   rp->subpasses = vk_zalloc2(&device->vk.alloc, pAllocator,
                              subpass_count * sizeof(*rp->subpasses), 8,
                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!rp->subpasses) {
      r300vk_render_pass_destroy_partial(device, pAllocator, rp);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   rp->subpass_count = subpass_count;
   return VK_SUCCESS;
}

/* An attachment that is both an output and an input in the same subpass is a
 * feedback loop r300 cannot read coherently (it binds input attachments as
 * fragment textures), so it is rejected.  Across subpasses it is fine: subpass N
 * reads subpass M<N's output after the pass flushes between them.  Returns the
 * aliasing attachment index, or VK_ATTACHMENT_UNUSED when none overlaps. */
static uint32_t
r300vk_subpass_self_dependency(const struct r300vk_subpass *sp)
{
   for (uint32_t i = 0; i < sp->input_attachment_count; i++) {
      if (sp->input_attachment_refs[i] == VK_ATTACHMENT_UNUSED)
         continue;
      for (uint32_t j = 0; j < sp->color_attachment_count; j++) {
         if (sp->color_attachment_refs[j] == sp->input_attachment_refs[i])
            return sp->input_attachment_refs[i];
      }
      if (sp->depth_stencil_attachment_ref == sp->input_attachment_refs[i])
         return sp->input_attachment_refs[i];
   }
   return VK_ATTACHMENT_UNUSED;
}

/* After every subpass's references are stored in rp->subpasses, validate each
 * subpass's self-dependency, compute first_use_subpass (the subpass where each
 * attachment's loadOp applies), and mirror subpass 0 into the legacy members the
 * begin recorder reads.  Shared by the 1.0 and 2.0 create paths. */
static VkResult
r300vk_render_pass_finalize(struct r300vk_device *device,
                            const VkAllocationCallbacks *pAllocator,
                            struct r300vk_render_pass *rp)
{
   for (uint32_t a = 0; a < PIPE_MAX_COLOR_BUFS + 1; a++)
      rp->first_use_subpass[a] = R300VK_ATTACHMENT_NO_FIRST_USE;

   for (uint32_t s = 0; s < rp->subpass_count; s++) {
      const struct r300vk_subpass *sp = &rp->subpasses[s];

      const uint32_t dep = r300vk_subpass_self_dependency(sp);
      if (dep != VK_ATTACHMENT_UNUSED) {
         r300vk_render_pass_destroy_partial(device, pAllocator, rp);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "r300vk: attachment %u is both an output and an input "
                          "attachment in subpass %u; r300 cannot read an "
                          "attachment it is concurrently writing", dep, s);
      }

      for (uint32_t i = 0; i < sp->color_attachment_count; i++) {
         const uint32_t a = sp->color_attachment_refs[i];
         if (a != VK_ATTACHMENT_UNUSED && a < PIPE_MAX_COLOR_BUFS + 1 &&
             rp->first_use_subpass[a] == R300VK_ATTACHMENT_NO_FIRST_USE)
            rp->first_use_subpass[a] = (uint8_t)s;
      }
      const uint32_t ds = sp->depth_stencil_attachment_ref;
      if (ds != VK_ATTACHMENT_UNUSED && ds < PIPE_MAX_COLOR_BUFS + 1 &&
          rp->first_use_subpass[ds] == R300VK_ATTACHMENT_NO_FIRST_USE)
         rp->first_use_subpass[ds] = (uint8_t)s;
   }

   if (rp->subpass_count > 0) {
      const struct r300vk_subpass *s0 = &rp->subpasses[0];
      rp->color_attachment_count = s0->color_attachment_count;
      memcpy(rp->color_attachment_refs, s0->color_attachment_refs,
             sizeof(rp->color_attachment_refs));
      rp->input_attachment_count = s0->input_attachment_count;
      memcpy(rp->input_attachment_refs, s0->input_attachment_refs,
             sizeof(rp->input_attachment_refs));
      rp->depth_stencil_attachment_ref = s0->depth_stencil_attachment_ref;
   }
   return VK_SUCCESS;
}

/* Reject a subpass whose color or input count exceeds the fixed storage. */
static VkResult
r300vk_subpass_bounds_ok(struct r300vk_device *device,
                         const VkAllocationCallbacks *pAllocator,
                         struct r300vk_render_pass *rp, uint32_t s,
                         uint32_t color_count, uint32_t input_count)
{
   if (color_count > PIPE_MAX_COLOR_BUFS) {
      r300vk_render_pass_destroy_partial(device, pAllocator, rp);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r300vk: subpass %u colorAttachmentCount %u exceeds r300 "
                       "fixed storage %u", s, color_count, PIPE_MAX_COLOR_BUFS);
   }
   if (input_count > PIPE_MAX_COLOR_BUFS) {
      r300vk_render_pass_destroy_partial(device, pAllocator, rp);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r300vk: subpass %u inputAttachmentCount %u exceeds r300 "
                       "fixed storage %u", s, input_count, PIPE_MAX_COLOR_BUFS);
   }
   return VK_SUCCESS;
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

   /* Store every subpass's color, input, and depth/stencil references.  A
    * later subpass that reads an input attachment is now supported: the replay
    * flushes between subpasses and binds the prior output as a fragment texture
    * (subpassLoad reads it at the same coordinate). */
   VkResult r =
      r300vk_render_pass_alloc_subpasses(device, pAllocator, rp,
                                         pCreateInfo->subpassCount);
   if (r != VK_SUCCESS)
      return r;
   for (uint32_t s = 0; s < pCreateInfo->subpassCount; s++) {
      const VkSubpassDescription *sp = &pCreateInfo->pSubpasses[s];
      r = r300vk_subpass_bounds_ok(device, pAllocator, rp, s,
                                   sp->colorAttachmentCount,
                                   sp->inputAttachmentCount);
      if (r != VK_SUCCESS)
         return r;
      struct r300vk_subpass *d = &rp->subpasses[s];
      d->color_attachment_count = sp->colorAttachmentCount;
      for (uint32_t i = 0; i < sp->colorAttachmentCount; i++)
         d->color_attachment_refs[i] = sp->pColorAttachments[i].attachment;
      d->input_attachment_count = sp->inputAttachmentCount;
      for (uint32_t i = 0; i < sp->inputAttachmentCount; i++)
         d->input_attachment_refs[i] = sp->pInputAttachments[i].attachment;
      d->depth_stencil_attachment_ref =
         sp->pDepthStencilAttachment ? sp->pDepthStencilAttachment->attachment
                                     : VK_ATTACHMENT_UNUSED;
   }

   r = r300vk_render_pass_finalize(device, pAllocator, rp);
   if (r != VK_SUCCESS)
      return r;

   *pRenderPass = r300vk_render_pass_to_handle(rp);
   return VK_SUCCESS;
}

/* VK_KHR_create_renderpass2.  The 2.0 create info chains sType onto the
 * attachment, subpass, and attachment-reference structs but exposes the same
 * fields r300 consumes (format, load/store op, final layout; subpass 0 color
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

   VkResult r =
      r300vk_render_pass_alloc_subpasses(device, pAllocator, rp,
                                         pCreateInfo->subpassCount);
   if (r != VK_SUCCESS)
      return r;
   for (uint32_t s = 0; s < pCreateInfo->subpassCount; s++) {
      const VkSubpassDescription2 *sp = &pCreateInfo->pSubpasses[s];
      r = r300vk_subpass_bounds_ok(device, pAllocator, rp, s,
                                   sp->colorAttachmentCount,
                                   sp->inputAttachmentCount);
      if (r != VK_SUCCESS)
         return r;
      struct r300vk_subpass *d = &rp->subpasses[s];
      d->color_attachment_count = sp->colorAttachmentCount;
      for (uint32_t i = 0; i < sp->colorAttachmentCount; i++)
         d->color_attachment_refs[i] = sp->pColorAttachments[i].attachment;
      d->input_attachment_count = sp->inputAttachmentCount;
      for (uint32_t i = 0; i < sp->inputAttachmentCount; i++)
         d->input_attachment_refs[i] = sp->pInputAttachments[i].attachment;
      d->depth_stencil_attachment_ref =
         sp->pDepthStencilAttachment ? sp->pDepthStencilAttachment->attachment
                                     : VK_ATTACHMENT_UNUSED;
   }

   r = r300vk_render_pass_finalize(device, pAllocator, rp);
   if (r != VK_SUCCESS)
      return r;

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
   vk_free2(&device->vk.alloc, pAllocator, rp->subpasses);
   vk_free2(&device->vk.alloc, pAllocator, rp);
}
