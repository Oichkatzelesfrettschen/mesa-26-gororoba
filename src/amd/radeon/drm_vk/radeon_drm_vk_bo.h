/*
 * SPDX-License-Identifier: MIT
 *
 * Radeon GEM buffer-object transport for the Vulkan transport layer.
 */

#ifndef RADEON_DRM_VK_BO_H
#define RADEON_DRM_VK_BO_H

#include <stdbool.h>
#include <stdint.h>

struct radeon_drm_vk_device;

/* One GEM buffer object.  Domains and flags are RADEON_GEM_* policy inputs
 * chosen by the caller; Vulkan memory-property translation stays above the
 * transport.  A shareable BO participates in the device's shared-handle
 * reference count so PRIME import and free serialize on one GEM close.
 */
struct radeon_drm_vk_bo {
   uint64_t size;
   uint32_t handle;
   uint32_t domains;
   bool shareable;
};

/* DRM_RADEON_GEM_CREATE. Returns 0 or a negative errno. */
int radeon_drm_vk_bo_create(struct radeon_drm_vk_device *device, uint64_t size,
                            uint64_t alignment, uint32_t domains,
                            uint32_t flags, bool shareable,
                            struct radeon_drm_vk_bo *bo);

/* DRM_RADEON_GEM_MMAP plus a shared OS mapping. Returns 0 or a negative
 * errno; *map points at size mapped bytes on success.
 */
int radeon_drm_vk_bo_map(struct radeon_drm_vk_device *device,
                         const struct radeon_drm_vk_bo *bo, void **map);

void radeon_drm_vk_bo_unmap(struct radeon_drm_vk_device *device,
                            const struct radeon_drm_vk_bo *bo, void *map);

/* Writes back and invalidates every CPU cache line covering a host mapping.
 * The RS485M-family GART runs with request snooping disabled
 * (rs400_gart_enable programs RS480_AGP_MODE_CNTL with REQ_TYPE_SNOOP_DIS),
 * and radeon_bo_create strips RADEON_GEM_GTT_WC and RADEON_GEM_GTT_UC on
 * every non-PCIE device, so a GTT mapping is always ttm_cached and the
 * driver keeps the coherency promise itself: it publishes host writes
 * before command submission and invalidates before host reads of device
 * output.  CLFLUSH both writes back and invalidates, so one primitive
 * serves both directions.  A null or empty range is a no-op.  The kernel
 * symbol and call-site paths are reproducible with `rg --fixed-strings
 * rs400_gart_enable drivers/gpu/drm/radeon/` and `rg --fixed-strings
 * radeon_bo_create drivers/gpu/drm/radeon/` in the Linux kernel source.
 */
void radeon_drm_vk_bo_cache_sync(struct radeon_drm_vk_device *device,
                                 const void *map, uint64_t size);

/* The BO-aware form records the handle in host-model event instrumentation
 * while applying the same cache-line publication primitive. */
void radeon_drm_vk_bo_cache_sync_for_bo(struct radeon_drm_vk_device *device,
                                        const struct radeon_drm_vk_bo *bo,
                                        const void *map, uint64_t size);

void radeon_drm_vk_bo_free(struct radeon_drm_vk_device *device,
                           struct radeon_drm_vk_bo *bo);

/* PRIME export; DRM_CLOEXEC always, DRM_RDWR when writable. Returns 0 or a
 * negative errno; the BO must be shareable.
 */
int radeon_drm_vk_bo_export_fd(struct radeon_drm_vk_device *device,
                               const struct radeon_drm_vk_bo *bo,
                               bool writable, int *prime_fd);

/* PRIME import; the resulting BO is shareable and reference-counted against
 * other imports of the same GEM handle. Returns 0 or a negative errno.
 */
int radeon_drm_vk_bo_import_fd(struct radeon_drm_vk_device *device,
                               int prime_fd, uint64_t size, uint32_t domains,
                               struct radeon_drm_vk_bo *bo);

#endif /* RADEON_DRM_VK_BO_H */
