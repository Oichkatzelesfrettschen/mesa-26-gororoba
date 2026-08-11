/*
 * SPDX-License-Identifier: MIT
 *
 * Host test for relocation-list aggregation semantics.
 */

/* The asserts carry the test's side effects and verdicts, so they stay
 * live in NDEBUG builds.
 */
#undef NDEBUG

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
test_duplicate_handle_preserves_index_map(void)
{
   struct radeon_drm_vk_reloc_list list;
   radeon_drm_vk_reloc_list_init(&list);

   uint32_t first_index;
   uint32_t second_index;
   uint32_t duplicate_index;
   assert(radeon_drm_vk_reloc_list_add(&list, 31,
                                       RADEON_GEM_DOMAIN_GTT, 0, 0,
                                       &first_index) == 0);
   assert(radeon_drm_vk_reloc_list_add(&list, 47, 0,
                                       RADEON_GEM_DOMAIN_GTT, 0,
                                       &second_index) == 0);
   assert(radeon_drm_vk_reloc_list_add(&list, 31, 0,
                                       RADEON_GEM_DOMAIN_VRAM, 0,
                                       &duplicate_index) == 0);
   assert(first_index == 0);
   assert(second_index == 1);
   assert(duplicate_index == first_index);
   assert(list.count == 2);
   assert(list.relocs[first_index].handle == 31);
   assert(list.relocs[first_index].read_domains == RADEON_GEM_DOMAIN_GTT);
   assert(list.relocs[first_index].write_domain == RADEON_GEM_DOMAIN_VRAM);
   assert(list.relocs[second_index].handle == 47);

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
   test_duplicate_handle_preserves_index_map();
   test_growth_preserves_order();
   printf("radeon_drm_vk_reloc_test: all checks passed\n");
   return 0;
}
