/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_cpu_sync.h"

#include "vk_log.h"
#include "util/os_time.h"

#include <stdint.h>

static VkResult
r300vk_cpu_sync_init(UNUSED struct vk_device *device,
                     struct vk_sync *vk_sync,
                     uint64_t initial_value)
{
   struct r300vk_cpu_sync *sync = r300vk_cpu_sync_from_vk(vk_sync);

   if (mtx_init(&sync->lock, mtx_plain) != thrd_success)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   if (u_cnd_monotonic_init(&sync->changed) != thrd_success) {
      mtx_destroy(&sync->lock);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   sync->signaled = (initial_value != 0);
   return VK_SUCCESS;
}

static void
r300vk_cpu_sync_finish(UNUSED struct vk_device *device,
                       struct vk_sync *vk_sync)
{
   struct r300vk_cpu_sync *sync = r300vk_cpu_sync_from_vk(vk_sync);

   u_cnd_monotonic_destroy(&sync->changed);
   mtx_destroy(&sync->lock);
}

static VkResult
r300vk_cpu_sync_signal(UNUSED struct vk_device *device,
                       struct vk_sync *vk_sync,
                       UNUSED uint64_t value)
{
   struct r300vk_cpu_sync *sync = r300vk_cpu_sync_from_vk(vk_sync);

   mtx_lock(&sync->lock);
   sync->signaled = true;
   u_cnd_monotonic_broadcast(&sync->changed);
   mtx_unlock(&sync->lock);

   return VK_SUCCESS;
}

static VkResult
r300vk_cpu_sync_reset(UNUSED struct vk_device *device,
                      struct vk_sync *vk_sync)
{
   struct r300vk_cpu_sync *sync = r300vk_cpu_sync_from_vk(vk_sync);

   mtx_lock(&sync->lock);
   sync->signaled = false;
   u_cnd_monotonic_broadcast(&sync->changed);
   mtx_unlock(&sync->lock);

   return VK_SUCCESS;
}

static VkResult
r300vk_cpu_sync_wait(struct vk_device *device,
                     struct vk_sync *vk_sync,
                     UNUSED uint64_t wait_value,
                     enum vk_sync_wait_flags wait_flags,
                     uint64_t abs_timeout_ns)
{
   struct r300vk_cpu_sync *sync = r300vk_cpu_sync_from_vk(vk_sync);

   mtx_lock(&sync->lock);

   uint64_t now_ns = os_time_get_nano();
   while (!sync->signaled) {
      if (wait_flags & VK_SYNC_WAIT_PENDING) {
         /* PENDING is satisfied by an unsignaled-but-submitted fence.
          * r300vk has no deferred submission, so pending == signaled. */
         break;
      }

      if (now_ns >= abs_timeout_ns) {
         mtx_unlock(&sync->lock);
         return VK_TIMEOUT;
      }

      if (abs_timeout_ns >= (uint64_t)INT64_MAX) {
         u_cnd_monotonic_wait(&sync->changed, &sync->lock);
      } else {
         /* Convert the Vulkan monotonic absolute timeout to a timespec for
          * u_cnd_monotonic_timedwait, which uses CLOCK_MONOTONIC internally
          * to match os_time_get_nano(). */
         struct timespec abs_ts = {
            .tv_sec  = (time_t)(abs_timeout_ns / 1000000000ULL),
            .tv_nsec = (long)(abs_timeout_ns % 1000000000ULL),
         };
         int rc = u_cnd_monotonic_timedwait(&sync->changed, &sync->lock,
                                            &abs_ts);
         if (rc == thrd_timedout) {
            mtx_unlock(&sync->lock);
            return VK_TIMEOUT;
         }
      }

      now_ns = os_time_get_nano();
   }

   mtx_unlock(&sync->lock);
   return VK_SUCCESS;
}

const struct vk_sync_type r300vk_cpu_sync_type = {
   .size     = sizeof(struct r300vk_cpu_sync),
   /* GPU_WAIT lets this CPU-backed sync stand in for a binary semaphore:
    * get_semaphore_sync_type (vk_semaphore.c) requires GPU_WAIT | BINARY, and
    * vk_common_CreateSemaphore asserts a match.  On the single-queue serialized
    * CPU-replay model a queue wait on a semaphore is satisfied by submit
    * ordering, so advertising GPU_WAIT is honest -- the wait is already complete
    * by the time the next submit replays (the software-rasterizer pattern). */
   .features = VK_SYNC_FEATURE_BINARY     |
               VK_SYNC_FEATURE_GPU_WAIT   |
               VK_SYNC_FEATURE_CPU_WAIT   |
               VK_SYNC_FEATURE_CPU_RESET  |
               VK_SYNC_FEATURE_CPU_SIGNAL |
               VK_SYNC_FEATURE_WAIT_ANY   |
               VK_SYNC_FEATURE_WAIT_PENDING,
   .init     = r300vk_cpu_sync_init,
   .finish   = r300vk_cpu_sync_finish,
   .signal   = r300vk_cpu_sync_signal,
   .reset    = r300vk_cpu_sync_reset,
   .wait     = r300vk_cpu_sync_wait,
};
