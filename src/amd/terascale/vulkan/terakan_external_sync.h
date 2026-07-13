/* SPDX-License-Identifier: MIT */
#ifndef TERAKAN_EXTERNAL_SYNC_H
#define TERAKAN_EXTERNAL_SYNC_H

/*
 * External-sync bridge for terakan on top of the radeon DRM uAPI.
 *
 * Vulkan exposes VK_KHR_external_semaphore_fd (#80, core 1.1) and
 * VK_KHR_external_fence_fd (#116, core 1.1) which let applications
 * import / export semaphore + fence payloads as either OPAQUE_FD
 * (a duplicated DRM syncobj handle) or SYNC_FD (a struct sync_file
 * fd, single binary signal-at-a-moment).
 *
 * Behind the runtime entrypoints `vkGetSemaphoreFdKHR` /
 * `vkImportSemaphoreFdKHR` (and the fence equivalents) Mesa's
 * shared `vk_drm_syncobj` sync type implements the FD<->handle
 * conversions by issuing the standard `DRM_IOCTL_SYNCOBJ_*` ioctls
 * on the device's render-node fd.  Those ioctls are part of the
 * core DRM uAPI and the in-tree radeon driver has exposed them
 * since Linux 4.13 (syncobj base) / 5.5 (timeline + sync_file),
 * predating any kernel terakan supports.
 *
 * What this bridge adds beyond the runtime helpers:
 *
 *   1) On submit:  detect signal entries whose `vk_sync` is a
 *      `vk_drm_syncobj` (i.e. an externally-imported / exportable
 *      payload) and queue a post-submission SYNCOBJ_SIGNAL /
 *      SYNCOBJ_TIMELINE_SIGNAL so the external waiter sees the
 *      kernel handle become signaled when terakan's own completion
 *      BO retires.  Without this hook the queue submit path would
 *      assert because it expects every signal entry to be a
 *      `terakan_sync_completion` (in-process binary/timeline).
 *
 *   2) On submit:  surface the imported-syncobj wait fan-out to
 *      the CPU-wait path.  The radeon CS submit ioctl
 *      (`DRM_IOCTL_RADEON_CS`) carries only RELOCS / IB / FLAGS /
 *      CONST_IB chunks per `<drm/radeon_drm.h>` -- it has no
 *      SYNCOBJ_IN / SYNCOBJ_OUT chunk equivalent to amdgpu's
 *      `AMDGPU_CHUNK_ID_SYNCOBJ_*`.  Imported waits therefore
 *      cannot be attached to the submit ioctl directly; the
 *      bridge falls back to blocking on the imported syncobj via
 *      the runtime's `vk_sync_wait_many` call (which dispatches
 *      to `DRM_IOCTL_SYNCOBJ_WAIT`) before the CS submit.  This
 *      preserves ordering at the cost of a CPU stall per imported
 *      wait; that cost is documented and accepted for r600-class
 *      hardware whose throughput is bounded elsewhere.
 *
 * Cross-process timeline semantics:
 *
 *   The radeon kernel exposes `DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL`
 *   and `_TIMELINE_WAIT`, so a timeline semaphore exported as
 *   OPAQUE_FD and re-imported on another process retains its
 *   monotonic counter.  However a SYNC_FD payload encodes only a
 *   single binary signal point; per the Vulkan spec
 *   (33.7.3 "Importing Fence Payloads") a SYNC_FD import resets
 *   the semaphore / fence to binary semantics for that payload.
 *
 *   See AMD Radeon HD 6000-Series ISA + Linux DRM uAPI
 *   `<drm/drm.h>` `DRM_IOCTL_SYNCOBJ_*` definitions for the
 *   wire-level contract.
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

/* Per-submission record of an external syncobj that must be signaled
 * after the radeon CS submission completes.  Allocated on the stack
 * during `terakan_queue_submit` and copied into the submission's
 * post-completion list. */
struct terakan_external_signal {
   /* DRM syncobj handle (the `vk_drm_syncobj::syncobj` field). */
   uint32_t kernel_handle;
   /* Timeline point to signal; 0 for binary semaphores. */
   uint64_t point;
   /* If true, issue DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL with `point`.
    * If false, issue DRM_IOCTL_SYNCOBJ_SIGNAL (binary signal). */
   bool is_timeline;
};

/* Test whether a `vk_sync` is a DRM-syncobj-backed sync (i.e. a
 * payload imported via vkImportSemaphoreFdKHR / vkImportFenceFdKHR
 * or registered as `device->sync_type_drm_syncobj`).
 *
 * Returns the underlying kernel handle in `*kernel_handle_out` when
 * true; the caller may pass NULL to test without retrieving. */
bool
terakan_sync_is_external_drm_syncobj(struct vk_sync const *sync,
                                     uint32_t *kernel_handle_out);

/* Issue DRM_IOCTL_SYNCOBJ_SIGNAL (binary) or
 * DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL (timeline) for the given handle.
 * `drm_fd` is the device render-node fd (terakan_device::render_node_fd).
 * Returns VK_SUCCESS on success, VK_ERROR_DEVICE_LOST on ioctl failure. */
VkResult
terakan_external_syncobj_signal(int drm_fd,
                                struct terakan_external_signal const *sig);

/* Bulk-issue post-completion signals.  Called from the queue completion
 * worker after the winsys reports the CS submission's completion BO is
 * idle. */
VkResult
terakan_external_syncobj_signal_many(int drm_fd,
                                     uint32_t count,
                                     struct terakan_external_signal const *sigs);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_EXTERNAL_SYNC_H */
