/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_framebuffer.h"
#include "r300vk_device.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"

#include "pipe/p_state.h"

#include <string.h>

VkResult
r300vk_CreateFramebuffer(VkDevice _device,
                          const VkFramebufferCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkFramebuffer *pFramebuffer)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   struct r300vk_framebuffer *fb;

   fb = vk_zalloc2(&device->vk.alloc, pAllocator,
                   sizeof(*fb), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!fb)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &fb->base, VK_OBJECT_TYPE_FRAMEBUFFER);

   if (pCreateInfo->attachmentCount > PIPE_MAX_COLOR_BUFS + 1) {
      vk_object_base_finish(&fb->base);
      vk_free2(&device->vk.alloc, pAllocator, fb);
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: framebuffer attachmentCount %u exceeds fixed storage %u",
                       pCreateInfo->attachmentCount, PIPE_MAX_COLOR_BUFS + 1);
   }

   fb->width            = pCreateInfo->width;
   fb->height           = pCreateInfo->height;
   fb->attachment_count = pCreateInfo->attachmentCount;
   if (pCreateInfo->attachmentCount > 0)
      memcpy(fb->attachments, pCreateInfo->pAttachments,
             pCreateInfo->attachmentCount * sizeof(VkImageView));

   *pFramebuffer = r300vk_framebuffer_to_handle(fb);
   return VK_SUCCESS;
}

void
r300vk_DestroyFramebuffer(VkDevice _device,
                           VkFramebuffer _fb,
                           const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_framebuffer, fb, _fb);
   if (!fb)
      return;

   vk_object_base_finish(&fb->base);
   vk_free2(&device->vk.alloc, pAllocator, fb);
}
