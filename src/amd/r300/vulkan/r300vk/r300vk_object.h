/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_OBJECT_H
#define R300VK_OBJECT_H

#include "vk_object.h"

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

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
