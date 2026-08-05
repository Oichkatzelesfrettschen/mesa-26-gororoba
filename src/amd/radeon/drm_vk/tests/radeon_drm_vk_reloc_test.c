/*
 * SPDX-License-Identifier: MIT
 *
 * Host test for relocation-list aggregation semantics.
 */

#include "radeon_drm_vk_reloc.h"

#include <assert.h>
#include <stdio.h>

#include <radeon_drm.h>

static void
test_first_add_assigns_ordered_indices(void)
{
   struct radeon_drm_vk_reloc_list list;
   radeon_drm_vk_reloc_list_init(&list);

   uint32_t index;
   assert(radeon_drm_vk_reloc_list_add(&list, 7, RADEON_GEM_DOMAIN_GTT, 0, 0,
                                       &index) == 0);
   assert(index == 0);
   assert(radeon_drm_vk_reloc_list_add(&list, 9, RADEON_GEM_DOMAIN_VRAM, 0, 0,
                                       &index) == 0);
   assert(index == 1);
   assert(list.count == 2);
   assert(list.relocs[0].handle == 7);
   assert(list.relocs[1].handle == 9);

   radeon_drm_vk_reloc_list_finish(&list);
}

static void
test_duplicate_handle_merges(void)
{
   struct radeon_drm_vk_reloc_list list;
   radeon_drm_vk_reloc_list_init(&list);

   uint32_t index;
   assert(radeon_drm_vk_reloc_list_add(&list, 7, RADEON_GEM_DOMAIN_GTT, 0, 2,
                                       &index) == 0);
   assert(index == 0);
   /* Same handle: read domains OR, write domain OR, priority MAX, index
    * stable, count unchanged.
    */
   assert(radeon_drm_vk_reloc_list_add(&list, 7, RADEON_GEM_DOMAIN_VRAM,
                                       RADEON_GEM_DOMAIN_GTT, 1,
                                       &index) == 0);
   assert(index == 0);
   assert(list.count == 1);
   assert(list.relocs[0].read_domains ==
          (RADEON_GEM_DOMAIN_GTT | RADEON_GEM_DOMAIN_VRAM));
   assert(list.relocs[0].write_domain == RADEON_GEM_DOMAIN_GTT);
   assert(list.relocs[0].flags == 2);

   radeon_drm_vk_reloc_list_finish(&list);
}

static void
test_growth_preserves_order(void)
{
   struct radeon_drm_vk_reloc_list list;
   radeon_drm_vk_reloc_list_init(&list);

   for (uint32_t handle = 1; handle <= 40; handle++) {
      uint32_t index;
      assert(radeon_drm_vk_reloc_list_add(&list, handle,
                                          RADEON_GEM_DOMAIN_GTT, 0, 0,
                                          &index) == 0);
      assert(index == handle - 1);
   }
   assert(list.count == 40);
   for (uint32_t i = 0; i < 40; i++) {
      assert(list.relocs[i].handle == i + 1);
   }

   radeon_drm_vk_reloc_list_finish(&list);
}

int
main(void)
{
   test_first_add_assigns_ordered_indices();
   test_duplicate_handle_merges();
   test_growth_preserves_order();
   printf("radeon_drm_vk_reloc_test: all checks passed\n");
   return 0;
}
