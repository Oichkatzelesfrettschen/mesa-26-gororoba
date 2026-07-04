/* SPDX-License-Identifier: MIT */

/*
 * External-sync bridge for terakan: ties Vulkan's
 * VK_KHR_external_semaphore_fd (#80, core 1.1) and
 * VK_KHR_external_fence_fd (#116, core 1.1) to the radeon kernel's
 * `DRM_IOCTL_SYNCOBJ_*` uAPI.  The runtime helper `vk_drm_syncobj`
 * already implements the FD <-> kernel-handle conversions; this
 * file provides the queue-submit-side glue that the runtime cannot
 * supply because radeon's CS submit ioctl carries no syncobj chunk.
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

bool
terakan_sync_is_external_drm_syncobj(struct vk_sync const * const sync,
                                     uint32_t * const kernel_handle_out)
{
   if (sync == NULL) {
      return false;
   }

   /* `vk_sync_as_drm_syncobj` returns non-NULL only when the sync's
    * `type->finish == vk_drm_syncobj_finish`.  Both the per-device
    * registered `sync_type_drm_syncobj` and any future timeline-
    * syncobj subtype share this finish callback, so the test covers
    * both binary and timeline external payloads. */
   struct vk_drm_syncobj const * const drm =
      vk_sync_as_drm_syncobj((struct vk_sync *)sync);
   if (drm == NULL) {
      return false;
   }

   if (kernel_handle_out != NULL) {
      *kernel_handle_out = drm->syncobj;
   }
   return true;
}

VkResult
terakan_external_syncobj_signal(int const drm_fd,
                                struct terakan_external_signal const * const sig)
{
   if (sig->is_timeline) {
      /* DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL accepts an array of
       * (handle, point) pairs.  Issue one at a time here; the
       * batched variant lives in `_signal_many`. */
      struct drm_syncobj_timeline_array args = {
         .handles = (uintptr_t)&sig->kernel_handle,
         .points = (uintptr_t)&sig->point,
         .count_handles = 1,
         .flags = 0,
      };
      if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args) != 0) {
         return VK_ERROR_DEVICE_LOST;
      }
   } else {
      struct drm_syncobj_array args = {
         .handles = (uintptr_t)&sig->kernel_handle,
         .count_handles = 1,
         .pad = 0,
      };
      if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &args) != 0) {
         return VK_ERROR_DEVICE_LOST;
      }
   }
   return VK_SUCCESS;
}

VkResult
terakan_external_syncobj_signal_many(int const drm_fd,
                                     uint32_t const count,
                                     struct terakan_external_signal const * const sigs)
{
   /* Partition into binary-signal and timeline-signal buckets so each
    * ioctl is invoked at most twice regardless of `count`.  Stack-
    * bounded arrays: external-signal counts per submit are limited
    * by Vulkan's `VkSubmitInfo::signalSemaphoreCount` which apps
    * typically keep small (<= 8). */
   enum { TERAKAN_EXTERNAL_SIGNAL_STACK_MAX = 32 };
   if (count > TERAKAN_EXTERNAL_SIGNAL_STACK_MAX) {
      /* Pathological: fall back to single-signal iteration so we do
       * not VLA-overflow the stack on adversarial input. */
      for (uint32_t i = 0; i < count; ++i) {
         VkResult const r = terakan_external_syncobj_signal(drm_fd, &sigs[i]);
         if (r != VK_SUCCESS) {
            return r;
         }
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
      if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &args) != 0) {
         return VK_ERROR_DEVICE_LOST;
      }
   }

   if (timeline_count != 0) {
      struct drm_syncobj_timeline_array args = {
         .handles = (uintptr_t)timeline_handles,
         .points = (uintptr_t)timeline_points,
         .count_handles = timeline_count,
         .flags = 0,
      };
      if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args) != 0) {
         return VK_ERROR_DEVICE_LOST;
      }
   }

   return VK_SUCCESS;
}
