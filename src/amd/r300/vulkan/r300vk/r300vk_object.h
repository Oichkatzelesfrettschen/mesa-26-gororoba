/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_OBJECT_H
#define R300VK_OBJECT_H

#include "vk_object.h"
#include "vk_query_pool.h"

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One query slot's CPU-replay result.  r300 hardware supports only occlusion
 * queries (PIPE_QUERY_OCCLUSION_*); the replay brackets the spanned draws of a
 * single-tile submit with one r300 occlusion query and stores the sample count
 * here at end-query replay (get_query_result waits for the GPU).  available is
 * set once a result is stored and cleared by vkCmdResetQueryPool. */
struct r300vk_query {
   uint64_t result;
   bool     available;
};

/* r300vk query pool: the runtime vk_query_pool base (which owns query_type and
 * query_count) followed by one r300vk_query per query.  vk_query_pool_create
 * zero-initializes the whole allocation, so every slot starts unavailable with
 * a zero result.  vk stays first so a vk_query_pool handle round-trips. */
struct r300vk_query_pool {
   struct vk_query_pool vk;
   struct r300vk_query  queries[];
};

static inline struct r300vk_query_pool *
r300vk_query_pool(struct vk_query_pool *vk)
{
   return (struct r300vk_query_pool *)vk;
}

/* vk_query_pool.h defines the base struct but not its handle casts (unlike
 * vk_sampler.h / vk_buffer_view.h), so declare them here; the command recorder
 * and the replay both resolve a VkQueryPool through this. */
VK_DEFINE_NONDISP_HANDLE_CASTS(vk_query_pool, base, VkQueryPool,
                               VK_OBJECT_TYPE_QUERY_POOL)

/* vkEvent on the single-queue serialized CPU-replay model: a host signal flag.
 * status holds VK_EVENT_SET or VK_EVENT_RESET.  CmdSetEvent2/CmdResetEvent2 are
 * recorded and applied in the post-fence CPU pass (so a host GetEventStatus
 * after submit observes them); CmdWaitEvents2 is a no-op because the CPU pass
 * replays entries in recorded order.  Shared via this header so the command
 * recorder (r300vk_cmd_buffer.c) and the replay (r300vk_queue.c) can reach the
 * status field. */
struct r300vk_event {
   struct vk_object_base base;
   VkResult              status;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_event, base, VkEvent,
                               VK_OBJECT_TYPE_EVENT)

#ifdef __cplusplus
}
#endif

#endif /* R300VK_OBJECT_H */
