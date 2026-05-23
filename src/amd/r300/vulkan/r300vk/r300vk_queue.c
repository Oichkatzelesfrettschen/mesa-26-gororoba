/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_device.h"

#include "vk_queue.h"

/* PR 1 stub: no command buffer replay yet.  Extended in PR 5 to replay
 * r300vk_cmd_entry arrays against the pipe_context and fence-wait for
 * GPU completion before returning. */
VkResult
r300vk_queue_driver_submit(struct vk_queue *vkq,
                            struct vk_queue_submit *submit)
{
   (void)vkq;
   (void)submit;
   return VK_SUCCESS;
}
