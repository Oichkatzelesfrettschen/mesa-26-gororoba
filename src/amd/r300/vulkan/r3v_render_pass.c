/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_render_pass.h"
#include "r3v_device.h"

#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_image.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_util.h"

#include <stdint.h>
#include <string.h>

/* Tear down a render pass that failed validation partway through creation.
 * Every reject path below allocated rp and ran vk_object_base_init, so the
 * cleanup is identical; factoring it keeps each reject site to the error it
 * reports. */
static void
r3v_render_pass_destroy_partial(struct r3v_device *device,
                                   const VkAllocationCallbacks *pAllocator,
                                   struct r3v_render_pass *rp)
{
   vk_object_base_finish(&rp->base);
   vk_free2(&device->vk.alloc, pAllocator, rp->subpasses);
   vk_free2(&device->vk.alloc, pAllocator, rp);
}

static bool
r3v_render_pass_subpass_storage_size(uint32_t subpass_count,
                                        size_t *storage_size)
{
   if ((size_t)subpass_count > SIZE_MAX / sizeof(struct r3v_subpass))
      return false;

   *storage_size = (size_t)subpass_count * sizeof(struct r3v_subpass);
   return true;
}

static VkResult
r3v_render_pass_alloc_subpasses(struct r3v_device *device,
                                   const VkAllocationCallbacks *pAllocator,
                                   struct r3v_render_pass *rp,
                                   uint32_t subpass_count)
{
   size_t subpass_storage_size = 0;

   if (subpass_count == 0)
      return VK_SUCCESS;

   if (!r3v_render_pass_subpass_storage_size(subpass_count,
                                                &subpass_storage_size)) {
      r3v_render_pass_destroy_partial(device, pAllocator, rp);
      return vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY,
                       "r3v: subpassCount %u overflows subpass storage",
                       subpass_count);
   }

   rp->subpasses = vk_zalloc2(&device->vk.alloc, pAllocator,
                              subpass_storage_size, 8,
                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!rp->subpasses) {
      r3v_render_pass_destroy_partial(device, pAllocator, rp);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   rp->subpass_count = subpass_count;
   return VK_SUCCESS;
}

/* An attachment that is both a writable output and an input in the same subpass
 * is a feedback loop r300 cannot read coherently: input attachments are bound as
 * fragment textures.  A read-only depth/stencil attachment can be sampled by
 * subpassLoad because the subpass does not write the sampled aspect.  Across
 * subpasses it is fine: subpass N reads subpass M<N's output after the pass
 * flushes between them.  Returns the aliasing attachment index, or
 * VK_ATTACHMENT_UNUSED when none overlaps. */
/* Vulkan 1.0 input attachment references lack aspectMask, so derive the
 * sampled aspects from the render-pass attachment format. */
static VkImageAspectFlags
r3v_render_pass_attachment_aspects(const struct r3v_render_pass *rp,
                                      uint32_t attachment)
{
   if (attachment == VK_ATTACHMENT_UNUSED || attachment >= rp->attachment_count)
      return 0;

   return vk_format_aspects(rp->attachments[attachment].format);
}

/* VK_KHR_maintenance2 lets vkCreateRenderPass override the sampled aspect for
 * a depth/stencil input attachment.  Preserve that v1 metadata before falling
 * back to the format-derived Vulkan 1.0 aspect set. */
static VkImageAspectFlags
r3v_render_pass_input_attachment_aspects(
   const struct r3v_render_pass *rp,
   const VkRenderPassInputAttachmentAspectCreateInfo *aspect_info,
   uint32_t subpass, uint32_t input_attachment_index, uint32_t attachment)
{
   if (aspect_info) {
      for (uint32_t i = 0; i < aspect_info->aspectReferenceCount; i++) {
         const VkInputAttachmentAspectReference *ref =
            &aspect_info->pAspectReferences[i];

         if (ref->subpass == subpass &&
             ref->inputAttachmentIndex == input_attachment_index)
            return ref->aspectMask;
      }
   }

   return r3v_render_pass_attachment_aspects(rp, attachment);
}

/* Mixed depth/stencil layouts are read-only only for the named aspect.  Reject
 * unknown aspect masks so malformed metadata cannot bypass feedback checks. */
static bool
r3v_depth_stencil_layout_is_read_only_for_aspects(
   const struct r3v_subpass *sp, VkImageAspectFlags aspects)
{
   const VkImageAspectFlags ds_aspects =
      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

   if ((aspects & ds_aspects) == 0 || (aspects & ~ds_aspects) != 0)
      return false;

   if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) &&
       !vk_image_layout_is_read_only(sp->depth_stencil_attachment_layout,
                                     VK_IMAGE_ASPECT_DEPTH_BIT))
      return false;

   if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
       !vk_image_layout_is_read_only(sp->depth_stencil_stencil_layout,
                                     VK_IMAGE_ASPECT_STENCIL_BIT))
      return false;

   return true;
}

static uint32_t
r3v_subpass_self_dependency(const struct r3v_subpass *sp)
{
   for (uint32_t i = 0; i < sp->input_attachment_count; i++) {
      if (sp->input_attachment_refs[i] == VK_ATTACHMENT_UNUSED)
         continue;
      for (uint32_t j = 0; j < sp->color_attachment_count; j++) {
         if (sp->color_attachment_refs[j] == sp->input_attachment_refs[i])
            return sp->input_attachment_refs[i];
      }
      if (sp->depth_stencil_attachment_ref == sp->input_attachment_refs[i] &&
          !r3v_depth_stencil_layout_is_read_only_for_aspects(
             sp, sp->input_attachment_aspects[i]))
         return sp->input_attachment_refs[i];
   }
   return VK_ATTACHMENT_UNUSED;
}

/* After every subpass's references are stored in rp->subpasses, mark each
 * subpass whose input attachment aliases one of its own writable outputs (the
 * replay routes that read through a snapshot copy), compute first_use_subpass
 * (the subpass where each attachment's loadOp applies), and mirror subpass 0
 * into the legacy members the begin recorder reads.  Shared by the 1.0 and
 * 2.0 create paths. */
static VkResult
r3v_render_pass_finalize(struct r3v_device *device,
                            const VkAllocationCallbacks *pAllocator,
                            struct r3v_render_pass *rp)
{
   for (uint32_t a = 0; a < PIPE_MAX_COLOR_BUFS + 1; a++)
      rp->first_use_subpass[a] = R3V_ATTACHMENT_NO_FIRST_USE;

   for (uint32_t s = 0; s < rp->subpass_count; s++) {
      struct r3v_subpass *sp = &rp->subpasses[s];

      sp->self_dep_attachment = r3v_subpass_self_dependency(sp);

      for (uint32_t i = 0; i < sp->color_attachment_count; i++) {
         const uint32_t a = sp->color_attachment_refs[i];
         if (a != VK_ATTACHMENT_UNUSED && a < PIPE_MAX_COLOR_BUFS + 1 &&
             rp->first_use_subpass[a] == R3V_ATTACHMENT_NO_FIRST_USE)
            rp->first_use_subpass[a] = (uint8_t)s;
      }
      const uint32_t ds = sp->depth_stencil_attachment_ref;
      if (ds != VK_ATTACHMENT_UNUSED && ds < PIPE_MAX_COLOR_BUFS + 1 &&
          rp->first_use_subpass[ds] == R3V_ATTACHMENT_NO_FIRST_USE)
         rp->first_use_subpass[ds] = (uint8_t)s;
   }

   if (rp->subpass_count > 0) {
      const struct r3v_subpass *s0 = &rp->subpasses[0];
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
r3v_subpass_bounds_ok(struct r3v_device *device,
                         const VkAllocationCallbacks *pAllocator,
                         struct r3v_render_pass *rp, uint32_t s,
                         uint32_t color_count, uint32_t input_count)
{
   if (color_count > PIPE_MAX_COLOR_BUFS) {
      r3v_render_pass_destroy_partial(device, pAllocator, rp);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r3v: subpass %u colorAttachmentCount %u exceeds r300 "
                       "fixed storage %u", s, color_count, PIPE_MAX_COLOR_BUFS);
   }
   if (input_count > PIPE_MAX_COLOR_BUFS) {
      r3v_render_pass_destroy_partial(device, pAllocator, rp);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r3v: subpass %u inputAttachmentCount %u exceeds r300 "
                       "fixed storage %u", s, input_count, PIPE_MAX_COLOR_BUFS);
   }
   return VK_SUCCESS;
}

VkResult
r3v_CreateRenderPass(VkDevice _device,
                         const VkRenderPassCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkRenderPass *pRenderPass)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   struct r3v_render_pass *rp;

   rp = vk_zalloc2(&device->vk.alloc, pAllocator,
                   sizeof(*rp), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!rp)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &rp->base, VK_OBJECT_TYPE_RENDER_PASS);
   rp->depth_stencil_attachment_ref = VK_ATTACHMENT_UNUSED;

   if (pCreateInfo->attachmentCount > PIPE_MAX_COLOR_BUFS + 1) {
      r3v_render_pass_destroy_partial(device, pAllocator, rp);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r3v: attachmentCount %u exceeds r300 fixed storage %u",
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
      r3v_render_pass_alloc_subpasses(device, pAllocator, rp,
                                         pCreateInfo->subpassCount);
   if (r != VK_SUCCESS)
      return r;

   const VkRenderPassInputAttachmentAspectCreateInfo *input_aspect_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO);

   for (uint32_t s = 0; s < pCreateInfo->subpassCount; s++) {
      const VkSubpassDescription *sp = &pCreateInfo->pSubpasses[s];
      r = r3v_subpass_bounds_ok(device, pAllocator, rp, s,
                                   sp->colorAttachmentCount,
                                   sp->inputAttachmentCount);
      if (r != VK_SUCCESS)
         return r;
      struct r3v_subpass *d = &rp->subpasses[s];
      d->color_attachment_count = sp->colorAttachmentCount;
      for (uint32_t i = 0; i < sp->colorAttachmentCount; i++)
         d->color_attachment_refs[i] = sp->pColorAttachments[i].attachment;
      d->input_attachment_count = sp->inputAttachmentCount;
      for (uint32_t i = 0; i < sp->inputAttachmentCount; i++) {
         d->input_attachment_refs[i] = sp->pInputAttachments[i].attachment;
         d->input_attachment_aspects[i] =
            r3v_render_pass_input_attachment_aspects(
               rp, input_aspect_info, s, i,
               sp->pInputAttachments[i].attachment);
      }
      d->depth_stencil_attachment_ref =
         sp->pDepthStencilAttachment ? sp->pDepthStencilAttachment->attachment
                                     : VK_ATTACHMENT_UNUSED;
      if (sp->pDepthStencilAttachment) {
         d->depth_stencil_attachment_layout =
            vk_image_layout_depth_only(sp->pDepthStencilAttachment->layout);
         d->depth_stencil_stencil_layout =
            vk_image_layout_stencil_only(sp->pDepthStencilAttachment->layout);
      }
   }

   r = r3v_render_pass_finalize(device, pAllocator, rp);
   if (r != VK_SUCCESS)
      return r;

   *pRenderPass = r3v_render_pass_to_handle(rp);
   return VK_SUCCESS;
}

/* VK_KHR_create_renderpass2.  The 2.0 create info chains sType onto the
 * attachment, subpass, and attachment-reference structs but exposes the same
 * fields r300 consumes (format, load/store op, final layout; subpass 0 color
 * and input references), so this mirrors r3v_CreateRenderPass onto the 2.0
 * structures and produces the identical r3v_render_pass.  A bespoke 2.0
 * entry point on r3v's own object keeps the common-runtime emulation, which
 * would read the handle as a vk_render_pass, out of the path. */
VkResult
r3v_CreateRenderPass2(VkDevice _device,
                          const VkRenderPassCreateInfo2 *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkRenderPass *pRenderPass)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   struct r3v_render_pass *rp;

   rp = vk_zalloc2(&device->vk.alloc, pAllocator,
                   sizeof(*rp), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!rp)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &rp->base, VK_OBJECT_TYPE_RENDER_PASS);
   rp->depth_stencil_attachment_ref = VK_ATTACHMENT_UNUSED;

   if (pCreateInfo->attachmentCount > PIPE_MAX_COLOR_BUFS + 1) {
      r3v_render_pass_destroy_partial(device, pAllocator, rp);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r3v: attachmentCount %u exceeds r300 fixed storage %u",
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
      r3v_render_pass_alloc_subpasses(device, pAllocator, rp,
                                         pCreateInfo->subpassCount);
   if (r != VK_SUCCESS)
      return r;
   for (uint32_t s = 0; s < pCreateInfo->subpassCount; s++) {
      const VkSubpassDescription2 *sp = &pCreateInfo->pSubpasses[s];
      r = r3v_subpass_bounds_ok(device, pAllocator, rp, s,
                                   sp->colorAttachmentCount,
                                   sp->inputAttachmentCount);
      if (r != VK_SUCCESS)
         return r;
      struct r3v_subpass *d = &rp->subpasses[s];
      d->color_attachment_count = sp->colorAttachmentCount;
      for (uint32_t i = 0; i < sp->colorAttachmentCount; i++)
         d->color_attachment_refs[i] = sp->pColorAttachments[i].attachment;
      d->input_attachment_count = sp->inputAttachmentCount;
      for (uint32_t i = 0; i < sp->inputAttachmentCount; i++) {
         d->input_attachment_refs[i] = sp->pInputAttachments[i].attachment;
         d->input_attachment_aspects[i] = sp->pInputAttachments[i].aspectMask;
         if (d->input_attachment_aspects[i] == 0)
            d->input_attachment_aspects[i] =
               r3v_render_pass_attachment_aspects(
                  rp, sp->pInputAttachments[i].attachment);
      }
      d->depth_stencil_attachment_ref =
         sp->pDepthStencilAttachment ? sp->pDepthStencilAttachment->attachment
                                     : VK_ATTACHMENT_UNUSED;
      if (sp->pDepthStencilAttachment) {
         d->depth_stencil_attachment_layout =
            vk_image_layout_depth_only(sp->pDepthStencilAttachment->layout);
         d->depth_stencil_stencil_layout =
            vk_image_layout_stencil_only(
               vk_att_ref_stencil_layout(sp->pDepthStencilAttachment,
                                         pCreateInfo->pAttachments));
      }
   }

   r = r3v_render_pass_finalize(device, pAllocator, rp);
   if (r != VK_SUCCESS)
      return r;

   *pRenderPass = r3v_render_pass_to_handle(rp);
   return VK_SUCCESS;
}

void
r3v_DestroyRenderPass(VkDevice _device,
                          VkRenderPass _rp,
                          const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_render_pass, rp, _rp);
   if (!rp)
      return;

   vk_object_base_finish(&rp->base);
   vk_free2(&device->vk.alloc, pAllocator, rp->subpasses);
   vk_free2(&device->vk.alloc, pAllocator, rp);
}
