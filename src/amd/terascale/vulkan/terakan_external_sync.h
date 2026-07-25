/*
 * Copyright (c) 2024 Vitaliy "Triang3l" Kuzmin
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_EXTERNAL_SYNC_H
#define TERAKAN_EXTERNAL_SYNC_H

/*
 * External-sync helpers for terakan on the radeon DRM syncobj uAPI.
 *
 * Vulkan's VK_KHR_external_semaphore_fd and VK_KHR_external_fence_fd
 * import and export semaphore or fence payloads as OPAQUE_FD (a DRM
 * syncobj handle) or SYNC_FD (a one-shot sync_file). Mesa's shared
 * vk_drm_syncobj type performs the FD and handle conversions with the
 * standard DRM_IOCTL_SYNCOBJ_* ioctls on the render-node fd. The in-tree
 * radeon driver has exposed those ioctls since Linux 4.13 (base) / 5.5
 * (timeline and sync_file).
 *
 * This header declares the signal helpers and the external-syncobj
 * recognition used by the queue path. It does not itself implement
 * queue-submit wait fan-out or post-completion signaling; those live
 * in the queue implementation that calls these helpers.
 *
 * SYNC_FD payloads encode one binary signal point, so a SYNC_FD import
 * resets the object to binary semantics for that payload. Timeline
 * OPAQUE_FD payloads retain the monotonic counter through
 * DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL and TIMELINE_WAIT.
 */

#include "vk_sync.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_device;
struct terakan_queue;
struct terakan_queue_completion_submission;

/* Per-signal record for an external DRM syncobj handle that must be
 * signaled after a CS submission completes. Callers allocate these on
 * the stack and pass them to the signal helpers. */
struct terakan_external_signal {
   uint32_t kernel_handle;
   uint64_t point;
   bool is_timeline;
};

/* True when sync is a DRM syncobj payload. On true, writes the kernel
 * handle into kernel_handle_out when non-NULL. */
bool terakan_sync_is_external_drm_syncobj(struct vk_sync const *sync,
                                          uint32_t *kernel_handle_out);

/* Signal one external syncobj (binary or timeline). Retries the DRM
 * ioctl on EINTR and EAGAIN. Non-retryable failures return
 * VK_ERROR_DEVICE_LOST. */
VkResult terakan_external_syncobj_signal(
   int drm_fd, struct terakan_external_signal const *sig);

/* Signal many external syncobjs, batching binary and timeline ioctls.
 * Retries on EINTR and EAGAIN. */
VkResult terakan_external_syncobj_signal_many(
   int drm_fd, uint32_t count, struct terakan_external_signal const *sigs);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_EXTERNAL_SYNC_H */
