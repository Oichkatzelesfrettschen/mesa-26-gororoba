/*
 * SPDX-License-Identifier: MIT
 *
 * Finite completion object over Radeon GEM idle waits.
 */

#ifndef RADEON_DRM_VK_COMPLETION_H
#define RADEON_DRM_VK_COMPLETION_H

#include "radeon_drm_vk_bo.h"

#include <stdint.h>

struct radeon_drm_vk_device;
struct radeon_drm_vk_reloc_list;

/* One 4-byte GTT BO referenced as a write target in the signaling
 * submission.  The kernel attaches a fence to the BO's reservation object at
 * CS-submit time, so GEM_WAIT_IDLE on the BO returns when the submission
 * retires.
 */
struct radeon_drm_vk_completion {
   struct radeon_drm_vk_bo bo;
};

/* Returns 0 or a negative errno. */
int radeon_drm_vk_completion_init(struct radeon_drm_vk_device *device,
                                  struct radeon_drm_vk_completion *completion);

void radeon_drm_vk_completion_finish(
   struct radeon_drm_vk_device *device,
   struct radeon_drm_vk_completion *completion);

/* Adds the write-domain reference that binds the completion BO to the
 * signaling submission. Returns 0 or -ENOMEM.
 */
int radeon_drm_vk_completion_reference(
   const struct radeon_drm_vk_completion *completion,
   struct radeon_drm_vk_reloc_list *relocs, uint32_t *index);

/* DRM_RADEON_GEM_WAIT_IDLE with a bounded -EBUSY retry budget; the kernel
 * bounds each wait (~30 s), so the call returns in finite time.  Returns 0 on
 * retirement or the last negative errno; an exhausted retry budget returns
 * -EBUSY, and the caller escalates to device-loss handling.
 */
int radeon_drm_vk_completion_await(
   struct radeon_drm_vk_device *device,
   const struct radeon_drm_vk_completion *completion);

#endif /* RADEON_DRM_VK_COMPLETION_H */
