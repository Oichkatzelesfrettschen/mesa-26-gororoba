/*
 * Copyright © 2024-2026 Vitaliy Triang3l Kuzmin
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

/* FIX-AC: per-submit silicon-latch CB-exporter warmup dispatch.
 *
 * Evergreen silicon's MEM_RAT_STORE_TYPED pipeline silently drops the R-dword
 * on the first dispatch after a cold context (power-on or after an idle
 * interval where the CB-exporter state has been reset).  Prepending a
 * single-thread r32ui imageStore before every vkQueueSubmit forces the
 * silicon into the WARM state before the user's compute dispatches execute.
 *
 * Empirical validation (tranche 19 steinmarder):
 *   r32_uint primer, 5 tries: 5/0 pass/fail when silicon is in COLD state.
 * DEEP_COLD (power-on after hard reset) may require 2 sequential dispatches;
 * this implementation records 2 dispatches in the pre-recorded command buffer.
 *
 * Cost: 1 r32ui VkImage (1x1, 4 bytes VRAM), 1 pre-compiled VkPipeline,
 * 1 pre-recorded VkCommandBuffer.  Per-submit overhead: ~1 kernel CS ioctl.
 *
 * Controlled by TERAKAN_FIX_AC_WARMUP=1 (default true).
 * Tracked: findings/active/
 * Claims:
 */

#ifndef TERAKAN_FIX_AC_WARMUP_H
#define TERAKAN_FIX_AC_WARMUP_H

#include "vk_device.h"

#include <stdbool.h>

struct terakan_device;
struct terakan_queue;

struct terakan_fix_ac_warmup {
   VkDescriptorSetLayout descriptor_set_layout;
   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;
   VkImage image;
   VkDeviceMemory memory;
   VkImageView image_view;
   VkDescriptorPool descriptor_pool;
   VkDescriptorSet descriptor_set;
   VkCommandPool command_pool;
   VkCommandBuffer command_buffer;
};

VkResult terakan_fix_ac_warmup_create(struct terakan_device *device,
                                      struct terakan_fix_ac_warmup **out);

void terakan_fix_ac_warmup_destroy(struct terakan_device *device,
                                   struct terakan_fix_ac_warmup *warmup);

VkResult terakan_fix_ac_warmup_submit_prelude(struct terakan_device *device,
                                              struct terakan_fix_ac_warmup *warmup,
                                              struct terakan_queue *queue);

#endif /* TERAKAN_FIX_AC_WARMUP_H */
