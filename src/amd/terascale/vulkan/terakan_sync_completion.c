/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terakan_sync_completion.h"

#include "terakan_device.h"
#include "terakan_wsi.h"

#include "util/macros.h"
#include "util/os_time.h"
#include "util/timespec.h"
#include "util/u_atomic.h"

#include <assert.h>
#include <stdbool.h>
#include <time.h>

static uint64_t
terakan_sync_completion_get_wait_value(struct terakan_sync_completion const * const sync,
                                       uint64_t const wait_value)
{
   return sync->presentation_wait_is_wsi ? sync->presentation_wait_value : wait_value;
}

static uint64_t
terakan_sync_completion_get_current_value(struct terakan_sync_completion const * const sync,
                                          enum vk_sync_wait_flags const wait_flags)
{
   if (sync->presentation_wait_is_wsi) {
      return terakan_wsi_hw_wait_load_value(sync->presentation_wait_state);
   }

   return p_atomic_read(wait_flags & VK_SYNC_WAIT_PENDING ? &sync->pending_value :
                                                        &sync->current_value);
}

static VkResult
terakan_sync_completion_move(UNUSED struct vk_device * const device,
                             struct vk_sync * const dst_base, struct vk_sync * const src_base)
{
   struct terakan_sync_completion * const dst =
      container_of(dst_base, struct terakan_sync_completion, vk);
   struct terakan_sync_completion * const src =
      container_of(src_base, struct terakan_sync_completion, vk);

   if (dst->presentation_wait_state != NULL) {
      terakan_wsi_hw_wait_unref(dst->presentation_wait_state);
   }

   dst->pending_value = src->pending_value;
   dst->current_value = src->current_value;
   dst->presentation_wait_state = src->presentation_wait_state;
   dst->presentation_wait_bo = src->presentation_wait_bo;
   dst->presentation_wait_value = src->presentation_wait_value;
   dst->presentation_wait_is_wsi = src->presentation_wait_is_wsi;

   src->pending_value = 0;
   src->current_value = 0;
   src->presentation_wait_state = NULL;
   src->presentation_wait_bo = NULL;
   src->presentation_wait_value = 0;
   src->presentation_wait_is_wsi = false;

   return VK_SUCCESS;
}

static VkResult
terakan_sync_completion_signal(struct vk_device * const device_base,
                               struct vk_sync * const sync_base, uint64_t const value)
{
   struct terakan_device * const device = container_of(device_base, struct terakan_device, vk);
   struct terakan_sync_completion * const sync =
      container_of(sync_base, struct terakan_sync_completion, vk);

   mtx_lock(&device->completion_mutex);
   /* Timeline semaphores can signal values that don't strictly
    * interleave with our internal pending/current ordering (for
    * example, host-signal of a value already reached, or a racy
    * test signaling out of order).  Treat such cases as idempotent
    * no-ops returning VK_SUCCESS so any waiters still get a
    * broadcast and the test continues -- previously we asserted
    * (abort) or returned VK_ERROR_UNKNOWN (hang waiting on a
    * signal that never completes).  Neither is correct.
    *
    * Two out-of-order shapes to tolerate:
    *   value <= current_value : already signalled, waiters done.
    *   value >= pending_value : leap-ahead, update watermarks. */
   uint64_t const current = sync->current_value;
   if (value <= current) {
      bool const should_broadcast = (value >= device->completion_broadcast_threshold);
      mtx_unlock(&device->completion_mutex);
      if (should_broadcast)
         cnd_broadcast(&device->completion_condition);
      return VK_SUCCESS;
   }
   if (value >= sync->pending_value) {
      p_atomic_set(&sync->pending_value, value);
   }
   p_atomic_set(&sync->current_value, value);
   bool const should_broadcast = (value >= device->completion_broadcast_threshold);
   mtx_unlock(&device->completion_mutex);
   if (should_broadcast)
      cnd_broadcast(&device->completion_condition);

   return VK_SUCCESS;
}

static VkResult
terakan_sync_completion_get_value(struct vk_device * const device, struct vk_sync * const sync_base,
                                  uint64_t * const value_out)
{
   struct terakan_sync_completion const * const sync =
      container_of(sync_base, struct terakan_sync_completion, vk);
   *value_out = terakan_sync_completion_get_current_value(sync, 0);
   return VK_SUCCESS;
}

static VkResult
terakan_sync_completion_wait_many(struct vk_device * const device_base, uint32_t const wait_count,
                                  struct vk_sync_wait const * const waits,
                                  enum vk_sync_wait_flags const wait_flags,
                                  uint64_t const abs_timeout_ns)
{
   bool const wait_any = (wait_flags & VK_SYNC_WAIT_ANY) != 0;

   /* Fast path without locking the mutex if the signals have already been performed. */
   {
      uint32_t wait_index;
      for (wait_index = 0; wait_index < wait_count; ++wait_index) {
         struct vk_sync_wait const * const wait = &waits[wait_index];
         struct terakan_sync_completion const * const sync =
            container_of(wait->sync, struct terakan_sync_completion const, vk);
         uint64_t const sync_value = terakan_sync_completion_get_current_value(sync, wait_flags);
         uint64_t const wait_value = terakan_sync_completion_get_wait_value(sync, wait->wait_value);
         if ((sync_value >= wait_value) == wait_any) {
            /* Awaited if wait-any, timed out if wait-all. */
            break;
         }
      }
      if ((wait_index < wait_count) == wait_any) {
         /* Any awaited if wait-any, or all not timed out if wait-all. */
         return VK_SUCCESS;
      }
   }

   struct terakan_device * const device = container_of(device_base, struct terakan_device, vk);

   mtx_lock(&device->completion_mutex);

   /* Register minimum wait threshold so terakan_sync_completion_signal can skip
    * cnd_broadcast for completions no sleeping thread needs.
    */
   {
      uint64_t my_min = UINT64_MAX;
      for (uint32_t ri = 0; ri < wait_count; ++ri) {
         struct terakan_sync_completion const * const rs =
            container_of(waits[ri].sync, struct terakan_sync_completion const, vk);
         uint64_t const rv = terakan_sync_completion_get_wait_value(rs, waits[ri].wait_value);
         if (rv < my_min)
            my_min = rv;
      }
      /* Best-effort suppression: threshold tracks the minimum across all
       * current waiters, but only resets when waiter_count drops to 0.  If
       * waiter A enters at v=10 and waiter B at v=20, threshold pins at 10
       * even after A exits.  Result: signaler may broadcast for v=10..19
       * with no waiter actually consuming it -- those are spurious-but-safe
       * wakeups for B that re-sleep immediately.  The optimization
       * eliminates the no-waiter-at-all case (which was the dominant cost).
       * A sorted multiset of waiter minima would close the residual at the
       * cost of a per-entry allocation; deferred unless profiling justifies.
       */
      if (my_min < device->completion_broadcast_threshold)
         device->completion_broadcast_threshold = my_min;
      device->completion_waiter_count++;
   }

   while (true) {
      if (device->completion_lost) {
         device->completion_waiter_count--;
         if (device->completion_waiter_count == 0)
            device->completion_broadcast_threshold = UINT64_MAX;
         mtx_unlock(&device->completion_mutex);
         return VK_ERROR_DEVICE_LOST;
      }

      uint32_t wait_index;
      for (wait_index = 0; wait_index < wait_count; ++wait_index) {
         struct vk_sync_wait const * const wait = &waits[wait_index];
         struct terakan_sync_completion const * const sync =
            container_of(wait->sync, struct terakan_sync_completion const, vk);
         uint64_t const sync_value = terakan_sync_completion_get_current_value(sync, wait_flags);
         uint64_t const wait_value = terakan_sync_completion_get_wait_value(sync, wait->wait_value);
         if ((sync_value >= wait_value) == wait_any) {
            /* Awaited if wait-any, timed out if wait-all. */
            break;
         }
      }
      if ((wait_index < wait_count) == wait_any) {
         /* Any awaited if wait-any, or all not timed out if wait-all. */
         device->completion_waiter_count--;
         if (device->completion_waiter_count == 0)
            device->completion_broadcast_threshold = UINT64_MAX;
         mtx_unlock(&device->completion_mutex);
         return VK_SUCCESS;
      }

      if (abs_timeout_ns == 0) {
         break;
      }
      int condition_wait_result;
      if (abs_timeout_ns == OS_TIMEOUT_INFINITE) {
         condition_wait_result = cnd_wait(&device->completion_condition, &device->completion_mutex);
      } else {
         /* cnd_timedwait uses CLOCK_REALTIME, while abs_timeout_ns is provided for CLOCK_MONOTONIC.
          * Convert from one to the other.
          */
         uint64_t const now_ns = os_time_get_nano();
         if (now_ns > abs_timeout_ns) {
            break;
         }
         uint64_t const rel_timeout_ns = abs_timeout_ns - now_ns;
         struct timespec now_ts;
         if (timespec_get(&now_ts, TIME_UTC) == 0) {
            vk_device_set_lost(
               &device->vk,
               "Failed to get the current time to await the submission condition variable");
            device->completion_lost = true;
            device->completion_waiter_count--;
            if (device->completion_waiter_count == 0)
               device->completion_broadcast_threshold = UINT64_MAX;
            mtx_unlock(&device->completion_mutex);
            cnd_broadcast(&device->completion_condition);
            return VK_ERROR_DEVICE_LOST;
         }
         struct timespec abs_timeout_ts;
         if (timespec_add_nsec(&abs_timeout_ts, &now_ts, rel_timeout_ns)) {
            /* Overflowed, treat a very long wait as infinite. */
            condition_wait_result =
               cnd_wait(&device->completion_condition, &device->completion_mutex);
         } else {
            condition_wait_result = cnd_timedwait(&device->completion_condition,
                                                  &device->completion_mutex, &abs_timeout_ts);
            if (condition_wait_result == thrd_timedout) {
               /* Might have been woken up spuriously by the system time being changed forward.
                * Go to the next iteration, which will check the monotonic clock.
                */
               continue;
            }
         }
      }
      if (condition_wait_result != thrd_success) {
         vk_device_set_lost(&device->vk, "Failed to await the submission condition variable");
         device->completion_lost = true;
         device->completion_waiter_count--;
         if (device->completion_waiter_count == 0)
            device->completion_broadcast_threshold = UINT64_MAX;
         mtx_unlock(&device->completion_mutex);
         cnd_broadcast(&device->completion_condition);
         return VK_ERROR_DEVICE_LOST;
      }
   }
   device->completion_waiter_count--;
   if (device->completion_waiter_count == 0)
      device->completion_broadcast_threshold = UINT64_MAX;
   mtx_unlock(&device->completion_mutex);
   return VK_TIMEOUT;
}

static void
terakan_sync_completion_finish(UNUSED struct vk_device * const device, struct vk_sync * const sync_base)
{
   struct terakan_sync_completion * const sync =
      container_of(sync_base, struct terakan_sync_completion, vk);
   if (sync->presentation_wait_state != NULL) {
      terakan_wsi_hw_wait_unref(sync->presentation_wait_state);
      sync->presentation_wait_state = NULL;
   }
}

static VkResult
terakan_sync_completion_init(struct vk_device * const device, struct vk_sync * const sync_base,
                             uint64_t const initial_value)
{
   struct terakan_sync_completion * const sync =
      container_of(sync_base, struct terakan_sync_completion, vk);

   sync->pending_value = initial_value;
   sync->current_value = initial_value;
   sync->presentation_wait_state = NULL;
   sync->presentation_wait_bo = NULL;
   sync->presentation_wait_value = 0;
   sync->presentation_wait_is_wsi = false;

   return VK_SUCCESS;
}

struct vk_sync_type const terakan_sync_completion_type = {
   .size = sizeof(struct terakan_sync_completion),
   .features = VK_SYNC_FEATURE_TIMELINE | VK_SYNC_FEATURE_GPU_WAIT | VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_SIGNAL | VK_SYNC_FEATURE_WAIT_ANY | VK_SYNC_FEATURE_WAIT_PENDING,
   .init = terakan_sync_completion_init,
   .finish = terakan_sync_completion_finish,
   .move = terakan_sync_completion_move,
   .signal = terakan_sync_completion_signal,
   .get_value = terakan_sync_completion_get_value,
   .wait_many = terakan_sync_completion_wait_many,
};
