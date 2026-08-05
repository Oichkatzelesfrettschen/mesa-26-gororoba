/*
 * SPDX-License-Identifier: MIT
 *
 * Relocation-list aggregation for Radeon DRM command submission.
 */

#ifndef RADEON_DRM_VK_RELOC_H
#define RADEON_DRM_VK_RELOC_H

#include <stdint.h>

#include <radeon_drm.h>

/* Aggregates the per-submission BO reference list.  A handle appears once:
 * repeated adds OR the read domains, OR the write domain, and keep the
 * maximum priority, matching how the kernel merges duplicate references.
 * The array order is first-add order, so PM4 emitters can name a reference
 * by the index this list returns.
 */
struct radeon_drm_vk_reloc_list {
   struct drm_radeon_cs_reloc *relocs;
   uint32_t count;
   uint32_t capacity;
};

void radeon_drm_vk_reloc_list_init(struct radeon_drm_vk_reloc_list *list);

void radeon_drm_vk_reloc_list_finish(struct radeon_drm_vk_reloc_list *list);

/* Returns 0 or -ENOMEM; *index names the reference slot for the handle.
 * priority takes RADEON_RELOC_PRIO_MASK-bounded values.
 */
int radeon_drm_vk_reloc_list_add(struct radeon_drm_vk_reloc_list *list,
                                 uint32_t handle, uint32_t read_domains,
                                 uint32_t write_domain, uint32_t priority,
                                 uint32_t *index);

#endif /* RADEON_DRM_VK_RELOC_H */
