/* SPDX-License-Identifier: MIT */

#include "radeon_drm_vk_reloc.h"

#include "util/macros.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>

void
radeon_drm_vk_reloc_list_init(struct radeon_drm_vk_reloc_list *list)
{
   list->relocs = NULL;
   list->count = 0;
   list->capacity = 0;
}

void
radeon_drm_vk_reloc_list_finish(struct radeon_drm_vk_reloc_list *list)
{
   free(list->relocs);
   radeon_drm_vk_reloc_list_init(list);
}

int
radeon_drm_vk_reloc_list_add(struct radeon_drm_vk_reloc_list *list,
                             uint32_t handle, uint32_t read_domains,
                             uint32_t write_domain, uint32_t priority,
                             uint32_t *index)
{
   assert((priority & ~(uint32_t)RADEON_RELOC_PRIO_MASK) == 0);

   for (uint32_t i = 0; i < list->count; i++) {
      struct drm_radeon_cs_reloc *reloc = &list->relocs[i];
      if (reloc->handle == handle) {
         reloc->read_domains |= read_domains;
         reloc->write_domain |= write_domain;
         reloc->flags = MAX2(reloc->flags, priority);
         *index = i;
         return 0;
      }
   }

   if (list->count == list->capacity) {
      uint32_t capacity = list->capacity == 0 ? 8 : list->capacity * 2;
      struct drm_radeon_cs_reloc *relocs =
         realloc(list->relocs, capacity * sizeof(*relocs));
      if (relocs == NULL) {
         return -ENOMEM;
      }
      list->relocs = relocs;
      list->capacity = capacity;
   }

   list->relocs[list->count] = (struct drm_radeon_cs_reloc){
      .handle = handle,
      .read_domains = read_domains,
      .write_domain = write_domain,
      .flags = priority,
   };
   *index = list->count;
   list->count++;
   return 0;
}
