/*
 * Copyright (c) 2024 Vitaliy "Triang3l" Kuzmin
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * External-sync helpers for terakan: signal DRM syncobj handles that
 * back VK_KHR_external_semaphore_fd / VK_KHR_external_fence_fd payloads.
 * The runtime helper vk_drm_syncobj owns FD and handle conversion; this
 * file issues SYNCOBJ_SIGNAL / SYNCOBJ_TIMELINE_SIGNAL after the driver
 * decides a submission has completed.
 */

#include "terakan_external_sync.h"

#include "vk_drm_syncobj.h"
#include "vk_log.h"
#include "vk_sync.h"

#include "drm-uapi/drm.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>

/* Retry transient DRM ioctl failures. EINTR and EAGAIN are not device loss. */
static int
terakan_drm_ioctl(int const drm_fd, unsigned long const request, void *const arg)
{
   int ret;
   do {
      ret = ioctl(drm_fd, request, arg);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
   return ret;
}

bool
terakan_sync_is_external_drm_syncobj(struct vk_sync const *const sync,
                                     uint32_t *const kernel_handle_out)
{
   if (sync == NULL || sync->type == NULL)
      return false;

   /* Type-check first so the cast does not strip const through the
    * non-const vk_sync_as_drm_syncobj helper. base is the first field. */
   if (!vk_sync_type_is_drm_syncobj(sync->type))
      return false;

   struct vk_drm_syncobj const *const drm =
      (struct vk_drm_syncobj const *)(void const *)sync;

   if (kernel_handle_out != NULL)
      *kernel_handle_out = drm->syncobj;
   return true;
}

VkResult
terakan_external_syncobj_signal(int const drm_fd,
                                struct terakan_external_signal const *const sig)
{
   if (sig->is_timeline) {
      struct drm_syncobj_timeline_array args = {
         .handles = (uintptr_t)&sig->kernel_handle,
         .points = (uintptr_t)&sig->point,
         .count_handles = 1,
         .flags = 0,
      };
      if (terakan_drm_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args) != 0)
         return VK_ERROR_DEVICE_LOST;
   } else {
      struct drm_syncobj_array args = {
         .handles = (uintptr_t)&sig->kernel_handle,
         .count_handles = 1,
         .pad = 0,
      };
      if (terakan_drm_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &args) != 0)
         return VK_ERROR_DEVICE_LOST;
   }
   return VK_SUCCESS;
}

VkResult
terakan_external_syncobj_signal_many(int const drm_fd, uint32_t const count,
                                     struct terakan_external_signal const *const sigs)
{
   /* Partition into binary and timeline buckets so each ioctl runs at most
    * twice for typical submit signal counts. */
   enum { TERAKAN_EXTERNAL_SIGNAL_STACK_MAX = 32 };
   if (count > TERAKAN_EXTERNAL_SIGNAL_STACK_MAX) {
      for (uint32_t i = 0; i < count; ++i) {
         VkResult const r = terakan_external_syncobj_signal(drm_fd, &sigs[i]);
         if (r != VK_SUCCESS)
            return r;
      }
      return VK_SUCCESS;
   }

   uint32_t binary_handles[TERAKAN_EXTERNAL_SIGNAL_STACK_MAX];
   uint32_t timeline_handles[TERAKAN_EXTERNAL_SIGNAL_STACK_MAX];
   uint64_t timeline_points[TERAKAN_EXTERNAL_SIGNAL_STACK_MAX];
   uint32_t binary_count = 0;
   uint32_t timeline_count = 0;

   for (uint32_t i = 0; i < count; ++i) {
      if (sigs[i].is_timeline) {
         timeline_handles[timeline_count] = sigs[i].kernel_handle;
         timeline_points[timeline_count] = sigs[i].point;
         ++timeline_count;
      } else {
         binary_handles[binary_count++] = sigs[i].kernel_handle;
      }
   }

   if (binary_count != 0) {
      struct drm_syncobj_array args = {
         .handles = (uintptr_t)binary_handles,
         .count_handles = binary_count,
         .pad = 0,
      };
      if (terakan_drm_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &args) != 0)
         return VK_ERROR_DEVICE_LOST;
   }

   if (timeline_count != 0) {
      struct drm_syncobj_timeline_array args = {
         .handles = (uintptr_t)timeline_handles,
         .points = (uintptr_t)timeline_points,
         .count_handles = timeline_count,
         .flags = 0,
      };
      if (terakan_drm_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args) != 0)
         return VK_ERROR_DEVICE_LOST;
   }

   return VK_SUCCESS;
}
