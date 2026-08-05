/*
 * SPDX-License-Identifier: MIT
 *
 * Host test for GEM BO transport policy, error paths, and PRIME
 * reference counting.
 */

#include "radeon_drm_vk_bo.h"
#include "radeon_drm_vk_completion.h"
#include "radeon_drm_vk_device.h"
#include "radeon_drm_vk_reloc.h"
#include "tests/radeon_drm_vk_mock.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

static void
test_create_passes_domain_and_flag_policy(void)
{
   radeon_drm_vk_mock_reset();
   struct radeon_drm_vk_device device;
   assert(radeon_drm_vk_device_init(&device, 42,
                                    &radeon_drm_vk_mock_ops) == 0);

   struct radeon_drm_vk_bo bo;
   assert(radeon_drm_vk_bo_create(&device, 4096, 256,
                                  RADEON_GEM_DOMAIN_GTT,
                                  RADEON_GEM_CPU_ACCESS | RADEON_GEM_GTT_WC,
                                  false, &bo) == 0);
   assert(bo.handle == 1);
   assert(radeon_drm_vk_mock.last_gem_create.size == 4096);
   assert(radeon_drm_vk_mock.last_gem_create.alignment == 256);
   assert(radeon_drm_vk_mock.last_gem_create.initial_domain ==
          RADEON_GEM_DOMAIN_GTT);
   assert(radeon_drm_vk_mock.last_gem_create.flags ==
          (RADEON_GEM_CPU_ACCESS | RADEON_GEM_GTT_WC));

   radeon_drm_vk_bo_free(&device, &bo);
   assert(radeon_drm_vk_mock.gem_close_calls == 1);
   assert(radeon_drm_vk_mock.gem_close_last_handle == 1);

   radeon_drm_vk_device_finish(&device);
}

static void
test_create_error_propagates(void)
{
   radeon_drm_vk_mock_reset();
   struct radeon_drm_vk_device device;
   assert(radeon_drm_vk_device_init(&device, 42,
                                    &radeon_drm_vk_mock_ops) == 0);

   radeon_drm_vk_mock.create_result_once = -ENOMEM;
   struct radeon_drm_vk_bo bo;
   assert(radeon_drm_vk_bo_create(&device, 4096, 0, RADEON_GEM_DOMAIN_GTT, 0,
                                  false, &bo) == -ENOMEM);
   assert(radeon_drm_vk_mock.gem_create_calls == 0);

   radeon_drm_vk_device_finish(&device);
}

static void
test_map_reaches_gem_mmap(void)
{
   radeon_drm_vk_mock_reset();
   struct radeon_drm_vk_device device;
   assert(radeon_drm_vk_device_init(&device, 42,
                                    &radeon_drm_vk_mock_ops) == 0);

   struct radeon_drm_vk_bo bo;
   assert(radeon_drm_vk_bo_create(&device, 2048, 0, RADEON_GEM_DOMAIN_GTT,
                                  RADEON_GEM_CPU_ACCESS, false, &bo) == 0);
   void *map = NULL;
   assert(radeon_drm_vk_bo_map(&device, &bo, &map) == 0);
   assert(map != NULL);
   assert(radeon_drm_vk_mock.last_gem_mmap.handle == bo.handle);
   assert(radeon_drm_vk_mock.last_gem_mmap.size == 2048);
   radeon_drm_vk_bo_unmap(&device, &bo, map);

   radeon_drm_vk_bo_free(&device, &bo);
   radeon_drm_vk_device_finish(&device);
}

static void
test_prime_refcount_two_imports_one_close(void)
{
   radeon_drm_vk_mock_reset();
   struct radeon_drm_vk_device device;
   assert(radeon_drm_vk_device_init(&device, 42,
                                    &radeon_drm_vk_mock_ops) == 0);

   /* Two imports resolve to the same GEM handle; three frees in total run
    * exactly one GEM close, on the last reference.
    */
   radeon_drm_vk_mock.prime_import_handle = 77;
   struct radeon_drm_vk_bo import_a;
   struct radeon_drm_vk_bo import_b;
   assert(radeon_drm_vk_bo_import_fd(&device, 1077, 4096,
                                     RADEON_GEM_DOMAIN_GTT,
                                     &import_a) == 0);
   assert(radeon_drm_vk_bo_import_fd(&device, 1077, 4096,
                                     RADEON_GEM_DOMAIN_GTT,
                                     &import_b) == 0);
   assert(import_a.handle == 77 && import_b.handle == 77);
   assert(import_a.shareable && import_b.shareable);

   radeon_drm_vk_bo_free(&device, &import_a);
   assert(radeon_drm_vk_mock.gem_close_calls == 0);
   radeon_drm_vk_bo_free(&device, &import_b);
   assert(radeon_drm_vk_mock.gem_close_calls == 1);
   assert(radeon_drm_vk_mock.gem_close_last_handle == 77);

   radeon_drm_vk_device_finish(&device);
}

static void
test_export_requires_shareable(void)
{
   radeon_drm_vk_mock_reset();
   struct radeon_drm_vk_device device;
   assert(radeon_drm_vk_device_init(&device, 42,
                                    &radeon_drm_vk_mock_ops) == 0);

   struct radeon_drm_vk_bo bo;
   assert(radeon_drm_vk_bo_create(&device, 4096, 0, RADEON_GEM_DOMAIN_GTT, 0,
                                  false, &bo) == 0);
   int prime_fd = -1;
   assert(radeon_drm_vk_bo_export_fd(&device, &bo, false,
                                     &prime_fd) == -EINVAL);

   struct radeon_drm_vk_bo shared;
   assert(radeon_drm_vk_bo_create(&device, 4096, 0, RADEON_GEM_DOMAIN_GTT, 0,
                                  true, &shared) == 0);
   assert(radeon_drm_vk_bo_export_fd(&device, &shared, false,
                                     &prime_fd) == 0);
   assert(prime_fd == (int)(1000 + shared.handle));

   radeon_drm_vk_bo_free(&device, &bo);
   radeon_drm_vk_bo_free(&device, &shared);
   radeon_drm_vk_device_finish(&device);
}

static void
test_completion_awaits_with_bounded_retries(void)
{
   radeon_drm_vk_mock_reset();
   struct radeon_drm_vk_device device;
   assert(radeon_drm_vk_device_init(&device, 42,
                                    &radeon_drm_vk_mock_ops) == 0);

   struct radeon_drm_vk_completion completion;
   assert(radeon_drm_vk_completion_init(&device, &completion) == 0);
   assert(radeon_drm_vk_mock.last_gem_create.size == sizeof(uint32_t));
   assert(radeon_drm_vk_mock.last_gem_create.initial_domain ==
          RADEON_GEM_DOMAIN_GTT);

   /* The completion reference is a pure write-domain reloc. */
   struct radeon_drm_vk_reloc_list relocs;
   radeon_drm_vk_reloc_list_init(&relocs);
   uint32_t index;
   assert(radeon_drm_vk_completion_reference(&completion, &relocs,
                                             &index) == 0);
   assert(relocs.relocs[index].read_domains == 0);
   assert(relocs.relocs[index].write_domain == RADEON_GEM_DOMAIN_GTT);
   radeon_drm_vk_reloc_list_finish(&relocs);

   /* One transient -EBUSY, then retirement. */
   radeon_drm_vk_mock.wait_idle_results[0] = -EBUSY;
   radeon_drm_vk_mock.wait_idle_result_count = 1;
   assert(radeon_drm_vk_completion_await(&device, &completion) == 0);
   assert(radeon_drm_vk_mock.wait_idle_calls == 2);

   /* A persistent -EBUSY exhausts the retry budget and fails closed. */
   radeon_drm_vk_mock.wait_idle_calls = 0;
   radeon_drm_vk_mock.wait_idle_results[0] = -EBUSY;
   radeon_drm_vk_mock.wait_idle_results[1] = -EBUSY;
   radeon_drm_vk_mock.wait_idle_results[2] = -EBUSY;
   radeon_drm_vk_mock.wait_idle_result_count = 3;
   assert(radeon_drm_vk_completion_await(&device, &completion) == -EBUSY);
   assert(radeon_drm_vk_mock.wait_idle_calls == 3);

   radeon_drm_vk_completion_finish(&device, &completion);
   radeon_drm_vk_device_finish(&device);
}

int
main(void)
{
   test_create_passes_domain_and_flag_policy();
   test_create_error_propagates();
   test_map_reaches_gem_mmap();
   test_prime_refcount_two_imports_one_close();
   test_export_requires_shareable();
   test_completion_awaits_with_bounded_retries();
   printf("radeon_drm_vk_bo_test: all checks passed\n");
   return 0;
}
