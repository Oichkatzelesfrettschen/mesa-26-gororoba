/*
 * SPDX-License-Identifier: MIT
 *
 * Host test for DRM_RADEON_CS argument construction and mock submission.
 */

/* The asserts carry the test's side effects and verdicts, so they stay
 * live in NDEBUG builds.
 */
#undef NDEBUG

#include "radeon_drm_vk_bo.h"
#include "radeon_drm_vk_cs.h"
#include "radeon_drm_vk_device.h"
#include "radeon_drm_vk_reloc.h"
#include "tests/radeon_drm_vk_mock.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct radeon_drm_vk_device *boundary_sync_device;
static char boundary_sync_range[256];

static int
boundary_sync_mock_command_write_read(int fd, unsigned long request,
                                       void *data, unsigned size)
{
   int result = radeon_drm_vk_mock_command_write_read(fd, request, data,
                                                      size);
   if (result == 0 && request == DRM_RADEON_CS) {
      radeon_drm_vk_bo_cache_sync(boundary_sync_device,
                                  boundary_sync_range,
                                  sizeof(boundary_sync_range));
   }
   return result;
}

static void
test_build_constructs_three_chunks(void)
{
   struct radeon_drm_vk_reloc_list relocs;
   radeon_drm_vk_reloc_list_init(&relocs);
   uint32_t index;
   assert(radeon_drm_vk_reloc_list_add(&relocs, 5, RADEON_GEM_DOMAIN_GTT,
                                       RADEON_GEM_DOMAIN_GTT, 0,
                                       &index) == 0);

   uint32_t ib[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
   struct radeon_drm_vk_cs cs;
   radeon_drm_vk_cs_build(&cs, ib, 4, &relocs, 0, true);

   assert(cs.args.num_chunks == 3);
   assert(cs.chunks[0].chunk_id == RADEON_CHUNK_ID_RELOCS);
   assert(cs.chunks[0].length_dw ==
          sizeof(struct drm_radeon_cs_reloc) / sizeof(uint32_t));
   assert(cs.chunks[1].chunk_id == RADEON_CHUNK_ID_IB);
   assert(cs.chunks[1].length_dw == 4);
   assert(cs.chunks[2].chunk_id == RADEON_CHUNK_ID_FLAGS);
   assert(cs.chunks[2].length_dw == 3);
   assert(cs.flags[0] == RADEON_CS_KEEP_TILING_FLAGS);
   assert(cs.flags[1] == 0);

   radeon_drm_vk_reloc_list_finish(&relocs);
}

static void
test_submit_passes_exact_bytes(void)
{
   radeon_drm_vk_mock_reset();
   struct radeon_drm_vk_device device;
   assert(radeon_drm_vk_device_init(&device, 42,
                                    &radeon_drm_vk_mock_ops) == 0);

   struct radeon_drm_vk_reloc_list relocs;
   radeon_drm_vk_reloc_list_init(&relocs);
   uint32_t index;
   assert(radeon_drm_vk_reloc_list_add(&relocs, 11, RADEON_GEM_DOMAIN_GTT, 0,
                                       3, &index) == 0);
   assert(radeon_drm_vk_reloc_list_add(&relocs, 12, 0,
                                       RADEON_GEM_DOMAIN_VRAM, 1,
                                       &index) == 0);

   uint32_t ib[7];
   for (uint32_t i = 0; i < 7; i++) {
      ib[i] = 0xC0DE0000u | i;
   }

   struct radeon_drm_vk_cs cs;
   radeon_drm_vk_cs_build(&cs, ib, 7, &relocs, 0, true);
   assert(radeon_drm_vk_cs_submit(&device, &cs) == 0);

   assert(radeon_drm_vk_mock.cs_called);
   assert(radeon_drm_vk_mock.cs_chunk_count == 3);
   /* The IB chunk reaches the kernel dword-exact. */
   assert(radeon_drm_vk_mock.cs_ib_dword_count == 7);
   assert(memcmp(radeon_drm_vk_mock.cs_ib_dwords, ib, sizeof(ib)) == 0);
   /* Both relocations arrive with their merged domains and priorities. */
   assert(radeon_drm_vk_mock.cs_reloc_count == 2);
   assert(radeon_drm_vk_mock.cs_relocs[0].handle == 11);
   assert(radeon_drm_vk_mock.cs_relocs[0].read_domains ==
          RADEON_GEM_DOMAIN_GTT);
   assert(radeon_drm_vk_mock.cs_relocs[0].flags == 3);
   assert(radeon_drm_vk_mock.cs_relocs[1].handle == 12);
   assert(radeon_drm_vk_mock.cs_relocs[1].write_domain ==
          RADEON_GEM_DOMAIN_VRAM);
   /* Flags chunk carries {flags, ring, priority}. */
   assert(radeon_drm_vk_mock.cs_flags_count == 3);
   assert(radeon_drm_vk_mock.cs_flags[0] == RADEON_CS_KEEP_TILING_FLAGS);
   assert(radeon_drm_vk_mock.cs_flags[1] == 0);
   assert(radeon_drm_vk_mock.cs_flags[2] == 0);

   radeon_drm_vk_reloc_list_finish(&relocs);
   radeon_drm_vk_device_finish(&device);
}

static void
test_submit_snapshots_boundary_before_ioctl(void)
{
   radeon_drm_vk_mock_reset();
   struct radeon_drm_vk_ioctl_ops boundary_ops = radeon_drm_vk_mock_ops;
   boundary_ops.command_write_read = boundary_sync_mock_command_write_read;

   struct radeon_drm_vk_device device;
   assert(radeon_drm_vk_device_init(&device, 42, &boundary_ops) == 0);
   boundary_sync_device = &device;

   radeon_drm_vk_bo_cache_sync(&device, boundary_sync_range,
                               sizeof(boundary_sync_range));
   radeon_drm_vk_bo_cache_sync(&device, boundary_sync_range,
                               sizeof(boundary_sync_range));
   assert(device.cache_sync_count == 2);

   struct radeon_drm_vk_reloc_list relocs;
   radeon_drm_vk_reloc_list_init(&relocs);
   uint32_t ib[2] = {0x80000000, 0};
   struct radeon_drm_vk_cs cs;
   radeon_drm_vk_cs_build(&cs, ib, 2, &relocs, 0, false);
   assert(radeon_drm_vk_cs_submit(&device, &cs) == 0);

   /* The mock adds a sync inside the ioctl callback.  A boundary snapshot
    * placed after that callback would observe three instead of two.
    */
   assert(device.submit_boundary_sync_count == 2);
   assert(device.cache_sync_count == 3);

   radeon_drm_vk_reloc_list_finish(&relocs);
   boundary_sync_device = NULL;
   radeon_drm_vk_device_finish(&device);
}

static void
test_empty_reloc_list_builds_zero_length_chunk(void)
{
   struct radeon_drm_vk_reloc_list relocs;
   radeon_drm_vk_reloc_list_init(&relocs);

   uint32_t ib[2] = {0x80000000, 0};
   struct radeon_drm_vk_cs cs;
   radeon_drm_vk_cs_build(&cs, ib, 2, &relocs, 0, false);

   assert(cs.chunks[0].length_dw == 0);
   assert(cs.flags[0] == 0);

   radeon_drm_vk_reloc_list_finish(&relocs);
}

int
main(void)
{
   test_build_constructs_three_chunks();
   test_submit_passes_exact_bytes();
   test_submit_snapshots_boundary_before_ioctl();
   test_empty_reloc_list_builds_zero_length_chunk();
   printf("radeon_drm_vk_cs_test: all checks passed\n");
   return 0;
}
