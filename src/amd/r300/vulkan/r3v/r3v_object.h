/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_OBJECT_H
#define R3V_OBJECT_H

#include "vk_object.h"
#include "vk_query_pool.h"
#include "vk_sampler.h"

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One query slot's CPU-replay result.  r300 hardware supports only occlusion
 * queries (PIPE_QUERY_OCCLUSION_*); the replay brackets the spanned draws of a
 * single-tile submit with one r300 occlusion query and stores the sample count
 * here at end-query replay (get_query_result waits for the GPU).  available is
 * set once a result is stored and cleared by vkCmdResetQueryPool. */
struct r3v_query {
   uint64_t result;
   bool     available;
};

/* r3v query pool: the runtime vk_query_pool base (which owns query_type and
 * query_count) followed by one r3v_query per query.  vk_query_pool_create
 * zero-initializes the whole allocation, so every slot starts unavailable with
 * a zero result.  vk stays first so a vk_query_pool handle round-trips. */
struct r3v_query_pool {
   struct vk_query_pool vk;
   struct r3v_query  queries[];
};

static inline struct r3v_query_pool *
r3v_query_pool(struct vk_query_pool *vk)
{
   return (struct r3v_query_pool *)vk;
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
 * recorder (r3v_cmd_buffer.c) and the replay (r3v_queue.c) can reach the
 * status field. */
struct r3v_event {
   struct vk_object_base base;
   VkResult              status;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_event, base, VkEvent,
                               VK_OBJECT_TYPE_EVENT)

/* r3v extends the runtime vk_sampler -- which drops the VkFilter / mipmap /
 * compare fields after vk_sampler_create -- with the pre-built Gallium sampler
 * state object.  vkCreateSampler maps the full VkSamplerCreateInfo to a
 * pipe_sampler_state once and caches the CSO here; the draw replay binds it for
 * a fragment combined-image-sampler.  vk stays first so a VkSampler handle
 * round-trips through vk_sampler_{to,from}_handle. */
struct r3v_sampler {
   struct vk_sampler vk;
   void             *pipe_cso;
   /* The runtime vk_sampler drops the filter/mipmap/compare fields, so cache at
    * create time whether this sampler is eligible for experimental NEAREST tile
    * stitching: NEAREST mag/min/mip, CLAMP_TO_EDGE on every axis, normalized
    * coordinates, and no compare.  A split (multi-tile) sampled image may only
    * stitch through an eligible sampler. */
   bool              nearest_stitch_eligible;
   /* LINEAR variant: a split image samples through the overlapped halo atlas with
    * a LINEAR/CLAMP_TO_EDGE/normalized/no-compare/no-aniso sampler, where the
    * duplicated seam texels keep the bilinear footprint inside one chart. */
   bool              linear_stitch_eligible;
};

static inline struct r3v_sampler *
r3v_sampler_from_vk(struct vk_sampler *sampler)
{
   return (struct r3v_sampler *)sampler;
}

#ifdef __cplusplus
}
#endif

#endif /* R3V_OBJECT_H */
