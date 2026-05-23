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

   if (cnd_init(&sync->changed) != thrd_success) {
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

   cnd_destroy(&sync->changed);
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
   cnd_broadcast(&sync->changed);
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
   cnd_broadcast(&sync->changed);
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
         cnd_wait(&sync->changed, &sync->lock);
      } else {
         uint64_t rel_ns = abs_timeout_ns - now_ns;
         struct timespec now_ts, abs_ts;
         timespec_get(&now_ts, TIME_UTC);
         abs_ts.tv_sec  = now_ts.tv_sec  + (time_t)(rel_ns / 1000000000ULL);
         abs_ts.tv_nsec = now_ts.tv_nsec + (long)(rel_ns % 1000000000ULL);
         if (abs_ts.tv_nsec >= 1000000000L) {
            abs_ts.tv_sec++;
            abs_ts.tv_nsec -= 1000000000L;
         }
         int rc = cnd_timedwait(&sync->changed, &sync->lock, &abs_ts);
         (void)rc;
      }

      now_ns = os_time_get_nano();
   }

   mtx_unlock(&sync->lock);
   return VK_SUCCESS;
}

const struct vk_sync_type r300vk_cpu_sync_type = {
   .size     = sizeof(struct r300vk_cpu_sync),
   .features = VK_SYNC_FEATURE_BINARY     |
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
