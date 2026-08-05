/*
 * SPDX-License-Identifier: MIT
 *
 * DRM_RADEON_CS argument construction and submission.
 */

#ifndef RADEON_DRM_VK_CS_H
#define RADEON_DRM_VK_CS_H

#include <stdbool.h>
#include <stdint.h>

#include <radeon_drm.h>

struct radeon_drm_vk_device;
struct radeon_drm_vk_reloc_list;

/* Fully constructed DRM_RADEON_CS arguments.  Build is pure argument
 * construction over caller-owned IB and reloc storage; submit performs the
 * one ioctl.  The chunk and pointer arrays live inside this struct and the
 * chunk_data fields point at the caller's IB and reloc arrays, so the caller
 * keeps both alive until submit returns and does not relocate this struct
 * between build and submit.
 */
struct radeon_drm_vk_cs {
   uint32_t flags[3];
   struct drm_radeon_cs_chunk chunks[3];
   uint64_t chunk_pointers[3];
   struct drm_radeon_cs args;
};

/* The kernel rejects an empty IB with -EINVAL, so ib_size_dwords is nonzero.
 * keep_tiling_flags belongs on the GFX ring, where CS submission must not
 * rewrite surface tiling state.
 */
void radeon_drm_vk_cs_build(struct radeon_drm_vk_cs *cs, const uint32_t *ib,
                            uint32_t ib_size_dwords,
                            const struct radeon_drm_vk_reloc_list *relocs,
                            uint32_t ring, bool keep_tiling_flags);

/* Drains CPU write-combining buffers, then issues DRM_RADEON_CS. Returns 0
 * or a negative errno.
 */
int radeon_drm_vk_cs_submit(struct radeon_drm_vk_device *device,
                            struct radeon_drm_vk_cs *cs);

#endif /* RADEON_DRM_VK_CS_H */
