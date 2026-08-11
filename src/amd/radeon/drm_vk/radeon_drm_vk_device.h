/*
 * SPDX-License-Identifier: MIT
 *
 * Radeon DRM device transport state for the Vulkan transport layer.
 */

#ifndef RADEON_DRM_VK_DEVICE_H
#define RADEON_DRM_VK_DEVICE_H

#include "radeon_drm_vk_ioctl.h"

#include "c11/threads.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

struct hash_table;

struct radeon_drm_vk_cache_event {
   uint64_t sequence;
   uintptr_t map;
   uint32_t bo_handle;
};

struct radeon_drm_vk_close_event {
   uint64_t sequence;
   uint32_t bo_handle;
};

/* Transport view of one opened radeon render node.  The caller owns file
 * descriptor acquisition, version checking, and close; the device borrows the
 * descriptor for its lifetime.  The shared-BO reference-count table keeps one
 * GEM handle open while multiple imports or a concurrent free race on it, so
 * DRM_IOCTL_GEM_CLOSE runs exactly once per handle.
 */
struct radeon_drm_vk_device {
   int fd;
   const struct radeon_drm_vk_ioctl_ops *ops;
   mtx_t shared_bo_mutex;
   mtx_t cache_event_mutex;
   struct hash_table *shared_bo_reference_counts;
   /* The count stays atomic because Vulkan permits independent
    * VkDeviceMemory frees to execute concurrently.  The last-event records
    * stay under cache_event_mutex so map, BO handle, and sequence form one
    * coherent host-model witness. */
   _Atomic uint64_t cache_sync_count;
   uint64_t cache_event_sequence;
   struct radeon_drm_vk_cache_event cache_sync_last;
   struct radeon_drm_vk_close_event bo_close_last;
};

/* ops == NULL selects the production libdrm table. Returns 0 or -ENOMEM. */
int radeon_drm_vk_device_init(struct radeon_drm_vk_device *device, int fd,
                              const struct radeon_drm_vk_ioctl_ops *ops);

void radeon_drm_vk_device_finish(struct radeon_drm_vk_device *device);

/* DRM_RADEON_GETPARAM wrapper. Returns 0 or a negative errno. */
int radeon_drm_vk_device_getparam(struct radeon_drm_vk_device *device,
                                  uint32_t param, uint32_t *value);

/* DRM_RADEON_INFO wrapper. Returns 0 or a negative errno. */
int radeon_drm_vk_device_info(struct radeon_drm_vk_device *device,
                              uint32_t request, uint32_t *value);

#endif /* RADEON_DRM_VK_DEVICE_H */
