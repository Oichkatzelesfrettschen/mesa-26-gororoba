/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_CPU_SYNC_H
#define R300VK_CPU_SYNC_H

#include "vk_sync.h"
#include "c11/threads.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CPU-side binary sync for r300vk.  r300vk's submit path calls
 * pipe->flush() + fence_finish() synchronously before driver_submit returns,
 * so fences are always in the signaled state by the time vkWaitForFences
 * checks them.  The type still provides correct mutex+condvar semantics so
 * any caller that inspects the signaled state before submit completes will
 * block rather than observe stale data. */
struct r300vk_cpu_sync {
   struct vk_sync vk;
   mtx_t          lock;
   cnd_t          changed;
   bool           signaled;
};

static inline struct r300vk_cpu_sync *
r300vk_cpu_sync_from_vk(struct vk_sync *sync)
{
   return (struct r300vk_cpu_sync *)sync;
}

extern const struct vk_sync_type r300vk_cpu_sync_type;

#ifdef __cplusplus
}
#endif

#endif /* R300VK_CPU_SYNC_H */
