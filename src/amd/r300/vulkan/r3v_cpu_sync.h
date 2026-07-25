/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_CPU_SYNC_H
#define R3V_CPU_SYNC_H

#include "vk_sync.h"
#include "c11/threads.h"
#include "util/cnd_monotonic.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CPU-side binary sync for r3v.  r3v's submit path calls
 * pipe->flush() + fence_finish() synchronously before driver_submit returns,
 * so fences are always in the signaled state by the time vkWaitForFences
 * checks them.  The type still provides correct mutex+condvar semantics so
 * any caller that inspects the signaled state before submit completes will
 * block rather than observe stale data. */
struct r3v_cpu_sync {
   struct vk_sync          vk;
   mtx_t                   lock;
   struct u_cnd_monotonic  changed;
   bool                    signaled;
};

static inline struct r3v_cpu_sync *
r3v_cpu_sync_from_vk(struct vk_sync *sync)
{
   return (struct r3v_cpu_sync *)sync;
}

extern const struct vk_sync_type r3v_cpu_sync_type;

#ifdef __cplusplus
}
#endif

#endif /* R3V_CPU_SYNC_H */
